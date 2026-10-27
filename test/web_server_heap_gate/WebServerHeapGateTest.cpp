#include <gtest/gtest.h>
#include <util/WebServerHeapGate.h>

namespace {

// A healthy heap clears both the mode-selection gate and the begin() tail gate.
TEST(WebServerHeapGateTest, ProceedAtAndAboveFloor) {
  EXPECT_EQ(webgate::verdict(webgate::kWebStackFloor, webgate::kWebStackFloor, webgate::kWebStackFloor, false),
            webgate::Verdict::Proceed);
  EXPECT_EQ(webgate::verdict(webgate::kWebStackFloor + 1, webgate::kWebStackFloor + 1, webgate::kWebStackFloor, false),
            webgate::Verdict::Proceed);
  EXPECT_EQ(webgate::verdict(300000, 200000, webgate::kWebStackFloor, false), webgate::Verdict::Proceed);
}

TEST(WebServerHeapGateTest, DefragOnLowFreeHeap) {
  EXPECT_EQ(webgate::verdict(webgate::kWebStackFloor - 1, webgate::kWebStackFloor, webgate::kWebStackFloor, false),
            webgate::Verdict::Defrag);
  EXPECT_EQ(webgate::verdict(0, webgate::kWebStackFloor, webgate::kWebStackFloor, false), webgate::Verdict::Defrag);
}

// The X3 failure mode is fragmentation: plenty of free heap in total, but no
// contiguous block large enough for the WebSocket client array.
TEST(WebServerHeapGateTest, DefragOnFragmentedHeapEvenWithFreeTotal) {
  EXPECT_EQ(webgate::verdict(webgate::kWebStackFloor, webgate::kWebStackFloor - 1, webgate::kWebStackFloor, false),
            webgate::Verdict::Defrag);
  EXPECT_EQ(webgate::verdict(300000, webgate::kWebStackFloor - 1, webgate::kWebStackFloor, false),
            webgate::Verdict::Defrag);
  EXPECT_EQ(webgate::verdict(webgate::kWebStackFloor, 0, webgate::kWebStackFloor, false), webgate::Verdict::Defrag);
}

// Below the floor with no retry yet left: a reboot is attempted (Defrag). Once
// the one-shot reboot retry is exhausted, the same low heap gives up (Fallback)
// so a structurally-underprovisioned device never boot-loops.
TEST(WebServerHeapGateTest, BothLowDefragsUntilRetryExhausted) {
  EXPECT_EQ(webgate::verdict(0, 0, webgate::kWebStackFloor, false), webgate::Verdict::Defrag);
  EXPECT_EQ(webgate::verdict(0, 0, webgate::kWebStackFloor, true), webgate::Verdict::Fallback);
}

TEST(WebServerHeapGateTest, FallbackOnRetryExhausted) {
  EXPECT_EQ(webgate::verdict(webgate::kWebStackFloor - 1, webgate::kWebStackFloor, webgate::kWebStackFloor, true),
            webgate::Verdict::Fallback);
  EXPECT_EQ(webgate::verdict(0, 0, webgate::kWebStackFloor, true), webgate::Verdict::Fallback);
}

// A healthy heap must still Proceed even after the reboot retry is exhausted —
// the latch only escalates low-heap verdicts, never blocks a genuinely-fit one.
TEST(WebServerHeapGateTest, ProceedEvenWhenRetryExhaustedIfFloorMet) {
  EXPECT_EQ(webgate::verdict(webgate::kWebStackFloor, webgate::kWebStackFloor, webgate::kWebStackFloor, true),
            webgate::Verdict::Proceed);
  EXPECT_EQ(webgate::verdict(300000, 200000, webgate::kWebStackFloor, true), webgate::Verdict::Proceed);
}

// The mode-selection gate is stricter than the begin() tail gate: a heap just
// above the web-stack floor but below the combined (WiFi+web) floor Defrags at
// the mode-selection site, the only place a reboot is safe (WiFi not up yet).
TEST(WebServerHeapGateTest, ModeSelectionFloorExceedsTailFloor) {
  ASSERT_GT(webgate::kModeSelectionFloor, webgate::kWebStackFloor);
  EXPECT_EQ(webgate::verdict(webgate::kWebStackFloor, webgate::kWebStackFloor, webgate::kModeSelectionFloor, false),
            webgate::Verdict::Defrag);
  EXPECT_EQ(
      webgate::verdict(webgate::kModeSelectionFloor, webgate::kModeSelectionFloor, webgate::kModeSelectionFloor, false),
      webgate::Verdict::Proceed);
}

TEST(WebServerHeapGateTest, FloorConstantsMatchDesignDoc) {
  // Web-stack floor sits in the design-doc's 32-48 KB range; the reserve lifts
  // the mode-selection gate to the ~45 KB WiFi+web combined floor.
  EXPECT_EQ(webgate::kWebStackFloor, 40000u);
  EXPECT_EQ(webgate::kWifiWorkingSetReserve, 5000u);
  EXPECT_EQ(webgate::kModeSelectionFloor, webgate::kWebStackFloor + webgate::kWifiWorkingSetReserve);
}

}  // namespace
