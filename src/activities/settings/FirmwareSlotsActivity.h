#pragma once

#include <GfxRenderer.h>
#include <I18n.h>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "network/DevOtaClient.h"  // EntryBuffer: the manifest the picker fetched
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
  bool handleCustomInput() override;
  const char* headerTitle() const override;

  const esp_partition_t* slotPartition(int index) const;
  void bootSlot(int index);

  // §20: the content-band geometry buildScreen() renders with, shared by
  // the row-value composition so a composed value cannot overprint the
  // label or spill the row edge. The band width is what ui::list gives the
  // rowArea (content band, theme listInset, nav-reserved scroll strip).
  freeink::ui::Insets contentInsets() const;
  int listBandWidth() const;
  // Pixel width the row's value text may use: band minus the resolved row
  // side padding, the row's measured label and the value text gap.
  int valueSlotWidth(int index) const;

  freeink::ui::ListItem rowItems[kSlotCount]{};
  // Display strings must outlive the row items that point into them; one
  // buffer per row, rebuilt in onEnter(). Sized for the longest real value —
  // a full 31-byte dev stamp + the running marker + NUL has to fit.
  char valueText[kSlotCount][64]{};
  // §20 cross-reference: the dev manifest's PR entries turn a dev slot's
  // stamped version into the picker's own "pr <num> <sha>" badge. Held so
  // the fetch happens once per screen entry; zeroed when the fetch is not
  // attempted (radio down, no dev stamp — the stamp itself is the identity).
  dev_ota::EntryBuffer devEntries_{};
  OptionPopup<> bootPopup;
};
