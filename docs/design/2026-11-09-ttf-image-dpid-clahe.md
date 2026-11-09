# TTF image pipeline — DPID resize + CLAHE local contrast

Date: 2026-11-09 · Scope: TTF reader image path only · Parents: 2026-11-08-ttf-image-contrast-and-fit.md, 2026-11-08-ttf-image-texture-quality.md, 2026-11-08-ttf-image-text-in-image.md (dropped) · PR #303

## Problem

Soak chain 2026-11-08: contrast fixed (adaptive stretch), fit fixed (logical bounds),
texture fixed (flat-field snap, threshold noise, stroke anchor), text-in-image edge
pass attempted and **dropped** (its drop ruling is recorded in
2026-11-08-ttf-image-text-in-image.md §As-built). The user's residual verdict: text
inside illustrations is still barely readable, and image sharpness in general trails
what the offline optimizers produce.

**The remaining defect is upstream of the tone bake: the resize.** Both resize paths
smear detail before the bake ever sees it:

- **BMP path** (`TtfImageEngine.cpp:299-375`): box-average for ≥2× shrinks — with a
  truncation bug at line 357 (`sum / (x1-x0)` truncates per-row before summing, so
  rows lose brightness); nearest-neighbor otherwise (magnification and small shrinks).
- **JPEG/PNG path** (SDK `ImageRenderer.cpp:18-19` `RowScaler`): box-filter averaging,
  plus TJpgDec power-of-two downscale first for JPEG.

