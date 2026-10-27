#include <gtest/gtest.h>

#include "activities/reader/PageTurnBatch.h"
#include "activities/reader/TurnNetting.h"

namespace {

using turnnet::TriggerCounts;

// ── Netting (§3.4): counts of the direction trigger sets, single-shots ±1 ──

TEST(TurnNettingTest, ThreeQueuedNextPressesNetToThree) {
  TriggerCounts t;
  t.nextCount = 3;
  EXPECT_EQ(turnnet::net(t), 3);
}

// Netting: 2 next + 1 prev in one frame = ONE page forward, not three turns.
TEST(TurnNettingTest, TwoNextOnePrevNetsToOne) {
  TriggerCounts t;
  t.nextCount = 2;
  t.prevCount = 1;
  EXPECT_EQ(turnnet::net(t), 1);
}

// Net zero (Next, Prev, Next, Prev) is a no-op.
TEST(TurnNettingTest, BalancedDirectionsNetToZero) {
  TriggerCounts t;
  t.nextCount = 2;
  t.prevCount = 2;
  EXPECT_EQ(turnnet::net(t), 0);
}

// A held button queues exactly one press edge (§3.3): hold = count 1.
TEST(TurnNettingTest, HoldCountsOne) {
  TriggerCounts t;
  t.prevCount = 1;
  EXPECT_EQ(turnnet::net(t), -1);
}

// Single-shot sources contribute ±1 each (state flags, not queued edges).
TEST(TurnNettingTest, SingleShotSourcesFoldIntoTheNet) {
  TriggerCounts t;
  t.touchNext = true;
  EXPECT_EQ(turnnet::net(t), 1);
  TriggerCounts u;
  u.tiltPrev = true;
  u.powerTurn = true;
  EXPECT_EQ(turnnet::net(u), 0);
  TriggerCounts v;
  v.homeNextPage = true;
  v.touchPrev = true;
  EXPECT_EQ(turnnet::net(v), 0);
}

// Queued presses and single-shots stack in one frame: 2 queued Next + a
// touch-swipe Next = 3 pages in one refresh.
TEST(TurnNettingTest, QueuedCountsAndTouchStack) {
  TriggerCounts t;
  t.nextCount = 2;
  t.touchNext = true;
  EXPECT_EQ(turnnet::net(t), 3);
}

// Magnitude-carrying pendingManualTurn: a frame's net joins the parked jump,
// saturated to int8_t range (audit #4 — the full magnitude applies once).
TEST(TurnNettingTest, AccumulateJoinsAndSaturates) {
  EXPECT_EQ(turnnet::accumulate(0, 3), 3);
  EXPECT_EQ(turnnet::accumulate(3, -1), 2);
  EXPECT_EQ(turnnet::accumulate(-2, -2), -4);
  EXPECT_EQ(turnnet::accumulate(120, 120), 127);
  EXPECT_EQ(turnnet::accumulate(-120, -120), -127);
  EXPECT_EQ(turnnet::accumulate(5, 0), 5);
}

// ── Batch loop (§3.2): |steps| single-step turns, ONE render at the end ────

// A fake flat page model (XTC shape): N steps forward land +N pages.
struct FakeBook {
  int page = 0;
  int pageCount = 100;
  int calls = 0;

