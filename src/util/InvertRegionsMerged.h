#pragma once

#include <GfxRenderer.h>

#include <algorithm>
#include <vector>

// Coalescing for XOR-based selection/clip inversion: the +2 px padding around
// adjacent highlighted tokens makes their padded boxes touch or overlap, and
// inverting them separately would cancel the shared band (XOR twice = restore).
// Rects are grouped by VERTICAL INTERVAL OVERLAP (tokens on one visual line
// from runs with different sizePx share the line band even though their exact
// top/height differ — superscript runs, mixed-size spans), then each band is
// merged by x separately and every merged box is inverted once. Sorts the
// caller's vector in place (no extra allocation on the paint path).
// Merging per band — not one global x scan — matters for correctness: in the
// old single pass a deeper-y rect could extend an ALREADY emitted x segment
// left, double-inverting the overlap columns back to white under the
// highlight (CodeRabbit, PR #306). Bands are row-disjoint, so per-band
// x-merge keeps every region downloaded exactly once.
inline void invertRegionsMerged(GfxRenderer& renderer, std::vector<Rect>& rects) {
  if (rects.empty()) return;
  std::sort(rects.begin(), rects.end(),
            [](const Rect& a, const Rect& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
  size_t band = 0;
  while (band < rects.size()) {
    // Band = a maximal chain whose vertical intervals overlap (the first rect
    // of the next band starts at or below every member of this one).
    int bandBottom = rects[band].y + rects[band].height;
    size_t bandEnd = band + 1;
    while (bandEnd < rects.size() && rects[bandEnd].y < bandBottom) {
      bandBottom = std::max(bandBottom, rects[bandEnd].y + rects[bandEnd].height);
      ++bandEnd;
    }
    // The band's rects occupy [band, bandEnd). With them x-ascending, every
    // x merge extends only the CURRENT (unemitted) segment — emitted ones
    // can no longer be overlapped later.
    std::stable_sort(rects.begin() + band, rects.begin() + bandEnd,
                     [](const Rect& a, const Rect& b) { return a.x < b.x; });
    size_t m = band;
    while (m < bandEnd) {
      int x = rects[m].x;
      int y = rects[m].y;
      int right = rects[m].x + rects[m].width;
      int bottom = rects[m].y + rects[m].height;
      size_t n = m + 1;
      while (n < bandEnd && rects[n].x <= right) {
        x = std::min(x, rects[n].x);
        y = std::min(y, rects[n].y);
        right = std::max(right, rects[n].x + rects[n].width);
        bottom = std::max(bottom, rects[n].y + rects[n].height);
        ++n;
      }
      renderer.invertRegion(x, y, right - x, bottom - y);
      m = n;
    }
    band = bandEnd;
  }
}
