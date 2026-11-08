// Host-test stub of HalMemory.h — generous fixed heap stats (the GIF
// converter's free-heap gate must pass on host) and nothing else; the TTF
// image engine only reads the two stats structs.
#pragma once

#include <cstddef>
#include <cstdint>

namespace HalMemory {
struct HeapStats {
  uint32_t freeBytes = 0;
  uint32_t largestBlockBytes = 0;
};

inline HeapStats getInternalHeap() { return {64u * 1024 * 1024, 32u * 1024 * 1024}; }
inline HeapStats getPsramHeap() { return {64u * 1024 * 1024, 32u * 1024 * 1024}; }
}  // namespace HalMemory
