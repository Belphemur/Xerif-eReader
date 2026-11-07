#include "FirmwareSlotsActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"
#include "network/OtaBootSwitch.h"

namespace fui = freeink::ui;

const esp_partition_t* FirmwareSlotsActivity::slotPartition(const int index) const {
  return index == 0 ? ota_slots::stableSlot() : ota_slots::devSlot();
}

void FirmwareSlotsActivity::onEnter() {
  UiListActivity::onEnter();

  rowItems[0].label = tr(STR_SLOT_STABLE);
  rowItems[1].label = tr(STR_SLOT_DEV);
  const esp_partition_t* running = ota_slots::runningSlot();
  for (int i = 0; i < kSlotCount; ++i) {
    ota_slots::SlotInfo info;
    valueText[i][0] = '\0';
    if (ota_slots::readSlotInfo(slotPartition(i), info)) {
      snprintf(valueText[i], sizeof(valueText[i]), "%s", info.version);
    } else {
      snprintf(valueText[i], sizeof(valueText[i]), "%s", tr(STR_SLOT_EMPTY));
    }
    if (ota_slots::sameSlot(slotPartition(i), running)) {
      // The version string is at most 32 bytes, so the marker always fits.
      strlcat(valueText[i], " · ", sizeof(valueText[i]));
      strlcat(valueText[i], tr(STR_SLOT_RUNNING), sizeof(valueText[i]));
    }
    rowItems[i].value = valueText[i];
    rowItems[i].actionValue = static_cast<int16_t>(i);
  }
}

const char* FirmwareSlotsActivity::headerTitle() const { return tr(STR_FIRMWARE_SLOTS); }

void FirmwareSlotsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems;
  props.count = kSlotCount;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);
}

void FirmwareSlotsActivity::render(RenderLock&& lock) {
  if (bootPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

void FirmwareSlotsActivity::bootSlot(const int index) {
  const esp_partition_t* dest = slotPartition(index);
  if (!dest) {
    // No second OTA slot (single-slot boards): the dev row shows empty and
    // must not offer a boot action.
    LOG_ERR("SLOTS", "boot requested for missing slot %d", index);
    return;
  }
  LOG_INF("SLOTS", "switching boot target to %s", dest->label);
  if (!ota_boot::switchTo(dest)) {
    LOG_ERR("SLOTS", "otadata switch failed");
    return;
  }
  // otadata now points at `dest`; a plain restart boots it. No silent-restart
  // magic: the boot splash coming back is honest feedback that the slot
  // changed, and the freshly booted slot must run its own first-paint confirm.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void FirmwareSlotsActivity::activateIndex(const int index) {
  app.clearTapFlash();
  nav.selected = index;

  if (!slotPartition(index)) return;

  // Confirm before the sticky otadata flip: a misclick on a 2-row list must
  // not reboot the device.
  const char* options[] = {tr(STR_CANCEL), tr(STR_CONFIRM)};
  bootPopup.show(tr(STR_SLOT_BOOT_PROMPT), options, 2, 0, [this, index](const int idx) {
    if (idx == 1) bootSlot(index);
  });
  requestUpdate();
}
