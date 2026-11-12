#include <gtest/gtest.h>

#include <cstdint>

#include "util/HomeButtonInput.h"

namespace {

class HomeButtonInputTest : public ::testing::Test {
 protected:
  using A = HomeButtonAction;

  HomeButtonAction tick(uint32_t now, bool tap = false, bool hold = false, bool swipe = false, bool press = false) {
    return input.update(now, tap, hold, swipe, press, A::Home, A::ToggleFrontlight, A::ReaderMenu);
  }

  HomeButtonInput input;
};

TEST_F(HomeButtonInputTest, RecognizesConfiguredGestures) {
  EXPECT_EQ(tick(0, true), A::Ignore);
  EXPECT_EQ(tick(350), A::Ignore);
  EXPECT_EQ(tick(351), A::Home);
  EXPECT_EQ(tick(352), A::Ignore);
  EXPECT_EQ(tick(1000, true), A::Ignore);
  EXPECT_EQ(tick(1350, true), A::ToggleFrontlight);
  EXPECT_EQ(tick(1701), A::Ignore);
  EXPECT_EQ(tick(2000, false, true), A::ReaderMenu);
  EXPECT_EQ(tick(2001), A::Ignore);
}

TEST_F(HomeButtonInputTest, SwipeCancelsPendingTapAndHoldIsConsumed) {
  EXPECT_EQ(tick(0, true), A::Ignore);
  EXPECT_EQ(tick(100, false, false, true), A::Ignore);
  EXPECT_EQ(tick(500), A::Ignore);

  EXPECT_EQ(tick(1000, false, true), A::ReaderMenu);
  EXPECT_EQ(tick(1001), A::Ignore);

  EXPECT_EQ(tick(2000, false, true), A::ReaderMenu);
  EXPECT_EQ(tick(2001), A::Ignore);
}

TEST_F(HomeButtonInputTest, ReportsGestureProvenance) {
  EXPECT_EQ(tick(0, true), A::Ignore);
  EXPECT_EQ(input.lastGesture(), HomeButtonGesture::None);

  EXPECT_EQ(tick(351), A::Home);
  EXPECT_EQ(input.lastGesture(), HomeButtonGesture::Tap);

  EXPECT_EQ(tick(1000, true), A::Ignore);
  EXPECT_EQ(tick(1350, true), A::ToggleFrontlight);
  EXPECT_EQ(input.lastGesture(), HomeButtonGesture::DoubleTap);

  EXPECT_EQ(tick(2000, false, true), A::ReaderMenu);
  EXPECT_EQ(input.lastGesture(), HomeButtonGesture::Hold);
  EXPECT_EQ(tick(2001), A::Ignore);
  EXPECT_EQ(input.lastGesture(), HomeButtonGesture::None);
}

TEST_F(HomeButtonInputTest, TapThenHoldSuppressesEarlySingleTap) {
  EXPECT_EQ(tick(3000, true), A::Ignore);
  EXPECT_EQ(tick(3200, false, false, false, true), A::Ignore);
  EXPECT_EQ(tick(3400), A::Ignore);
  EXPECT_EQ(tick(3900, false, true), A::ReaderMenu);
  EXPECT_EQ(tick(4000), A::Ignore);
}

TEST_F(HomeButtonInputTest, ResetClearsPendingTap) {
  EXPECT_EQ(tick(5000, true), A::Ignore);
  EXPECT_EQ(tick(5100, true, false, true), A::Ignore);
  EXPECT_EQ(tick(5500), A::Ignore);
  EXPECT_EQ(tick(6000, true), A::Ignore);
  input.reset();
  EXPECT_EQ(tick(6500), A::Ignore);
}

TEST_F(HomeButtonInputTest, MillisecondClockWrapUsesUnsignedSubtraction) {
  EXPECT_EQ(tick(UINT32_MAX - 100, true), A::Ignore);
  EXPECT_EQ(tick(100, true), A::ToggleFrontlight);
  EXPECT_EQ(tick(UINT32_MAX - 100, true), A::Ignore);
  EXPECT_EQ(tick(251), A::Home);
}

TEST_F(HomeButtonInputTest, SeparatedTapsEachProduceOneAction) {
  EXPECT_EQ(tick(7000, true), A::Ignore);
  EXPECT_EQ(tick(7400, true), A::Home);
  EXPECT_EQ(tick(7751), A::Home);
  // Disabling double tap removes the single-tap delay.
  EXPECT_EQ(input.update(8000, true, false, false, false, A::Bookmark, A::Ignore, A::ReaderMenu), A::Bookmark);
  // Every Home gesture can launch clipping selection without changing persisted action IDs.
  static_assert(static_cast<uint8_t>(A::ToggleFrontlight) == 10);
  EXPECT_EQ(input.update(9000, true, false, false, false, A::CreateClipping, A::Ignore, A::Home), A::CreateClipping);
  EXPECT_EQ(input.update(10000, true, false, false, false, A::Home, A::CreateClipping, A::Ignore), A::Ignore);
  EXPECT_EQ(input.update(10100, true, false, false, false, A::Home, A::CreateClipping, A::Ignore), A::CreateClipping);
  EXPECT_EQ(input.update(11000, false, true, false, false, A::Home, A::Ignore, A::CreateClipping), A::CreateClipping);
}

TEST_F(HomeButtonInputTest, DisabledDoubleTapRemovesDelay) {
  EXPECT_EQ(input.update(8000, true, false, false, false, A::Bookmark, A::Ignore, A::ReaderMenu), A::Bookmark);
  EXPECT_EQ(input.lastGesture(), HomeButtonGesture::Tap);
}

// Combination matrix (docs/design/2026-09-22-input-coverage.md §3): every
// gesture × every legal action value resolves through the passed mapping —
// the classifier is agnostic to WHICH action it carries.
TEST_F(HomeButtonInputTest, GestureActionMatrixTap) {
  for (uint8_t a = 0; a < static_cast<uint8_t>(A::Count); ++a) {
    const auto action = static_cast<A>(a);
    HomeButtonInput in;
    // Tap action delivered directly when the double-tap slot is Ignore.
    EXPECT_EQ(in.update(1000, true, false, false, false, action, A::Ignore, action), action);
    EXPECT_EQ(in.lastGesture(), HomeButtonGesture::Tap);
  }
}

TEST_F(HomeButtonInputTest, GestureActionMatrixDouble) {
  for (uint8_t a = 0; a < static_cast<uint8_t>(A::Count); ++a) {
    const auto action = static_cast<A>(a);
    HomeButtonInput in;
    // First tap parks (Ignore arrives only when the window resolves) — except
    // when the double-tap slot is Ignore, which disables the window and
    // delivers the tap slot directly.
    EXPECT_EQ(in.update(1000, true, false, false, false, A::Home, action, A::Home),
              a == static_cast<uint8_t>(A::Ignore) ? A::Home : A::Ignore);
    EXPECT_EQ(in.update(1100, true, false, false, false, A::Home, action, A::Home),
              a == static_cast<uint8_t>(A::Ignore) ? A::Home : action);
    EXPECT_EQ(in.lastGesture(),
              a == static_cast<uint8_t>(A::Ignore) ? HomeButtonGesture::Tap : HomeButtonGesture::DoubleTap);
  }
}

TEST_F(HomeButtonInputTest, GestureActionMatrixHold) {
  for (uint8_t a = 0; a < static_cast<uint8_t>(A::Count); ++a) {
    const auto action = static_cast<A>(a);
    HomeButtonInput in;
    EXPECT_EQ(in.update(1000, false, true, false, false, A::Home, A::Home, action), action);
    EXPECT_EQ(in.lastGesture(), HomeButtonGesture::Hold);
    // One action per hold: the next quiet tick is inert.
    EXPECT_EQ(in.update(1100, false, false, false, false, A::Home, A::Home, action), A::Ignore);
  }
}

// A second contact (press) inside the double-tap window converts the pending
// tap into a double-tap on the NEXT tap — never a stray single tap in between.
TEST_F(HomeButtonInputTest, SecondContactWithinWindowBridgesToDoubleTap) {
  EXPECT_EQ(tick(1000, true), A::Ignore);
  EXPECT_EQ(tick(1100, false, false, false, true), A::Ignore);
  EXPECT_EQ(tick(1200, true), A::ToggleFrontlight);
  EXPECT_EQ(input.lastGesture(), HomeButtonGesture::DoubleTap);
}

// Hold wins over a pending tap: the pending single tap is dropped when the
// hold fires (no stale tap replay after the hold action).
TEST_F(HomeButtonInputTest, HoldDuringPendingTapDropsTap) {
  EXPECT_EQ(tick(1000, true), A::Ignore);
  EXPECT_EQ(tick(1200, false, true), A::ReaderMenu);
  EXPECT_EQ(tick(1400), A::Ignore);
}
TEST(HomeButtonValues, UpstreamAndForkIndicesAreStable) {
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Home), 0);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Ignore), 1);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::NextPage), 2);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Refresh), 3);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Footnotes), 4);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Confirm), 5);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Sync), 6);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Bookmark), 7);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Dictionary), 8);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::ReaderMenu), 9);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::ToggleFrontlight), 10);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Sleep), 11);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Screenshot), 12);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::GoBack), 13);
  // CreateClipping appends after every pre-existing value: saved settings.json
  // indices keep their meaning, only Count moves.
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::CreateClipping), 14);
  // FileBrowser (upstream #4460) appends after CreateClipping: saved
  // settings.json indices keep their meaning, only Count moves.
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::FileBrowser), 15);
  EXPECT_EQ(static_cast<uint8_t>(HomeButtonAction::Count), 16);
}

}  // namespace
