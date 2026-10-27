#include "MappedInputManager.h"

#include <BoardConfig.h>
#include <FreeInkUICore.h>
#include <GfxRenderer.h>
#include <HalHaptics.h>

#include <algorithm>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "components/HeaderBackTapTarget.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

void MappedInputManager::update(const bool deferHomeButtonAction) const {
  // Frame boundary history: develop's async-input lineage called
  // gpio.beginInputFrame() here to clear drain-latched edges; the pinned
  // SDK (#32 consume-on-check pop protocol) replaced the frame ack with a
  // documented no-op, and this branch's HalGPIO therefore exposes only
  // consumeTouchFrame(). Edges move exclusively through the per-read pop
  // consumed by the snapshot below.
  // Consume what the poll task produced (soak-fix7 consume-on-check):
  // gpio.update() is a no-op in async mode (edges move only through the
  // per-read pop); on sync builds update() keeps the historical one-shot
  // path and consumeTouchFrame() is a no-op. Then take the per-tick edge
  // snapshot — each physical button's edges consumed exactly once here,
  // served to every activity read this tick (multi-read safe).
  remoteInput.reset();
  gpio.update();
  gpio.consumeTouchFrame();
  // Render-stall sampling (X3/X4 double input): on non-PSRAM boards this
  // update() runs on the loop task, so the gap since the previous dispatch is
  // everything the loop did in between — a full e-ink refresh, a multi-second
  // build window. Charging that to hold time makes auto-repeat answer a still
  // held button a second time, so only the gap in excess of the threshold
  // accrues. An OPENING-CONTACT press edge — and only that — clears the total
  // later in update(); a second button pressed while the navigation button is
  // still down keeps its discount (the SDK's held clock is aggregate).
  if (kRepeatHoldExcludesStalls()) {
    stallWindow.sampleFrame(millis());
  }

  const bool asyncInput = gpio.asyncInputActive();
  uint8_t pressedEdges = 0;
  uint8_t releasedEdges = 0;
  uint8_t drainedPress[HalGPIO::BTN_POWER + 1] = {};
  uint8_t drainedRelease[HalGPIO::BTN_POWER + 1] = {};
  for (uint8_t physical = HalGPIO::BTN_BACK; physical <= HalGPIO::BTN_POWER; ++physical) {
    if (asyncInput) {
      // Queued-edge aggregation (§3.1): each probe consumes one queued edge,
      // so the drain loop recovers how many presses/releases the poll task
      // queued since the last dispatch. A held button queues exactly one
      // press edge, so a hold still counts 1 (§3.3). Sync builds keep the
      // historical single check (edge probes are idempotent within a frame
      // there — the event bit lives until gpio.update() clears it).
      drainedPress[physical] = edgecount::drainCount([this, physical] { return gpio.wasPressed(physical); });
      drainedRelease[physical] = edgecount::drainCount([this, physical] { return gpio.wasReleased(physical); });
    } else {
      drainedPress[physical] = gpio.wasPressed(physical) ? 1 : 0;
      drainedRelease[physical] = gpio.wasReleased(physical) ? 1 : 0;
    }
    if (drainedPress[physical]) pressedEdges |= static_cast<uint8_t>(1u << physical);
    if (drainedRelease[physical]) releasedEdges |= static_cast<uint8_t>(1u << physical);
  }
  if (deferHomeButtonAction) {
#if FREEINK_CAP_TOUCH
    // Entering a blocking transfer (kody 7JYi): verdicts raised/consumed by
    // THIS dispatch already delivered — clear them at the transition into
    // the pump so they cannot re-fire on the first post-transfer dispatch.
    // The synthesized press arm dies with them: edge state does not cross a
    // transfer boundary (kody 8Y5e/8Y70 rule 34). The !pumpingDispatch
    // guard makes this an entry-only clear: pump ticks after entry do NOT
    // clear — verdicts raised mid-transfer must survive to the dispatch
    // that can act on them (coderabbit 97X_ / kody 8-LvT).
    if (!pumpingDispatch) {
      powerConfirmClickFrame = false;
      powerDoubleClickFrame = false;
      powerConfirmPressActive = false;
      powerConfirmPressArmed = false;
    }
    pumpingDispatch = true;
#endif
    // Blocking-transfer pump (OpdsBookBrowserActivity, FontDownloadActivity,
    // CrossPointWebServerActivity). Pump frames serve THIS tick's fresh
    // edges in the live masks (kody 8Y_L): Back cancellation reads
    // wasPressed/wasReleased(Back) right after update(true), so the fresh
    // edges must be visible there. The callbacks' contract (Back/Home/touch
    // only) makes Back pump-owned: consumed by this pump frame or lost —
    // never parked, so no phantom Back re-delivery after the transfer.
    // Every other edge parks in the pending registers for the next
    // main-loop dispatch (qodo T1); the live masks are rebuilt from
    // scratch on the next tick, so nothing here persists in them. Power
    // releases are excluded here (coderabbit 97YL / kody 8-Lx1): they are
    // classified by resolvePowerDoubleClickWindow below, and the parked
    // outcome — serve keeps the bit, hold strips it — is parked AFTER that
    // classification, so pending never holds a raw Power release.
    constexpr uint8_t kBackEdge = static_cast<uint8_t>(1u << HalGPIO::BTN_BACK);
    constexpr uint8_t kPowerEdge = static_cast<uint8_t>(1u << HalGPIO::BTN_POWER);
    pendingPressed = static_cast<uint8_t>(pendingPressed | (pressedEdges & ~kBackEdge));
    pendingReleased = static_cast<uint8_t>(pendingReleased | (releasedEdges & ~(kBackEdge | kPowerEdge)));
    for (uint8_t physical = HalGPIO::BTN_BACK; physical <= HalGPIO::BTN_POWER; ++physical) {
      // Count halves of the same park (§3.1): Back is pump-consumed, Power
      // releases are parked after classification below.
      if (physical != HalGPIO::BTN_BACK) {
        pendingPressedCounts[physical] =
            static_cast<uint8_t>(pendingPressedCounts[physical] + drainedPress[physical]);
        if (physical != HalGPIO::BTN_POWER) {
          pendingReleasedCounts[physical] =
              static_cast<uint8_t>(pendingReleasedCounts[physical] + drainedRelease[physical]);
        }
      }
      framePressedCounts[physical] = drainedPress[physical];
      frameReleasedCounts[physical] = drainedRelease[physical];
    }
    framePressedEdges = pressedEdges;
    frameReleasedEdges = releasedEdges;
  } else {
#if FREEINK_CAP_TOUCH
    if (pumpingDispatch) {
      // First dispatch after a transfer: verdicts raised mid-transfer
      // deliver in THIS frame — keep them; the per-frame reset resumes
      // on the next dispatch.
      pumpingDispatch = false;
    } else {
      // Per-frame verdict reset (coderabbit 5r7l): the double-click /
      // Confirm verdicts are ONE dispatch frame's output — cleared with
      // the frame they were published into, so a verdict cannot outlive
      // its dispatch frame.
      powerConfirmClickFrame = false;
      powerDoubleClickFrame = false;
    }
    // Frame-scoped synthesized press shift (kody 8Y5e/8Y70): what the
    // previous dispatch armed becomes this frame's deliverable press edge;
    // the arm register clears so it cannot serve twice.
    powerConfirmPressActive = powerConfirmPressArmed;
    powerConfirmPressArmed = false;
#endif
    // Normal dispatch (kody 8ZCi): the pending edges compose into THIS
    // frame exactly once, BEFORE the Power window classifies below.
    // Parked release bits are post-classification outcomes (they were
    // parked only after resolvePowerDoubleClickWindow ran at their pump
    // tick), so they are deliverable as-is; the fresh releases classify
    // normally. Registers clear once composed.
    framePressedEdges = static_cast<uint8_t>(pressedEdges | pendingPressed);
    frameReleasedEdges = static_cast<uint8_t>(releasedEdges | pendingReleased);
    for (uint8_t physical = HalGPIO::BTN_BACK; physical <= HalGPIO::BTN_POWER; ++physical) {
      // Count halves of the same composition: parked counts merge with this
      // tick's drain exactly once, then the registers clear with the masks.
      framePressedCounts[physical] =
          static_cast<uint8_t>(drainedPress[physical] + pendingPressedCounts[physical]);
      frameReleasedCounts[physical] =
          static_cast<uint8_t>(drainedRelease[physical] + pendingReleasedCounts[physical]);
      pendingPressedCounts[physical] = 0;
      pendingReleasedCounts[physical] = 0;
    }
    pendingPressed = 0;
    pendingReleased = 0;
  }
  frameHiddenActivity = false;
  resolvePowerDoubleClickWindow(releasedEdges);
  if (deferHomeButtonAction) {
    // Park what classification left in the served mask (kody 8Y_ZCi): a
    // serve outcome keeps the Power bit (delivered post-transfer), a hold
    // stripped it from the mask, and a disabled window passes the raw
    // release through unclassified. Pending never holds an unclassified
    // Power release.
    pendingReleased = static_cast<uint8_t>(pendingReleased | (frameReleasedEdges & (1u << HalGPIO::BTN_POWER)));
    // Park the Power release count only when its bit actually survived
    // classification; a withheld (window-armed) release keeps its edge inside
    // the window and is re-published as a mask bit with count 0 later — no
    // count consumer reads Power, so the count drop is unobservable there.
    if ((frameReleasedEdges & (1u << HalGPIO::BTN_POWER)) != 0) {
      pendingReleasedCounts[HalGPIO::BTN_POWER] = static_cast<uint8_t>(
          pendingReleasedCounts[HalGPIO::BTN_POWER] + frameReleasedCounts[HalGPIO::BTN_POWER]);
    }
  }
  // Counts stay in lockstep with the served masks (audit #2): wherever the
  // mask machinery withholds a bit (Power click window above, suppressed
  // releases), the matching count is dropped with it, so wasPressed() ==
  // (wasPressedCount() > 0) holds for every physical button every frame.
  for (uint8_t physical = HalGPIO::BTN_BACK; physical <= HalGPIO::BTN_POWER; ++physical) {
    if ((framePressedEdges & (1u << physical)) == 0) framePressedCounts[physical] = 0;
    if ((frameReleasedEdges & (1u << physical)) == 0) frameReleasedCounts[physical] = 0;
  }
#if FREEINK_CAP_TOUCH
  // Arm the synthesized press the tick the Confirm verdict DELIVERS (kody
  // 6O2u's one-frame lag): normal dispatches only — pump frames deliver
  // nothing, and the arm is edge state that dies at the next shift. The
  // mode gate matches wasPowerConfirmClick()'s release side: tick() raises
  // confirmEdge for every expiring window (frontlight double-click users
  // included), but only a PWR_CONFIRM shortcut may synthesize a Confirm
  // press (kody -1HF).
  if (!pumpingDispatch && powerConfirmClickFrame &&
      SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PWR_CONFIRM) {
    powerConfirmPressArmed = true;
  }
#endif
  const bool pagePressed =
      SETTINGS.vibration == CrossPointSettings::VIBRATION_TOUCH_PAGE && gpio.wasCapacitivePagePressed();
  HalHaptics::feedback(SETTINGS.vibration != CrossPointSettings::VIBRATION_OFF, pagePressed, SETTINGS.hapticIntensity);
  homeAction = HomeButtonAction::Ignore;
  homeGesture = HomeButtonGesture::None;
  if (gpio.hasHomeKey()) {
    homeAction = homeButtonInput.update(millis(), gpio.wasHomeKeyTapped(), gpio.wasHomeKeyLongPressed(),
                                        wasSwipe() != SwipeDir::None, gpio.wasHomeKeyPressed(),
                                        static_cast<HomeButtonAction>(SETTINGS.homeButtonTapAction),
                                        static_cast<HomeButtonAction>(SETTINGS.homeButtonDoubleTapAction),
                                        static_cast<HomeButtonAction>(SETTINGS.homeButtonLongPressAction));
    homeGesture = homeButtonInput.lastGesture();
    if (gpio.wasHomeKeyLongPressed() && homeAction != HomeButtonAction::Ignore) {
      HalHaptics::longPress(SETTINGS.vibration != CrossPointSettings::VIBRATION_OFF, SETTINGS.hapticIntensity);
    } else if (homeAction != HomeButtonAction::Ignore) {
      HalHaptics::feedback(SETTINGS.vibration != CrossPointSettings::VIBRATION_OFF, true, SETTINGS.hapticIntensity);
    }
  }
  // Touch boards without a home key: a short power click can carry a reader
  // shortcut (see SHORT_PWRBTN).
  if (homeAction == HomeButtonAction::Ignore && mapButton(Button::Power, &HalGPIO::wasReleased)) {
    switch (SETTINGS.shortPwrBtn) {
      case CrossPointSettings::PWR_BOOKMARK:
        homeAction = HomeButtonAction::Bookmark;
        break;
      case CrossPointSettings::PWR_SYNC:
        homeAction = HomeButtonAction::Sync;
        break;
      case CrossPointSettings::PWR_READER_MENU:
        homeAction = HomeButtonAction::ReaderMenu;
        break;
      default:
        break;
    }
  }
  if (deferHomeButtonAction) {
    // Keep the first action observed during a synchronous transfer. Home must
    // still be visible now so the transfer can cancel and unwind promptly.
    if (homeAction != HomeButtonAction::Ignore && deferredHomeAction == HomeButtonAction::Ignore) {
      deferredHomeAction = homeAction;
      deferredHomeGesture = homeGesture;
    }
  } else if (deferredHomeAction != HomeButtonAction::Ignore) {
    // A fresh gesture is newer than anything captured during a blocking
    // transfer; otherwise replaying stale input could dispatch actions out of
    // order. Keep only the fresh action and drop the stale latch.
    if (homeAction != HomeButtonAction::Ignore) {
      deferredHomeAction = HomeButtonAction::Ignore;
      deferredHomeGesture = HomeButtonGesture::None;
    } else {
      homeAction = deferredHomeAction;
      homeGesture = deferredHomeGesture;
      deferredHomeAction = HomeButtonAction::Ignore;
      deferredHomeGesture = HomeButtonGesture::None;
    }
  }
  uint8_t heldButtons = 0;
  for (uint8_t value = 0; value <= static_cast<uint8_t>(Button::ScreenDown); ++value) {
    const bool held = isPressed(static_cast<Button>(value));
    if (!held) longPressFiredButtons &= ~(1u << value);
    if (held) heldButtons |= static_cast<uint8_t>(1u << value);
  }
  // A press edge that STARTS a contact opens a clean hold window: the stalls
  // before it belong to earlier contacts. A second button pressed while the
  // navigation button is still held keeps its discount (the SDK's held clock
  // is aggregate), as does an edge parked across a blocking transfer.
  if (kRepeatHoldExcludesStalls()) {
    stallWindow.deliverFrame(framePressedEdges != 0, heldButtons != 0);
  }
}

