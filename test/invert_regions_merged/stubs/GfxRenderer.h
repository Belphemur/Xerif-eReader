#pragma once

#include <vector>

// Minimal GfxRenderer stand-in for the header-only invertRegionsMerged util:
// the real one drags in the whole paint stack, and the contract under test is
// exactly which rectangles reach invertRegion, in what order.

struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool operator==(const Rect& other) const {
    return x == other.x && y == other.y && width == other.width && height == other.height;
  }
};

class GfxRenderer {
 public:
  void invertRegion(int x, int y, int width, int height) const {
    if (width <= 0 || height <= 0) return;
    ++calls;
    spans.push_back(Rect{x, y, width, height});
  }

  // XOR-overlap accounting: two inverted overlapping rects cancel each other
  // in that overlap, which is exactly the defect the coalescing must prevent.
  // Marks per row a small occupancy mask; assert no column row is covered
  // twice.
  static bool overlapsAny(const std::vector<Rect>& spans) {
    for (size_t i = 0; i < spans.size(); ++i) {
      for (size_t j = i + 1; j < spans.size(); ++j) {
        const Rect& a = spans[i];
        const Rect& b = spans[j];
        if (a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height) return true;
      }
    }
    return false;
  }

  mutable int calls = 0;
  mutable std::vector<Rect> spans;
};
