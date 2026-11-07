#pragma once

#include <GfxRenderer.h>
#include <I18n.h>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "network/OtaSlots.h"

class MappedInputManager;

// Firmware slot chooser (design §3, ships in EVERY build). Lists the two OTA
// slots with the version read from each image's esp_app_desc_t, marks the
// running slot, and boots the chosen slot via ota_boot::switchTo + reboot —
// no reflash, the choice lives in otadata only.
class FirmwareSlotsActivity final : public UiListActivity {
 public:
  explicit FirmwareSlotsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("FirmwareSlots", renderer, mappedInput) {}

  void onEnter() override;

 private:
  static constexpr int kSlotCount = 2;  // app0 (stable) + app1 (dev)

  int listCount() const override { return kSlotCount; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void render(RenderLock&& lock) override;
  const char* headerTitle() const override;

  const esp_partition_t* slotPartition(int index) const;
  void bootSlot(int index);

  freeink::ui::ListItem rowItems[kSlotCount]{};
  // Display strings must outlive the row items that point into them; one
  // buffer per row, rebuilt in onEnter().
  char valueText[kSlotCount][48]{};
  OptionPopup<> bootPopup;
};