bool MappedInputManager::isNavDirectionSwapped() const {
  // Touch boards always follow the rendered orientation; button-only boards keep the user toggle.
  // Home and settings render in portrait, so neither path swaps them.
  const auto orientation = renderer.getOrientation();
  return (gpio.hasTouch() || SETTINGS.frontButtonFollowOrientation) &&
         (orientation == GfxRenderer::PortraitInverted || orientation == GfxRenderer::LandscapeCounterClockwise);
}

MappedInputManager::Button MappedInputManager::mapScreenDirection(const Button button) const {
  // Rows follow GfxRenderer::Orientation's declared order.
  static constexpr Button directions[][4] = {
      {Button::Left, Button::Right, Button::Up, Button::Down},
      {Button::Down, Button::Up, Button::Left, Button::Right},
      {Button::Right, Button::Left, Button::Down, Button::Up},
      {Button::Up, Button::Down, Button::Right, Button::Left},
  };

  uint8_t direction = 0;
  switch (button) {
    case Button::ScreenLeft:
      direction = 0;
      break;
    case Button::ScreenRight:
      direction = 1;
      break;
    case Button::ScreenUp:
      direction = 2;
      break;
    case Button::ScreenDown:
      direction = 3;
      break;
    default:
      return button;
  }

  const uint8_t orientation =
      SETTINGS.frontButtonFollowOrientation ? static_cast<uint8_t>(renderer.getOrientation()) : 0;
  return directions[orientation][direction];
}

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  return mapButtonWith(button, [this, fn](const uint8_t physical) { return (gpio.*fn)(physical); });
}

