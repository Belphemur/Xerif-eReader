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

**Verified 2026-11-08 (host probes P1–P6, /tmp/pi_ttf_texture_final_report.md):** F1 and
F2 both reproduce on host with the real `ToneBake4` before any panel LUT is involved.
Flat 127 bakes a strict 2×2 checkerboard; flat 40 carries 6% white dropout specks; flat
100 maps to 91 and bakes **100% solid level-1, zero dither** — tone permanently wrong.
Mechanism: diffusion factor 0.8 discards 20% of each error (`kDiffusionNum/kDiffusionDen`,
`TtfImageCache.h:65-66`), so small per-pixel errors never cross the next threshold;
`toneDir` (`TtfImageCache.h:151-174`) quantizes with `t = (v+42)/85`, weights 7/3/5/1
scaled by 0.8/16 = 1/20. F2: full 240×240 vertical gradient 30→210 shows period-2 row
alternation — mean |row-to-row delta| 16.6 dots/row vs ideal ~2.0; transition rows
alternate 0/79 level-2 dots. Cause isolation: serpentine carryover is NOT the cause
(plain-FS variant still bands, 15.9); `nextRow` (`TtfImageCache.h:189-194`) and the
phase-1→2 reset (`TtfImageCache.cpp:140-143`) are correct and stateless-clean. The
horizontal-only mirror variant reduces to ~11.0 but does not eliminate. **Origin: the
0.8 diffusion factor + 4-level threshold-crossing dynamics.** The §2.3 "panel-side,
stop" branch does NOT apply.

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

## 5. As-built (implementation 2026-11-08)

Shipped in `ToneBake4` (`src/activities/reader/TtfImageCache.h`), cache rev 's'→'t':

- **Diffusion factor 0.8 → 1.0** (`kDiffusionNum`/`kDiffusionDen` 2560/2560): the 20%
  error discard was the F1 origin (small errors never cross the next threshold — flat
  100 stuck at solid 85, flat 40 lost dither density) and fed the F2 limit cycle.
- **Zero-mean pseudo-random threshold offset** (±8, integer bit-mix of (x, row); a
  `row` counter member incremented in `nextRow`, reset in `beginRemap`): breaks the
  row-as-mega-pixel mode-locking behind period-2 gradient banding. The offset only
  shifts the quantizer decision — it never enters `errVal`, so emitted tone stays
  true (pinned by `GradientToneIsPreservedByTheThresholdNoise`). Deterministic pure
  function: worker/foreground parity holds (pinned by `BakeIsDeterministicAcrossRuns`).
  Rejected alternatives, measured on the host probes: kSnap-on-error (freezes
  gradients into plateaus, then jumps bands — worse than baseline), row-invariant
  column mask (reinforces the resonance, 12.7), row-parity checkerboard mask (9.3),
  noise amplitude 5 (4.7) and 12 (4.4) — amplitude 8 is the sweet spot (3.7–3.9 vs
  ideal 2.0; baseline 16.6).
- **Flat-run snap on the mapped tone** (F1): when both horizontal neighbours map
  within `kFlatTol` (2) of the pixel, the incoming error is small (`kFlatErr` 8 — no
  vertical gradient flowing through), and the mapped tone sits within `kSnap` (21) of
  one level, the pixel bakes that level and drops the residual error instead of
  diffusing texture into the flat. Out-of-image neighbours count as continuing the
  flat (deviation from the §3 sketch's "edges fall back to dithering": edge-column
  speckle on otherwise-uniform fields was judged worse than the deviation).
- **Flat mid-tones between two levels** (e.g. mapped 127): no single level is within
  ±kSnap, so the field stays a tone-correct fine-grain dither (asserted 40–60% per
  side) — the documented 4-level physical limit; the strict 2×2 checkerboard is gone
  (the noise decorrelates it).
- **Thin-stroke anchoring** (F3): a source pixel mapping ≤ `kStrokeMap` (42) bakes
  level 0 even when accumulated neighbour error would push it up a shelf; the FULL
  error still diffuses, so local tone is preserved (no halo — pinned by
  `ThinStrokesSurviveTheBake`, which also asserts the neighbour columns stay bright).

Host proof (test/ttf_image_fit/TtfImageTextureTest.cpp, 8 tests): flats near a level
bake 100% one level; gradient mean |row-to-row L1 delta| 3.9 vs ideal ~2.0 (baseline
16.6) with no sustained opposite-sign swings; threshold noise tone-neutral and
deterministic; 1px/2px strokes 100% retained without halos.
