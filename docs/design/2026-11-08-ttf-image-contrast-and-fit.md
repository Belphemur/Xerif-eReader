# TTF Image Path — Contrast Redesign & Full-Fit Guarantee (v2)

Date: 2026-11-08
Status: Draft for owner review (design-only; no code in this doc)
Scope: **TTF reader path only** (`TtfImageEngine`, `TtfImageCache`, the SDK render paths it
drives, `ChapterLayout` image placement). The legacy bitmap reader is out of scope and must
not change.

## 0. Context

PR #301 (`96dc72ea` on develop) shipped the 4-level ToneBake4 + absolute-gray routing.
Device soak on the X4 Pro (owner photos, 2026-11-08) shows two remaining defects, both
reproducible on the DanMachi vol-1 EPUB:

- **A — washed-out contrast**: the "Insert" illustration renders almost white; only the
  darkest outlines survive. Panel nominal white is paper-gray, and every midtone lands in
  the panel's two lightest levels.
- **B — large images cropped instead of resized**: the same chapter's illustration paints
  smaller than its placement box with content missing at the edges, blank space below —
  the image is being *cropped/short-read*, not scaled to fit.

Quality bar: match or exceed the legacy bitmap reader's look (the owner considers that
path "doing the image justice").

## 1. Root causes

### A. The bake destroys tonal separation on a 4-level panel

Current pipeline (ToneBake4, `TtfImageCache.h:52-…`, merged as `96dc72ea`):

```
gray → gammaLut (out = 255·(i/255)^(1/2.2)  — BRIGHTENING)
     → contrastLut ((v−128)·1.3 + 128)
     → + FS error → nearest of {0, 85, 170, 255}
```

Mechanism (verified numerically): source mid-gray 128 → gamma 2.2 maps it to **186**
(255·(128/255)^(1/2.2) = 186.4) → contrast 1.3 pivot 128 → **203**. On the 4-level scale
`{0, 85, 170, 255}` that quantizes to the second-lightest level 170 with error 33 (with no
incoming diffusion error). Any source pixel brighter than ~100 lands at 170 or 255. The
gamma brightening is the 1-bit reference's recipe (harperreed/eink-image): there it exists
to push tones toward the *dither threshold* so a 1-bit dot pattern can represent them. On
a 4-level panel it is destructive — it collapses the source's midtones into the panel's
two lightest levels before diffusion ever runs. The photos are the exact signature of this.

Secondary factors:

1. **Order**: contrast-after-gamma operates on brightened values with a fixed pivot 128,
   which further lifts the distribution.
2. **Fixed contrast 1.3** cannot rescue pale scans (the "Insert" source itself is
   low-contrast) nor dark ones; the needed op is a **per-image dynamic-range stretch**,
   not a fixed slope.
3. **Level targets**: the panel's absolute-gray waveform levels are not at nominal
   reflectances; quantization thresholds in mapped space should still be nearest-of-4
   (they move with the LUTs), but the *pre-quantize map* must spread tones across the
   full 0–255 range.

### B. Placement vs. decode dimensions can diverge → short read = crop look

`ChapterLayout::placeImage` (`freeink-sdk/libs/book/FreeInkBook/src/layout/ChapterLayout.cpp:1338-…`)
fits correctly: box-filter fit into the content box, aspect preserved, never upscaled,
centered. The defect is downstream. Candidates (verify each, find the actual file:line):

- **C1 — baseline JPEG clamp**: `ImageRenderer.cpp:268-269`
  `dstW = min(image.width, srcW); dstH = min(image.height, srcH);`
  When the decoded source is smaller than the placement (or after TJpgDec power-of-two
  scale selection at :257-263 the banded decode is smaller than the placement), the cache
  is written at *decoded* dims ≠ `spec.w/spec.h`. Whether `readValidCacheHeader`
  (`TtfImageEngine.cpp` blit path) validates dims or merely reads them decides whether
  this yields a rejected cache (safe) or a **short blit at spec.x/spec.y** (the crop).