bool MappedInputManager::edgeSnapshot(const Button button, const bool pressed) const {
  const uint8_t edges = pressed ? framePressedEdges : frameReleasedEdges;
  return mapButtonWith(button, [edges](const uint8_t physical) { return (edges & (1u << physical)) != 0; });
}

uint8_t MappedInputManager::edgeCountSnapshot(const Button button, const bool pressed) const {
  const uint8_t* counts = pressed ? framePressedCounts : frameReleasedCounts;
  // Pure fold over every physical the logical button maps to: the probe never
  // short-circuits (always false), so composite mappings (PREV_PREV, Nav*)
  // visit all their physicals and the counts sum.
  uint32_t sum = 0;
  mapButtonWith(button, [counts, &sum](const uint8_t physical) {
    sum += counts[physical];
    return false;
  });
  return static_cast<uint8_t>(sum);
}

uint8_t MappedInputManager::wasPressedCount(const Button button) const {
  uint8_t count = edgeCountSnapshot(button, true);
  // Single-shot sources (not queued edges) count as the one click they are,
  // so wasPressedCount mirrors wasPressed for every button (ADR §3.1).
  // Deliberately read-only: consumePowerConfirmPress() must stay available to
  // the boolean read.
  const bool singleShot =
      (button == Button::Confirm &&
       (remoteInput.button == RemoteButton::Confirm || homeAction == HomeButtonAction::Confirm
#if FREEINK_CAP_TOUCH
        || powerConfirmPressActive
#endif
        )) ||
      (button == Button::Back && (remoteInput.button == RemoteButton::Back || wasBackGesture())) ||
      ((button == Button::NavNext || button == Button::Right) && remoteInput.button == RemoteButton::Next) ||
      ((button == Button::NavPrevious || button == Button::Left) && remoteInput.button == RemoteButton::Previous);
  if (singleShot && count == 0) count = 1;
  return count;
}

