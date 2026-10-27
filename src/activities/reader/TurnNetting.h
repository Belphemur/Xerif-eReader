#pragma once

#include <cstdint>

// Turn netting for the reader page-turn dispatch (queued-input aggregation,
// docs/design/2026-10-27-queued-input-aggregation.md §3.2/§3.4). The drained
// per-button edge counts of the direction trigger sets are netted into ONE
// page-jump magnitude per dispatch frame; single-shot sources (touch zones,
// tilt, power-turn, home-key NextPage) each contribute ±1. Net zero (Next,
// Prev, Next, Prev) is a no-op. Pure so the arithmetic stays host-testable.
namespace turnnet {

struct TriggerCounts {
  // Drained edge counts of the direction trigger sets (wasPressedCount /
  // wasReleasedCount over the mapped buttons, in the trigger's edge mode).
  uint8_t nextCount = 0;
  uint8_t prevCount = 0;
  // Single-shot sources for this frame (state flags / one-frame events).
  bool touchNext = false;
  bool touchPrev = false;
  bool tiltNext = false;
  bool tiltPrev = false;
  bool powerTurn = false;
  bool homeNextPage = false;
};

// Net pages to move this frame: positive = forward.
inline int net(const TriggerCounts& t) {
  int steps = static_cast<int>(t.nextCount) - static_cast<int>(t.prevCount);
  if (t.touchNext) ++steps;
  if (t.touchPrev) --steps;
  if (t.tiltNext) ++steps;
  if (t.tiltPrev) --steps;
  if (t.powerTurn) ++steps;
  if (t.homeNextPage) ++steps;
  return steps;
}

// Magnitude-carrying pendingManualTurn: a frame's net joins a jump already
// parked behind a busy render gate, saturated to int8_t range so repeated
// guarded frames cannot wrap.
inline int8_t accumulate(int8_t pending, int steps) {
  const int joined = static_cast<int>(pending) + steps;
  return static_cast<int8_t>(joined > 127 ? 127 : (joined < -127 ? -127 : joined));
}

// Render-gate decision for the manual turn (queued-input aggregation §3.2,
// audit #4): a frame that carries a pending magnitude while the guard window
// is OPEN applies the jump once, joined with that frame's fresh net; every
// other frame (nothing pending, or the guard is busy — a render in flight or
// inside the 200 ms kMinManualTurnGapMs gap) parks the frame's net INTO the
// pending running sum via accumulate().
enum class GateAction { Park, Apply };
inline GateAction pendingManualTurnGate(int8_t pending, bool turnGuardActive) {
  return (pending != 0 && !turnGuardActive) ? GateAction::Apply : GateAction::Park;
}

// Chapter-skip long-press amount (CHAPTER_SKIP long-press behavior): Next
// skips FORWARD, Prev backward — skipPages() ADDS the result to the current
// position (XtcReaderActivity::skipPages), so the sign is the whole piece of
// arithmetic that encodes direction. Pure helper so the dispatch ternary
// cannot flip silently (regression: ReaderActivity's aggregation dispatch
// turned a Next long-press into -10 pages).
inline int chapterSkipAmount(bool nextTriggered, int magnitude) {
  return nextTriggered ? magnitude : -magnitude;
}

}  // namespace turnnet
