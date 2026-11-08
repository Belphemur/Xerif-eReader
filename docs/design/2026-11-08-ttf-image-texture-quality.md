# TTF image texture quality — flat fields, banding, small text

Date: 2026-11-09 · Scope: TTF reader image path only · Parent: 2026-11-08-ttf-image-contrast-and-fit.md

## 0. Evidence (device soak 2026-11-08 11:41, post-fix build `a20f3e0`)

Defects A (wash) and B (crop) from the parent design are **verified fixed on hardware**:
the Insert page renders full-figure and contrast is punchy. What remains, from phone
photos of the panel:

- **F1 — flat-field checkerboard.** Large flat gradients (Insert page floor) render as a
  dense black-dot field with visible checkerboard/ordered-dither character; dark regions
  carry white dropout specks.
- **F2 — horizontal banding rows.** Periodic rows of alternating dot density run through
  the flat gradient.
- **F3 — small text destroyed.** Script lettering and publisher credits fragment into
  dither noise at panel resolution.

Owner ruling: the missing Ch.1 credit line (F4) is an artifact — **excluded**.

## 1. Mechanism analysis (to verify in task 1 before coding)

The 4-level bake (`ToneBake4`, `TtfImageCache.h:59`, quantizes to {0, 85, 170, 255})
error-diffuses at weight 0.8 (`kDiffuse` 2048/2560) serpentine (`toneRow`,
`TtfImageCache.h:177`). On a *flat* region of mid tone, Floyd–Steinberg at 4 levels has
a known equilibrium: a regular checkerboard of the two bracketing levels. The texture in
F1 matches that signature — the dither is *correct* per-pixel but perceptually wrong on
flat fields, where the eye integrates the pattern instead of the tone.

F2 has two candidate origins that must be discriminated, not assumed:
(a) the bake's serpentine/error-state handling interacts with phase-1→phase-2 reset
(`TtfImageCache.cpp:139-140`) or with row-start carryover, producing row-periodic
density; (b) the panel's external-LUT absolute-gray path (UC8279, 5×49) renders rows
unevenly — outside this design's scope.

## 2. Contract

1. Output tone stays 4-level {0, 85, 170, 255}; the bake stays integer-only end to end.
2. **Flat-field preservation (F1):** regions whose local tone varies less than a small
   fixed band around one target level must quantize *uniformly* (single level, no dither
   field). Dither is reserved for genuine gradients. The perceptual target: the Insert
   floor reads as one smooth gray, not a dot texture. No new heap allocations; the
   flatness signal must come from state already affordable in the collect pass
   (1 KB budget precedent, `TtfImageCache.cpp:39`).
3. **Band-free gradients (F2):** the baked output of a synthetic flat gradient must show
   no row-periodic density modulation. Host-verify first (`test/ttf_image_fit`-style
   suite): bake a vertical gradient + flat patches, assert no row-to-row dot-count
   periodicity in flat regions and no column-direction seams. If the host output is
   provably clean and banding persists on device, document it as panel-side evidence and
   **stop** — the driver/LUT path is out of scope.
4. **Stroke retention (F3):** thin near-black strokes (text, linework) survive the bake
   as connected dark marks instead of dissolving. Acceptance: a synthetic page with
   small script-scale text retains legible strokes in the baked 4-level output. This is
   a stretch goal — a partial improvement that doesn't regress F1/F2 is acceptable; a
   mechanism that cannot meet it without violating 1–3 is documented and dropped.
5. Regressions forbidden: highlights stay paper-white (no re-wash), geometry unchanged,
   legacy FS4 path (`imageDecodeQuality = 0`) untouched, X3/1-bit paths untouched.
6. Bake revision letter bumps ('s' → 't') so existing caches re-decode
   (`TtfImageCache.cpp:227-233`).

## 3. Design sketch (binding only through the contract above)

- **F1:** during `beginCollect`/`collectRow`, accumulate a cheap gradient-energy signal
  alongside the histogram (e.g. sum of |row[x+1]−row[x]| per tone bucket). In
  `beginRemap`, derive per-region flatness; at `toneRow`, when the local neighborhood
  (the error state already in hand) indicates a flat run within ±k of one quantization
  level, emit that level directly and suppress diffusion emission for that pixel.
  Edge rows/columns fall back to ordinary dithering. All decisions integer.
- **F2:** first reproduce on host. If reproduced: inspect serpentine carryover
  (odd-row right-to-left error wiring, `TtfImageCache.h:177`) and the phase-2 error reset;
  fix and pin with the synthetic-gradient test. If not reproduced: document with the
  host evidence, mark panel-side, done.
- **F3:** candidate: stroke-protection map — pixels below a low threshold with ≥2 dark
  neighbors force level 0 before diffusion (thin-stroke anchoring); evaluate against the
  no-regression rule (5) before keeping.

## 4. Gates and delivery

Same battery as the parent campaign: `pio run -e x4pro`, `pio run -e default`, host
ctest (extend the texture tests), cppcheck, clang-format. Push `--force-with-lease` to
`feat/ttf-image-quality2` (PR #303); report-first discipline; marker
`TEXTURE_QUALITY_DONE`.
