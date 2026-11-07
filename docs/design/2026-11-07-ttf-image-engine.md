# TTF-path image engine — predecode, format parity, quality

Date: 2026-11-07
Status: Draft for owner review (design-only; no code in this doc)
Supersedes: `2026-11-06-image-predecode.md` (BITMAP-path scope, closed with PR #292)

## 1. Summary

The native-TTF reader engine paints images synchronously inside the page
paint: `PageRenderer::renderImages` → `ImageRenderer::render` decodes every
image on the page (streaming PNG/JPEG decode + error-diffusion blit) into the
live framebuffer on the loop task. On the X4 Pro this produces multi-second
loop stalls on first view of image pages (measured 3.5–4.1 s loop maxima and
3.8–4.5 s clearScreen→displayBuffer on DanMachi vol 01, 2026-11-07 soak).

Additionally the TTF path supports only non-interlaced PNG and JPEG: GIF,
BMP, SVG and WebP are skipped at layout (`probeImage` → `Kind::Unknown`) and
render as nothing at all.

This design replaces the TTF path's image handling with a proper image
engine:

1. **Predecode** — a core-0 worker decodes upcoming images in the background
   into a cached tone file; paint consumes the cache with a row blit.
2. **Format parity** — GIF (first frame) and BMP join JPEG/PNG, matching the
   BITMAP path's supported set exactly.
3. **Quality** — the decode moves off the loop task, giving one decode path
   for all four formats: JPEG/PNG keep the SDK's existing quality (it is
   already area-averaged and error-diffused — see §2), and GIF/BMP, which
   today render nothing at all, are decoded to that same quality bar.

## 2. Current state (verified)

| Concern | Today (TTF path) | Evidence |
|---|---|---|
| Paint entry | `paintCapturedPage` calls SDK statics individually: `renderText`, `renderRules`, `renderImages` | `src/activities/reader/QuickPageRelayout.cpp:69-117` |
| Image decode | `PageRenderer::renderImages` → `ImageRenderer::render(source, zip, image, scratch, ImageBlit::onRow, &blit)`, synchronous, loop task | `freeink-sdk/.../render/PageRenderer.cpp:224-247` |
| Formats | PNG (non-interlaced) + JPEG (baseline + progressive DC path). GIF/SVG/WebP → `Kind::Unknown` → **skipped at layout, no reserved space** | `freeink-sdk/.../include/epub/ImageProbe.h:23-26` |
| Scaling | **Area-averaged** (SDK: box filter via `sums`/`counts` accumulator — `emitAveraged`, `ImageRenderer.cpp:40-86`; TJpgDec power-of-two prescale then `RowScaler` remainder for JPEG, `ImageRenderer.cpp:255-276`) | same — the app converter path (BITMAP engine) is the nearest-neighbor one: 16.16 fixed-point, `JpegToFramebufferConverter.cpp:42-49` |
| Dither | Error-diffusion at blit time (per-row `err`/`errNext`, `PageRenderer.cpp:230-239`) | same for the SDK path; app converter path uses Bayer 4×4 (`DitherUtils.h`) |
| Page cache | `.fibp` blobs carry per-page `images {x, y, w, h, href}` — layout data already exists for detection | `freeink-sdk/.../include/cache/PageCache.h` format block |
| BITMAP path | Full converter set (JPEG/PNG/GIF-first-frame/BMP) via `ImageDecoderFactory`; `.pxc` streaming 4-level tone cache | `lib/Epub/Epub/converters/` |

Position dependence (carried over from the closed design): both Bayer (absolute
`outX/outY`) and error-diffusion (row-sequential `err` state seeded at image
`x`) make cached pixels depend on the image's screen position. Cache keys must
capture placement.

## 3. Goals

- G1: First view of an image page costs no more than a text page plus a blit
  (target carried from the closed design: within ~250 ms of a text-only page
  turn at equal panel refresh).
- G2: GIF and BMP images lay out and render on the TTF path (GIF = first
  frame, same as the BITMAP path).
- G3: Cached decode is byte-identical to what the foreground fallback would
  produce (parity by construction: same decode path, worker just runs it
  earlier into a scratch sink).
- G4: Buttons never feel dead — the worker runs on core 0 at low priority;
  foreground always wins (abort-at-boundary semantics carried over).
- G5: No regression for the BITMAP engine path (it keeps its own flow; this
  design only re-points shared pieces).

## 4. Non-goals

- SVG and WebP (owner decision 2026-11-07: BITMAP-path format parity only).
- Animated GIF playback (first frame only, matching the BITMAP path).
- Non-PSRAM boards: the worker compiles to the existing inert stub there.

## 5. Design

### 5.1 Worker (reuse from #292)

`ImagePredecodeWorker` lifecycle carries over unchanged: static DRAM stack +
TCB, core 0 / low priority, fixed-capacity POD job ring (~64 records),
generation guard, `cancel()` + bounded idle-unlink wait, `BOARD_HAS_PSRAM`
gate. The gate condition flips from `READER_ENGINE_BITMAP` to
`READER_ENGINE_TTF` (`EpubReaderActivity.cpp:871-875` region): per owner,
TTF is the enabled engine on all PSRAM devices, so the worker arms exactly
where images actually render through this path.

