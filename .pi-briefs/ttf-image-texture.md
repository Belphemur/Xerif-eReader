# Brief: TTF image texture quality (flat fields, banding, small text)

## Build-complete header

You continue PR #303 on branch `feat/ttf-image-quality2` (head `0a02a809e`), worktree
`/home/balor/workspace/eink/crosspoint-x-reader/_worktrees/ttf-image-quality2`. The
parent campaign (crop + wash) is DONE and verified on device. This campaign fixes the
residual texture defects from the 2026-11-08 11:41 device soak.

**Design doc: `docs/design/2026-11-08-ttf-image-texture-quality.md` (commit `0a02a809e`)
is the contract. Read it first. Do not contradict it; amend it in the same commit if
implementation must deviate.**

Device-soak evidence (extracted from photos, treat as ground truth):
- F1: flat gradients (Insert page floor) render as dense checkerboard dot fields; dark
  regions carry white dropout specks.
- F2: horizontal banding rows of alternating dot density through flat gradients.
- F3: small script text and publisher credits dissolve into dither noise.
- F4 (Ch.1 credit line absent) is EXCLUDED by owner ruling — do not investigate it.

## Standing rules

- Default/global git config only; never set user.name/email; never amend authorship.
  Commits as `pi <pi@local>`; no Co-authored-by trailers.
- TTF reader image path ONLY. Never touch: legacy FS4 path (`imageDecodeQuality = 0`
  revert switch must keep working), X3/1-bit paths, the UC8279 driver or its LUTs, the
  freeink-sdk submodule pointer, layout/placement code.
- Tone output stays 4-level {0,85,170,255}; integer-only per-pixel math; no new heap
  allocations in the bake (1 KB state budget precedent).
- Gates before EVERY push: `pio run -e x4pro`, `pio run -e default`, host ctest,
  cppcheck, clang-format.
- Push with `--force-with-lease=refs/heads/feat/ttf-image-quality2` after
  `git fetch origin` (rebase onto origin/develop if it moved).
- Report-first: post a short progress report before starting each major task.

## Tasks

1. **Verify mechanisms** against the design doc §1: confirm the checkerboard signature
   is FS-at-4-levels equilibrium on flat fields; probe the serpentine error carryover
   and the phase-1→2 reset (`TtfImageCache.cpp:139-140`) for row-periodic state bugs.
   Report file:line evidence before coding.
2. **F1 flat-field preservation** per doc §2.2/§3: flat runs snap to a single level,
   dither suppressed there. Host proof: synthetic flat-gradient bake shows uniform
   single-level regions, gradients still dithered smoothly.
3. **F2 banding** per doc §2.3: FIRST reproduce on host with a synthetic vertical
   gradient + flat patches. Host-clean ⇒ document panel-side evidence and STOP (do not
   touch the driver). Host-bandy ⇒ fix the bake bug and pin it with a
   row-periodicity assertion.
4. **F3 stroke retention** per doc §2.4 (stretch goal): thin-stroke anchoring; drop the
   mechanism if it regresses F1/F2 or highlights.
5. **Delivery:** bake revision 's'→'t' (`TtfImageCache.cpp:227-233`), extend host tests
   (new texture suite: flat-region uniformity, row-periodicity, stroke retention),
   full gate battery, push, verify `git ls-remote` == HEAD, write the final report to
   `/tmp/pi_ttf_texture_final_report.md` with: head SHA, gate results, F1/F2/F3
   disposition (fixed / panel-side / dropped), and emit the marker
   `TEXTURE_QUALITY_DONE` as the last line of your reply.

## Completion contract

- F1 and F2 dispositioned (fixed in code with host-test proof, or documented
  panel-side), F3 fixed or explicitly dropped with evidence.
- Legacy paths untouched; cache rev bumped; CI gates green locally; head pushed.
- Marker `TEXTURE_QUALITY_DONE` emitted.
