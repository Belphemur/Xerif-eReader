"""Run with python3 test/test_remote_button_input.py (requires a host C++ compiler)."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/MappedInputManager.cpp").read_text()
# Exercise production frame/dispatch methods with physical input under test control.
methods = []
for signature, following in (
    ("void MappedInputManager::update(", "bool MappedInputManager::isNavDirectionSwapped("),
    ("bool MappedInputManager::injectRemoteClick(", "bool MappedInputManager::wasLongPressed("),
    ("bool MappedInputManager::isPressed(", "MappedInputManager::Labels MappedInputManager::mapLabels("),
    # Fork additions (async edge scan) called from update()/wasPressed: extract them too.
    ("bool MappedInputManager::edgeSnapshot(", "namespace {"),
    ("void MappedInputManager::resolvePowerDoubleClickWindow(", "bool MappedInputManager::isPowerClickHoldCandidate("),
):
    methods.append(signature + source.split(signature, 1)[1].split(following, 1)[0])

stub = r"""
#pragma once
#include <cstdint>
struct HalGPIO {
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_UP = 2;
  static constexpr uint8_t BTN_DOWN = 3;
  static constexpr uint8_t BTN_LEFT = 4;
  static constexpr uint8_t BTN_RIGHT = 5;
  static constexpr uint8_t BTN_POWER = 6;
  bool pressed = false, released = false, held = false;
  uint8_t button = 0;
  void update() {}
  // Queued-edge aggregation (docs/design/2026-10-27-queued-input-aggregation.md
  // §3.1): asyncMode models the SDK consume-on-check queue (each probe consumes
  // one queued edge); sync mode keeps the historical idempotent event bit.
  bool asyncMode = false;
  mutable uint8_t pendingPressCount[7] = {}, pendingReleaseCount[7] = {};
  bool asyncInputActive() const { return asyncMode; }
  bool wasAnyPressed() const { return asyncMode ? anyPending() : pressed; }
  bool wasAnyReleased() const { return asyncMode ? anyPending() : released; }
  bool anyPending() const {
    for (int b = 0; b < 7; ++b)
      if (pendingPressCount[b] > 0 || pendingReleaseCount[b] > 0) return true;
    return false;
  }
  void queuePress(uint8_t b) { if (pendingPressCount[b] < 255) ++pendingPressCount[b]; }
  void queueRelease(uint8_t b) { if (pendingReleaseCount[b] < 255) ++pendingReleaseCount[b]; }
  bool wasPressed(uint8_t b) const {
    if (asyncMode) {
      if (pendingPressCount[b] > 0) {
        --pendingPressCount[b];
        return true;
      }
      return false;
    }
    return b == button && pressed;
  }
  bool wasReleased(uint8_t b) const {
    if (asyncMode) {
      if (pendingReleaseCount[b] > 0) {
        --pendingReleaseCount[b];
        return true;
      }
      return false;
    }
    return b == button && released;
  }
  bool isPressed(uint8_t b) const { return b == button && held; }
  unsigned long getHeldTime() const { return 900; }
  bool wasCapacitivePagePressed() const { return false; }
  bool hasHomeKey() const { return false; }
  bool wasHomeKeyTapped() const { return false; }
  bool wasHomeKeyLongPressed() const { return false; }
  bool wasHomeKeyPressed() const { return false; }
  void consumeTouchFrame() {}
};
"""
harness = r"""
#include <cassert>
#include "MappedInputManager.h"
class GfxRenderer {};
unsigned long millis() { return 1000; }
constexpr unsigned long TOUCH_HELD_OVERRIDE_WINDOW_MS = 250;
struct CrossPointSettings {
  enum { VIBRATION_OFF, VIBRATION_TOUCH_PAGE };
  enum { PREV_NEXT, NEXT_PREV, PREV_PREV, NEXT_NEXT, SIDE_BUTTONS_DISABLED };
  int sideButtonLayout = PREV_NEXT;
  int frontButtonBack = 0, frontButtonConfirm = 1, frontButtonLeft = 2, frontButtonRight = 3;
  enum { IGNORE = 0, PWR_BOOKMARK = 7, PWR_SYNC = 8, PWR_READER_MENU = 9 };
  int shortPwrBtn = IGNORE;
  int vibration = VIBRATION_OFF, hapticIntensity = 0;
  int homeButtonTapAction = 0, homeButtonDoubleTapAction = 0, homeButtonLongPressAction = 0;
} SETTINGS;
struct HalHaptics {
  static void feedback(bool, bool, int) {}
  static void longPress(bool, int) {}
};
bool MappedInputManager::mapButton(Button b, bool (HalGPIO::*fn)(uint8_t) const) const {
  return (gpio.*fn)(static_cast<uint8_t>(b));
}
bool MappedInputManager::wasBackGesture() const { return false; }
bool MappedInputManager::isNavDirectionSwapped() const { return false; }
MappedInputManager::Button MappedInputManager::mapScreenDirection(MappedInputManager::Button b) const { return b; }
MappedInputManager::SwipeDir MappedInputManager::wasSwipe(int*, int*) const { return SwipeDir::None; }
""" + "\n".join(methods) + r"""
int main() {
  HalGPIO gpio;
  GfxRenderer renderer;
  MappedInputManager input(gpio, renderer);
  using Button = MappedInputManager::Button;
  struct Mapping { RemoteButton remote; Button button; Button other; };
  for (const auto mapping : {
      Mapping{RemoteButton::Confirm, Button::Confirm, Button::Back},
      Mapping{RemoteButton::Back, Button::Back, Button::Confirm},
      Mapping{RemoteButton::Next, Button::NavNext, Button::NavPrevious},
      Mapping{RemoteButton::Next, Button::Right, Button::Left},
      Mapping{RemoteButton::Previous, Button::NavPrevious, Button::NavNext},
      Mapping{RemoteButton::Previous, Button::Left, Button::Right}}) {
    const auto remote = mapping.remote;
    const auto button = mapping.button;
    const auto other = mapping.other;
    input.update();
    assert(input.injectRemoteClick(remote));
    assert(input.hasRemoteClick());
    assert(input.wasPressed(button) && input.wasReleased(button));
    assert(!input.wasPressed(other) && !input.wasReleased(other));
    assert(!input.isPressed(button) && input.getHeldTime() == 0);
    assert(!input.injectRemoteClick(remote));
    input.update();
    assert(!input.hasRemoteClick());
    assert(!input.wasPressed(button) && !input.wasReleased(button));
    assert(input.getHeldTime() == 900);
  }
  // Physical edges and held Confirm/Back must win over a simultaneous remote click.
  gpio.pressed = true;
  assert(!input.injectRemoteClick(RemoteButton::Confirm));
  gpio.pressed = false;
  gpio.released = true;
  assert(!input.injectRemoteClick(RemoteButton::Back));
  gpio.released = false;
  gpio.held = true;
  for (Button button : {Button::Confirm, Button::Back}) {
    gpio.button = static_cast<uint8_t>(button);
    assert(!input.injectRemoteClick(RemoteButton::Confirm));
    assert(!input.injectRemoteClick(RemoteButton::Back));
  }
  gpio.held = false;
  assert(input.injectRemoteClick(RemoteButton::Back));

  // Queued-edge aggregation against the PRODUCTION update() (ADR §6): three
  // queued presses of one button drain as wasPressedCount() == 3 in one
  // frame — and do NOT replay over the following frames.
  input.update();  // clears the remote click above
  gpio.asyncMode = true;
  for (int i = 0; i < 3; ++i) gpio.queuePress(static_cast<uint8_t>(Button::Right));
  input.update();
  assert(input.wasPressedCount(Button::Right) == 3);
  assert(input.wasPressed(Button::Right));
  assert(input.wasPressedCount(Button::Left) == 0);
  input.update();
  assert(input.wasPressedCount(Button::Right) == 0 && !input.wasPressed(Button::Right));
  // A held button queues exactly ONE press edge: the count stays 1 (§3.3).
  gpio.queuePress(static_cast<uint8_t>(Button::Right));
  input.update();
  assert(input.wasPressedCount(Button::Right) == 1 && input.wasPressed(Button::Right));
  // Release counts drain the same way (release-mode triggers).
  gpio.queueRelease(static_cast<uint8_t>(Button::Right));
  gpio.queueRelease(static_cast<uint8_t>(Button::Right));
  input.update();
  assert(input.wasReleasedCount(Button::Right) == 2 && input.wasReleased(Button::Right));
  assert(input.wasPressedCount(Button::Right) == 0);
}
"""
manager_source = (ROOT / "src/activities/ActivityManager.cpp").read_text()
remote_turn = "bool ActivityManager::remoteTurn(" + manager_source.split(
    "bool ActivityManager::remoteTurn(", 1
)[1].split("bool ActivityManager::handleForcedRefresh()", 1)[0]
harness = harness.replace("int main() {", r"""
#include <atomic>
#include <memory>
#include "activities/reader/RemoteTurnGate.h"
struct RenderLock { static bool peek() { return false; } };
struct ReaderActivity {
  std::atomic<bool> pageRendered{true};
  bool menu = false;
  int pages = 0, chapters = 0;
  bool inputOverPage() const { return menu; }
  bool pageTurn(bool forward) { pages += forward ? 1 : -1; return true; }
  bool skipPages(int step) { chapters += step; return true; }
};
struct ActivityManager {
  MappedInputManager& mappedInput;
  std::unique_ptr<ReaderActivity> currentActivity = std::make_unique<ReaderActivity>();
  bool context = true, foregroundReader = true;
  bool isRemoteReaderContext() const { return context; }
  bool isForegroundReader() const { return foregroundReader; }
  bool remoteClick(RemoteButton button) { return mappedInput.injectRemoteClick(button); }
  void requestUpdate() {}
  bool remoteTurn(bool forward, bool chapter);
};
""" + remote_turn + r"""
void checkRouting(MappedInputManager& input) {
  ActivityManager manager{input};
  input.update();
  assert(manager.remoteTurn(true, false));
  assert(manager.currentActivity->pages == 1 && !input.hasRemoteClick());
  for (bool reader : {true, false}) {
    manager.foregroundReader = reader;
    manager.currentActivity->menu = true;
    for (bool forward : {true, false}) {
      input.update();
      assert(manager.remoteTurn(forward, false));
      assert(input.wasPressed(forward ? MappedInputManager::Button::NavNext
                                     : MappedInputManager::Button::NavPrevious));
      assert(manager.currentActivity->pages == 1);
      assert(!manager.remoteTurn(forward, false));
    }
    input.update();
    assert(!manager.remoteTurn(true, true));
    assert(manager.currentActivity->chapters == 0 && !input.hasRemoteClick());
  }
  manager.context = false;
  assert(!manager.remoteTurn(true, false));
}
int main() {
""")
harness = harness.replace("  using Button =", "  checkRouting(input);\n  using Button =")
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    (directory / "HalGPIO.h").write_text(stub)
    cpp = directory / "remote_input.cpp"
    executable = directory / "remote_input"
    cpp.write_text("#include <initializer_list>\n" + harness)
    subprocess.run([
        "c++", "-std=c++20", "-DFREEINK_CAP_TOUCH=0", "-I", str(directory),
        "-I", str(ROOT / "src"), str(cpp), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
print("Remote button input regression checks passed")
