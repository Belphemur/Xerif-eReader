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

##### Amendment 4 (2026-11-08, kody sweep on the rebased head)

- **Gray-write failure is sticky and stops the collectors**: `ToneBake4::collectRow`
  writes/logs at most once (`grayOpen && !grayFailed`); the new `ToneBaker::grayWriteFailed()`
  probe lets the JPEG/PNG row callback, the BMP row loop and the GIF converter's
  `qFlushRow` stop feeding rows instead of finishing a decode whose temp is dead. The
  JPEG/PNG render failure carrying `collectFailed` routes to `legacyPaintPass` (the
  storage-failure doctrine), and a collect-mode GIF/BMP decode failure now falls through
  to `legacyPaintPass` too instead of returning Failed with a blank placement (aborts
  still return `Busy` first; in legacy mode the failed decode already was the paint
  attempt). Pinned by `ImageFitFallback.GrayWriteFailureStopsRetryingWrites`.
- **Gray temp handles are released before removal**: `remapGrayToCache` closes the read
  handle before every `grayDiscard()` (FAT refuses unlink on an open handle), and
  `grayDone()` keeps `grayOpen` set on a failed close so the engine callers' discard can
  still close + remove; `grayDone`-false paths in `decodeAndCache` now discard explicitly.
  The fit-suite `HalStorage` stub gained FAT-faithful open-handle remove refusal, which
  makes `AdaptiveBake.GrayTempStreamsEveryRowByteExact` and the end-to-end no-`.g`
  asserts fail without the fix (non-vacuous).
- **Test scaffolding hygiene**: `HostFileSource` holds its `FILE*` in an RAII
  `unique_ptr` with the `fclose` deleter; `gen_fixtures.py` writes every fixture through
  a temp + `os.replace` (atomic generation).
- **Deferred (kody, perf)**: band-buffering the gray temp (4–8 KB write/read bands to
  cut HalFile transactions ~16×). Rejected for this PR: SdFat coalesces the contiguous
  row writes in its sector cache, so physical SD traffic is already per-sector rather
  than per-row, and the band buffer would add a decode-path heap allocation on the
  380 KB-RAM baseline this firmware targets. Revisit only if SD-lock telemetry shows
  real contention.

##### Amendment 5 (2026-11-10, fidelity fix — decoder traps, halo-free sharpen, Sierra-Lite quantizer)

Owner photo-vs-source A/B on the X4 Pro soak pinned four failure modes on three
pages (all sources verified byte-identical to `test-images/epub/`):

| Source | Format | Device photo | Symptom |
| --- | --- | --- | --- |
| `00004.jpeg` | progressive 577×1400 | PXL_…235030137 (GOOD) | reference page |
| `00005.jpeg` | baseline 934×1400 | PXL_…235037333 | muddy wall, clumpy fade, broken fine lines, smeared © text |
| `00006.jpeg` | baseline 965×1400 | PXL_…235049369 | 3.6 px Lego blocks in white paper, stair-block letterforms, muddy art |

**Part 0 verdict (measured on host with the real decode stack).**

- *Claim B confirmed (both bad pages)*: the baseline pre-scale loop
  (`ImageRenderer.cpp` `renderJpeg`) picked TJpgDec scale=1 — 934×1400 decoded at
  467×700 for a 454×680 placement, 965×1400 at 482×700 — and the app box re-resampled
  the 1.03–1.09× remainder. Every 1.5 px ink line landed on the power-of-two grid:
  the Lego/stair blocks and the clumpy fade are that re-resample's signature (host
  probe: dims hook reported 467×700 / 482×700 pre-fix, full frame dims post-fix).
- *Claim A refuted as stated*: the progressive DC-only fallback is not in play on
  `00006` (baseline SOF0; TJpgDec serves it). The fallback mechanisms the owner
  flagged ARE real and fixed anyway: `renderJpegProgressiveFull` could report frame
  dims and then fail mid-decode, the DC fallback re-reporting 1/8-scale dims (double
  report); a DC-thumbnail was cache-eligible (`degraded` tracked only detail/CLAHE
  rung loss); `ProgressiveJpeg::emit` allocated its IDCT strip from the internal heap.

