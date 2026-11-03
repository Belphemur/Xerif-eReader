#include <gtest/gtest.h>
#include <util/MemoryClaimGate.h>
#include <util/TlsHeapGuard.h>

// Host tests for the memory-claim gate's pure core (docs/design/2026-11-03-
// memory-claims.md §3.1, §9). The device adapter (util/MemoryClaimGateDevice.h)
// is not compiled here; the tests pin the planner's contract and the pass
// bookkeeping the device pump must implement. Floors composed from the tlsheap
// constants are exactly what the sites pass:
//   - TTF/screens on PSRAM boards: the plain tlsheap floor;
//   - no-PSRAM (.cpfont) manifest stage: tlsheap floor + the manifest buffer's
//     DRAM budget (kManifestDramBudgetBytes in FontDownloadActivity.cpp).
namespace mc = memclaim;
namespace tl = tlsheap;

namespace {
// The .cpfont screen's manifest buffer budget (FontDownloadActivity.cpp), the
// only budget the composed floors differ by. Kept a literal here so the test
// does not depend on the device TU.
constexpr uint32_t kManifestBudgetBytes = 65536;
constexpr uint32_t kFreeFloorBudgeted = tl::kMinFreeHeap + kManifestBudgetBytes;
constexpr uint32_t kMaxBlockFloorBudgeted = tl::kMinLargestBlock + kManifestBudgetBytes;
// The release budget the device pump mirrors from kBuildReleaseTimeoutMs
// (Runtime.cpp, 3,000 ms).
constexpr uint32_t kBleStopDeadlineMs = 3000;

// The device pump's loop (MemoryClaimGateDevice.h pumpOnePass), simulated over
// the pure core's helpers with a scripted heap and stop-settle clock. This is
// the same algorithm the adapter executes; a test that diverges from it pins
// the wrong thing, so any device-side change must keep this copy in sync.
struct SimulatedPump {
  uint8_t stepsTried = 0;
  bool radioStopStarted = false;
  uint32_t radioFirstStopMs = 0;

  void reset() {
    stepsTried = 0;
    radioStopStarted = false;
    radioFirstStopMs = 0;
  }

  // suspendSettles=false models the non-blocking stop pass returning false
  // (teardown work remains); the heap is scripted, so released bytes are not
  // re-measured here — the lever recording is what these tests pin.
  memclaim::Phase pass(const uint32_t free, const uint32_t maxBlock, const uint32_t freeFloor,
                       const uint32_t maxBlockFloor, const uint32_t nowMs, const bool suspendSettles) {
    auto p = mc::plan(free, maxBlock, freeFloor, maxBlockFloor, stepsTried);
    if (p.verdict == mc::Verdict::Defrag && p.next == mc::Step::FontCaches) {
      // Synchronous within the pass; a no-op release still counts as pulled.
      stepsTried = mc::triedAfter(stepsTried, mc::Step::FontCaches);
      p = mc::plan(free, maxBlock, freeFloor, maxBlockFloor, stepsTried);
    }
    if (p.verdict == mc::Verdict::Defrag && p.next == mc::Step::Radio) {
      if (!radioStopStarted) {
        radioStopStarted = true;
        radioFirstStopMs = nowMs;
      }
      const bool settled = suspendSettles || mc::radioStopDeadlineExpired(nowMs, radioFirstStopMs, kBleStopDeadlineMs);
      if (settled) stepsTried = mc::triedAfter(stepsTried, mc::Step::Radio);
      p = mc::plan(free, maxBlock, freeFloor, maxBlockFloor, stepsTried);
    }
    return p;
  }
};

}  // namespace

