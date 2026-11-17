# Brief: texture gate fix — two stale test pins

## Header

PR #303, branch `feat/ttf-image-quality2`, head `4f6318a8c`, worktree
`/home/balor/workspace/eink/crosspoint-x-reader/_worktrees/ttf-image-quality2`.
The texture campaign landed correctly, but CI (run 42510455569) failed **Host unit
tests**, and a local fresh rebuild reproduces exactly 2 failures — both are stale
contract pins in `test/ttf_image_engine/TtfImageEngineTest.cpp` that were not updated
when the design changed. The implementation is correct per the design doc §5 as-built;
update the pins. Do not touch the implementation.

## Failure 1 — `ToneBake4.DiffusionWeightScalesEveryKernelTerm` (:296-306)

Pins the OLD 0.8 factor: comment says `W = 2048/2560 = 4/5`, expects
`diffuse(20,7)==7`, `diffuse(20,3)==3`, `diffuse(20,5)==5`, `diffuse(20,1)==1`,
`diffuse(-40,7)==-14`, `diffuse(3,7)==1`.

The factor is now 1.0 (`kDiffusionNum` 2560/2560 — design doc §5 as-built, the F2 fix).
Rewrite the test for the 1.0 kernel: read the actual `diffuse()` implementation first
(`TtfImageCache.h`) and compute the new expectations from ITS formula and truncation
semantics (e.g. `20·7/16`, `−40·7/16`, `3·7/16`) — verify each by running the real
kernel, do not guess. Keep the test's purpose identical: every kernel term scales with
the weight. Update the comment (`W = 2560/2560 = 1`, terms `errVal·k/16`).

## Failure 2 — `CacheIdentity.PolicyLetterSeparatesBakes` (:69-79)

Pins the OLD rev letter: expects `/is` in the quality cache name (and comment
"'s' = the adaptive two-phase ToneBake4 (design §2, amendment 2)").

Rev is now `'t'` (`kQualityCacheRevision`, `TtfImageCache.cpp:231` — texture bake).
Change the expectation to `/it` and the comment: `'t'` = the adaptive two-phase bake
with flat-field snap, threshold-noise and stroke anchoring (design
2026-11-08-ttf-image-texture-quality.md §5 as-built); `'s'` was the pre-texture
adaptive bake. Do not rename the test or loosen the assertion — the pin is the point.

## Process rules

- Default/global git config only; pi identity; no Co-authored-by.
- Gates before push — and REBUILD the host test tree from scratch before ctest
  (the 1521/1521 pass claimed earlier was against stale binaries; never report ctest
  results without a fresh `cmake --build build/test` first):
  fresh ctest (expect 1521/1521), `pio run -e x4pro`, `pio run -e default`,
  `pio check -e x4pro`, clang-format-21 dry-run on touched files.
- Commit shape: one commit, `test(image): pin the 1.0 diffusion kernel and the 't'
  bake revision`, GPG/default signing only.
- `git fetch origin` first; rebase onto origin/develop if it moved; push
  `--force-with-lease=refs/heads/feat/ttf-image-quality2`; verify
  `git ls-remote` == HEAD.
- Report-first: post findings (the new expected values you computed and how you
  verified them) before committing.
- Final report to `/tmp/pi_ttf_gatefix_final_report.md` with head SHA, fresh ctest
  count, gate results; emit marker `TEXTURE_GATEFIX_DONE` as the last line.