Job record gains the fields the TTF path needs: spine index, page index,
image index, href, placement (x, y, w, h), capture-time layout generation.

### 5.2 Detection

Jobs are sighted from the `.fibp` page cache: after the TTF background build
commits pages (`pumpChapterChunk` ticks / `openChapterCache`), the sighting
hook reads the pages in the current + next spine (owner scope from the closed
design, unchanged) via `readPage` and enqueues a job per uncached image
record. No new scanning layer — the layout pass already did the work.

### 5.3 Sink and cache

The worker keeps the #292 sink shape: a worker-owned `GfxRenderer` over a
48 KB PSRAM scratch via `GfxRenderer::beginWithScratch(buf, w, h, bytesPerRow)`
— unchanged from the merged-into-branch seam. Per format:

- **JPEG/PNG** are decoded by the SDK `ImageRenderer::render` (unchanged),
  with the worker's row function writing the tone cache. This is what makes
  G3 parity hold: the foreground fallback (`PageRenderer::renderImages`)
  runs the same SDK stack, so worker-ahead and fallback produce the same
  area-averaged rows; only the dither stage differs (see 5.6).
- **GIF/BMP** run through the app converter stack (`ImageDecoderFactory` →
  `GifToFramebufferConverter` / `BmpToBmpConverter`), which stays unchanged.

Output is the same `.pxc` streaming 4-level tone file the BITMAP path uses
(keyed by href + placement + generation), written band-by-band.

Consequence: **one image cache format across both engines**, and the
decoder factory's single-threaded lazy-singleton constraint is already
solved by the factory-init mutex from #292.

### 5.4 Consume side

In `paintCapturedPage` (and `QuickPageRelayout`'s image step), the image pass
becomes:

- For each `PageImage`: if a valid `.pxc` exists for (href, placement,
  generation), blit its tone rows through the live `GfxRenderer` at (x, y)
  — a row-wise blit, no decode, no dither recompute beyond the final 1-bit
  quantization the renderer already performs.
- Else: fall back to today's behavior for JPEG/PNG (SDK `renderImages`), and
  to the app converter for GIF/BMP (see 5.5). The fallback must produce
  byte-identical cache content (G3) so a foreground decode during fallback
  also populates the cache for later views — same first-write-cache semantics
  as the BITMAP path.

No SDK paint changes are required: the app already owns the image step.

### 5.5 Format parity

| Format | Layout (probe) | Decode |
|---|---|---|
| JPEG (baseline + progressive) | already probed | SDK `ImageRenderer` (unchanged) |
| PNG (non-interlaced) | already probed | SDK `ImageRenderer` (unchanged) |
| PNG (interlaced) | already probed | **new**: worker-only path — full-image buffer is affordable in PSRAM; paint never decodes live, so the streaming constraint that forced the rejection no longer binds (stretch item, §9) |
| GIF | **new SDK probe**: `GIF87a`/`GIF89a` signature, logical-screen dims at fixed offset | app `GifToFramebufferConverter` (first frame, proven), running into the worker scratch renderer |
| BMP | **new SDK probe**: `BM` header, DIB dims | app `BmpToBmpConverter`-based path into the worker scratch renderer |

The SDK change is probe-only (dimensions without decode) — the minimal
surface that unblocks layout reservation. Decode stays app-side where the
converters already exist and are soak-proven.

### 5.6 Quality

The correction of record (owner, 2026-11-07): today's TTF paint decode is
**already** area-averaged (`emitAveraged`, `ImageRenderer.cpp:40-86`) and
error-diffused at blit (`PageRenderer.cpp:230-239`). Nearest-neighbor +
Bayer 4×4 is the app-converter (BITMAP) path
(`JpegToFramebufferConverter.cpp:42-49`, `DitherUtils.h`). The decode runs
on core 0 with no frame deadline; the quality work is therefore:

- **JPEG/PNG — parity, not upgrade.** The worker calls the SDK
  `ImageRenderer` directly (still area-averaged; the scaler is unchanged),
  so cached rows match the foreground fallback by construction. The only
  difference between worker and fallback output is the tone-bake stage
  below.
- **GIF/BMP — quality bar.** These formats render nothing today, so their
  worker-side decode is new: area-averaged downscale (box filter, matching
  the SDK `RowScaler`) for ≥ 2× downscales and error-diffusion
  (Floyd–Steinberg, same family as the SDK blit) baked into the 4-level
  tone, eliminating the crosshatch the app converters' Bayer would produce
  on photographs.
- **Tone bake.** The final 1-bit quantization happens at paint; the worker
  reduces 8-bit gray to the 4-level tone with error diffusion rather than
  truncation, for all formats.

Both colour/quality decisions are behind a settings-visible default with
the legacy behavior available (revert switch) — for JPEG/PNG the switch
has no effect (both paths are the SDK stack either way), for GIF/BMP it
reverts to the app converters' today-behavior (nearest-neighbor + Bayer
4-level via `DitherUtils.h`). This matches the repo's pattern for
rendering changes.