Box averaging integrates *everything* equally — thin strokes lose their energy against
the background and the 4-level bake cannot recover them. The offline tools that look
good (XteinkImageRefiner, cbz2xtc's Lanczos path) all resize with detail preservation
first, then tone work. Our pipeline has the order backwards: the cheapest,
lowest-quality step (box average / nearest) runs before the most sophisticated step
(ToneBake4).

## Mechanism (to verify in task 1 before coding)

**DPID — Rapid, Detail-Preserving Image Downscaling** (Nehab/Weber/Amend 2016;
reference implementation `mergian/dpid`, MATLAB reference `dpid.m`):

- **Pass 1 — area average:** exact fractional-coverage average of each output patch
  (the same operation as box-average, minus the truncation bug — fractional coverage
  at patch borders instead of integer clipping).
- **Pass 2 — detail-weighted reconstruction:** a separable 3×3 tent average of the
  area-average image (1:2:1 weights, ~6 ops/pixel) gives each output pixel its local
  average context; then each output pixel is re-accumulated from its source patch
  with weight `|avg3x3 − pixel| / 255` (grayscale scalar; the RGB norm reduces to
  this for grayscale). Pixels that deviate from the local average — detail — get
  more weight, so high-frequency content dominates instead of being averaged away.
  λ (the exponent on the weight) defaults to 1.0, where `f^λ = f` — **no pow needed**.
- λ=0 degenerates to exactly the current box-average (the fallback); λ=1 is the
  paper's recommended default.

**CLAHE — Contrast Limited Adaptive Histogram Equalization:** tile-grid (8×8) local
histograms, clip limit redistributes excess, bilinear interpolation between tiles.
Applied on the grayscale 8-bit image *after* the resize, *before* the tone bake.
This is the local-contrast enhancement ToneBake4's global histogram cannot do: on a
4-level panel with compressed dynamic range, local contrast is what makes shadow and
gradient structure visible.

Recipe provenance: XteinkImageRefiner (`feeeeeeen/XteinkImageRefiner`,
`image_processor.py` — pre-blur → INTER_AREA → unsharp mask → CLAHE → dither) and the
CBZ optimizers (cbz2xtc/epub2xtc — Lanczos resize, then 1-bit). The user's framing:
"proper resize first to keep sharpness before the rest of the processing".

**DPID replaces the whole blur/resize/sharpen triple of the offline recipe with one
content-aware pass** — no Lanczos ringing, no blind unsharp mask, no double-pass
artifacts — and the same order of cost (~2× box-average).

## Contract

1. **Task 1 verifies mechanisms from source, report-first, no code:**
   - Which components own the resize on each decoder path: the app's inline loop
     (BMP) and the SDK `RowScaler` (JPEG/PNG). **Determine whether `RowScaler` is
     shared with the legacy bitmap reader's decode path.** If it is shared, DPID
     must be introduced as a selectable scaler so the legacy path keeps bit-identical
     box-average behavior — never change the legacy path's decode by default.
   - The truncation bug at `TtfImageEngine.cpp:357`: confirm the per-row truncation
     loses brightness (probe on a synthetic field), since fixing it changes baked
     output and is part of this campaign.
   - Memory model: the bake is streaming; the DPID pass needs the full destination
     image resident (pass 2 iterates it). Verify the PSRAM budget carries one
     full-image 8-bit scratch (480×800 = 384 KB) — the PSRAM policy correction in
     2026-11-08-ttf-image-text-in-image.md legitimizes this; the arena model is
     PSRAM-backed (`TtfBookRuntime.h:9-15`).
   - Reference implementations to study: `mergian/dpid` (CUDA + MATLAB), Kopf's
     content-adaptive downscaling paper (the DPID comparison target, fetched at
     `/tmp/kopf_downscaling.pdf`), XteinkImageRefiner's CLAHE constants.

2. **DPID resize** — integer fixed-point, no floating point, no pow (λ fixed at 1.0):
   - Pass 1 streams source rows into a full-image destination buffer with fractional
     coverage accumulation (fixing the truncation bug by construction).
   - Pass 2 walks the destination buffer with a 3-row ring (tent filter is local),
     computes the detail weight, re-accumulates from the source patch, and emits
     8-bit rows that feed ToneBake4's existing streaming collectRow → remap pipeline
     unchanged.
   - The source patch pixels for pass 2 must remain available — either held (PSRAM,
     they are the decode output) or re-derivable from the destination buffer's
     provenance; task 1 decides from the memory model. Never re-decode.
   - Tiny images (dst ≥ src) skip the detail pass and degenerate to plain sampling.

3. **CLAHE** — integer fixed-point, gated by the reader setting:
   - 8×8 tiles, clip limit fixed at the reference's default regime (clip ≈ 2.0 in
     XteinkImageRefiner terms; the exact integer mapping is task 1's evidence call).
   - Operates in place on the destination buffer (histogram pass + interpolation
     pass); no second full-image buffer.
   - Applied only to the grayscale path after resize, before ToneBake4's collect.

4. **Reader setting — "Picture detail"** (owner-approved name; amend in lockstep if
   renamed):
   - Two values: **"Sharp"** (DPID resize only) and **"Enhanced"** (DPID + CLAHE).
     DPID is always on — the setting selects only the local-contrast stage.
   - Default: **"Sharp"** (conservative — CLAHE is the unproven stage; flipping it
     needs no reflash).
   - Plumbing follows the `imageDecodeQuality` pattern (`CrossPointSettings.h:472`
     field, `CrossPointSettings.cpp` load/save, reader menu entry, engine read).
   - The menu label and the two option labels are user-facing strings — follow the
     translation workflow rules (generated-files-cache guidance).

5. **No-regression invariants:**
   - ToneBake4 is untouched — the texture campaign's F1/F2 invariants hold by
     construction (same bake, different input). Host tests must still pass unchanged
     where they feed synthetic rows directly.
   - Legacy FS4 path (`imageDecodeQuality = 0`) untouched; X3/1-bit paths untouched;
     legacy bitmap reader's decode path bit-identical (task 1's finding governs how).
   - The cache identity must change: cache revision `'t'` → `'v'` (`'u'` was burned
     by the reverted edge pass and shipped on the dev channel; reuse would collide).
   - Output determinism: DPID and CLAHE are deterministic integer operations (same
     input → same bytes); pin with tests.

6. **README** — update the README's reader-engine feature line (README.md:86
   "image handling") to mention the picture-detail setting in user-facing language.

## Acceptance

- Synthetic probes (host, in the existing `test/ttf_image_fit` harness):
  - Sharp synthetic strokes survive the DPID resize with measurably more energy than
    box-average (the exact failure the soak chain flagged).
  - No ringing: every output pixel lies within the min/max of its source patch (a
    hard property of weighted averages — pin it).
  - Fractional coverage: flat fields bake to exact averages (the truncation bug is
    gone by construction).
  - λ=0 degenerates to box-average (regression pin).
  - CLAHE: clip redistribution bounded, tile borders interpolated, in-place,
    deterministic; "Sharp" setting produces output identical to CLAHE-disabled runs.
