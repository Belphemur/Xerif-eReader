#pragma once

#include <cstdint>

// Queued button-edge counting (docs/design/2026-10-27-queued-input-aggregation.md
// §3.1). In async-input mode a wasPressed()/wasReleased() probe consumes ONE
// queued edge per call (SDK consume-on-check), so the single check per frame
// destroyed the multiplicity of rapid presses. Looping the probe drains the
// queue and recovers the count at the app layer — no SDK API change.
//
// Pure policy, kept host-testable outside MappedInputManager.
namespace edgecount {

// Matches the SDK per-button queue capacity (HalGPIO::beginAsync queueLen=16).
// The SDK queue is saturating, so past the cap the count is a lower bound and
// overflowed edges are lost — the pre-existing single-check behavior, not a
// new loss.
inline constexpr uint8_t kMaxQueuedEdges = 16;

// Drains `probe` (each true return consumed one queued edge) into a count.
// The cap is tested BEFORE each probe so the edge at the cap boundary stays
// queued for the next frame instead of being consumed and dropped.
template <typename Probe>
uint8_t drainCount(Probe&& probe) {
  uint8_t count = 0;
  while (count < kMaxQueuedEdges && probe()) ++count;
  return count;
}

}  // namespace edgecount
