// Host tests for the header-only invertRegionsMerged coalescer (PR #306
// CodeRabbit finding): padded highlight/clip boxes invert exactly once
// everywhere — a double inversion restores pixels back to white under the
// highlight. The old single-pass x scan let a deeper-y rect extend an
// already emitted segment left and re-cover its columns.
#include <gtest/gtest.h>

#include "GfxRenderer.h"
#include "util/InvertRegionsMerged.h"

namespace {

Rect rectAt(const int x, const int y, const int width, const int height) { return Rect{x, y, width, height}; }

}  // namespace

TEST(InvertRegionsMerged, DeepYRectLeftOfAnEmittedSegmentDoesNotDoubleInvert) {
  // The exact CodeRabbit sequence: A[0,14)@y0, B[20,34)@y0 emitted, then
  // C[10,24)@y1 (a lower box top on the same baseline) merges into B's
  // original segment — columns [10,14) must not be covered twice.
  GfxRenderer renderer;
  std::vector<Rect> rects{rectAt(0, 0, 14, 20), rectAt(20, 0, 14, 20), rectAt(10, 1, 14, 29)};
  invertRegionsMerged(renderer, rects);
  ASSERT_FALSE(renderer.spans.empty());
  EXPECT_FALSE(GfxRenderer::overlapsAny(renderer.spans)) << "double XOR restores the overlap band";
}

TEST(InvertRegionsMerged, MergedBoxesAreXDisjoint) {
  GfxRenderer renderer;
  std::vector<Rect> rects{
      rectAt(0, 0, 30, 20), rectAt(20, 0, 30, 20),  // touching → one box
      rectAt(100, 0, 30, 20), rectAt(130, 0, 30, 20)
      // gap between 50..100 keeps two segments; no overlap either way
  };
  invertRegionsMerged(renderer, rects);
  EXPECT_EQ(renderer.calls, 2u);
  EXPECT_FALSE(GfxRenderer::overlapsAny(renderer.spans));
}

TEST(InvertRegionsMerged, DistinctBandsEmitPerSegmentWithOwnRows) {
  // Two y-disjoint bands; the first band's second segment must not inherit
  // the first segment's bottom rows (running-bottom restart contract).
  GfxRenderer renderer;
  std::vector<Rect> rects{rectAt(0, 0, 100, 20), rectAt(120, 0, 20, 20), rectAt(0, 30, 20, 40)};
  invertRegionsMerged(renderer, rects);
  ASSERT_EQ(renderer.spans.size(), 3u);
  EXPECT_EQ(renderer.spans[0], (Rect{0, 0, 100, 20}));
  EXPECT_EQ(renderer.spans[1], (Rect{120, 0, 20, 20}));
  EXPECT_EQ(renderer.spans[2], (Rect{0, 30, 20, 40}));
  EXPECT_FALSE(GfxRenderer::overlapsAny(renderer.spans));
}

TEST(InvertRegionsMerged, SingleRectInvertsOnce) {
  GfxRenderer renderer;
  std::vector<Rect> rects{rectAt(5, 5, 10, 10)};
  invertRegionsMerged(renderer, rects);
  EXPECT_EQ(renderer.calls, 1u);
  EXPECT_EQ(renderer.spans[0], (Rect{5, 5, 10, 10}));
}

TEST(InvertRegionsMerged, EmptyInputIsANoOp) {
  GfxRenderer renderer;
  std::vector<Rect> rects;
  invertRegionsMerged(renderer, rects);
  EXPECT_EQ(renderer.calls, 0u);
}