uint8_t MappedInputManager::wasReleasedCount(const Button button) const {
  uint8_t count = edgeCountSnapshot(button, false);
  // Same single-shot folds as wasReleased(), for release-mode triggers.
  const bool singleShot =
      (button == Button::Confirm &&
       (remoteInput.button == RemoteButton::Confirm || homeAction == HomeButtonAction::Confirm
#if FREEINK_CAP_TOUCH
        || wasPowerConfirmClick()
#endif
        )) ||
      (button == Button::Back && (remoteInput.button == RemoteButton::Back || wasBackGesture())) ||
      ((button == Button::NavNext || button == Button::Right) && remoteInput.button == RemoteButton::Next) ||
      ((button == Button::NavPrevious || button == Button::Left) && remoteInput.button == RemoteButton::Previous);
  if (singleShot && count == 0) count = 1;
  return count;
}

template <typename Probe>
bool MappedInputManager::mapButtonWith(const Button button, Probe&& probe) const {
  const auto sideLayout = SETTINGS.sideButtonLayout;

  switch (button) {
    case Button::Back:
      // Logical Back maps to user-configured front button.
      return probe(SETTINGS.frontButtonBack);
    case Button::Confirm:
      // Logical Confirm maps to user-configured front button.
      return probe(SETTINGS.frontButtonConfirm);
    case Button::Left:
      // Logical Left maps to user-configured front button.
      return probe(SETTINGS.frontButtonLeft);
    case Button::Right:
      // Logical Right maps to user-configured front button.
      return probe(SETTINGS.frontButtonRight);
    case Button::Up:
      // Side buttons remain fixed for Up/Down.
      return probe(HalGPIO::BTN_UP);
    case Button::Down:
      // Side buttons remain fixed for Up/Down.
      return probe(HalGPIO::BTN_DOWN);
    case Button::Power:
      // Power button bypasses remapping.
      return probe(HalGPIO::BTN_POWER);
    case Button::PageBack:
      // Reader page navigation uses side buttons and can be swapped via settings.
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
          return probe(isNavDirectionSwapped() ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP);
        case CrossPointSettings::NEXT_PREV:
          return probe(isNavDirectionSwapped() ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN);
        case CrossPointSettings::PREV_PREV:
          return probe(HalGPIO::BTN_UP) || probe(HalGPIO::BTN_DOWN);
        case CrossPointSettings::NEXT_NEXT:
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
    case Button::PageForward:
      // Reader page navigation uses side buttons and can be swapped via settings.
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
          return probe(isNavDirectionSwapped() ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN);
        case CrossPointSettings::NEXT_PREV:
          return probe(isNavDirectionSwapped() ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP);
        case CrossPointSettings::NEXT_NEXT:
          return probe(HalGPIO::BTN_UP) || probe(HalGPIO::BTN_DOWN);
        case CrossPointSettings::PREV_PREV:
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
    case Button::NavNext:
      // Logical "next item" navigation: side Down + front Right, with the control axis flipped in
      // INVERTED / LANDSCAPE_CCW under the live orientation policy, matching the rotated hint labels.
      return isNavDirectionSwapped() ? (mapButtonWith(Button::Up, probe) || mapButtonWith(Button::Left, probe))
                                     : (mapButtonWith(Button::Down, probe) || mapButtonWith(Button::Right, probe));
    case Button::NavPrevious:
      // Logical "previous item" navigation: side Up + front Left, axis-flipped in the same orientations.
      return isNavDirectionSwapped() ? (mapButtonWith(Button::Down, probe) || mapButtonWith(Button::Right, probe))
                                     : (mapButtonWith(Button::Up, probe) || mapButtonWith(Button::Left, probe));
    case Button::ScreenLeft:
    case Button::ScreenRight:
    case Button::ScreenUp:
    case Button::ScreenDown:
      return mapButtonWith(mapScreenDirection(button), probe);
  }

  return false;
}

