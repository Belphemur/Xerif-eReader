# Brief: TTF image pipeline — DPID resize + CLAHE, with the Picture detail setting

## Build-complete header

You continue PR #303 on branch `feat/ttf-image-quality2` (head `39006e506`, worktree
`/home/balor/workspace/eink/crosspoint-x-reader/_worktrees/ttf-image-quality2`). The
soak chain is: contrast fixed, fit fixed, texture fixed, edge pass attempted and
dropped. The owner's residual verdict: text in illustrations still barely readable,
image sharpness trails the offline optimizers.

## Read first — the design doc is the contract

`docs/design/2026-11-09-ttf-image-dpid-clahe.md` (commit `39006e506` + the decision
log commit). Read it in full before any code — especially the **§Decision log**
(DRY > SOLID > KISS, strict priority; the coding-philosophy skill governs every
trade-off in this campaign, and the decision log is its anchor — follow it and
amend it in the same commit when reality diverges). Do not contradict the doc;
amend it in the same commit if implementation must deviate. The parent docs it references:
- `docs/design/2026-11-08-ttf-image-texture-quality.md` §5 as-built (the current bake)
- `docs/design/2026-11-08-ttf-image-text-in-image.md` §As-built + §PSRAM policy
  correction (the edge-pass drop ruling; the PSRAM policy correction legitimizes
  full-image PSRAM scratch — `TtfBookRuntime.h:9-15` arena model is PSRAM-backed)

