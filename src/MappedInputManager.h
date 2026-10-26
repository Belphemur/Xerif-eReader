#pragma once

#include <HalGPIO.h>

#include "util/HomeButtonInput.h"
#include "util/PowerClickWindow.h"
#include "util/RemoteButtonInput.h"
#include "util/RepeatHoldDiscount.h"

class GfxRenderer;
namespace freeink {
namespace ui {
enum class ScreenEdge : uint8_t;
}
}  // namespace freeink

class MappedInputManager {
 public:
  enum class Button {
    Back,
    Confirm,
    Left,
    Right,
    Up,
    Down,
    Power,
    PageBack,
    PageForward,
    NavNext,
    NavPrevious,
    ScreenLeft,
    ScreenRight,
    ScreenUp,
    ScreenDown
  };
  enum class SwipeDir { None, Left, Right, Up, Down };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  MappedInputManager(HalGPIO& gpio, const GfxRenderer& renderer) : gpio(gpio), renderer(renderer) {}

  // True when auto-repeat hold time must discount render stalls. Boards with
  // PSRAM run the 10 ms async input poll task (lib/hal/HalGPIO.cpp:159), which
  // samples through a blocking e-ink refresh and stamps the press at real
  // press time, so their held time already tracks the physical hold and this
  // stays off to leave them byte-identical. The non-PSRAM boards (X3/X4, C3)
  // have no poll task: update() runs on the loop task, a refresh blocks it for
  // 1-2 s, and wall-clock held time then reads as a deliberate hold — the
  // first tick after the render answers the same press a second time.
  static constexpr bool kRepeatHoldExcludesStalls() {
#if defined(BOARD_HAS_PSRAM)
    return false;
#else
    return true;
#endif
  }
  // One inter-tick gap above this counts as a render stall (X3/X4 double
  // input): generous against a normal 10-50 ms tick so ordinary jitter is
  // never charged to the hold window.
  static constexpr unsigned long kFrameStallThresholdMs = repeathold::kStallThresholdMs;

  // Blocking transfer loops pump physical input themselves. Defer configured
  // Home-key actions so the next main-loop pass can dispatch them, while the
  // current action remains available for immediate Home cancellation.
  void update(bool deferHomeButtonAction = false) const;
  // True exactly once when the frontlight double-click window resolved with a
  // second click (soak-fix7 JFhK): the main loop toggles the frontlight on
  // this and must not see the swallowed releases.
  bool consumePowerDoubleClick();
  // X4 Pro frontlight double-click window owner (update() calls it).
  // Constants live in util/PowerClickWindow.h (the pure policy this adapts).
  static constexpr unsigned long kPowerDoubleClickWindowMs = PowerClickWindow::kDoubleClickWindowMs;
  static constexpr unsigned long kPowerClickMaxHoldMs = PowerClickWindow::kClickMaxHoldMs;
  // Drops any open frontlight click window and discards its held release —
  // the screenshot combo's Power release must not resolve as a short-power
  // click (main.cpp combo handler calls this when the combo ends staggered).
  void cancelPowerClickWindow() const { powerClickWindowState.open = false; }
  void resolvePowerDoubleClickWindow(uint8_t newReleasedEdges) const;
  // Re-serves ONE synthesized Confirm PRESS edge for the PWR_CONFIRM power
  // click, the tick after its release was served (kody 6O2u): the click is
  // release-driven, but press-driven Confirm consumers (keyboard-entry
  // confirmHeld, auto-connect, installer guards) need an activation edge
  // too. Consumed by the first wasPressed(Confirm) read.
  bool consumePowerConfirmPress() const;
  // True while an ambiguous first click is parked in the frontlight
  // double-click window (main.cpp's sleep-on-release + power-off guards read
  // this instead of the old file-scope click state).
  bool isPowerClickWindowPending() const { return powerClickWindowState.open; }
  // True while a power press in progress is still a double-click candidate
  // (hold not yet past the click window): suppresses button-down power-off.
  bool isPowerClickHoldCandidate() const;
  bool injectRemoteClick(RemoteButton button);
  bool hasRemoteClick() const { return remoteInput.button != RemoteButton::None; }
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  // One-shot threshold event while the button is down; consumes its release.
  bool wasLongPressed(Button button, unsigned long thresholdMs) const;
  bool consumeSuppressedRelease() const;
  bool isPressed(Button button) const;
  bool hasTouch() const;
  bool wasScreenTapped(int& x, int& y) const;
  // Immediate press edge for drag handles, without the row-selection dwell.
  bool wasScreenTouchPressed(int& x, int& y) const;
  bool wasScreenTouchDown(int& x, int& y) const;
  // One-shot long-press from the SDK touch classifier, fired WHILE the finger
  // is still down (stationary contact held past the SDK threshold). Consuming
  // it suppresses the remainder of the contact — its continued hold and its
  // release edge — so the ensuing finger lift can't also tap-dismiss the popup
  // the long-press opened. The SDK owns that latch and self-clears it once the
  // contact ends.
  bool wasScreenLongPress(int& x, int& y) const;
  bool isScreenTouchHeld(int& x, int& y) const;
  // Raw release edge, also true when the contact ended in a swipe or drag-off
  // (which wasScreenTapped never reports). InputSnapshot builders forward it
  // off-target so FreeInkUI routing clears its pressed-element state.
  bool wasScreenTouchReleased() const;