// §9: the planner is total — every (free, maxBlock, floors, stepsTried) input
// maps to a verdict, and a Defrag always names a lever that is NOT recorded in
// stepsTried (a recorded lever is never re-suggested).
TEST(MemoryClaimGate, PlanIsTotalOverAllInputs) {
  const uint32_t freeValues[] = {0, 1, 40'000, 105'535, 105'536, 250'000, 4294967295u};
  const uint32_t maxBlockValues[] = {0, 1, 20'000, 85'535, 85'536, 200'000, 4294967295u};
  for (const uint32_t freeHeap : freeValues) {
    for (const uint32_t maxBlock : maxBlockValues) {
      for (uint8_t steps = 0; steps <= mc::kAllStepsMask; steps++) {
        for (const uint32_t freeFloor : {tl::kMinFreeHeap, kFreeFloorBudgeted}) {
          for (const uint32_t maxBlockFloor : {tl::kMinLargestBlock, kMaxBlockFloorBudgeted}) {
            const mc::Phase p = mc::plan(freeHeap, maxBlock, freeFloor, maxBlockFloor, steps);
            EXPECT_TRUE(p.verdict == mc::Verdict::Proceed || p.verdict == mc::Verdict::Defrag ||
                        p.verdict == mc::Verdict::Fallback)
                << "f=" << freeHeap << " b=" << maxBlock << " ff=" << freeFloor << " mf=" << maxBlockFloor
                << " steps=" << static_cast<int>(steps);
            if (p.verdict == mc::Verdict::Defrag) {
              EXPECT_FALSE(mc::stepTried(steps, p.next))
                  << "re-suggests a recorded lever: steps=" << static_cast<int>(steps);
            }
          }
        }
      }
    }
  }
}

// §9: Proceed exactly at the tlsheap floors — one byte below either criterion
// is not Proceed. The composed (budgeted) floor behaves identically around its
// own boundary.
TEST(MemoryClaimGate, ProceedExactlyAtTlsheapFloors) {
  EXPECT_EQ(mc::plan(tl::kMinFreeHeap, tl::kMinLargestBlock, tl::kMinFreeHeap, tl::kMinLargestBlock, 0).verdict,
            mc::Verdict::Proceed);
  EXPECT_EQ(mc::plan(tl::kMinFreeHeap - 1, tl::kMinLargestBlock, tl::kMinFreeHeap, tl::kMinLargestBlock, 0).verdict,
            mc::Verdict::Defrag);
  EXPECT_EQ(mc::plan(tl::kMinFreeHeap, tl::kMinLargestBlock - 1, tl::kMinFreeHeap, tl::kMinLargestBlock, 0).verdict,
            mc::Verdict::Defrag);

  EXPECT_EQ(mc::plan(kFreeFloorBudgeted, kMaxBlockFloorBudgeted, kFreeFloorBudgeted, kMaxBlockFloorBudgeted, 0).verdict,
            mc::Verdict::Proceed);
  EXPECT_EQ(
      mc::plan(kFreeFloorBudgeted - 1, kMaxBlockFloorBudgeted, kFreeFloorBudgeted, kMaxBlockFloorBudgeted, 0).verdict,
      mc::Verdict::Defrag);
  EXPECT_EQ(
      mc::plan(kFreeFloorBudgeted, kMaxBlockFloorBudgeted - 1, kFreeFloorBudgeted, kMaxBlockFloorBudgeted, 0).verdict,
      mc::Verdict::Defrag);
}

// §3: the manifest budget applies to BOTH tlsheap criteria (the buffer
// consumes free heap AND the largest block): a heap at the plain tlsheap floor
// with the budget's worth of headroom missing must not Proceed.
TEST(MemoryClaimGate, ManifestBudgetRaisesBothFloors) {
  // Exactly the plain tlsheap floor: enough for TLS, not for TLS + a 64 KB
  // no-PSRAM manifest buffer (would then leave the post-allocation
  // heapAvailableForTransfer check short in the device flow).
  if (kManifestBudgetBytes > 0) {
    EXPECT_EQ(mc::plan(tl::kMinFreeHeap, tl::kMinLargestBlock, kFreeFloorBudgeted, kMaxBlockFloorBudgeted, 0).verdict,
              mc::Verdict::Defrag);
  }
}

// §9: a lever recorded in stepsTried is NEVER re-suggested (FontCaches after a
// no-op release goes to Radio/Fallback, not back to FontCaches).
TEST(MemoryClaimGate, FontCachesRecordedIsNeverReSuggested) {
  const mc::Phase p = mc::plan(0, 0, tl::kMinFreeHeap, tl::kMinLargestBlock, mc::stepMask(mc::Step::FontCaches));
  EXPECT_EQ(p.verdict, mc::Verdict::Defrag);
  EXPECT_EQ(p.next, mc::Step::Radio);
}