**Owner directive (steer, 2026-11-09): you may adopt an existing ESP32 library if it
beats hand-rolling.** Before implementing DPID/CLAHE from scratch, research what
exists for ESP32/ESP-IDF: the ESP Component Registry (components.espressif.com —
e.g. `cleishm/epaper-image` surfaced in an earlier search), GitHub ESP32 image
libraries, IDF bundled modules. If a well-maintained library provides
detail-preserving downscale and/or CLAHE compatible with our constraints (integer or
configurable fixed-point, no blocking float in the hot path, PSRAM-friendly, license
compatible, builds under the project's toolchain), prefer adopting it over a
hand-rolled implementation — cite the evidence (library, license, API, what it
covers vs what remains hand-rolled) in the task 1 report EITHER WAY. The
constraints are unchanged: legacy decode path bit-identical, TTF path only, and the
decision log's DRY/SOLID/KISS order applies to adopt-vs-roll exactly as to code.

Reference implementations to study (fetchable URLs, fetch them yourself):
- `https://raw.githubusercontent.com/mergian/dpid/master/matab/dpid.m` — the DPID
  reference (MATLAB; the algorithm is fully specified there)
- Kopf's content-adaptive downscaling paper is cached at
  `/tmp/kopf_downscaling.pdf` (may be gone after a reboot — re-fetch from
  `https://johanneskopf.de/publications/downscaling/paper/downscaling.pdf`)
- XteinkImageRefiner: `https://raw.githubusercontent.com/feeeeeeen/XteinkImageRefiner/main/image_processor.py`
  (CLAHE constants: 8×8 tiles, clip_limit = max(0.5, clahe/100·4.0))
- cbz2xtc/epub2xtc (Lanczos offline path, context only)

## Standing rules

- TTF path only. **The legacy bitmap reader's decode path must stay bit-identical** —
  if the SDK `RowScaler` is shared with legacy, DPID enters as a selectable scaler,
  never by changing the default. Legacy FS4 (`imageDecodeQuality = 0`) untouched,
  X3/1-bit paths untouched, driver/LUT untouched.
- Integer fixed-point only, no float, no pow (λ fixed at 1.0 → the weight is linear).
- No new heap in the render hot path; full-image PSRAM scratch is legitimate (the
  policy correction), but justify every allocation against the arena budget
  (480×800 8-bit scratch ≈ 384 KB).
- Default/global git config only; never set user.name/email; no Co-authored-by.
- Gates before EVERY push — and a FRESH REBUILD before ctest (the stale-binaries
  lesson: `rm -rf build/test` → cmake configure → full build → ctest; never report
  counts from an incremental build).
- Push `--force-with-lease=refs/heads/feat/ttf-image-quality2` after fetch/rebase.
- Report-first: short progress report before each major task.
- Stall-detector survival: short chatty commands; long builds in the background with
  polling; never one silent multi-minute command.

## Tasks

0. **Rebase on develop** — fetch, rebase the PR onto `origin/develop` (it moves
   constantly), force-push the rebased branch first, verify `ls-remote` == local.
   Amend the design doc in lockstep if the rebase shifts any number.

1. **Verify mechanisms from source, report-first, no code** (the design doc
   contract item 1):
   - Ownership of the resize per decoder path: app inline loop (`TtfImageEngine.cpp`
     BMP path, including the truncation bug at line 357) vs SDK `RowScaler`
     (`freeink-sdk` `ImageRenderer.cpp`). Verify from the code who calls what, and
     whether `RowScaler` is shared with the legacy bitmap reader's decode.
   - PSRAM budget check: the full-image 8-bit scratch vs the arena budget.
   - Host probe (reuse the `/tmp/texture_probe*.cpp` / `/tmp/edge_probe*.cpp`
     harness pattern if still present): box-average with the truncation bug vs
     exact fractional coverage vs DPID λ=1, on a synthetic field with thin strokes —
     measure stroke energy. This evidence justifies the campaign numerically.

2. **Implement DPID resize** (design doc contract item 2): pass 1 streams source
   rows into the full-image destination buffer with fractional-coverage
   accumulation; pass 2 walks the buffer with a 3-row ring, computes the 3×3 tent
   average, re-accumulates with the `|avg3x3 − pixel|/255` detail weight, and emits
   8-bit rows into ToneBake4's existing streaming collect → remap pipeline
   unchanged. Placement (app vs SDK vs selectable scaler) per task 1's evidence.
   Tiny images (dst ≥ src) skip the detail pass. Cache revision `'t'` → `'v'`.

3. **Implement CLAHE** (contract item 3): 8×8 tiles, integer clip redistribution,
   bilinear tile interpolation, in place on the destination buffer, after resize
   before ToneBake4's collect. Gated by the setting.

4. **Reader setting "Picture detail"** (contract item 4): two values
   **"Sharp"** (DPID only, default) and **"Enhanced"** (DPID + CLAHE). Plumb through
   the `imageDecodeQuality` pattern: `CrossPointSettings.h` field + load/save,
   reader menu entry in the settings UI, engine read alongside the existing
   `imageDecodeQuality` threading (predecode worker + relayout). User-facing
   strings follow the translation workflow. DPID is ALWAYS on; the setting gates
   only CLAHE.

5. **README** (contract item 6): update the reader-engine feature line in
   `README.md` (~line 79, "image handling") to mention the Picture detail setting
   in user-facing language ("sharper picture rendering; optional local-contrast
   enhancement").

6. **Tests** (contract item 5 / acceptance):
   - DPID: stroke-energy pin vs box-average; the no-ringing property (output within
     source patch min/max); fractional-coverage flat fields; λ=0 degenerates to
     box-average; determinism; tiny-image skip; border coverage.
   - CLAHE: clip redistribution bounds, tile-border interpolation, in-place,
     determinism; "Sharp" value == CLAHE-disabled output (byte-identical).
   - Parent invariants: ToneBake4 tests unchanged and green; legacy-path decoder
     tests unchanged (proving bit-identical legacy behavior if `RowScaler` is
     shared).
   - `PolicyLetterSeparatesBakes` pin updated to `/iv`.

7. **Gates → push → report → marker**: fresh rebuild ctest, `pio run -e x4pro` +
   `pio run -e default`, `pio check -e x4pro`, clang-format on touched files.
   Push `--force-with-lease`, verify `ls-remote` == HEAD. Write the final report to
   `/tmp/pi_ttf_dpid_clahe_final_report.md` (head SHA, gate results, placement
   decision + evidence, what changed, setting behavior) and emit `DPID_CLAHE_DONE`
   as the last line of your reply.

## Completion contract

- DPID resize live on all TTF-path image decoders (placement evidence recorded),
  truncation bug gone by construction, legacy paths bit-identical.
- CLAHE live behind the setting; "Sharp" == CLAHE-off byte-identical.
- Setting persisted, menu-visible, translated.
- README updated. Cache rev 'v'. Fresh ctest green. Gates green. Head pushed.
- Marker `DPID_CLAHE_DONE` emitted.