**Fixes (SDK branch `feat/jpeg-fidelity` off b3efb446a, app on this branch).**

1. *Pre-scale 1.5× margin* (SDK): TJpgDec scale steps only while the halved dims
   still leave a 1.5× margin on both axes — `934 → 454` decodes at full resolution.
2. *Single dims report* (SDK): the full-fidelity progressive path defers the dims
   report to the first emitted row (the PNG path's dimsSent shape); a mid-decode
   fallback reports once (1/8-scale dims) and LOG_WRNs status + SOF component count
   (`probeImage` now reports `jpegComponents`).
3. *PSRAM emit strip* (SDK): `ProgressiveJpeg::emit` allocates the stride·8 strip
   from the same PSRAM-backed store pool as the coefficient store; internal-heap
   fragmentation no longer drops a wide page to the DC fallback.
4. *DC-fallback thumbnails* (app): the probe result rides in `DpidCtx`; a
   progressive stream reporting `srcW < info.width` sets `dcFallback` — bilinear
   class (nearest suppressed via `DpidScaler::Plan::bilinear`) and `degraded` (never
   published under the requested quality's cache name).
5. *Halo-free dark-stroke sharpen* (app, this amendment's §2 slot): after the
   scaler's emit walk, before `collectRow`. Pure core `sharpenRowHaloFree`
   (TtfImageCache.h): 3×3 tent (1,2,1)⊗(1,2,1)/16, ±3 coring deadzone, asymmetric
   Q4 gain (dark strokes 16/16, light side 6/16), output strictly clamped to the
   original 3×3 [min,max] — zero overshoot, no halos. Streaming wrapper: 3-row
   original ring + one output row (4 × spec.w ≈ 1.8 KB at the 454 px placement)
   from its own pool block; allocation failure or out-of-order rows degrade to
   un-sharpened. Gate `sharpenOk` (>1.25× on either axis, the tentPreblurOk shape)
   — 1:1 ornaments and the Insert-page upscale are untouched. The one-row delay
   flushes through the same sinks after `finish()` (decode failure paths included).
6. *Sierra-Lite quantizer* (app): `ToneBake4::toneDir` rebuilt — levels
   {0, 80, 155, 255} (level 2 off the eink-image 170), midpoints {40, 117, 205};
   white-paper snap (mapped ≥ 252 → level 3, slot zeroed, no diffusion); serpentine
   Sierra-Lite in exact 1/4-level units (forward 2/4 + consume residue, down-behind
   1/4, down 1/4 — bit-exact mass conservation, no ±8 threshold noise, no 0.8
   weight). BeginRemap paper-guard implemented verbatim (owner directive): with
   ≥5% non-paper pixels the 250..255 bins are ignored when locating the dark anchor.
   **Measured scope note**: the guard is inert under the current percentile
   arithmetic — the ascending lo loop already lands on content whenever content
   ≥ 5%, and the anchor only enters the paper bins when content < 0.5% (guard does
   not fire; pre-campaign behavior stands, pinned as
   `PaperGuardKeepsTheDarkAnchorOnWhiteHeavyPages` pin 2). White-page cleanliness
   rides on the white-paper snap + the corrected decode instead.
7. *In-file cache identity header* (app, owner steer 0b): `.pxc` files now open
   with an 11-byte header — magic `'C','P'`, bake revision letter, identity-hash
   top 32 bits, then the dims — verified on every read (`hasValidCache`,
   `blitFromCache`), together with the file length. What the check proves: the
   file was written by THIS bake letter under THIS placement identity and is
   long enough to hold its declared grid — an older bake, a wrong placement,
   a torn/short write surviving finalize are all caught and the file is
   removed and regenerated. What it does NOT prove: the payload bytes are
   intact — a same-length bit-flip inside the tone data passes the header
   check and displays until the file is regenerated some other way, and two
   images whose 64-bit identity hashes collide (top 32 bits equal, same
   length) are indistinguishable. Cache letters bump 'm'/'n' → 'o'/'p' (both
   never used in history — swept); the letter change alone invalidates every
   pre-campaign cache.

**Host A/B (real decode stack, owner pages, placements 319×774 / 454×680 / 454×658).**
Metrics over the final tone grid vs the scaler's gray (lower toneErr = truer tone;
salt = tone-isolated single pixels; hardSteps = ≥2-level adjacent jumps — crisper
edges raise it by design):

| page | bake | toneErr | salt | hardSteps |
| --- | --- | --- | --- | --- |
| 00005 | old (FS 0.8 + noise, pre-scale 467) | 26.81 | 12407 | 1264 |
| 00005 | new quantizer | 29.80 | 14007 | 2756 |
| 00005 | new quantizer + sharpen | 29.62 | 13934 | 3959 |
| 00006 | old | 4.10 | 504 | 1566 |
| 00006 | new quantizer + sharpen | 4.11 | 562 | 3909 |
| 00004 | old | 38.41 | 8675 | 1585 |
| 00004 | new quantizer + sharpen | 38.82 | 8715 | 2724 |

toneErr is measured against each bake's own level set (old {0,85,170,255}, new
{0,80,155,255}) — cross-bake comparisons of toneErr are indicative only; the
gallery vision check is the arbiter. hardSteps roughly doubles with sharpening — that IS the contrast restoration; the galleries (vision
-checked) show the decisive deltas: 00005's faces/hair recover definition and the
wall's clumped 3.6 px bricks dissolve into uniform fine-grain dither; 00006's
letterforms lose the stair-block raggedness and Bell's boots tighten with cleaner
white paper; 00004 (the good page) stays at least as good, slightly crisper.

Flat-field duty under the new quantizer (texture suite re-pins): flat 91 → 85.7%
level 1 (predicted (91−80)/(155−80) = 14.7% level 2), flat 13 → 84.1% level 0
(predicted 16.25% dropout), flat 126 → 38.4/61.6% — Sierra-Lite is tone-correct to
within ~1 point of the ideal duty on all three.

**Decode budget**: the new stages are O(dst) integer — sharpen ≈ 30 ops/px on 454 px
rows + the ring copy; Sierra-Lite replaces the old plane arithmetic 1:1 (no new
plane pass). The pre-scale change decodes MORE pixels (full 934×1400 vs 467×700):
TJpgDec IDCT work scales ~4× on such pages — measured against the 00001-class page
(889×1400), which already decoded full-resolution pre-campaign under the same ~2 s
cold budget, so the budget bound stands; the sharpen/quantizer additions are noise
against it.

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

##### Amendment 6 (2026-11-11, iteration 3 — owner audit: panel-calibrated quantizer, Atkinson diffusion, sharpen darkFloor, decode arena)

The owner's post-flash audit (attachment `pasted-text-2026-11-11_10-45-57-036.md`,
"We have more fix to do, this is the way forward" — normative owner directive)
measured the PR #303 head on the three soak pages and named one build trap plus
three algorithmic defects. Every fix below implements that audit verbatim.