  pageturn::Step step(const bool forward) {
    ++calls;
    const int before = page;
    if (forward) {
      if (page >= pageCount) return pageturn::Step::Blocked;
      ++page;
    } else {
      if (page <= 0) return pageturn::Step::Blocked;
      --page;
    }
    return page != before && (forward ? page >= pageCount : page <= 0) ? pageturn::Step::Boundary
                                                                       : pageturn::Step::Turned;
  }
};

// Three queued presses land +3 pages in ONE batch (one render follows).
TEST(PageTurnBatchTest, ThreePressesLandThreePages) {
  FakeBook book;
  EXPECT_TRUE(pageturn::turnBy(3, [&](bool f) { return book.step(f); }));
  EXPECT_EQ(book.page, 3);
  EXPECT_EQ(book.calls, 3);
}

// End of book clamps: the jump stops at the last page, single render.
TEST(PageTurnBatchTest, EndOfBookClampsTheJump) {
  FakeBook book;
  book.page = 98;
  EXPECT_TRUE(pageturn::turnBy(5, [&](bool f) { return book.step(f); }));
  EXPECT_EQ(book.page, book.pageCount);
  EXPECT_EQ(book.calls, 2);  // 99, 100(EOB) — clamped, no overshoot
}

// Backward at the start of the book blocks with nothing turned.
TEST(PageTurnBatchTest, BackwardAtStartIsANoOp) {
  FakeBook book;
  book.page = 0;
  EXPECT_FALSE(pageturn::turnBy(-3, [&](bool f) { return book.step(f); }));
  EXPECT_EQ(book.page, 0);
  EXPECT_EQ(book.calls, 1);
}

// Net zero applies no step at all (no pageTurn calls, no render-worthy change).
TEST(PageTurnBatchTest, NetZeroCallsNoSteps) {
  FakeBook book;
  EXPECT_FALSE(pageturn::turnBy(0, [&](bool f) { return book.step(f); }));
  EXPECT_EQ(book.calls, 0);
  EXPECT_EQ(book.page, 0);
}

// ── Chapter-skip long-press direction (ReaderActivity/XTC regression pin) ──
// skipPages() ADDS the amount to the position, so a Next long-press must pass
// +10 (forward) and a Prev long-press -10. The aggregation dispatch initially
// flipped the sign (Next landed -10); this pins the production helper.
TEST(ChapterSkipTest, NextSkipsForwardPrevSkipsBackward) {
  EXPECT_EQ(turnnet::chapterSkipAmount(true, 10), 10);
  EXPECT_EQ(turnnet::chapterSkipAmount(false, 10), -10);
  EXPECT_EQ(turnnet::chapterSkipAmount(true, 1), 1);
  EXPECT_EQ(turnnet::chapterSkipAmount(false, 1), -1);
  EXPECT_EQ(turnnet::chapterSkipAmount(false, 0), 0);
}

// ── Render-gate parking (audit #4): presses behind the guard are not lost ──
// Frames whose presses arrive while the turn guard is busy (a render in
// flight or inside the kMinManualTurnGapMs window) park INTO the pending
// running sum; the first frame with a pending magnitude and an open gate
// applies the joined total exactly once.
TEST(AggregationGateTest, PressesBehindAGuardWindowAllSurviveIntoTheAppliedJump) {
  int8_t pending = 0;
  int applied = 0;  // magnitude handed to pageTurnBy
  bool appliedThisFrame = false;
  const auto dispatch = [&](int freshNet, bool guardActive) {
    appliedThisFrame = false;
    if (turnnet::pendingManualTurnGate(pending, guardActive) == turnnet::GateAction::Apply) {
      applied = turnnet::accumulate(pending, freshNet);
      pending = 0;
      appliedThisFrame = true;
      return;
    }
    // Park site: the frame's net joins the pending sum.
    pending = turnnet::accumulate(pending, freshNet);
  };

  // Three Next presses arrive in successive frames while the guard window is
  // busy: each parks into the pending sum, none fires a turn.
  dispatch(1, /*guardActive=*/true);
  EXPECT_EQ(pending, 1);
  EXPECT_FALSE(appliedThisFrame);
  dispatch(1, /*guardActive=*/true);
  EXPECT_EQ(pending, 2);
  EXPECT_FALSE(appliedThisFrame);
  // The guard opens: ONE applied jump of the joined magnitude — every press
  // survived.
  dispatch(1, /*guardActive=*/false);
  EXPECT_TRUE(appliedThisFrame);
  EXPECT_EQ(applied, 3);

  // Mixed directions behind a repeat burst: 2 next + 1 prev parked while the
  // gate is busy net to +1 applied once.
  dispatch(2, true);
  dispatch(-1, true);
  dispatch(0, false);
  EXPECT_TRUE(appliedThisFrame);
  EXPECT_EQ(applied, 1);
}

// A sustained burst across never-opening guard windows saturates at the
// int8_t clamp and never wraps backwards.
TEST(AggregationGateTest, SustainedBurstBehindAllBusyGuardSaturatesWithoutWrapping) {
  int8_t pending = 0;
  for (int i = 0; i < 300; ++i) {
    EXPECT_NE(turnnet::pendingManualTurnGate(pending, /*guardActive=*/true), turnnet::GateAction::Apply);
    pending = turnnet::accumulate(pending, 1);
  }
  EXPECT_EQ(pending, 127);
}

}  // namespace