  // Combined touch interaction for a band of equal rows with caller-supplied
  // geometry — the shared hit-test for lists the theme helpers above do not
  // cover (custom row heights, option prompts, menus). Down = a held
  // tap-candidate is on a row (update the selection highlight); Tap = a tap
  // released on one (activate). rowHeight limits the hit to the top rowHeight
  // px of each step (0 = the full step, no gap band).
  enum class RowTouch : uint8_t { None, Down, Tap };
  RowTouch rowTouch(int& row, int top, int rowStep, int rowCount, int xStart = 0, int xEnd = INT32_MAX,
                    int rowHeight = 0) const;
  // Horizontal variant for side-by-side button pairs (confirmation prompts).
  RowTouch colTouch(int& col, int left, int colStep, int colCount, int yStart, int yEnd, int colWidth = 0) const;

  SwipeDir wasSwipe(int* startX = nullptr, int* startY = nullptr) const;
  // Back = left-to-right swipe anchored at the left edge. Public so swipe-mode
  // page turns (reader) can exclude it from a plain SwipeDir::Right.
  bool wasBackGesture() const;
  // Home-key boards use a short Home-key tap to exit; their bottom-edge swipe
  // is intentionally unused. Other boards retain the bottom-edge Home gesture.
  // The reader menu remains on its existing top-edge gesture and middle tap.
  bool wasHomeGesture() const;
  // Configured one-frame action, independent of the gesture that triggered it.
  HomeButtonAction homeButtonAction() const { return homeAction; }
  // Which transition produced this frame's action; used by the fork invariant
  // that single taps are inert on the device home screen.
  HomeButtonGesture homeButtonGesture() const { return homeGesture; }
  void resetHomeButtonInput() const {
    homeButtonInput.reset();
    homeAction = HomeButtonAction::Ignore;
    homeGesture = HomeButtonGesture::None;
    deferredHomeAction = HomeButtonAction::Ignore;
    deferredHomeGesture = HomeButtonGesture::None;
  }
  bool wasMenuGesture() const;
  // Bottom-edge up-swipe as the reader-menu gesture (SHOW_READER_MENU's Swipe
  // Up option). Only meaningful on home-key boards, where Home lives on the
  // key and the bottom edge is free; elsewhere the same swipe is the Home
  // gesture and this returns false.
  bool wasReaderMenuSwipeUp() const;
  // Top-edge down-swipe opens the control center on touch boards.
  // ActivityManager consumes it before activity input.
  bool wasLightPanelGesture() const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  // Raw "user is interacting right now" signal for deferring loop-task
  // background layout: a pending press/release edge, an open button contact,
  // or an open touch contact. Non-const: rawInputActive() reads the ADC.
  bool rawInputPriority();
  unsigned long getHeldTime() const;
  // Hold time for AUTO-REPEAT only: getHeldTime() minus the render stalls
  // observed since the current press. Shares getHeldTime()'s Home-action and
  // touch-override preconditions; long-press detection and every other
  // consumer keep reading raw getHeldTime().
  unsigned long getRepeatHeldTime() const;
  const GfxRenderer& getRenderer() const { return renderer; }
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Maps four screen-direction labels onto the two physical front-button roles
  // using the same live-orientation transform as ScreenLeft/Right/Up/Down.
  Labels mapDirectionalLabels(const char* back, const char* confirm, const char* left, const char* right,
                              const char* up, const char* down) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

  // True when the control axis is flipped relative to the physical buttons: always on touch boards,
  // or when button-only boards opt in, while the screen is currently INVERTED / LANDSCAPE_CCW.
  [[nodiscard]] bool isNavDirectionSwapped() const;

 private:
  HalGPIO& gpio;
  // Logical-to-physical button mapping depends on what the user is actually looking at: when the
  // screen is rendered rotated, the directional buttons must flip to match. The renderer is the only
  // authority on the *live* orientation (the reader rotates it and restores portrait on exit), so we
  // read it here instead of CrossPointSettings.orientation, which is just the persisted reader
  // preference and stays "rotated" even while portrait UI like home/settings is on screen.
  const GfxRenderer& renderer;

