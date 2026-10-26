#pragma once

#include <cstdint>

// Heap pre-flight for starting a TLS session on this device. The wolfSSL
// handshake working set and its ~17KB record buffer must fit in the largest
// free block, and the session itself in free heap. The floor is the
// device-measured TLS demand already used by HttpDownloader (which aliases
// these constants), DictHtmlPages and SdCardFontSystem guards.
// Pure and constexpr so callers and host tests can evaluate it without a
// live heap.
namespace tlsheap {

enum class Verdict : uint8_t {
  Proceed,
  Skip,
};

constexpr uint32_t kMinFreeHeap = 40000;
constexpr uint32_t kMinLargestBlock = 20000;

constexpr Verdict verdict(uint32_t freeHeap, uint32_t maxAllocHeap) {
  return (freeHeap >= kMinFreeHeap && maxAllocHeap >= kMinLargestBlock) ? Verdict::Proceed : Verdict::Skip;
}

}  // namespace tlsheap
