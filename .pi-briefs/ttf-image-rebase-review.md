# Review brief: PR #303 — final rebase + review convergence

## Build-complete header

PR #303 (`fix(reader): full-fit image paint bounds and adaptive-stretch tone bake`) is
squash-merge-ready except for one open review thread and a rebase onto develop.

- Worktree: `/home/balor/workspace/eink/crosspoint-x-reader/_worktrees/ttf-image-quality2`
- Branch: `feat/ttf-image-quality2`, head `42136b277`
- PR: https://github.com/Belphemur/Xerif-eReader/pull/303
- PR state: OPEN, MERGEABLE, CLEAN, APPROVED, CI green on head `42136b277`

## Standing rules (git identity + gates + push discipline)

- Default/global git config only; never set `user.name`/`user.email`; never amend
  authorship. Pi commits as `pi <pi@local>`; no Co-authored-by trailers.
- Before every push: `git fetch origin develop` (rebase on develop if it moved).
- Gates before push: `pio run -e x4pro`, `pio run -e default`, host tests
  (`cd build/test && ctest`), cppcheck, clang-format.
- Push with `--force-with-lease` after a rebase; never `--force` bare.
- The owner merges; the daemon never does.

## Task 1: Rebase on develop

Develop has moved well past the PR head (last seen tip `c13841a90 ci(dev-ota):
stamp PR firmware and give PR entries a version`, with `a76c32aab fix(ota):
recompute the image XOR checksum when stamping dev builds` and `2fdc1463f ci:
add a one-line --validate publish gate` underneath — RE-FETCH, the tip may have
moved further while this round runs).

Rebase `feat/ttf-image-quality2` onto `origin/develop` (re-fetch first). All 9 commits
should apply cleanly — the PR touches `src/activities/reader/TtfImageEngine.cpp` and the
test stubs; develop's 3 commits touch OTA/dev-build paths. If any conflict, resolve it by
the file:line evidence in the design doc, not by dropping the PR's fix.

## Task 2: Fix the one open review thread

Open thread: CodeRabbit minor on `test/ttf_image_fit/stubs/Epub.h:27`.

- Finding: `poolMalloc(0)` can return a non-null pointer while `data()` is null
  (empty vector), and the following `memcpy` has undefined behavior.
- Fix: reject empty items before allocation in the lookup path —
  `if (it == items.end()) return nullptr;` becomes
  `if (it == items.end() || it->second.empty()) return nullptr;`.
- Reply to the thread with the fix, then resolve it.
- After the fix, CodeRabbit/Kody may re-file new threads on the re-reviewed head —
  work them by the same file:line discipline.

## Task 3: Settle CI on the new head

- `gh pr checks 303 --repo Belphemur/Xerif-eReader --watch --fail-fast` — the full
  matrix must be green on the new head.
- The rebase may re-open CodeRabbit's `skipping` state — post `@coderabbitai review`
  if the row reads `skipping` after your push, then re-check the row flips to
  `pending`/`pass` before declaring convergence.

## Task 4: Push + report

- Push the rebased head with `--force-with-lease`.
- Verify `git ls-remote origin feat/ttf-image-quality2` == local HEAD.
- Write the final report to `/tmp/pi_ttfimage_rebase_review_final_report.md` with:
  new head SHA, the review thread count (0 open), CI status, and the rebase commit.
- Emit the marker `TTFIMAGE_REBASE_REVIEW_DONE` as the last line of your reply.

## Completion contract

- 0 open review threads on the current head.
- CI green on the current head.
- Local HEAD == remote HEAD.
- The marker `TTFIMAGE_REBASE_REVIEW_DONE` is emitted.
