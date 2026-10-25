#pragma once

#include <FrontlightManager.h>

// Minimum brightness the slider/panel can set (0 turns the light off entirely
// via setOn(false); the slider clamps to this floor so dragging to the bottom
// edge turns the light off, not to a visible 0% level). Shared between
// FrontlightPanelActivity and the reader's side-swipe handler so both
// present a consistent 1% minimum.
constexpr uint8_t FRONTLIGHT_MIN_BRIGHTNESS = 1;

// Thin firmware HAL over the SDK frontlight manager. It is inert on boards
// without a frontlight, so callers do not need board-specific conditionals.
class HalFrontlight {
 public:
  static HalFrontlight& getInstance() { return instance; }

  void begin(uint8_t brightness, uint8_t warmth, bool on);

  // Both getters are pure pass-throughs over the SDK manager. On boards whose
  // build drops FREEINK_CAP_FRONTLIGHT the manager's implementations are
  // constant `false`, so cppcheck value-flow folds these to a constant and
  // flags knownConditionTrueFalse — intentional here (the HAL stays inert and
  // callers stay board-agnostic), not a condition worth rewriting.
  // cppcheck-suppress knownConditionTrueFalse
  bool present() const { return manager.present(); }
  // cppcheck-suppress knownConditionTrueFalse
  bool hasColorTemperature() const { return manager.hasColorTemperature(); }

  void setBrightness(uint8_t percent);
  void setWarmth(uint8_t warmPercent);
  void setOn(bool on);

  // Drive the frontlight pads LOW + hold them through deep sleep so they don't
  // leak current with the master rail held up (PR #3215). Inert on boards without
  // a frontlight. Paired with releaseOnWake(), which begin() calls at boot.
  // Guarded by FREEINK_FRONTLIGHT_LS to match FrontlightManager::park()
  // (otherwise boards without it fail to compile/link).
#ifdef FREEINK_FRONTLIGHT_LS
  void park() { manager.park(); }
  void releaseOnWake() { manager.releaseOnWake(); }
#endif

  uint8_t brightness() const { return lastBrightness; }
  uint8_t warmth() const { return manager.colorTemperature(); }
  bool isOn() const { return lit; }

 private:
  HalFrontlight() = default;

  FrontlightManager manager;
  // The SDK represents off as brightness 0. Keep the selected brightness so
  // toggling back on restores it.
  uint8_t lastBrightness = 60;
  bool lit = false;

  static HalFrontlight instance;
};

#define Frontlight HalFrontlight::getInstance()
