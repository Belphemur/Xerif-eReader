#pragma once

#include <GfxRenderer.h>

#include <algorithm>
#include <vector>

// Coalescing for XOR-based selection/clip inversion: the +2 px padding around
// adjacent highlighted tokens makes their padded boxes touch or overlap, and
// inverting them separately would cancel the shared band (XOR twice = restore).
// Rects are grouped by VERTICAL INTERVAL OVERLAP (tokens on one visual line
// from runs with different sizePx share the line band even though their exact
// top/height differ — superscript runs, mixed-size spans), then merged by x
// with touch/overlap, and each merged rectangle is inverted once. Sorts the
// caller's vector in place (no extra allocation on the paint path); the band
// merge tracks the running LEFT edge too, because the (y, x) order lets a
// deeper band's rect sit left of an interval already emitted.
inline void invertRegionsMerged(GfxRenderer& renderer, std::vector<Rect> rects) {
  if (rects.empty()) return;
  std::sort(rects.begin(), rects.end(),
            [](const Rect& a, const Rect& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
  size_t i = 0;
  while (i < rects.size()) {
    // Band = a maximal chain whose vertical intervals overlap; the band's top
    // is the running minimum so a left-of-band rect reenters the interval.
    int x = rects[i].x;
    int y = rects[i].y;
    int right = rects[i].x + rects[i].width;
    int bottom = rects[i].y + rects[i].height;
    size_t j = i + 1;
    while (j < rects.size() && rects[j].y < bottom) {
      const int rectBottom = rects[j].y + rects[j].height;
      if (rects[j].x <= right) {
        x = std::min(x, rects[j].x);
        right = std::max(right, rects[j].x + rects[j].width);
      } else {
        renderer.invertRegion(x, y, right - x, bottom - y);
        x = rects[j].x;
        right = rects[j].x + rects[j].width;
        y = rects[j].y;
      }
      bottom = std::max(bottom, rectBottom);
      ++j;
    }
    renderer.invertRegion(x, y, right - x, bottom - y);
    i = j;
  }
}
