# TTF image path — grayscale quality (display routing + tone bake)

Date: 2026-11-08
Author: Hermes (orchestrator), design authority: Antoine Aflalo
Status: Draft for implementation (owner-directed: "fix that in a new worktree with pi", "the quality must be the best possible")
Supersedes: nothing — amends the quality decisions of `2026-11-07-ttf-image-engine.md` (§5.6, §11).

## 1. Symptom (owner, 2026-11-08)

The same DanMachi vol-01 page rendered by the new TTF image engine on the X4 Pro
(photo, device) is visibly degraded versus the expected rendering (viewer
screenshot): mid-tones collapse into coarse 1-bit noise, the black chapter
banner loses its solid fill, and the illustration reads as washed-out dither
mush instead of anti-aliased gray. The legacy/BITMAP-style rendering of the
same page shows smooth 4-level gray.

## 2. Root cause (verified in source)

Two independent defects stack. Fixing only the tone bake without the display
routing will not change what the owner sees.

### 2.1 Display routing: image pixels never reach the panel's gray waveform

`paintCapturedPage` (src/activities/reader/QuickPageRelayout.cpp:174-213) and
its display-time twin (src/activities/reader/EpubReaderActivity.cpp:4540-4551,
4762, 4780) compute:

```cpp
const bool grayParity = smoothText && !pageHasImages && renderer.grayscaleCapabilities().supported();
```

The code comment states the exclusion explicitly: "Images keep the 1bpp engine
path (no plane bits for image pixels)". So on any page carrying an image the
TTF reader drops out of the dual-plane gray path entirely, and the image is
painted by `paintTtfPageImages` (QuickPageRelayout.cpp:23) →
`DirectPixelWriter::writePixel` in plain `BW` mode.

Consequence: the 4-level tone the quality pipeline bakes (TtfImageCache.h
`FloydSteinberg4`, tones {0, 85, 170, 255}) is displayed through a 1-bit
representation. In `BW` mode `writePixel` maps `pixelValue < 3` → black,
`3` → white (DirectPixelWriter.h:158-192); the panel never sees the baked
levels as levels. The visible result is exactly the owner's photo: a 1-bit
dither of a dither, with the banner's solid black region and the
illustration's mid-grays both collapsing into noise.