- **C2 — progressive JPEG DC path**: `renderJpegProgressive` emits `blocksH` rows of a
  1/8-scale DC grid, bilinear-upscaled to the placement. If any guard (`emittedBlockRows`,
  `padBlocksW` clipping at :644-646, or `rs.feed`'s `y1 > srcY` break) drops rows/columns,
  the blit covers only part of the placement → truncated image + blank space (matches the
  ch-1 photo exactly).
- **C3 — probe/table mismatch**: `placeImage` uses `probed_[p].info` (pre-scan table); if a
  stale table entry or the inline-fallback probe returns different dims than the decode
  later sees, placement and decode diverge.

**Contract**: whatever the mechanism, the fix must guarantee the invariant
*decoded+cached pixel grid == placement box exactly* — either by making the decode
produce exactly `spec.w × spec.h` rows/cols, or by rejecting/falling back when it cannot.
A host test must pin this invariant (decode a known image at a fitted placement, assert
full coverage of the box).

#### Amendment 1 (2026-11-08, root cause found — C1/C2 refuted, paint bounds were the defect)

Host coverage pins through the real decode stack (suite `test/ttf_image_fit`, EPUB
fixtures `test_jpeg_images.epub`/`test_mixed_images.epub` + a generated progressive-JPEG
EPUB) **refute C1 and C2 as crop mechanisms**: for every probed-correct placement the SDK
decoders emit exactly the placement grid — `renderJpeg`'s scale selection cannot exit with
a source dimension below the placement (both dims are checked ≥ placement *before* each
increment, so `min()` at :268-269 always selects the placement), and the progressive
DC resampler emits all `dstH` rows (feed loop + `finish()`), with the engine's
width-mismatch guard aborting the decode (not short-painting) in the impossible case.
`ToneCacheWriter::finalize` additionally refuses any short row count, and
`readValidCacheHeader` enforces dims ±1 — a short **cache** is impossible; a short
**paint** was the defect:

- **Actual root cause — the engine painted/clipped with the PHYSICAL panel dims.**
  `TtfImageEngine.cpp`'s `paintRowOnScreen`/`paintColOnScreen` (rowBakeFn) and the
  `blitFromCache` row/column guards used `getDisplayWidth()/getDisplayHeight()` — the
  800×480 panel geometry — while the reader runs in `Portrait`: **480×800 logical
  coordinates** (`GfxRenderer.h:40`). Every image row at logical y ≥ 480 was silently
  dropped, in the fresh decode paint and the cache blit alike: the illustration shows only
  its top portion, content is missing at the clipped edge(s), the rest of the placement is
  blank — the reported defect, portrait-only and only for placements reaching below
  logical y = 480 (i.e. exactly the large images). The worker path is unaffected
  (`paintToTarget=false`), and the cache itself is complete and valid, so every pass
  re-blits and re-drops the same rows — a stable crop.
- The correct bounds are the orientation-aware **logical** screen dims
  `getScreenWidth()/getScreenHeight()` — the same accessors `ImageBlock::render` (legacy
  reader, `ImageBlock.cpp:367-368`) and the GIF converter (`GifContext.screenWidth/Height`)
  already use. Fix: switch the two helpers and the blit guards to the logical accessors.
  The legacy bitmap reader never had the bug and is untouched.
- Pins: `ImageFitPaint.*` (BMP decode paint: ≥2x downscale, near-1x, boxes below and
  straddling the physical height — fail pre-fix on exactly the dropped rows),
  `ImageFitBlit.*` (cache blit, same), `ImageFitSdk.*` (baseline JPEG ×3 sources, PNG,
  progressive JPEG DC path — full-grid invariant at ≥2x / 1:1 / near-1x / placeImage-fit
  placements), `ImageFitGif.*` (converter exact-dimension box below the physical height).

## 2. Design

### Phase B-first: full-fit guarantee (defect B)

Do this first — tone quality is irrelevant on wrong pixels.

1. Root-cause the crop with a failing host test first (test-driven): construct a source
   image whose decoded dims vs placement reproduce the divergence; run the blit; assert
   coverage.
2. Fix the identified mechanism so the invariant holds: decode-to-placement-size (resample
   to `spec.w × spec.h` in the existing streaming scalers — they already accept arbitrary
   dst dims) or reject-and-skip when the source cannot fill the placement.
3. Bump the cache identity if the cache format/semantics change (see §4).
4. Host pins: full coverage of the box for baseline JPEG, progressive JPEG, PNG, BMP, GIF
   at both ≥2× downscales and near-1× scales.

### Phase A: adaptive-contrast tone bake (defect A)

Replace ToneBake4's map with a 4-level-native pipeline (keep the name `ToneBake4` struct
shape or a new baker; DRY across all four decoders as today):

```
gray → per-image dynamic-range stretch (§2.1)
     → optional mild gamma for panel perceptual match (§2.2)
     → + FS error (0.8, serpentine — unchanged) → nearest of {0,85,170,255}
```

#### 2.1 Adaptive stretch

Compute the used dynamic range **per image** and stretch it to [0, 255]:

- During the streaming bake, accumulate a 256-bin histogram of source gray (cheap: 256×u16
  in the bake object, no full-image buffer needed for collection).
