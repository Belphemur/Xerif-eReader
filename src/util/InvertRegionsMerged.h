#pragma once

#include <GfxRenderer.h>

#include <algorithm>
#include <vector>

// Coalescing for XOR-based selection/clip inversion: the +2 px padding around
// adjacent highlighted tokens makes their padded boxes touch or overlap, and
// inverting them separately would cancel the shared band (XOR twice = restore).
// Rects are grouped by VERTICAL INTERVAL OVERLAP (tokens on one visual line
// from runs with different sizePx share the line band even though their exact
// top/height differ — superscript runs, mixed-size spans), then merged by
// ascending x with touch/overlap, and each merged rectangle is inverted once.
inline void invertRegionsMerged(GfxRenderer& renderer, const std::vector<Rect>& rects) {
  if (rects.empty()) return;
  // Sort by (y, x) so a vertical run of overlapping rects is contiguous.
  std::vector<Rect> sorted(rects);
  std::sort(sorted.begin(), sorted.end(), [](const Rect& a, const Rect& b) {
    if (a.y != b.y) return a.y < b.y;
    return a.x < b.x;
  });
  size_t i = 0;
  while (i < sorted.size()) {
    // Band = a maximal chain whose vertical intervals overlap: grow the band's
    // bottom while the next rect starts above it. Tokens on the same visual
    // line overlap; lines with tight spacing may overlap by 1-2 px too, which
    // still merges the rare edge case instead of XOR-restoring a seam.
    int x = sorted[i].x;
    int y = sorted[i].y;
    int right = sorted[i].x + sorted[i].width;
    int bottom = sorted[i].y + sorted[i].height;
    size_t j = i + 1;
    while (j < sorted.size() && sorted[j].y < bottom) {
      const int rectBottom = sorted[j].y + sorted[j].height;
      if (sorted[j].x <= right) {
        right = std::max(right, sorted[j].x + sorted[j].width);
      } else {
        renderer.invertRegion(x, y, right - x, bottom - y);
        x = sorted[j].x;
        right = sorted[j].x + sorted[j].width;
      }
      bottom = std::max(bottom, rectBottom);
      ++j;
    }
    renderer.invertRegion(x, y, right - x, bottom - y);
    i = j;
  }
}