The legacy reader proves the panel can do far better: on the same UC8279 X4
Pro, image pages take the **absolute grayscale waveform** (src/activities/
reader/EpubReaderActivity.cpp:5768-5800: `absoluteImageGrayscale` when
`display.getController() == HalDisplay::Controller::UC8279` and
`GrayscaleMode::Absolute` is supported — "absolute quality waveform", upstream
#3478), and the driver carries 4-level AA via its external-LUT path. The TTF
path simply never routes image pixels there.

### 2.2 Tone bake: missing pre-quantization tone mapping

`FloydSteinberg4::tone` (TtfImageCache.h:22-45) quantizes the decoder's gray
value directly: `t = (v + 42) / 85`, full 100% Floyd–Steinberg diffusion,
raster (left-to-right, every row same direction). Three established quality
steps are missing (research, §6):

1. **No gamma correction.** Dithering in sRGB space is perceptually wrong:
   equal quantization error is spread across perceptually unequal lightness
   steps, which over-noises shadows and washes out highlights — matching the
   symptom. Best practice is to gamma-decode (2.2) into linear light, quantize
   there, and re-encode (harperreed/eink-image pipeline step 3; epdiy applies
   a gamma LUT for the same reason).
2. **Full-strength diffusion.** 100% error diffusion produces "pepper noise"
   in highlights on e-paper; tuned diffusion (~0.8) is the established fix
   (eink-image `--diffusion 0.8` default).
3. **Raster scan direction.** Same-direction rows leave directional artifacts;
   serpentine (alternating) scanning removes them (epaper-dithering
   "Serpentine Scanning").

Additionally no dynamic-range mapping exists between the source's [0..255]
and what the panel actually renders: e-paper reflects less light than sRGB
white, so a linear 0→255 mapping wastes range (epaper-dithering measured-
palette rationale; Adafruit e-ink graphics guide).

## 3. Goals

- G1: An image page on the TTF path renders through the panel's grayscale
  waveform — the same absolute-gray mode the legacy reader picks for image
  pages on UC8279 (EpubReaderActivity.cpp:5768-5800) — so the baked 4 levels
  display as levels, not as 1-bit dither.
- G2: The tone bake implements linear-light (gamma 2.2) processing, tuned
  diffusion, and serpentine scanning, with integer-only arithmetic
  (LUT-based), host-testable, for all four formats' quality path.
- G3: No regression for text-only pages: the existing `grayParity` text path
  is untouched; text on image pages keeps its current behavior unless the
  owner opts into full gray parity.
- G4: The revert switch (`imageDecodeQuality == 0`) keeps the previous
  behavior verbatim, including this display routing.
- G5: The unresolved SDK PR #40 review threads are closed as part of this
  campaign (they gate the same probe code this feature depends on).

## 4. Non-goals

- 16-level grayscale waveforms beyond what the driver's gray capabilities
  already advertise (the panel exposes 4 levels on this path).
- SVG/WebP (owner decision 2026-11-07, unchanged).
- BITMAP-engine path changes (its image display already routes through gray).
- Retiming the predecode worker (perf shipped in 5a4b32ce is untouched; only
  the tone math and display routing change).

## 5. Design

### 5.1 Phase A — display routing (the structural fix)

Image pixels must enter the same gray waveform the legacy path uses. On the
X4 Pro (UC8279, `GrayscaleMode::Absolute` supported) that is the absolute
gray mode; the routing must follow `grayscaleCapabilities()` exactly as the
legacy reader does (EpubReaderActivity.cpp:5768-5800), so a panel without
absolute gray falls back to today's 1bpp dither rather than breaking.

Mechanism (as-built amendment 2026-11-08, implementation run): the route is
the legacy reader's **two-pass absolute walk** — not a DUAL-target walk.
As-builts on the original bullets:

- `DirectPixelWriter`'s GRAYSCALE_DUAL branch is overlay-only (its `dualFb`
  member exists but never consults `absolute`), and the engine's `FrameTarget`
  painters can only write the framebuffer — a DUAL-absolute walk would need
  new plane-mapping code in three places. The two-pass single-mode walk the
  legacy path already uses needs none: per pass the framebuffer IS the plane
  source (seeded 0xFF, cleared bit = dark), seeded by `clearScreen(0xFF)` and
  uploaded via `copyGrayscaleLsbBuffers()`/`copyGrayscaleMsbBuffers()` — the
  legacy arm at EpubReaderActivity.cpp:6188+ (the two-pass fallback).
  `grayPlanesAreAbsolute()` is true (set by
  `displayGrayscaleBase(GrayscaleMode::Absolute, ...)`), so the existing
  `grayPlanePixel` absolute branch and `DirectPixelWriter`'s existing
  single-mode absolute branch emit the per-plane bits for image tones with no
  changes to `DirectPixelWriter`.
