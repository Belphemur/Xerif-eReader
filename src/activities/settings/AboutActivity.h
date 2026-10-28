#pragma once
#include <string>

#include "activities/UiListActivity.h"

// Read-only device information: detected hardware (device profile, display
// controller, touch, frontlight, RTC, IMU), firmware version, MAC address, and
// a heap snapshot (free, minimum watermark, largest block, PSRAM) taken when
// the screen opens — the numbers support asks for in bug reports.
// Rows are informational; activating one does nothing.
class AboutActivity final : public UiListActivity {
 public:
  explicit AboutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int ITEM_COUNT = 15;

  void onEnter() override;

 private:
  int listCount() const override { return ITEM_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int) override {}
  const char* headerTitle() const override;

  std::string rowValues_[ITEM_COUNT];
  freeink::ui::ListItem rowItems_[ITEM_COUNT]{};
};
