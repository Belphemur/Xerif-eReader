# TTF image text-in-image legibility — edge handling pass

Date: 2026-11-08 · Scope: TTF reader image path only · Parent: 2026-11-08-ttf-image-texture-quality.md (F3) · PR #303

## Problem

Soak feedback (2026-11-08, post-texture-fix firmware): "Pictures are great now, text is
still hard to read." Text inside book illustrations — script "Chapter" lettering,
publisher credits, small title lines — fragments into dither noise at panel resolution.

The texture campaign's F3 answer was a **per-pixel magnitude anchor** (`kStrokeMap` 42,
`TtfImageCache.h:80-83`): a source pixel mapping ≤42 forces level 0. That protects the
stroke *core*, but the anti-aliased pixels *around* the stroke (the 43–85 range) still
go through the global 4-level FS diffusion, so the stroke's edge dithers away and thin
text fragments. The anchor was a documented partial answer to a stretch goal.

## Mechanism (to verify in task 1 before coding)

**The stroke problem is a local 2-color problem, not a global 4-level one.** For a
stroke edge, the correct decision is between the two bracketing palette colors — the
stroke's black (0) and the background's level (170 or 255) — not a global 4-level
diffusion driven by error accumulated from unrelated regions.

Reference: epdoptimize's "Edge handling" (paperlesspaper/epdoptimize, `src/dither/dither.ts`),
the same library sourced during campaign 1. Its mechanism, keyed by a Sobel gradient
mask on the source:

1. **Edge masks** — per-pixel Sobel luma gradient on the source; pixels above a
   `threshold` (42) are edge pixels; `core` is a stronger threshold (42 × 1.45) with
   its own dilation radius; `band` is a dilated radius around the edge.
2. **EdgePreservation** — edge-core pixels get the dithered result replaced with the
   source pixel's *nearest palette color* (no dithering on strong edges). Strength is
   a probability (0.65 default), so a fraction of the core keeps the dithered result —
   a texture-coherence trade-off.
3. **EdgeAntialiasing** — edge-band pixels get the dithered result replaced with a
   **local two-color decision**: find the two most common quantized colors in a local
   radius (the pair on either side of the edge), compute the pixel's coverage
   (projection of the source pixel onto the segment between them), and pick between
   the pair by coverage with a noise threshold. Pixels with residual > 95 (not on the
   segment) or coverage extremes (≤0.08 / ≥0.92) are left as the dithered result.

## Contract

1. **Task 1 verifies mechanisms from source** — file:line evidence for the current
   anchor's limits, Sobel mask cost on the row buffer, and the local-pair search
   cost at radius 2 — before any code.
2. **Edge handling pass** — post-dithering, keyed by a Sobel gradient mask on the
   *stretched* source (pre-bake, post-stretch). Two mechanisms:
   - **Edge-core preservation** — a source pixel on a strong edge (Sobel gradient above
     the core threshold) bakes its nearest palette color directly, no dithering.
     Strength is a probability with a deterministic integer noise threshold — the
     same bit-mix noise family as the F2 fix.
   - **Edge-band antialiasing** — a source pixel in the edge band (dilated radius
     around the core) gets the dithered result replaced with a local 2-color decision:
     the two most common *quantized* colors in a small radius (2), coverage by
     projection onto their segment, decided by coverage against a deterministic noise
     threshold. Residual > 95 or coverage ≤0.08 / ≥0.92 → keep the dithered result.
3. **No-regression invariants** (from the parent texture campaign):
   - Flat 100 → 100% level-1 (F1); flat 40 → 100% level-0 (F1).
   - Banding probe (F2) ≤ 4.74 mean row-to-row L1 delta.
   - Highlights stay paper-white; geometry unchanged.
4. **Integer-only, no new heap.** The pass runs over the existing decode buffer and
   baked grid; the Sobel mask is a row-buffered computation (or a full-image
   `Uint8Array`-style buffer if that is already the bake's memory model — task 1
   decides from evidence). No new PSRAM allocation; if the existing buffer already
   holds the full image, the mask rides in it.
   *(Owner correction 2026-11-08: the TTF reader path is gated to PSRAM-class boards
   only by design — `platformio.ini` gates `CROSSPOINT_TTF_READER=1` to x4pro,
   x4pro_profile, x4c, papermono, all with `-DBOARD_HAS_PSRAM`, and the comment
   "Native-TTF reader path (design §14.1): PSRAM class boards only" says so. The
   no-new-PSRAM contract was written from the wrong assumption. Full-image PSRAM
   scratch is legitimate for future work; the arena model is PSRAM-backed
   (`TtfBookRuntime.h:9-15`).)*