- **Decode arena (audit §1.2, build trap)**: a deflated full-page JPEG needs
  ~74–81 KiB inside the decode arena (inflate window + tinfl ~46 KiB, skipBuf +
  TJpgDec ~9 KiB, srcW×16 band 15.5–22 KiB, RowScaler ~3.5 KiB);
  `ImagePredecodeWorker::kDecodeArenaBytes` was 32 KiB, so every deflated
  934–1400 px page (the two baseline pages the audit measures) stepped down
  through `renderJpeg`'s arena retry and painted at half resolution, uncached,
  with the `Decode scale reduced below the preferred pick` warning. Raised to
  128 KiB (PSRAM-backed, outside the DRAM pool; the 5 MiB engine budget pins
  hold — the budget gate measures the store + scaler scratch, the arena is a
  separate reserve-time PSRAM block).
- **Sharpen darkFloor (audit bug A)**: after the ~2.12× box downscale a 1.5 px
  black stroke's center IS the destination 3×3 minimum, so the old
  `outv >= mn` clamp forbade 100% of thin-stroke darkening (the PR's own
  `DarkStrokeRecoversItsShoulder` pin codified it: `out[7] == 120`). The dark
  side is no longer clamped to `mn` — dark gain is 1.25× (`kSharpenDarkQ4 20`,
  was 16/1.0) bounded by the local contrast span
  (`darkFloor = mn − (mx − mn)`, never below 0), so `[255,120,200,255]`
  recovers the stroke center (measured 60) while soft shading (amplitude ≤ 10)
  stays byte-identical. The bright side keeps the strict `outv <= mx` clamp
  (zero white halos) at 0.25× gain, coring is 3 → 6 so the 8×8 JPEG mosquito
  ripple (±6) is never sharpened, and the tent blur/Q4 rescale carry the
  audit's +8/−8 rounding.
