#include <gtest/gtest.h>
#include <util/WebServerHeapGate.h>

namespace {

// A healthy heap clears both the mode-selection gate and the begin() tail gate.
TEST(WebServerHeapGateTest, ProceedAtAndAboveFloor) {
  EXPECT_EQ(webgate::verdict(webgate::kWebStackFloor, webgate::kMaxBlockFloor, webgate::kWebStackFloor, false),
            webgate::Verdict::Proceed);
  EXPECT_EQ(webgate::verdict(webgate::kWebStackFloor + 1, webgate::kMaxBlockFloor + 1, webgate::kWebStackFloor, false),
            webgate::Verdict::Proceed);
  EXPECT_EQ(webgate::verdict(300000, 200000, webgate::kWebStackFloor, false), webgate::Verdict::Proceed);
}

TEST(WebServerHeapGateTest, DefragOnLowFreeHeap) {
  EXPECT_EQ(webgate::verdict(webgate::kWebStackFloor - 1, webgate::kMaxBlockFloor, webgate::kWebStackFloor, false),
            webgate::Verdict::Defrag);
  EXPECT_EQ(webgate::verdict(0, webgate::kMaxBlockFloor, webgate::kWebStackFloor, false), webgate::Verdict::Defrag);
}

// The X3 failure mode is fragmentation: plenty of free heap in total, but no
// contiguous block large enough for the web stack's burst allocations.
TEST(WebServerHeapGateTest, DefragOnFragmentedHeapEvenWithFreeTotal) {
  EXPECT_EQ(webgate::verdict(webgate::kWebStackFloor, webgate::kMaxBlockFloor - 1, webgate::kWebStackFloor, false),
            webgate::Verdict::Defrag);
  EXPECT_EQ(webgate::verdict(300000, webgate::kMaxBlockFloor - 1, webgate::kWebStackFloor, false),
            webgate::Verdict::Defrag);
  EXPECT_EQ(webgate::verdict(webgate::kWebStackFloor, 0, webgate::kWebStackFloor, false), webgate::Verdict::Defrag);
}

// Below the floor with no retry yet spent: a reboot is attempted (Defrag). Once
// the one-shot reboot retry is spent, the same low heap gives up (Fallback) so
// a structurally-underprovisioned device never boot-loops.
TEST(WebServerHeapGateTest, BothLowDefragsUntilRetryExhausted) {
  EXPECT_EQ(webgate::verdict(0, 0, webgate::kWebStackFloor, false), webgate::Verdict::Defrag);
  EXPECT_EQ(webgate::verdict(0, 0, webgate::kWebStackFloor, true), webgate::Verdict::Fallback);
}

TEST(WebServerHeapGateTest, FallbackOnRetryExhausted) {
  EXPECT_EQ(webgate::verdict(webgate::kWebStackFloor - 1, webgate::kMaxBlockFloor, webgate::kWebStackFloor, true),
            webgate::Verdict::Fallback);
  EXPECT_EQ(webgate::verdict(0, 0, webgate::kWebStackFloor, true), webgate::Verdict::Fallback);
}

// A healthy heap must still Proceed even after the reboot retry is spent — the
// latch only escalates low-heap verdicts, never blocks a genuinely-fit one.
TEST(WebServerHeapGateTest, ProceedEvenWhenRetryExhaustedIfFloorMet) {
  EXPECT_EQ(webgate::verdict(webgate::kWebStackFloor, webgate::kMaxBlockFloor, webgate::kWebStackFloor, true),
            webgate::Verdict::Proceed);
  EXPECT_EQ(webgate::verdict(300000, 200000, webgate::kWebStackFloor, true), webgate::Verdict::Proceed);
}

// The contiguous-block criterion is the TLS-record precedent, not the full free
// floor: the mode-selection gate must not require one 89 KB block on a
// fragmented heap (the web stack is many small allocations, not one block).
TEST(WebServerHeapGateTest, MaxBlockFloorIsTheContiguousCriterionNotTheFreeFloor) {
  ASSERT_LT(webgate::kMaxBlockFloor, webgate::kModeSelectionFreeFloor);
  // Free heap clears the full mode-selection floor but the largest block is
  // under the contiguous criterion: still Defrag (fragmented).
  EXPECT_EQ(webgate::verdict(webgate::kModeSelectionFreeFloor, webgate::kMaxBlockFloor - 1,
                             webgate::kModeSelectionFreeFloor, false),
            webgate::Verdict::Defrag);
  // Largest block clears the contiguous criterion but free heap is under the
  // mode-selection floor: still Defrag (not enough room for WiFi + web).
  EXPECT_EQ(webgate::verdict(webgate::kModeSelectionFreeFloor - 1, webgate::kMaxBlockFloor,
                             webgate::kModeSelectionFreeFloor, false),
            webgate::Verdict::Defrag);
  EXPECT_EQ(webgate::verdict(webgate::kModeSelectionFreeFloor, webgate::kMaxBlockFloor,
                             webgate::kModeSelectionFreeFloor, false),
            webgate::Verdict::Proceed);
}

// The mode-selection gate is stricter than the begin() tail gate: a heap just
// above the web-stack floor but below the combined (WiFi+web) floor Defrags at
// the mode-selection site, the only place a reboot is safe (WiFi not up yet).
TEST(WebServerHeapGateTest, ModeSelectionFloorExceedsTailFloor) {
  ASSERT_GT(webgate::kModeSelectionFreeFloor, webgate::kWebStackFloor);
  EXPECT_EQ(webgate::verdict(webgate::kWebStackFloor, webgate::kMaxBlockFloor, webgate::kModeSelectionFreeFloor, false),
            webgate::Verdict::Defrag);
  EXPECT_EQ(webgate::verdict(webgate::kModeSelectionFreeFloor, webgate::kMaxBlockFloor,
                             webgate::kModeSelectionFreeFloor, false),
            webgate::Verdict::Proceed);
}

TEST(WebServerHeapGateTest, FloorConstantsMatchDesignDoc) {
  // Web-stack floor sits in the design-doc's 32-48 KB range (44 KB = the X3
  // abort log's measured footprint); the reserve is the ~45 KB WiFi+lwIP+mDNS
  // resident set from the onEnter comment; the contiguous criterion mirrors
  // tlsheap::kMinLargestBlock.
  EXPECT_EQ(webgate::kWebStackFloor, 44000u);
  EXPECT_EQ(webgate::kWifiWorkingSetReserve, 45000u);
  EXPECT_EQ(webgate::kModeSelectionFreeFloor, webgate::kWebStackFloor + webgate::kWifiWorkingSetReserve);
  EXPECT_EQ(webgate::kMaxBlockFloor, 20000u);
}

}  // namespace