- `paintCapturedPage` gains the absolute-gray branch as a paint-pass
  parameter (`TtfPaintPass` Base/PlaneLsb/PlaneMsb): the base pass routes text
  through `PagePaint::paintText` (the shared quantizer's tone≥1 plotting) and
  the plane passes through the new `PagePaint::paintTextAbsolutePlanes` —
  the same walk with the absolute tone→plane mapping (tone 1 darkens LSB,
  tone 2 MSB, tone 3 both, via `grayPlanePixel`). Rules go through the engine
  (`FrameTarget`), images through the unchanged cache blit / foreground
  decode — the same writer both passes use, so worker/foreground parity is
  unchanged. The base image paint carries a Bayer dither
  (`setDitherImagesInBw(true)` for the base pass only), matching the legacy
  base snapshot the absolute base display consumes.
- Display-time route mirrors the legacy predicate (plus the quality path:
  the revert switch keeps today's 1bpp image display verbatim): image pages
  with absolute gray support take
  `displayGrayscaleBase(GrayscaleMode::Absolute, ...)` + `storeBwBuffer()` +
  the two-pass plane walk + `displayGrayBuffer()` + `restoreBwBuffer()`
  (EpubReaderActivity `renderTtfImageGrayAbsolute`), not the text-only
  `renderTtfGray*` walks, which stay for text-only pages. The status bar is
  re-painted inside each plane pass (the plane upload replaces the whole
  frame).
- A page that has images but a panel without absolute gray keeps today's
  1bpp path (no regression on X3-class hardware).
- The tone→plane mapping uses the existing absolute branch
  (`!(level == 3 || level == plane-level)` semantics in grayPlanePixel) — no
  new quantization is introduced at display time; the FS bake remains the
  single tone decision point.

Fallback composition rule (keep from the current code): on cache miss the
foreground decode runs `decodeAndCache(..., paint=&renderer)` — under the new
routing that paint target is the plane-enabled writer, so parity between
worker-ahead and foreground decode is preserved by construction (G3 of the
2026-11-07 design).

### 5.2 Phase B — tone bake quality (integer-only, host-testable)

Replace the interior of `FloydSteinberg4` (and the equivalent inline bake in
the GIF/BMP converter paths) with a `ToneBake4` that layers, in order:

1. **Gamma LUT** — 256-entry uint8 table, sRGB→linear decode with gamma 2.2,
   computed once at bake construction (pure function of the constant; no
   runtime float).
2. **Dynamic-range map** — table mapping linear light onto the panel's
   effective range. v1 uses a contrast-boost curve (the eink-image 1.3
   default family) implemented as a second 256-entry LUT so it stays a pure
   integer transform; the exact curve constant is a tuning knob surfaced in
   the constants table (below) and adjusted after device soak.
3. **Tuned diffusion** — error weight W (v1: 2048/2560 ≈ 0.8 in 8.8
   fixed-point) scaling every FS neighbor term. Integer shifts only.
4. **Serpentine scan** — row direction alternates; error buffers are walked
   left-to-right on even rows, right-to-left on odd rows (the FS kernel
   mirrors). Error state stays row-sequential over image-local columns
   (preserving the placement-independence invariant of the 2026-11-07
   design). As-built (review round 1): the scan direction is driven by the
   row-level `ToneBaker::toneRow(gray, tones)` API — one call bakes a whole
   row, visiting it in the bake's own direction (the mirrored kernel's
   horizontal term always lands on the next-visited pixel) and advancing
   the error state itself; the per-pixel `tone()` stays the FORWARD kernel
   (the interface keeps it for ascending-x callers and the A/B reference).
   The production decodes (SDK row sink, BMP row loop, GIF qFlushRow)
   call `toneRow` — a per-pixel serpentine flag callers ignore would
   discard 7/20 of the error on every odd row (review round-1 finding).

As-built amendment 2026-11-08 (implementation run):

- **Gamma direction.** The cited reference implements the LUT as
  `round(255·(i/255)^(1/γ))` — a BRIGHTENING curve (eink-image
  `src/main.rs apply_gamma_correction`), not a linear-light decode with a
  re-encode. The repo's own disabled gamma precedent
  (BitmapHelpers.cpp `applyGamma`) is the same direction ("brightens
  midtones"). ToneBake4 implements the reference formula; γ = 22/10 stays the
  constants-table value (the exponent is γ⁻¹ = 10/22). Darkening
  (the literal §5.2.1 wording) would crush shadows against the 0 tone and
  contradict the reference's documented behavior.
- **Integer arithmetic.** The LUT build is the bake's only float (two
  256-entry passes at construction); the contrast LUT is exact rational
  integer math `((26·v − 765)/20)`, pivot 128; the per-pixel path (LUT
  lookups + fixed-point diffusion) is integer-only as designed.
- **Injection.** The GIF converter's inline Floyd–Steinberg duplicate is
  retired: the converters receive the bake through
  `RenderConfig::toneBake` (a `ToneBaker` interface in DitherUtils.h), so all
  four quality decodes call the same `ToneBake4`. A null bake (construction
  OOM) degrades to the legacy Bayer path, as the converter's OOM path did.
- **Cache identity.** The quality cache letter moves 'q' → 'r'
  (TtfImageCache.cpp `kQualityCacheRevision`): the bake change would
  otherwise re-use tone caches produced by the old quantizer. Stale 'q'
  files live until the book cache sweep.

Quantization remains nearest-of-{0, 85, 170, 255} in the *mapped* space —
the thresholds move from the sRGB-uniform 85-step to the LUT-mapped
equivalents, so no new thresholds are hand-picked.

All four formats' quality paths (SDK JPEG/PNG row bake, GIF converter bake,
BMP decode bake) call the same `ToneBake4` — one implementation, DRY, no
per-format forks. The revert switch keeps the old bake (`FloydSteinberg4`)
available verbatim for A/B comparison on device.

### 5.3 SDK PR #40 unresolved threads (gates this feature)