- **Panel-calibrated quantizer (audit bug B/C)**: the X4 Pro UC8279's physical
  levels derive from the legacy `BitmapHelpers` err targets {15, 35, 90, 210}
  scaled to paper white — `kLevels {0, 48, 112, 255}`, midpoints
  `{28, 78, 182}` (were {0, 80, 155, 255}/{40, 117, 205}). Level 2 sits near
  ~112 on the panel, not 155, so the beige wall and blonde hair of 00005 bake
  onto the panel's own tone instead of 50–60% dark dots.
- **White snap 240 (audit bug B)**: `kWhiteSnap 252 → 240` (the KCC-standard
  white-point clip). 8×8 DCT quantization ringing (240..251) around black text
  on white paper no longer accumulates through the diffusion plane into
  3.76×3.76 px dot clouds — the "Lego blocks" of 00006's white field.
- **Serpentine Atkinson diffusion (audit bug C)**: the 100%-conserving
  quarter-level Sierra-Lite plane is replaced by the audit's serpentine
  Atkinson in exact 1/8-level units — six neighbors at 1/8 (forward 1, forward
  2, next row behind/below/ahead, two rows down below; planes `int32_t[w+4]`
  with a two-slot right margin so the last column's forward-2 term lands
  exactly on index w+3, no per-pixel bounds checks), consume rounding
  `e = (e8+4)>>3` / `−((4−e8)>>3)`, residual + 2/8 absorbed at consume. RAM
  +1 plane ≈ 1.8 KB at the 454 px placement. Measured trade-offs, kept
  deliberately (the owner ordered Atkinson over the worm-prone 100%
  conserving plane — KCC/HyperDither/the legacy `AtkinsonDitherer` path):
  band-boundary entry bursts (a full row's duty switching on then off —
  `GradientHasNoSustainedDensityOscillation` re-pinned 2 → 8 isolated bursts,
  measured 7), a slightly higher gradient row-delta floor (measured 6.06/8.54
  vs the mode-lock defect signature 16.6 — `GradientHasNoRowPeriodicBanding`
  re-pinned 6/6 → 10/11), and the low-duty dead zone (a flat field with a
  ~10% bracketing duty parks on the lower level — `FlatMidtone…` documents
  measured 100% level 2; the tone error is the panel's own nearest tone).
  Overall gradient tone preservation holds (measured avg 127.94 vs ideal
  127.5 on the 30→210 ramp).
- **Cache letters (Fix C)**: `cacheRevisionLetter` 'o'/'p' → 'a' (Sharp) /
  'b' (Enhanced) — both swept unused in this file's history; every pre-audit
  cache regenerates. Legacy 'l' unchanged.
- **Host pins**: Sharpen suite re-measured to the new contract (dark recovery
  `out[7] < 100`, shoulder `<= 200`, soft shading byte-identical + direct
  darkFloor arithmetic pin, JPEG-ring ripple byte-identical); ToneBake4 plane
  tests re-derived for the Atkinson plane (6/8 spread, consume rounding,
  snap-240 slot zeroing at `err[x+2]`); boundary picks re-pinned to the
  {28, 78, 182} midpoints; texture flat-field duties re-measured (flat100:
  72.5% level 2 vs the 67% prediction). Budget pins
  (`ProgressiveBudget.*`, `ImageBudgetGate.*`, `PsramBudget.*`) hold
  unchanged.