// §9: once every lever is recorded, the same unmet floor can only answer
// Fallback; a floor that IS met still Proceeds regardless of the lever state —
// the recording never escalates a healthy heap.
TEST(MemoryClaimGate, ExhaustionWalksEachLeverExactlyOnce) {
  uint8_t steps = 0;
  const mc::Phase first = mc::plan(0, 0, tl::kMinFreeHeap, tl::kMinLargestBlock, steps);
  ASSERT_EQ(first.verdict, mc::Verdict::Defrag);
  EXPECT_EQ(first.next, mc::Step::FontCaches);
  steps = mc::triedAfter(steps, first.next);

  const mc::Phase second = mc::plan(0, 0, tl::kMinFreeHeap, tl::kMinLargestBlock, steps);
  ASSERT_EQ(second.verdict, mc::Verdict::Defrag);
  EXPECT_EQ(second.next, mc::Step::Radio);
  steps = mc::triedAfter(steps, second.next);

  const mc::Phase third = mc::plan(0, 0, tl::kMinFreeHeap, tl::kMinLargestBlock, steps);
  EXPECT_EQ(third.verdict, mc::Verdict::Fallback);
  EXPECT_EQ(steps, mc::kAllStepsMask);

  EXPECT_EQ(mc::plan(tl::kMinFreeHeap, tl::kMinLargestBlock, tl::kMinFreeHeap, tl::kMinLargestBlock, steps).verdict,
            mc::Verdict::Proceed);
}

// §3.2: FontCaches is recorded in the SAME pass it is released (a synchronous
// no-op release still counts as pulled); the very next plan never names it
// again.
TEST(MemoryClaimGate, FontCachesRecordedSamePassNoDoubleSuggest) {
  SimulatedPump pump;
  // suspendSettles=false keeps the radio lever unsettled, so the pass's final
  // phase is the radio hand-off, not a full run to Fallback.
  const memclaim::Phase p = pump.pass(0, 0, tl::kMinFreeHeap, tl::kMinLargestBlock, 1000, /*suspendSettles=*/false);
  EXPECT_EQ(p.verdict, mc::Verdict::Defrag);
  EXPECT_EQ(p.next, mc::Step::Radio);  // not FontCaches again
  EXPECT_TRUE(mc::stepTried(pump.stepsTried, mc::Step::FontCaches));
}

// §3.2 Boundedness: the Radio lever re-suggests across passes ONLY while the
// stop has not settled and its deadline has not expired; recording happens
// the pass the stop settles, and the re-plan then can only answer Fallback
// while the floor stays unmet.
TEST(MemoryClaimGate, RadioLeverReSuggestsUntilSettledThenRecords) {
  SimulatedPump pump;
  // Pass 1: FontCaches recorded same pass; the radio hand-off re-planned
  // within the pass, so the pass's final phase names Radio (one stop pass
  // issued, not settled yet, not recorded).
  const memclaim::Phase first = pump.pass(0, 0, tl::kMinFreeHeap, tl::kMinLargestBlock, 100, false);
  EXPECT_TRUE(mc::stepTried(pump.stepsTried, mc::Step::FontCaches));
  EXPECT_FALSE(mc::stepTried(pump.stepsTried, mc::Step::Radio));
  EXPECT_EQ(first.verdict, mc::Verdict::Defrag);
  EXPECT_EQ(first.next, mc::Step::Radio);
  // Pass 2 re-issues ONE stop pass, still unsettled:
  memclaim::Phase p = pump.pass(0, 0, tl::kMinFreeHeap, tl::kMinLargestBlock, 150, /*settles=*/false);
  EXPECT_EQ(p.verdict, mc::Verdict::Defrag);
  EXPECT_EQ(p.next, mc::Step::Radio);  // not settled: re-suggested until it settles
  EXPECT_FALSE(mc::stepTried(pump.stepsTried, mc::Step::Radio));
  EXPECT_LT(mc::radioStopElapsedMs(150, pump.radioFirstStopMs), kBleStopDeadlineMs);

  // Pass 3 (well inside the budget): still unsettled, still not recorded.
  p = pump.pass(0, 0, tl::kMinFreeHeap, tl::kMinLargestBlock, 2500, false);
  EXPECT_EQ(p.verdict, mc::Verdict::Defrag);
  EXPECT_EQ(p.next, mc::Step::Radio);

  // Pass 4: the tear-down pass reports settled — recorded, and the plan
  // exhausts to Fallback (the floor is still unmet).
  p = pump.pass(0, 0, tl::kMinFreeHeap, tl::kMinLargestBlock, 2600, /*settles=*/true);
  EXPECT_TRUE(mc::stepTried(pump.stepsTried, mc::Step::Radio));
  EXPECT_EQ(p.verdict, mc::Verdict::Fallback);
}

