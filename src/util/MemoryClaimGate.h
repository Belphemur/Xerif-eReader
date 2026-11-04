#pragma once

#include <cstdint>

// Heap pre-flight + background-suspension planner for memory-heavy operations
// (font management, plugin installs, OTA). Sibling of WebServerHeapGate.h and
// TlsHeapGuard.h and reusing their vocabulary: the caller's floors ARE the
// tlsheap floors (possibly plus the operation's own DRAM budget — e.g. a
// no-PSRAM board's manifest buffer), never a new constant here. See
// docs/design/2026-11-03-memory-claims.md.
//
// The gate frees what the operation cannot need: rebuildable font caches and
// the BLE radio (when it is up at all — at these screens it is normally already
// down). Its middle verdict differs from webgate's: no heap-defrag REBOOT
// lives inside a settings screen, so `Defrag` here means "pull ONE suspension
// lever (memclaim::Step), re-measure, ask again".
//
// Pure: no allocation, no device includes, lever table inlined, so host tests
// evaluate it without a device. The planner is a PURE TOTAL function — every
// (free, maxBlock, floors, stepsTried) input maps to a verdict (§9).
namespace memclaim {

enum class Verdict : uint8_t {
  Proceed,   // floor met
  Defrag,    // floor not met, but a suspension lever remains — pull the one
             // `next` names, re-measure, ask again
  Fallback,  // all levers pulled, floor still unmet — structurally short
};

// A suspension lever, in the order the gate tries them (cheapest first).
enum class Step : uint8_t {
  FontCaches = 0,  // releaseSdFontCaches(): synchronous, rebuildable, free
  Radio = 1,       // BLE radio down: one non-blocking suspend(0) pass per pass
};

static constexpr uint8_t kStepCount = static_cast<uint8_t>(Step::Radio) + 1;

constexpr uint8_t stepMask(const Step step) { return static_cast<uint8_t>(uint8_t{1} << static_cast<uint8_t>(step)); }
// Bitset of every lever (the "all pulled" state).
constexpr uint8_t kAllStepsMask = static_cast<uint8_t>((uint8_t{1} << kStepCount) - 1);

constexpr bool stepTried(const uint8_t stepsTried, const Step step) { return (stepsTried & stepMask(step)) != 0; }
// The stepsTried value after recording `step` (idempotent: pulling a lever
// twice records once — §3.2 records FontCaches in the same pass it releases).
constexpr uint8_t triedAfter(const uint8_t stepsTried, const Step step) {
  return static_cast<uint8_t>(stepsTried | stepMask(step));
}

struct Phase {
  Verdict verdict = Verdict::Proceed;
  // Valid only when verdict == Defrag: the lever to pull this pass.
  Step next = Step::FontCaches;
};

// Decides whether the floor is met now, and if not, what to try next.
// `stepsTried` is a bitset of levers already pulled this claim; a lever
// recorded there is NEVER re-suggested (§9: re-suggesting a no-op lever
// spins the pass forever and Back never runs). Free/maxBlock/floors come
// straight from the caller's device measurement.
inline Phase plan(const uint32_t freeHeap, const uint32_t maxBlock, const uint32_t freeFloor,
                  const uint32_t maxBlockFloor, const uint8_t stepsTried) {
  if (freeHeap >= freeFloor && maxBlock >= maxBlockFloor) return {Verdict::Proceed, Step::FontCaches};
  for (uint8_t i = 0; i < kStepCount; i++) {
    const auto candidate = static_cast<Step>(i);
    if (!stepTried(stepsTried, candidate)) return {Verdict::Defrag, candidate};
  }
  return {Verdict::Fallback, Step::FontCaches};
}

// --- Radio lever pass bookkeeping --------------------------------------------
//
// The Radio lever is the device pump's only multi-pass lever: one non-blocking
// suspend(0) teardown pass per activity pass, recorded as pulled ONLY when a
// pass reports the stop settled (beforeScreenChange(0,false) == true). These
// two pure helpers carry the deadline rule (§3.2 Boundedness): if the stack
// has not stopped after the release deadline (kBuildReleaseTimeoutMs, the same
// 3,000 ms budget Runtime.cpp gives a build-starved release), the lever is
// latched recorded and the next plan can only answer Fallback.

// Wrap-safe monotonic elapsed (millis() arithmetic).
constexpr uint32_t radioStopElapsedMs(const uint32_t nowMs, const uint32_t firstStopMs) {
  return static_cast<uint32_t>(nowMs - firstStopMs);
}

// True once the radio stop has consumed its deadline without settling, so the
// device pump may record the lever as pulled. `stopStarted` carries the pump's
// own start flag — the timestamp carries NO sentinel meaning, so a stop pass
// issued at millis() == 0 still expires at the deadline.
constexpr bool radioStopDeadlineExpired(const uint32_t nowMs, const bool stopStarted, const uint32_t firstStopMs,
                                        const uint32_t deadlineMs) {
  return stopStarted && radioStopElapsedMs(nowMs, firstStopMs) >= deadlineMs;
}

}  // namespace memclaim
