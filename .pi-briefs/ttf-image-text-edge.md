# Brief: TTF image text-in-image legibility — edge handling pass

## Build-complete header

You continue PR #303 on branch `feat/ttf-image-quality2` (head `5a71fc4ea`, worktree
`/home/balor/workspace/eink/crosspoint-x-reader/_worktrees/ttf-image-quality2`). The
parent texture campaign (F1 flat-field snap, F2 banding fix, F3 stroke anchor) is
merged into the PR and CI-green. The owner's soak feedback: "Pictures are great now,
text is still hard to read" — text *inside* book illustrations (script lettering,
publisher credits, small title lines) fragments into dither noise.

## Read first

1. `docs/design/2026-11-08-ttf-image-text-in-image.md` — the binding contract. Read it
   in full before any code.
2. `docs/design/2026-11-08-ttf-image-texture-quality.md` §5 as-built — the current
   bake: flat-run snap, threshold noise, stroke anchor, diffusion factor 1.0.
3. `src/activities/reader/TtfImageCache.h/.cpp` — the current bake implementation.
4. The previous run's final report at `/tmp/pi_ttf_texture_impl_final_report.md` (if it
   still exists) — reuse its host probes instead of rewriting them.

## Task 0 — rebase on develop

Develop has moved 11 commits past the PR head (OTA work). Re-fetch, rebase the PR onto
`origin/develop`, and amend the design doc in lockstep if the rebase changes any of
its numbers. Force-push with `--force-with-lease` after the rebase (you own this
branch only).

## Task 1 — verify mechanisms from source (report-first, no code)

- File:line evidence for the current anchor's limits: `kStrokeMap` 42 protects the
  stroke core, but the anti-aliased pixels around it (43–85 range) still dither
  globally (`TtfImageCache.h:80-83`, `TtfImageCache.h:199-202`).
- Confirm the memory model: does the bake already hold the full decoded image and the
  full baked grid in memory? (It does — the cache stores the full image — but verify
  from the actual buffer lifetimes.) The edge pass must not add a heap allocation.
- Cost probe: the Sobel mask is one extra gradient pass over the row buffer; the
  local-pair search is a radius-2 window over the quantized grid. Measure both on the
  host probe harness before deciding the mask's buffer strategy.
- Reference: epdoptimize's `src/dither/dither.ts` — EdgePreservation (core pixels get
  the nearest palette color, no dithering) and EdgeAntialiasing (band pixels get a
  local 2-color decision by coverage). The reference code is on GitHub; fetch it if
  you need the exact formulas.

## Task 2 — edge handling pass (F3 completion)

- **Sobel mask** on the stretched source (pre-bake, post-stretch): threshold 42,
  core threshold × 1.45, band = dilated radius 1 around the core.
- **Core preservation:** a source pixel on a strong edge bakes its nearest palette
  color directly, no dithering. Strength is a probability with a deterministic
  integer noise threshold (the same bit-mix noise family as the F2 fix).
- **Band antialiasing:** a source pixel in the edge band gets the dithered result
  replaced with a local 2-color decision: the two most common quantized colors in a
  radius-2 window, coverage by projection onto their segment, decided by coverage
  against a deterministic noise threshold. Residual > 95 or coverage ≤0.08 / ≥0.92
  → keep the dithered result.
- Post-dithering order: bake (toneRow) → edge pass → cache write.
- Cache rev bump `'t'` → `'u'`.

## Task 3 — tests

- Pin the Sobel mask thresholds, the local-pair search, and the coverage decision.
- The parent texture tests (F1/F2 invariants) must still pass — flat 100 → 100%
  level-1, flat 40 → 100% level-0, banding probe ≤ 4.74.
- The stroke-anchor test (F3) still passes.
- New: a synthetic page with small script-scale text retains legible strokes in the
  baked 4-level output — this is the acceptance test the texture campaign's F3
  stretch goal pointed at.

## Task 4 — gates → push → report → marker

- Fresh rebuild → ctest (never report stale binaries — count from scratch).
- `pio run -e x4pro` and `pio run -e default` SUCCESS; `pio check` green; cppcheck;
  clang-format.
- Push `--force-with-lease` to `feat/ttf-image-quality2`, verify `ls-remote` == local.
- Write the final report to `/tmp/pi_ttf_text_edge_final_report.md` and emit
  `TEXT_EDGE_DONE` as the last line of your reply.

## Stall-detector survival rule

Long silent host compiles trigger the stall detector. Use short chatty commands, or
run long builds in the background and poll for completion. Never let a single command
run silently for minutes.

## Standing rules

- TTF path only; legacy FS4 (`imageDecodeQuality = 0`) untouched; X3/1-bit paths
  untouched; driver/LUT untouched.
- Integer-only, no new heap allocations.
- Default/global git config only; never set user.name/email; no Co-authored-by.
- Report-first discipline; gates before every push; force-with-lease.