namespace {
constexpr unsigned long TOUCH_DOWN_SELECT_DELAY_MS = 90;
constexpr unsigned long TOUCH_HELD_OVERRIDE_WINDOW_MS = 250;
}  // namespace

bool MappedInputManager::hasTouch() const { return gpio.hasTouch(); }

bool MappedInputManager::rawInputPriority() {
  // Snapshot masks, not gpio.wasAny*: in async mode the SDK's pending
  // counts are empty once update()'s snapshot consumed the edges (report-
  // only leftovers), so the button component must read this tick's masks.
  return framePressedEdges != 0 || frameReleasedEdges != 0 || gpio.wasTouchActivity() || gpio.isTouchContactActive() ||
         gpio.rawInputActive();
}

void MappedInputManager::rememberTouchHeldTime() const {
  touchHeldOverrideValid = true;
  touchHeldOverrideMs = gpio.lastTouchHeldMs();
  touchHeldOverrideAt = millis();
}

bool MappedInputManager::wasScreenTapped(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  int tapX = 0;
  int tapY = 0;
  renderer.tapToLogical(nx, ny, tapX, tapY);
  // A tap on the header back button is Button::Back (wasBackGesture), not a
  // screen tap: screens that route every tap (the keyboard's key router)
  // would otherwise swallow it before their Back check.
  if (HeaderBackTapTarget::contains(tapX, tapY)) return false;
  x = tapX;
  y = tapY;
  rememberTouchHeldTime();
  return true;
}

