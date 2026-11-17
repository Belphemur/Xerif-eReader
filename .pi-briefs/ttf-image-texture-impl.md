# Brief: TTF image texture — implementation (continuation of ttf-image-texture)

## Build-complete header

PR #303, branch `feat/ttf-image-quality2`, worktree
`/home/balor/workspace/eink/crosspoint-x-reader/_worktrees/ttf-image-quality2`.
Contract: `docs/design/2026-11-08-ttf-image-texture-quality.md`. The verification
campaign COMPLETED task 1 with host-probe evidence; implementation did NOT start. Your
job is tasks 2–5 of the design doc, starting from the evidence below — do not re-derive
it.

## Verified mechanism evidence (from /tmp/pi_ttf_texture_final_report.md — trust it)

- **F1 confirmed.** Flat 127 → strict 2×2 checkerboard (rows `1212…`/`2121…`).
  Flat 40 → 6% bright dropout specks in solid black. Flat 100 → maps to 91 and bakes
  **100% solid level-1, zero dither** — tone permanently wrong. Mechanism: diffusion
  factor 0.8 (`kDiffusionNum 2048 / kDiffusionDen 2560`, `TtfImageCache.h:65-66`)
  discards 20% of each error, so small errors never cross the next threshold; quantize
  is `t = (v + 42) / 85` in `toneDir` (`TtfImageCache.h:151-174`), weights 7/3/5/1
  scaled 0.8/16 = 1/20.
- **F2 reproduces on host — the bake is bandy.** Vertical gradient 30→210 shows
  period-2 row alternation: mean row-to-row L1 delta 16.6 dots/row vs ideal ~2.0;
  transition rows alternate 0/79 level-2 dots. Isolated via probes: serpentine carryover
  is NOT the cause; `nextRow` (`TtfImageCache.h:189-194`) and the phase-1→2 reset
  (`TtfImageCache.cpp:140-143`) are correct and stateless-clean. Plain-FS variant still
  bands (15.9); horizontal-only mirror variant C reduces to ~11.0 but does not
  eliminate. **Origin: the 0.8 diffusion factor + 4-level threshold-crossing dynamics.**
  The doc §2.3 "panel-side, stop" branch does NOT apply.
- **Reusable probes:** `/tmp/texture_probe.cpp`, `/tmp/texture_probe2.cpp`,
  `/tmp/texture_probe3.cpp` (host builds of the real bake with the suite's stubs).
  Promote them into the new texture test suite rather than rewriting from scratch.

## Operational warning: stall detector

The previous run was killed twice by the empty-turn stall detector during long silent
host-probe compiles (~90s without transcript growth reads as a stall). Keep every
command short-running and chatty: compile with `-j` in steps, run probes with output
per stage, never one multi-minute silent command. If a build must run long, run it in
the background and poll with short sleep+tail commands that print a line each.

## Standing rules

- Default/global git config only; never set user.name/email; never amend authorship.
  Commits as `pi <pi@local>`; no Co-authored-by trailers.
- TTF reader image path ONLY. Never touch: legacy FS4 path (`imageDecodeQuality = 0`),
  X3/1-bit paths, UC8279 driver/LUTs, freeink-sdk submodule, layout/placement.
- 4-level {0,85,170,255} stays; integer-only per-pixel; no new heap in the bake.
- Gates before EVERY push: `pio run -e x4pro`, `pio run -e default`, host ctest,
  cppcheck, clang-format.
- Push `--force-with-lease=refs/heads/feat/ttf-image-quality2` after fetch; never `--force`.
- Report-first: short progress report before each major task.

## Tasks

0. **Sync base:** `git fetch origin`; if `origin/develop` moved (it is 2 commits ahead),
   rebase `feat/ttf-image-quality2` onto it (expect clean — reader code vs workflow
   files). Amend the design doc §1/§2.3 in the FIRST implementation commit with the
   probe evidence (host-bandy conclusion supersedes the panel-side branch) — lockstep
   rule.
1. **F1 flat-field preservation** (doc §2.2/§3): collect-pass flatness signal + flat-run
   snap in `toneRow`; dither suppressed on flat runs within ±k of one level; edges fall
   back to normal dithering. Host proof: flat 100/127/40 regions bake uniformly and
   tonally correct; gradients still dither.
2. **F2 banding fix** (doc §2.3): target the diffusion dynamics for 4-level output
   (candidates: higher diffusion factor toward ~1.0 so errors accumulate; threshold
   hysteresis; gradient-gated diffusion strength). Pin with a row-periodicity assertion
   in the texture suite (mean row-to-row density delta on a synthetic gradient must
   approach the ideal gradient step, and flat fields must show zero alternation).
3. **F3 stroke retention** (stretch, doc §2.4): thin-stroke anchoring; drop with
   evidence if it regresses F1/F2 or highlights.
4. **Delivery:** bake rev 's'→'t' (`TtfImageCache.cpp:231`), texture suite promoted
   from the /tmp probes into `test/`, full gate battery, push, verify
   `git ls-remote origin feat/ttf-image-quality2` == HEAD, write the final report to
   `/tmp/pi_ttf_texture_impl_final_report.md` (head SHA, gate results, F1/F2/F3
   disposition, what changed vs the verified mechanism), emit marker
   `TEXTURE_IMPL_DONE` as the last line of your reply.

## Completion contract

- F1 and F2 fixed in code with texture-suite proof (row-periodicity + flat-uniformity
  assertions green); F3 fixed or explicitly dropped with evidence.
- Legacy paths untouched; cache rev 't'; gates green; head pushed; remote == local.
- Marker `TEXTURE_IMPL_DONE` emitted.
