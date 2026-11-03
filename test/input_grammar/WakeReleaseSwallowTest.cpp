// Host tests for the wake-release swallow policy (src/util/WakeReleaseSwallow.h,
// docs/design/2026-11-03-wake-release-shutdown-swallow.md): a power press that
// starts a boot must never dispatch an in-app action on its own release — no
// short-click sleep, no refresh, no reader shortcut — on every wake path
// (deep-sleep wake AND cold boot after power off) for every board with a
// power button. The SwallowFlow harness mirrors main.cpp's wiring: one arm
// decision in setup(), a one-shot consume per loop tick that cancels the
// frontlight click window and returns early instead of dispatching.
#include <gtest/gtest.h>

#include "util/WakeReleaseSwallow.h"

namespace {

using W = WakeReleaseSwallow;

// Mirrors main.cpp's swallow wiring: arm in setup() from the classification
// and the held-at-input-start sample; consume one-shot per loop tick.
struct SwallowFlow {
  bool pending = false;

  void setup(const bool classifiedWake, const bool powerHeldAtInputStart, const bool silentReboot) {
    if (W::shouldArm(classifiedWake, powerHeldAtInputStart, silentReboot)) pending = true;
  }

  // One loop tick. True = the swallow consumed this tick's frame (main.cpp
  // cancels the click window and returns early — the release never reaches
  // the sleep/refresh/shortcut handlers).
  bool tick(const bool powerPressed) {
    if (!W::shouldConsume(pending, powerPressed)) return false;
    pending = false;
    return true;
  }
};

// X4 Pro shutdown wake: the user long-presses the power button from power
// off; the press energizes the rail, the boot runs, and the release replays
// as a configured short power click (sleep). The held-at-input-start sample
// (taken before the poll task's first edge sample) must arm the swallow, and
// the release must be consumed before any dispatch.
TEST(WakeReleaseSwallowTest, ShutdownWakeHeldThroughBootIsSwallowed) {
  SwallowFlow flow;
  flow.setup(/*classifiedWake=*/false, /*powerHeldAtInputStart=*/true, /*silentReboot=*/false);
  EXPECT_EQ(flow.pending, true);

  // The hold runs through boot and into the app: while held, no consume, and
  // the pending swallow never costs an early return.
  EXPECT_EQ(flow.tick(/*powerPressed=*/true), false);
  EXPECT_EQ(flow.tick(true), false);
  EXPECT_EQ(flow.pending, true);

  // The wake press's release: consumed exactly once, dispatching nothing.
  EXPECT_EQ(flow.tick(/*powerPressed=*/false), true);
  EXPECT_EQ(flow.pending, false);

  // The swallow is self-consuming: the release is gone. A later genuine click
  // dispatches normally (the sleep branch fires) instead of being swallowed.
  EXPECT_EQ(flow.tick(true), false);
  EXPECT_EQ(flow.tick(false), false);
}

// The user releases the wake press while the boot is still running (queued
// press+release in the async queue): the held-at-input-start sample still
// arms the swallow, and the first loop tick — which drains both edges and
// reads the level released — consumes the frame before any sleep dispatch.
TEST(WakeReleaseSwallowTest, ShutdownWakeReleasedDuringBootIsSwallowed) {
  SwallowFlow flow;
  flow.setup(false, /*powerHeldAtInputStart=*/true, false);
  EXPECT_EQ(flow.pending, true);

  // First post-boot tick: the press+release queued during boot drains and the
  // level reads released. The swallow fires on this tick.
  EXPECT_EQ(flow.tick(/*powerPressed=*/false), true);
  EXPECT_EQ(flow.pending, false);

  // One-shot: nothing is left to swallow.
  EXPECT_EQ(flow.tick(false), false);
}

// A genuine post-boot click (a press that begins AFTER the first dispatch)
// must still sleep: nothing arms the swallow, so the release reaches the
// sleep branch.
TEST(WakeReleaseSwallowTest, GenuinePostBootClickStillDispatches) {
  SwallowFlow flow;
  flow.setup(/*classifiedWake=*/false, /*powerHeldAtInputStart=*/false, /*silentReboot=*/false);
  EXPECT_EQ(flow.pending, false);

  // Click begins after boot: press then release, both dispatch normally.
  EXPECT_EQ(flow.tick(true), false);
  EXPECT_EQ(flow.tick(false), false);
  EXPECT_EQ(flow.pending, false);
}

// Arming must cost nothing observable when the button is not held after
// boot: the swallow consumes on the very first tick (level already released)
// and is gone — one early-returned loop tick, no swallowed genuine input.
TEST(WakeReleaseSwallowTest, ArmingWithoutAHeldButtonIsOneInvisibleTick) {
  SwallowFlow flow;
  flow.setup(false, true, false);
  EXPECT_EQ(flow.pending, true);

  // The user released before boot finished: the swallow burns one tick and
  // never touches a genuine later click.
  EXPECT_EQ(flow.tick(false), true);
  EXPECT_EQ(flow.tick(true), false);
  EXPECT_EQ(flow.tick(false), false);
}

// Deep-sleep wake classification keeps arming the swallow even when the wake
// press is already released by setup time (chip reset latency) — the existing
// swallow behavior must not regress.
TEST(WakeReleaseSwallowTest, ClassifiedWakeArmsEvenWhenReleased) {
  SwallowFlow flow;
  flow.setup(/*classifiedWake=*/true, /*powerHeldAtInputStart=*/false, /*silentReboot=*/false);
  EXPECT_EQ(flow.pending, true);

  EXPECT_EQ(flow.tick(true), false);
  EXPECT_EQ(flow.tick(false), true);
  EXPECT_EQ(flow.pending, false);
}

// Silent maintenance reboots keep their own boot boundary: a button held
// across the restart must NOT have its release swallowed (the silent path
// discards queued edges and replays a held button's release deliberately),
// but the classified wake arm is untouched by the silent gate.
TEST(WakeReleaseSwallowTest, SilentRebootHeldSampleDoesNotArm) {
  SwallowFlow silent;
  silent.setup(/*classifiedWake=*/false, /*powerHeldAtInputStart=*/true, /*silentReboot=*/true);
  EXPECT_EQ(silent.pending, false);
  EXPECT_EQ(silent.tick(true), false);
  EXPECT_EQ(silent.tick(false), false);

  SwallowFlow classifiedSilent;
  classifiedSilent.setup(/*classifiedWake=*/true, true, /*silentReboot=*/true);
  EXPECT_EQ(classifiedSilent.pending, true);
  EXPECT_EQ(classifiedSilent.tick(false), true);
}

// The arm decision matrix, pinned cell by cell.
TEST(WakeReleaseSwallowTest, ArmDecisionMatrix) {
  EXPECT_EQ(W::shouldArm(false, true, false), true);    // shutdown wake (this fix)
  EXPECT_EQ(W::shouldArm(false, false, false), false);  // ordinary boot
  EXPECT_EQ(W::shouldArm(true, false, false), true);    // deep-sleep wake (existing)
  EXPECT_EQ(W::shouldArm(true, true, false), true);
  EXPECT_EQ(W::shouldArm(false, true, true), false);  // silent reboot boundary
  EXPECT_EQ(W::shouldArm(false, false, true), false);
  EXPECT_EQ(W::shouldArm(true, false, true), true);  // classification survives silent
}

// The consume decision matrix: one-shot on the released frame, never while
// held, never without a pending swallow.
TEST(WakeReleaseSwallowTest, ConsumeDecisionMatrix) {
  EXPECT_EQ(W::shouldConsume(true, false), true);    // armed + released → consume
  EXPECT_EQ(W::shouldConsume(true, true), false);    // armed + held → wait
  EXPECT_EQ(W::shouldConsume(false, false), false);  // nothing pending
  EXPECT_EQ(W::shouldConsume(false, true), false);
}

}  // namespace