- The bake is row-streaming (cache rows appended as decoded). Stretch requires knowing the
  range before emitting tones → **two-pass over the temp tone file**: the bake writes the
  stretched *later*… resolved by baking twice: pass 1 decodes and only collects the
  histogram + writes raw 8-bit gray rows to the `.t` temp (no cache); pass 2 reads the
  temp rows, applies stretch+gamma+diffusion, writes the 2bpp cache rows, paints. Decode
  cost doubles on first view of each image (the predecode worker absorbs it ahead of the
  page turn); steady-state cost is zero (cache). If double-decode is unacceptable for the
  foreground path, alternative: single pass collecting histogram while writing 8-bit gray
  rows to temp, then remap temp→cache without re-decoding (preferred — one decode, one
  extra disk pass over a small temp file).
- Percentile clip, not min/max: clip at ~1%/99% (integer math) to reject dust/spine
  shadows; guard degenerate histograms (flat scans) — fall back to fixed contrast 1.3.
- Deterministic + host-testable: pure integer math, pinned byte-exact outputs.

#### 2.2 Gamma

- Default the gamma stage to **off** (identity) for the 4-level path: the stretch already
  uses the full range, and the panel's absolute-gray waveform owns perceptual matching.
  Keep the LUT machinery with γ configurable so a device-side tune can enable a mild
  curve (the epdiy 4-bit reader uses 1/0.8 — see resources) without code churn.
- A/B switch `imageDecodeQuality = 0` keeps today's revert semantics.

#### Amendment 2 (2026-11-08, as-built for the adaptive bake)

- **Gray temp naming**: the phase-1 gray stream lives at `<cachePath>.g` — NOT the `.t`
  the sketch proposed — because `.t` remains the 2bpp cache's atomic-publish temp; the
  remap writes `<cache>.t` and publishes onto the final name exactly as before, so the
  interrupted-decode invariant (a torn write never reaches the final name) is preserved
  and the two temps cannot collide. The gray stream is a raw row-major 8-bit block (no
  header — the caller knows w×h from the spec) and is removed on every exit path; a
  power loss between phases leaves a stale `.g` that the next decode truncates and the
  book cache sweep collects.
- **Bake API**: `ToneBake4` gains `beginCollect(path)` / `collectRow(gray)` (phase 1:
  saturating 256-bin u16 histogram + gray-row append; the bake owns the file handle so
  every decoder routes through one writer) and `beginRemap()` (percentile clip
  `n·1%`/`n·99%` in u32 cumulative math → integer stretch LUT ⊙ gamma-when-enabled →
  `mapLut`; FS error state + serpentine reset). `toneRow()` keeps its signature and uses
  `mapLut` once `beginRemap()` ran, so the phase-2 loops of all four decoders are the
  familiar serpentine call. Degenerate histogram (empty or `hi ≤ lo`) falls back to the
  fixed contrast 1.3 map **without gamma** — a single-tone image has nothing to stretch.