- The setting switches behavior without a reflash; both values soak-testable.
- Owner device soak is the final gate (same as every prior campaign).

## Design sketch (binding only through the contract above)

- `DpidScaler`-shaped component owning pass 1 + pass 2, placed where task 1's
  shared-path evidence dictates (app-side, SDK-side, or duplicated with a
  documented DRY debt if the SDK is shared with legacy and a parameter would be
  invasive). One implementation, two callers, is the strong preference.
- CLAHE as a small stateless helper (histogram + redistribute + interpolate) over
  the destination buffer.
- The setting travels through the existing engine-config path that already carries
  `imageDecodeQuality` (the predecode worker and relayout already read it — follow
  the same threading).

## Decision log (coding-philosophy: DRY > SOLID > KISS, strict priority)

1. **DRY — one DPID implementation, two callers.** The resize knowledge already
   exists twice (app inline loop `TtfImageEngine.cpp:299-375`; SDK `RowScaler`
   `ImageRenderer.cpp:18-19`) — an existing fork. DPID lands once and both decoder
   paths consume it; a second copy is deleted where the fork allows. What the loser
   (SOLID-shaped per-path abstraction with duplicate internals) costs: two DPID
   copies drifting — the app path sharp, the SDK path smeared — the exact asymmetry
   the soak chain flagged. Task 1 verifies the fork's real shape before unifying
   ("two call sites with different futures are not one behavior" — if the SDK
   RowScaler is shared with legacy in a way that resists a parameter, the documented
   DRY debt is recorded rather than forced).
2. **SOLID — legacy decode stays bit-identical via a selectable scaler, never a
   default change.** Flipping the shared default would silently rewrite the legacy
   bitmap reader's output; the correctness invariant (legacy unchanged) outranks the
   simpler "just change RowScaler". Same rule inside the pass: scratch-allocation
   failure fails the decode before any row is written (the established ring-alloc
   fail-closed shape) — no partial-image decode can ship.
3. **SOLID — "Sharp" is a whole-pass gate.** The setting disables the entire CLAHE
   pass, so "Sharp" output is byte-identical to CLAHE-off by construction — one
   gate, testable, not a scattering of conditions.
4. **KISS — λ fixed at 1.0 as a plain constant.** No setting, no config row, no pow.
   The λ=0 degenerate path stays solely as a regression pin. A tunable λ would be a
   knob nobody has asked to tune.
5. **KISS — DPID absorbs blur+resize+sharpen as one pass.** The offline recipe's
   three stages collapse because DPID's area-average *is* the anti-alias filter and
   its detail weight *is* the sharpening; keeping the three separate stages next to
   DPID would be machinery whose job vanished.
6. **Lifetime rule — the full-image scratch is pool-owned and call-scoped.**
   `PoolBytes` from the arena, acquired at decode start, released before return; no
   raw owning pointer outlives it; rows handed to ToneBake4 are consumed
   synchronously within the call. The PSRAM policy correction
   (2026-11-08-ttf-image-text-in-image.md) legitimizes the size; the arena model
   (`TtfBookRuntime.h:9-15`) owns the memory.
7. **Equal values, different jobs — CLAHE's clip limit and ToneBake4's percentile
   clip are not one knob.** Both bound a histogram, but CLAHE's clip governs
   per-tile redistribution (local), ToneBake4's percentile clip governs global
   dynamic range; collapsing them would silently narrow one artifact's guarantee.
   Recorded so no future "twin constant" consolidation unifies them.

## Gates and delivery

Same battery: fresh-from-scratch ctest (never report stale binaries),
`pio run -e x4pro` + `pio run -e default`, cppcheck, clang-format. Push
`--force-with-lease` to `feat/ttf-image-quality2` (PR #303). Report-first
discipline. Marker `DPID_CLAHE_DONE`, final report
`/tmp/pi_ttf_dpid_clahe_final_report.md`.

## Rebase note

Develop moves constantly (11 OTA commits landed during the previous campaign). The
brief's task 0 re-bases the PR onto origin/develop before implementation and amends
this doc in lockstep if the rebase shifts any number above.