bool MappedInputManager::wasScreenTouchPressed(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchDown(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenTouchDown(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  unsigned long heldMs = 0;
  if (!gpio.isTouchTapCandidate(nx, ny, heldMs)) return false;
  if (heldMs < TOUCH_DOWN_SELECT_DELAY_MS) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenLongPress(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchLongPress(nx, ny)) return false;
  // Consuming the long-press implies acting on it: suppress the rest of the
  // contact so the finger lift can't also tap whatever the action opened.
  gpio.suppressTouchContact();
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::isScreenTouchHeld(int& x, int& y) const {
  // Live contact position while the finger is down (no tap-slop gate) — drag tracking.
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.isTouchHeldAt(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenTouchReleased() const { return gpio.wasTouchReleased(); }

MappedInputManager::RowTouch MappedInputManager::rowTouch(int& row, const int top, const int rowStep,
                                                          const int rowCount, const int xStart, const int xEnd,
                                                          const int rowHeight) const {
  if (rowStep <= 0 || rowCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (x < xStart || x >= xEnd || y < top) return false;
    const int r = (y - top) / rowStep;
    if (r >= rowCount) return false;
    if (rowHeight > 0 && (y - top) % rowStep >= rowHeight) return false;
    row = r;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

MappedInputManager::RowTouch MappedInputManager::colTouch(int& col, const int left, const int colStep,
                                                          const int colCount, const int yStart, const int yEnd,
                                                          const int colWidth) const {
  if (colStep <= 0 || colCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (y < yStart || y >= yEnd || x < left) return false;
    const int c = (x - left) / colStep;
    if (c >= colCount) return false;
    if (colWidth > 0 && (x - left) % colStep >= colWidth) return false;
    col = c;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

bool MappedInputManager::decodeSwipe(int& sx, int& sy, int& ex, int& ey) const {
  float nxs = 0.0f;
  float nys = 0.0f;
  float nxe = 0.0f;
  float nye = 0.0f;
  if (!gpio.wasSwipe(nxs, nys, nxe, nye)) return false;
  renderer.tapToLogical(nxs, nys, sx, sy);
  renderer.tapToLogical(nxe, nye, ex, ey);
  return true;
}

MappedInputManager::SwipeDir MappedInputManager::wasSwipe(int* startX, int* startY) const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return SwipeDir::None;
  if (startX) *startX = sx;
  if (startY) *startY = sy;
  switch (fui::swipeDirection(sx, sy, ex, ey)) {
    case fui::SwipeDir::Left:
      return SwipeDir::Left;
    case fui::SwipeDir::Right:
      return SwipeDir::Right;
    case fui::SwipeDir::Up:
      return SwipeDir::Up;
    case fui::SwipeDir::Down:
      return SwipeDir::Down;
    default:
      return SwipeDir::None;
  }
}

// Edge classification (which swipe counts as an edge gesture) lives in the
// SDK; only the MEANING of each edge — back, menu, home, light panel, and the
// home-key remap — is decided here.
bool MappedInputManager::wasEdgeSwipe(const freeink::ui::ScreenEdge edge) const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const bool hit = fui::edgeSwipe(edge, sx, sy, ex, ey, renderer.getScreenWidth(), renderer.getScreenHeight());
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasBackGesture() const {
  // Tap on the header back button (rect recorded by BaseTheme::drawHeader;
  // empty on screens without one). Folded into Button::Back alongside the
  // swipe so every activity's existing Back handling picks it up.
  float nx = 0.0f;
  float ny = 0.0f;
  if (gpio.wasTouchTap(nx, ny)) {
    int tapX = 0;
    int tapY = 0;
    renderer.tapToLogical(nx, ny, tapX, tapY);
    if (HeaderBackTapTarget::contains(tapX, tapY)) {
      rememberTouchHeldTime();
      return true;
    }
  }
  // Back = left-to-right swipe starting near the left edge. Edge-anchored so that
  // mid-screen horizontal swipes stay available to activities that consume
  // SwipeDir::Left/Right (e.g. percent selection, image viewer).
  return wasEdgeSwipe(fui::ScreenEdge::Left);
}

bool MappedInputManager::wasTopEdgeDownSwipe() const { return wasEdgeSwipe(fui::ScreenEdge::Top); }

bool MappedInputManager::wasBottomEdgeUpSwipe() const { return wasEdgeSwipe(fui::ScreenEdge::Bottom); }

bool MappedInputManager::wasMenuGesture() const { return wasTopEdgeDownSwipe(); }

bool MappedInputManager::wasReaderMenuSwipeUp() const { return gpio.hasHomeKey() && wasBottomEdgeUpSwipe(); }

bool MappedInputManager::wasHomeGesture() const {
  return gpio.hasHomeKey() ? homeAction == HomeButtonAction::Home : wasBottomEdgeUpSwipe();
}

bool MappedInputManager::wasLightPanelGesture() const { return wasTopEdgeDownSwipe(); }

void MappedInputManager::resolvePowerDoubleClickWindow(const uint8_t newReleasedEdges) const {
#if FREEINK_CAP_TOUCH
  if (!BoardConfig::isX4Pro() || !SETTINGS.doubleClickPwrLight) return;
  const unsigned long now = millis();
  // Classify only NEWLY captured releases (coderabbit 5r7g): the frame mask
  // also carries published (deferred-expiry) bits across blocking pumps —
  // feeding those back would re-arm and re-defer the already-resolved
  // release forever. Publication below still works on the full mask.
  const bool physicalRelease = (newReleasedEdges & (1u << HalGPIO::BTN_POWER)) != 0;
  const bool comboRelease = physicalRelease && (newReleasedEdges & (1u << HalGPIO::BTN_DOWN)) != 0;
  // PWR_CONFIRM carve-out threshold; 0 disables the carve-out.
  const uint32_t confirmHoldMs =
      SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PWR_CONFIRM ? SETTINGS.getPowerButtonDuration() : 0;
  const auto result = PowerClickWindow::tick(powerClickWindowState, physicalRelease, comboRelease, now,
                                             gpio.getPowerButtonHeldTime(), confirmHoldMs);
  if (result.serveRelease) {
    // The bit carries the EXPIRED release (coderabbit 3dSJ): publish it even
    // when a new click re-armed — the new click's edge was consumed by this
    // tick's snapshot and lives only in the re-armed window, so publishing
    // cannot double-fire it.
    frameReleasedEdges |= static_cast<uint8_t>(1u << HalGPIO::BTN_POWER);
  } else if (physicalRelease) {
    // Held (armed) or consumed by a double-click: the release stays out of
    // the served mask.
    frameReleasedEdges &= static_cast<uint8_t>(~(1u << HalGPIO::BTN_POWER));
  }
  if (result.doubleClick) powerDoubleClickFrame = true;
  if (result.confirmEdge) powerConfirmClickFrame = true;
  // A real release withheld from the served mask (armed, re-armed, or
  // carve-out) still counts as activity (qodo Q2).
  if (physicalRelease && (frameReleasedEdges & (1u << HalGPIO::BTN_POWER)) == 0) {
    frameHiddenActivity = true;
  }
#endif
}

bool MappedInputManager::isPowerClickHoldCandidate() const {
  // A power press in progress still inside the click window could resolve as
  // a double-click candidate on release — suppress button-down power-off.
  return BoardConfig::isX4Pro() && SETTINGS.doubleClickPwrLight &&
         gpio.getPowerButtonHeldTime() <= kPowerClickMaxHoldMs;
}

bool MappedInputManager::consumePowerDoubleClick() {
  if (!powerDoubleClickFrame) return false;
  powerDoubleClickFrame = false;
  return true;
}

#if FREEINK_CAP_TOUCH
bool MappedInputManager::consumePowerConfirmPress() const {
  if (!powerConfirmPressActive) return false;
  powerConfirmPressActive = false;
  return true;
}

bool MappedInputManager::wasPowerConfirmClick() const {
  if (!gpio.hasTouch() || SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::PWR_CONFIRM) return false;
  // Wait out the X4 Pro's frontlight double-click window before treating its
  // first release as Confirm. With the shortcut disabled, and on other touch
  // boards, the release counts directly — from the SNAPSHOT (soak-fix7): a
  // direct gpio.wasReleased would find the edge already consumed by the
  // snapshot loop (double-read loses it) and always report false.
  if (BoardConfig::isX4Pro() && SETTINGS.doubleClickPwrLight) return powerConfirmClickFrame;
  return edgeSnapshot(Button::Power, /*pressed=*/false) &&
         gpio.getPowerButtonHeldTime() <= SETTINGS.getPowerButtonDuration();
}
#endif

bool MappedInputManager::injectRemoteClick(const RemoteButton button) {
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || isPressed(Button::Confirm) || isPressed(Button::Back))
    return false;
  return remoteInput.click(button);
}

bool MappedInputManager::wasPressed(const Button button) const {
  if (button == Button::Confirm && remoteInput.button == RemoteButton::Confirm) return true;
  if (button == Button::Back && remoteInput.button == RemoteButton::Back) return true;
  if ((button == Button::NavNext || button == Button::Right) && remoteInput.button == RemoteButton::Next) return true;
  if ((button == Button::NavPrevious || button == Button::Left) && remoteInput.button == RemoteButton::Previous)
    return true;
  if (button == Button::Confirm && homeAction == HomeButtonAction::Confirm) return true;
  if (button == Button::Back && wasBackGesture()) return true;
#if FREEINK_CAP_TOUCH
  // PWR_CONFIRM: the click is release-driven (qodo Q1 — one power release
  // must not surface as press AND release in the same tick), but the
  // synthesized activation PRESS is served on a later read so press-driven
  // Confirm consumers still work (kody 6O2u).
  if (button == Button::Confirm && consumePowerConfirmPress()) return true;
#endif
  return edgeSnapshot(button, true);
}

bool MappedInputManager::wasReleased(const Button button) const {
  if (button == Button::Confirm && remoteInput.button == RemoteButton::Confirm) return true;
  if (button == Button::Back && remoteInput.button == RemoteButton::Back) return true;
  if ((button == Button::NavNext || button == Button::Right) && remoteInput.button == RemoteButton::Next) return true;
  if ((button == Button::NavPrevious || button == Button::Left) && remoteInput.button == RemoteButton::Previous)
    return true;
  if (button == Button::Confirm && homeAction == HomeButtonAction::Confirm) return true;
  if (button == Button::Back && wasBackGesture()) return true;
#if FREEINK_CAP_TOUCH
  if (button == Button::Confirm && wasPowerConfirmClick()) return true;
#endif
  return edgeSnapshot(button, false);
}

bool MappedInputManager::wasLongPressed(const Button button, const unsigned long thresholdMs) const {
  if (!isPressed(button)) return false;
  const uint16_t bit = 1u << static_cast<uint8_t>(button);
  if ((longPressFiredButtons & bit) != 0 || getHeldTime() < thresholdMs) return false;
  longPressFiredButtons |= bit;
  suppressNextRelease(button);
  if (mapButton(button, &HalGPIO::isCapacitivePagePressed)) {
    HalHaptics::longPress(SETTINGS.vibration == CrossPointSettings::VIBRATION_TOUCH_PAGE, SETTINGS.hapticIntensity);
  }
  return true;
}

void MappedInputManager::suppressNextRelease(const Button button) const {
  suppressedReleaseButtons |= 1u << static_cast<uint8_t>(button);
}

bool MappedInputManager::consumeSuppressedRelease() const {
  uint16_t released = 0;
  for (uint8_t value = 0; value <= static_cast<uint8_t>(Button::ScreenDown); ++value) {
    const uint16_t bit = 1u << value;
    if ((suppressedReleaseButtons & bit) != 0 && edgeSnapshot(static_cast<Button>(value), false)) {
      released |= bit;
      // Exactly-once (audit F1): also clear the PHYSICAL edge bits the
      // logical button maps to, so the suppressed release cannot also fire
      // wasReleased()/wasAnyReleased() later this tick. Composite logical
      // buttons (Nav*/Page*) map to several physical buttons — clear each.
      mapButtonWith(static_cast<Button>(value), [this](const uint8_t physical) {
        frameReleasedEdges &= static_cast<uint8_t>(~(1u << physical));
        // Count lockstep: a long-press hold's own release is suppressed, so
        // its drained count must not surface to a release-count consumer
        // (queued-input aggregation audit #2).
        frameReleasedCounts[physical] = 0;
        return false;
      });
    }
  }
  suppressedReleaseButtons &= ~released;
  return released != 0;
}

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasAnyPressed() const { return framePressedEdges != 0; }

bool MappedInputManager::wasAnyReleased() const { return frameReleasedEdges != 0 || frameHiddenActivity; }

unsigned long MappedInputManager::getHeldTime() const {
  // A mapped action has its own meaning, independent of the contact duration.
  if (hasRemoteClick() || homeAction != HomeButtonAction::Ignore) return 0;
  if (framePressedEdges == 0 && frameReleasedEdges == 0 && touchHeldOverrideValid &&
      millis() - touchHeldOverrideAt <= TOUCH_HELD_OVERRIDE_WINDOW_MS) {
    return touchHeldOverrideMs;
  }
  touchHeldOverrideValid = false;
  return gpio.getHeldTime();
}

unsigned long MappedInputManager::getRepeatHeldTime() const {
  const unsigned long held = getHeldTime();
  if (!kRepeatHoldExcludesStalls()) return held;
  return stallWindow.heldMs(held);
}

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Swap previous/next labels to match the page turn direction swap in INVERTED and LANDSCAPE_CCW.
  const bool swapLabels = isNavDirectionSwapped();
  const char* leftLabel = swapLabels ? next : previous;
  const char* rightLabel = swapLabels ? previous : next;

  return mapFrontLabels(back, confirm, leftLabel, rightLabel);
}

MappedInputManager::Labels MappedInputManager::mapDirectionalLabels(const char* back, const char* confirm,
                                                                    const char* left, const char* right, const char* up,
                                                                    const char* down) const {
  const auto labelForButton = [&](const Button rawButton) {
    if (mapScreenDirection(Button::ScreenLeft) == rawButton) return left;
    if (mapScreenDirection(Button::ScreenRight) == rawButton) return right;
    if (mapScreenDirection(Button::ScreenUp) == rawButton) return up;
    if (mapScreenDirection(Button::ScreenDown) == rawButton) return down;
    return "";
  };
  return mapFrontLabels(back, confirm, labelForButton(Button::Left), labelForButton(Button::Right));
}

MappedInputManager::Labels MappedInputManager::mapFrontLabels(const char* back, const char* confirm, const char* left,
                                                              const char* right) const {
  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    // Compare against configured logical roles and return the matching label.
    if (hw == SETTINGS.frontButtonBack) {
      return back;
    }
    if (hw == SETTINGS.frontButtonConfirm) {
      return confirm;
    }
    if (hw == SETTINGS.frontButtonLeft) {
      return left;
    }
    if (hw == SETTINGS.frontButtonRight) {
      return right;
    }
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  // Reads the per-tick snapshot (multi-read safe under consume-on-check).
  if ((framePressedEdges & (1u << HalGPIO::BTN_BACK)) != 0) {
    return HalGPIO::BTN_BACK;
  }
  if ((framePressedEdges & (1u << HalGPIO::BTN_CONFIRM)) != 0) {
    return HalGPIO::BTN_CONFIRM;
  }
  if ((framePressedEdges & (1u << HalGPIO::BTN_LEFT)) != 0) {
    return HalGPIO::BTN_LEFT;
  }
  if ((framePressedEdges & (1u << HalGPIO::BTN_RIGHT)) != 0) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}
