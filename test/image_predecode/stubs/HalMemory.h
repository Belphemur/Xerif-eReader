#pragma once

#include <cstddef>
#include <cstdint>

// Heap stats are not consulted by the decode paths under test beyond logging.
class HalMemory {
 public:
  struct HeapStats {
    size_t freeBytes;
    size_t totalBytes;
    size_t minFreeBytes;
    size_t largestBlockBytes;
  };
  static HeapStats stats() { return {1u << 20, 1u << 20, 1u << 20, 1u << 20}; }
  static HeapStats getInternalHeap() { return stats(); }
  static HeapStats getPsramHeap() { return stats(); }
  static HeapStats getDefaultHeap() { return stats(); }
};