5. **Cache rev bump** `'t'` → `'u'` so existing caches re-decode.
6. **Regressions forbidden:** legacy FS4 path (`imageDecodeQuality = 0`) untouched,
   X3/1-bit paths untouched, driver/LUT untouched, TTF path only.

## Acceptance

- A synthetic page with small script-scale text retains legible strokes in the baked
  4-level output — the same test the texture campaign used for F3, now with the
  anti-aliased edge pixels also surviving.
- All parent texture tests (F1/F2 invariants) still pass.
- The stroke-anchor test (F3) still passes.
- New tests pin the edge-mask thresholds and the local-pair search.

## Design sketch (binding only through the contract above)

- **Mask:** Sobel luma gradient on the stretched source; `threshold` 42, `core`
  threshold × 1.45, `band` = dilated radius 1 around the core. Constants in
  `TtfImageCache.h` next to the existing `kSnap`/`kFlatErr`/`kStrokeMap` family.
- **Core preservation:** post-dithering, replace the baked pixel with the source
  pixel's nearest palette color when the Sobel gradient clears the core threshold and
  the noise threshold (deterministic bit-mix of x, row) passes.
- **Band antialiasing:** post-dithering, replace the baked pixel with the local
  2-color decision when the Sobel gradient clears the band threshold and the noise
  threshold passes. The local pair comes from the two most common quantized colors in
  a radius-2 window; coverage by projection onto their segment; residual > 95 or
  coverage extremes keep the dithered result.
- **Order:** bake (toneRow) → edge pass → cache write. The pass sees the full baked
  grid and the stretched source; the cache already holds both.

## Gates and delivery

Same battery as the parent texture campaign: `pio run -e x4pro` and `pio run -e
default`, host ctest (fresh rebuild, from-scratch — never report stale binaries),
cppcheck, clang-format. Push `--force-with-lease` to `feat/ttf-image-quality2` (PR
#303); report-first discipline; marker `TEXT_EDGE_DONE`.

## Rebase note

Develop has moved 11 commits past the PR head at write time (OTA work). The brief's
task 0 rebases the PR onto origin/develop before any implementation work; the design
doc is amended in lockstep if the rebase changes any of the numbers above.


## As-built (2026-11-08) — DROPPED

The pass was implemented (commit `9cba080a0`, 16 tests, 1560/1560 host ctest) and
reverted the same day (revert `3138eae9e`) on owner soak feedback:

- **The picture quality regressed** — "the quality of the picture is the worst.
  Previous algorithm had better result." The band dilation (radius 1) plus the
  threshold 42 entry fires on soft illustration gradients, not just text strokes;
  the coverage mix is a hard binary decision between two palette levels, so the
  dithered tone the texture campaign tuned (FS at factor 1.0 + threshold noise) is
  replaced by crisp step edges everywhere the pass fires. Synthetics passed; real
  artwork failed.
- **The F3 goal was not met** — "text is still barely readable". At 480 px width the
  AA band around thin script strokes is 1–2 px wide; the coverage mix cannot
  reconstruct a stroke that dithered to 1 px of mid-level grain.
- **The design's own drop rule applies** — the parent texture campaign's F3 was a
  stretch goal with a documented drop condition: "a mechanism that cannot meet it
  without violating 1–3 is documented and dropped." The pass violated the
  no-regression rule (F1/F2 invariants held only on synthetic fixtures, not on real
  artwork), so it is dropped.

The revert restores the `'t'` bake (flat-field snap, threshold noise, stroke anchor,
FS at factor 1.0) — the texture campaign's shipped state. Cache rev stays `'t'`.
A future F3 attempt must satisfy the no-regression rule on real artwork, not just
synthetic fixtures, before it can ship.

## PSRAM policy correction (2026-11-08)

The no-new-PSRAM contract above was written from the wrong assumption: the TTF
reader path is gated to PSRAM-class boards **only** (`platformio.ini` gates
`CROSSPOINT_TTF_READER=1` to x4pro, x4pro_profile, x4c, papermono, all with
`-DBOARD_HAS_PSRAM`, and the comment "Native-TTF reader path (design §14.1): PSRAM
class boards only" says so). The reader's arena model is PSRAM-backed
(`TtfBookRuntime.h:9-15`). A full-image PSRAM scratch buffer is legitimate for future
work; the row-ringed design was the answer to a constraint that does not exist.