  Button mapScreenDirection(Button button) const;
  Labels mapFrontLabels(const char* back, const char* confirm, const char* left, const char* right) const;
  // Resolves `button` to a physical button (0..6) and evaluates `probe`
  // there; composite logical buttons (Nav*, Screen*) recurse. Returns false
  // when the logical button is disabled (side buttons off).
  template <typename Probe>
  bool mapButtonWith(const Button button, Probe&& probe) const;
  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
  // This tick's edge read for `button`, from the snapshot taken in update()
  // (soak-fix7: consume-once at the snapshot, multi-read safe).
  bool edgeSnapshot(const Button button, const bool pressed) const;
  // SDK edge classification (fui::edgeSwipe) + the shared decode/held-time
  // bookkeeping; the wrappers below give each edge its board meaning.
  bool wasEdgeSwipe(freeink::ui::ScreenEdge edge) const;
  bool wasTopEdgeDownSwipe() const;
  bool wasBottomEdgeUpSwipe() const;
  // Fetch the pending swipe (if any) and map both endpoints to logical screen coords
  bool decodeSwipe(int& sx, int& sy, int& ex, int& ey) const;
#if FREEINK_CAP_TOUCH
  bool wasPowerConfirmClick() const;
#endif
  void rememberTouchHeldTime() const;
  void suppressNextRelease(Button button) const;

  mutable RemoteButtonInput remoteInput;
  mutable HomeButtonInput homeButtonInput;
  mutable HomeButtonAction homeAction = HomeButtonAction::Ignore;
  mutable HomeButtonGesture homeGesture = HomeButtonGesture::None;
  mutable HomeButtonAction deferredHomeAction = HomeButtonAction::Ignore;
  mutable HomeButtonGesture deferredHomeGesture = HomeButtonGesture::None;
  mutable bool touchHeldOverrideValid = false;
  mutable unsigned long touchHeldOverrideMs = 0;
  mutable unsigned long touchHeldOverrideAt = 0;
  // Render-stall accounting for getRepeatHeldTime() (non-PSRAM boards only,
  // see kRepeatHoldExcludesStalls). The window owns the per-dispatch state
  // machine — when a gap is a stall, and when a press edge opens a new hold
  // window — so it stays host-testable outside this class.
  mutable repeathold::StallDiscountWindow stallWindow;
  mutable uint16_t longPressFiredButtons = 0;
  mutable uint16_t suppressedReleaseButtons = 0;
  // This tick's physical button edges, taken ONCE in update() (soak-fix7:
  // consume-on-check at the snapshot — each edge consumed exactly once and
  // served to every activity read this tick). Bit i = physical BTN_i.
  // REBUILT from scratch every tick — never OR'd across ticks (kody 6Ot2/
  // 6Oyk): edges that must outlive a blocking transfer live in the
  // pendingPending/pendingRelease registers below, which compose into
  // exactly one frame and clear when they do.
  mutable uint8_t framePressedEdges = 0;
  mutable uint8_t frameReleasedEdges = 0;
  // Edges captured on a blocking-transfer pump (update(true)) whose
  // callbacks did not inspect them (kody 6Ot2/6Oyk): held here — OUT of the
  // live frame masks — and composed into exactly ONE later dispatch frame
  // (next normal update() ORs them in once, then both registers clear). A
  // pump callback CAN read an edge directly (Back/Home); an edge it read
  // from the live masks was consumed by that pump frame and must NOT ride
  // the pending registers — the pump subtracts what it consumed.
  mutable uint8_t pendingPressed = 0;
  mutable uint8_t pendingReleased = 0;
  // A physical release withheld from frameReleasedEdges this tick (armed
  // double-click candidate / carve-out) still counts as user activity —
  // otherwise the inactivity timer can expire during the 500 ms window
  // despite a real click (qodo Q2).
  mutable bool frameHiddenActivity = false;
#if FREEINK_CAP_TOUCH
  mutable bool powerConfirmClickFrame = false;
  // PWR_CONFIRM synthesized press (kody 6O2u, frame-scoped per kody
  // 8Y5e/8Y70): ARMED on the dispatch that delivers the Confirm verdict;
  // the next normal dispatch shifts armed into ACTIVE, which lives exactly
  // one dispatch frame and is consumed by the first wasPressed(Confirm)
  // read. Edge state never survives two dispatch boundaries.
  mutable bool powerConfirmPressActive = false;
  mutable bool powerConfirmPressArmed = false;
  // True while a blocking transfer's pump ticks run: verdicts raised
  // mid-transfer survive to the first post-transfer dispatch (which keeps
  // them for that frame); the pump-entry transition clears pre-transfer
  // verdict state (kody 7JYi).
  mutable bool pumpingDispatch = false;
#endif
  // X4 Pro frontlight double-click window (soak-fix7 JFhK): the FIRST short
  // power release is ambiguous (frontlight toggle vs configured short-power
  // action) and is held out of the served mask until the window resolves.
  // Open/closed is explicit — 0 is a legal millis() value (boot, timer wrap).
  mutable PowerClickWindow::WindowState powerClickWindowState;
  mutable bool powerDoubleClickFrame = false;
};