- **High — "GIF/BMP images leave blank space"** (qodo-code-review,
  ImageProbe.cpp R96-99): the claim's failure mode is the app's revert
  switch (`imageDecodeQuality == 0` leaves reserved geometry blank by
  design, 2026-11-07 doc §5.6) — on the quality path the app paints GIF/BMP
  through its own converters. Answer the thread with that evidence; no SDK
  behavior change needed.
- **Medium ×2 — `INT32_MIN` negation UB in the BMP probe** (qodo + kody,
  ImageProbe.cpp:121-122): genuine bug, same class the app fixed in its own
  BMP parser (TtfImageEngine.cpp:194). Fix on the SDK fork (`feat/image-probe-gif-bmp`
  branch), add the host test, re-request review. The app-side pin bump for
  this fix rides this campaign.

## 6. Research basis (fetchable references — pi reads these itself)

- https://raw.githubusercontent.com/harperreed/eink-image/main/README.md —
  pipeline order (decode → luminance → gamma → contrast → dither), gamma 2.2
  default, tuned diffusion 0.8, "pepper noise in highlights" rationale.
- https://raw.githubusercontent.com/OpenDisplay/epaper-dithering/main/packages/javascript/README.md —
  serpentine scanning, GRAYSCALE_4 scheme, measured-palette rationale
  (e-paper reflects less light than sRGB white), OKLab matching background.
- https://raw.githubusercontent.com/aitjcize/epaper-image-convert/main/README.md —
  theoretical vs perceived palette separation.
- https://learn.adafruit.com/preparing-graphics-for-e-ink-displays —
  practical dither tuning guidance for e-ink.
- https://deepwiki.com/vroland/epdiy/7-image-processing — gamma LUT applied
  at render time on ESP32 e-paper.

## 7. Constants table (owner-tunable, lockstep doc rule applies)

| Constant | v1 | Where |
|---|---|---|
| `kGammaNum/kGammaDen` | 22/10 (γ=2.2, exponent γ⁻¹ — as-built §5.2) | ToneBake4 LUT build |
| `kDiffusionNum/kDiffusionDen` | 2048/2560 (0.8, 8.8 fixed) | ToneBake4 |
| `kContrastNum/kContrastDen` | 13/10 (1.3, eink-image default; pivot 128) | ToneBake4 contrast LUT |
| Serpentine | enabled | ToneBake4 |
| Absolute-gray routing | on when `GrayscaleMode::Absolute` supported | display route |
| `kQualityCacheRevision` | 'r' ('q' = the old bake) | cacheNameFor |

## 8. Execution plan

1. Owner reviews this doc (amend in lockstep if a decision changes).
2. This branch/worktree (`feat/ttf-image-quality` off origin/develop 29f7fe64)
   — doc commit first.
3. SDK: fix the `INT32_MIN` probe UB + host test on `feat/image-probe-gif-bmp`,
   answer the High thread with evidence, re-request review; bump the app
   submodule pin after the SDK PR is review-clean (owner merges SDK).
4. Phase A (display routing): TTF image pages through absolute gray on
   capable panels; host tests for the routing predicate and the plane-write
   mapping; device soak on the DanMachi page.
5. Phase B (tone bake): `ToneBake4` with gamma/contrast/diffusion/serpentine;
   host tests pinning bake outputs for gradient/step inputs; all four format
   paths switched to it.
6. Gates per commit: host ctest, `pio run -e x4pro`, `pio run -e default`,
   cppcheck, clang-format.
7. On-device soak: the owner's phone-photo comparison page (DanMachi vol 01
   ch.1 image page) re-rendered; expected outcome = the viewer screenshot's
   smooth 4-level gray. A/B against the revert switch (`imageDecodeQuality=0`)
   to prove the routing change is what closes the gap.
8. PR at the end; owner merges.

## 9. Risks / open items

- **Text-on-image-page gray**: routing the base text pass into the plane walk
  on image pages changes how text renders there (plane-based AA vs today's
  1bpp text over dither). This is desired (the legacy reader does the same)
  but must be verified for rubies/rules paths on the image branch.
- **Plane memory**: the dual-plane path allocates two full plane buffers
  (the 2026-11-07 design sized these for text-only pages); image pages take
  the same budget — verify against the pool budget on device.
- **Bake tuning is device-verified**: gamma/contrast constants are v1
  defaults from the research; the device soak decides final values. The LUT
  design makes retuning a table rebuild, not a code change.
- **Interlaced PNG / GIF first-frame**: unchanged by this design.
