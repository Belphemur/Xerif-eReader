// Host-test stub of HalMemory.h — generous fixed heap stats (the GIF
// converter's free-heap gate must pass on host) and nothing else; the TTF
// image engine only reads the stats structs. Field order matches the real
// HalMemory::HeapStats so positional aggregate inits stay truthful.
#pragma once

#include <cstddef>
#include <cstdint>

namespace HalMemory {
struct HeapStats {
  uint32_t freeBytes = 0;
  uint32_t totalBytes = 0;
  uint32_t minFreeBytes = 0;
  uint32_t largestBlockBytes = 0;
};

inline HeapStats& psramStats() {
  static HeapStats s{64u * 1024 * 1024, 64u * 1024 * 1024, 0, 32u * 1024 * 1024};
  return s;
}

// Tests of the PSRAM budget gate rewrite this and must restore it afterwards.
inline HeapStats getPsramHeap() { return psramStats(); }
inline void setPsramHeapStats(const HeapStats& stats) { psramStats() = stats; }

inline HeapStats getInternalHeap() { return {64u * 1024 * 1024, 64u * 1024 * 1024, 0, 32u * 1024 * 1024}; }
// The legacy converters' heap gates (Png/JpegToFramebufferConverter) read the
// default heap; same generous stats so the probe/decodes pass on host.
inline HeapStats getDefaultHeap() { return {32u * 1024 * 1024, 32u * 1024 * 1024, 0, 8u * 1024 * 1024}; }
}  // namespace HalMemory