// §3.2 Boundedness: without a settle, the deadline (3,000 ms, the same budget
// Runtime.cpp gives a build-starved release) latches the lever recorded, and
// the next plan can only answer Fallback. One pass per pass — a teardown that
// would have settled on a later pass is never misreported before those passes
// ran.
TEST(MemoryClaimGate, RadioLeverLatchExpiresAtDeadlineWithoutSettle) {
  SimulatedPump pump;
  memclaim::Phase p = pump.pass(0, 0, tl::kMinFreeHeap, tl::kMinLargestBlock, 100, false);
  // Pass 1 records FontCaches in the same pass and hands off to Radio within
  // the pass; the radio stop pass issues (unsettled) but does not record.
  EXPECT_EQ(p.next, mc::Step::Radio);
  EXPECT_FALSE(mc::stepTried(pump.stepsTried, mc::Step::Radio));
  // Passes 2..n-1 (unsettled, before the budget): not recorded, Radio stays
  // the suggestion.
  p = pump.pass(0, 0, tl::kMinFreeHeap, tl::kMinLargestBlock, 2500, false);
  EXPECT_FALSE(mc::stepTried(pump.stepsTried, mc::Step::Radio));
  EXPECT_EQ(p.next, mc::Step::Radio);
  // The pass whose clock passes the budget without a settle: latched recorded.
  p = pump.pass(0, 0, tl::kMinFreeHeap, tl::kMinLargestBlock, 3100, false);
  EXPECT_TRUE(mc::stepTried(pump.stepsTried, mc::Step::Radio));
  EXPECT_EQ(p.verdict, mc::Verdict::Fallback);
}

// Wrap safety for the deadline arithmetic (millis() wraps ~49.7 days).
TEST(MemoryClaimGate, RadioStopDeadlineIsWrapSafe) {
  constexpr uint32_t kWrapStart = 4294967280u;  // 0xFFFFFFF0
  EXPECT_TRUE(mc::radioStopDeadlineExpired(kWrapStart + 32, kWrapStart, 32));
  EXPECT_FALSE(mc::radioStopDeadlineExpired(kWrapStart + 31, kWrapStart, 32));
  // After the wrap (now=31 is 16 past zero, i.e. 47 ms elapsed since the start):
  // already past the 32 ms budget — expiry happened at now==16.
  EXPECT_TRUE(mc::radioStopDeadlineExpired(31, kWrapStart, 32));
  EXPECT_TRUE(mc::radioStopDeadlineExpired(32, kWrapStart, 32));  // also past
  // Inside the budget but across the wrap point: never expired.
  EXPECT_FALSE(mc::radioStopDeadlineExpired(15, kWrapStart, 32));
  // No stop pass issued yet (firstStopMs == 0): never expired.
  EXPECT_FALSE(mc::radioStopDeadlineExpired(4000, 0, 3000));
}

// The radio-lever helpers degenerate cleanly: no elapsed time, no expiry.
TEST(MemoryClaimGate, RadioStopElapsedIsMonotonicZero) {
  EXPECT_EQ(mc::radioStopElapsedMs(1'000, 1'000), 0u);
  EXPECT_EQ(mc::radioStopElapsedMs(1'500, 1'000), 500u);
}