### 5.7 Concurrency

The `ImageDecodeGate` single-writer claim registry carries over unchanged:
foreground paint always preempts (abort at band boundaries, worker deletes
the partial `.pxc`), factory-init mutex stays (never a decode-spanning
mutex — it would block the loop task during index-time probes).

## 6. Memory budget (X4 Pro reference)

- Worker stack: unchanged (static DRAM, HWM checked in soak).
- Job ring: unchanged (~14 KB DRAM inline).
- Scratch renderer: 48 KB PSRAM (unchanged).
- `.pxc` band buffer: unchanged (single MCU-row band working set).
- GIF/BMP decode working set: app converters' existing budgets.
- Net new over #292: the probe additions (flash) and the paint-side blit
  helper (small). The BITMAP-only arming condition removal frees the ~14 KB
  + 48 KB on TTF sessions where it previously idled.

## 7. Decisions (owner, 2026-11-07)

1. **TTF path only.** The BITMAP-engine predecode (#292) is not needed: TTF
   is the enabled engine on all PSRAM devices. PR #292 closed unmerged; its
   reusable pieces (worker lifecycle, job ring, decode gate, parity-test
   patterns, `beginWithScratch` seam) carry over.
2. **Fresh branch/worktree**, cherry-picking reusable pieces — clean review
   history. The `TtfUiFallback` negative-cache fix was salvaged first as its
   own PR (#293).
3. **Format scope**: parity with the BITMAP path — JPEG, PNG, GIF (first
   frame), BMP.
4. **Quality**: v1 includes worker-side tone-quality work (see §5.6 as
   corrected) — error-diffused 4-level tone bake for every format,
   area-averaged downscale for GIF/BMP decodes, behind a revert switch;
   JPEG/PNG decode quality matches the SDK by construction (it already
   carries both upgrades — see the 2026-11-07 addendum).

## 8. Execution plan

1. Owner reviews this doc; decisions in §7 become binding.
2. New worktree `_worktrees/ttf-image-engine`, branch `feat/ttf-image-engine`
   off `origin/develop`; design doc committed first (repo convention).
3. Supervised pi run (same brief structure as the closed run): tasks sequenced
   core → integration → tests → gates, separate commits per scope, standing
   git rules (default identity, `-S` signed commits, no trailers, owner
   merges).
4. Gates per commit: host ctest, `pio run -e x4pro`, `pio run -e default`,
   cppcheck (`--fail-on-defect low`), clang-format.
5. On-device soak on the X4 Pro: BITMAP→TTF engine session over the DanMachi
   book (JPEG pages) and a GIF/BMP-containing EPUB; `[IPR]` markers, no
   dead-input windows, G1 timing target from `BOOK_PROFILE`.

## 9. Risks / open items

- **Interlaced PNG** (stretch): worker full-buffer decode is feasible in
  PSRAM; if the parity test cost outweighs the benefit it defers to a
  follow-up without blocking the rest.
- **`.pxc` invalidation on layout generation change**: orphaned caches need
  the same cleanup policy the BITMAP path uses (cache dir sweep on
  generation change).
- **GIF first-frame parity**: the app converter warns "animation (using
  first frame only)"; the parity test must pin the first frame, not frame 0
  vs a later composition state.
- **Placement capture**: `x, y` in `.fibp` blobs are layout coordinates; the
  oriented margins/offsets the paint applies (`orientedMarginLeft/Top`) must
  be captured in the job exactly as #292 did for the LUT path.
- **SDK probe additions** touch the submodule — SDK-side change goes through
  the SDK fork's own review flow before the app pin bumps (owner's standing
  SDK-review rule).

## 10. Addendum — 2026-11-07, scaling/dither attribution correction (owner)

§2 (and with it §1.3, §5.6) originally attributed nearest-neighbor scaling
and Bayer 4×4 dithering to the TTF paint path. Verified against the pinned
SDK (freeink-sdk, digest 24e01a6): the SDK scaler is already area-averaged
(`emitAveraged` with `sums`/`counts` accumulators,
`libs/book/FreeInkBook/src/render/ImageRenderer.cpp:40-86`; both the JPEG
(TJpgDec power-of-two prescale + `RowScaler`, lines 255-276) and PNG paths
use it), and the SDK blit dithering is already error-diffusion (per-row
`err`/`errNext`, `PageRenderer.cpp:230-239`). Nearest-neighbor (16.16
fixed-point, `JpegToFramebufferConverter.cpp:42-49`) and Bayer 4×4
(`DitherUtils.h`) belong to the app-converter path used by the BITMAP
engine.

Consequences applied to this doc: §2 rows corrected; §5.6 rewritten — for
JPEG/PNG the worker must match the SDK's existing quality (parity, not
upgrade), and the area-averaged/error-diffusion language applies as the
quality bar for GIF/BMP, which today render nothing; the decode-seat
language of §5.3 was tightened so JPEG/PNG go through the SDK
`ImageRenderer` in the worker (required for §3 G3 parity); §7.4
re-worded to match. Revert-switch decision (§7.4) unchanged. No other
scope change.
