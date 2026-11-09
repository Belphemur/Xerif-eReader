#pragma once

#include <GfxRenderer.h>

#include <algorithm>
#include <vector>

// Coalescing for XOR-based selection/clip inversion: the +2 px padding around
// adjacent highlighted tokens makes their padded boxes touch or overlap, and
// inverting them separately would cancel the shared band (XOR twice = restore).
// Group the rects into same-line bands (same top edge and height — all padded
// boxes on one line share them) and merge x-intervals that touch, then invert
// each merged rectangle once. This nuance exists only for screen-rect fills;
// crossing rectangles must be a real single-region overlap.
inline void invertRegionsMerged(GfxRenderer& renderer, const std::vector<Rect>& rects) {
  if (rects.empty()) return;
  // Sort by (y, x) so same-line boxes are contiguous.
  std::vector<Rect> sorted(rects);
  std::sort(sorted.begin(), sorted.end(), [](const Rect& a, const Rect& b) {
    if (a.y != b.y) return a.y < b.y;
    return a.x < b.x;
  });
  size_t i = 0;
  while (i < sorted.size()) {
    // One band = a maximal run sharing the same top edge (padded boxes on a
    // line share y and height, so an exact key is enough).
    int x = sorted[i].x;
    int y = sorted[i].y;
    int right = sorted[i].x + sorted[i].width;
    const int bottom = sorted[i].y + sorted[i].height;
    size_t j = i + 1;
    while (j < sorted.size() && sorted[j].y == y && sorted[j].y + sorted[j].height == bottom) {
      if (sorted[j].x <= right) {
        right = std::max(right, sorted[j].x + sorted[j].width);
      } else {
        renderer.invertRegion(x, y, right - x, bottom - y);
        x = sorted[j].x;
        right = sorted[j].x + sorted[j].width;
      }
      ++j;
    }
    renderer.invertRegion(x, y, right - x, bottom - y);
    i = j;
  }
}