- **One remap pass for all decoders**: the engine's `remapGrayToCache(spec, bake, …)`
  streams `.g` back, bakes, appends the 2bpp cache rows and paints — shared by the SDK
  JPEG/PNG row sink, `decodeBmpRows` (now collect-only) and the GIF converter in a new
  **collect-only mode** (`RenderConfig::toneCollect`: `qFlushRow` feeds
  `bake->collectRow()` and writes no cache/paint; `config.cachePath` stays empty so the
  converter's own PixelCache machinery is bypassed). Worker/foreground parity is one
  code path by construction.
- **Gamma configurability** is the compile-time constant `ToneBake4::kGammaEnabled`
  (default `false`) next to the retained `kGammaNum/kGammaDen` — a device tune flips one
  constant; no settings storage.
- **Latency**: identical to the old pipeline on first view (one decode) plus one small
  disk pass over the gray temp (~0.3 MB for a full-page placement); the paint now happens
  in the remap pass after the decode — invisible on e-ink (the frame commits once at the
  end regardless). Steady state is the 2bpp cache as before.
- **Cache identity**: bake change → `kQualityCacheRevision` `'r'` → `'s'`.

##### Amendment 3 (2026-11-08, review round 1 hardening)

- **Histogram bins are u32** (1 KB in the bake object): percentile targets compare
  against n·1%/n·99% of the FULL pixel count, so a bin must hold the whole image — u16
  saturation on a flat full-page fill left `hi` at 255 and collapsed the stretch back to
  near-identity (the defect-A mechanism surviving inside the fix). Pinned by
  `AdaptiveBake.FullPageFlatFillPercentileNotSaturatedByU16Bins` (454×774, 90/10 two-tone).
- **Gray-temp unavailability degrades to uncached painting** (the pre-#301 contract):
  a failed `.g` open or a failed/short mid-stream write routes the decode through
  `legacyPaintPass` — a re-decode with the deterministic degenerate map (fixed contrast
  1.3, gamma off via `ToneBake4::beginLegacyFallback()`) straight to the target, no cache,
  no temp. Cache-only jobs (worker) no-op. `decodeBmpRows` gained an optional
  `legacyPaint` mode; the GIF converter's existing legacy quality paint serves the GIF
  fallback. Pinned by `ImageFitFallback.*` (open failure + mid-stream write failure →
  full box coverage, no cache debris).
- **Degraded cache streams no longer publish**: `remapGrayToCache` reports cache-stream
  completion through a `cacheStreamOk` out-param; the callers skip `publishResult` and
  return Ok (rows painted) when the writer could not finish — the pre-PR BMP contract.
- **GIF collect mode fails loudly on buffer-allocation failure** instead of silently
  rendering without collecting (which would strand the remap with a short gray stream).
- `beginCollect` validates the `snprintf` result before recording the path (a truncated
  path must never be opened).

## 3. Parity requirements (unchanged from PR #301)

- All four decoders (SDK JPEG/PNG rows, BMP decode, GIF converter) route through one bake.
- Worker/foreground cache parity (byte-identical `.t` temp semantics; same bake letter).
- Serpentine row bake; the ImageDecodeGate bus rules; scratch mark/release hygiene.
- No heap growth; no float in the per-pixel path; lean-device rules.

### 4. Cache identity

- Bake algorithm change → bump `kQualityCacheRevision` letter `'r'` → `'s'` (stale
  processed caches must never display).
- If defect B's fix changes cache dimensions/semantics, that letter bump covers it; verify
  the hash identity (generation + href + placement) still distinguishes placements.

## 5. Research (pi fetches and cites these)

1. **harperreed/eink-image** — <https://github.com/harperreed/eink-image> — the reference
   whose constants (γ 2.2, contrast 1.3, 0.8 diffusion) PR #301 adopted. Cite its pipeline
   as 1-bit-oriented; explain explicitly why brightening-in-the-map does not transfer to a
   4-level panel.
2. **OpenDisplay/epaper-dithering** (tone compression) —
   <https://github.com/OpenDisplay/epaper-dithering/blob/main/packages/python/README.md> —
   `tone="auto"` histogram-driven dynamic-range remap to the display's range before
   dithering (based on esp32-photoframe's `fast_compress_dynamic_range`). This is the
   adaptive-stretch reference for §2.1.
3. **atomic14/diy-esp32-epub-reader** (DeepWiki display chapter) —
   <https://deepwiki.com/atomic14/diy-esp32-epub-reader/6-display-and-rendering> — 4-bit
   grayscale with a γ = 1/0.8 curve applied at pixel draw; evidence that 4-level panels
   use gentle curves, not 2.2 brightening.
4. **Good Display e-paper dithering guide** —
   <https://www.good-display.com/news/19.html> — "boost contrast by 20-30% before
   dithering" guidance for e-paper.
5. **Adafruit, Preparing Graphics for E-Ink Displays** —
   <https://learn.adafruit.com/preparing-graphics-for-e-ink-displays> — diffusion-amount
   trade-off ("too contrasty" vs "snowy") for tuning the 0.8 weight.
6. **Legacy baseline** — `lib/GfxRenderer/BitmapHelpers.cpp` in this repo (and
   daveallie/crosspoint-reader upstream): contrast 1.15, no gamma by default, fixed
   thresholds — the on-device look the owner rates as good; use as the sanity baseline.

## 6. Deliverables & gates

1. Design doc amended in lockstep with any deviation (same commit).
2. Defect B: failing-test-first root cause, fix, full-coverage host pins.
3. Defect A: adaptive-stretch bake + optional-gamma machinery, DRY across decoders,
   deterministic host pins (byte-exact tone rows for pinned inputs).
4. Cache rev bump(s); GIF/BMP parity; revert switch intact.
5. Gates: `pio run -e x4pro`, `-e default`, host tests, cppcheck, clang-format — all green
   before commit/push.
6. Conventional PR title (Title Check); squash-merge by owner later.
7. Report-first discipline: write `~/.pi/supervisor/reports/<job>.md` at round start and
   update after each commit; marker `TTF_IMAGE_CONTRAST_DONE`.
