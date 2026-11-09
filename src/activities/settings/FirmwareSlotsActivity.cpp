#include "FirmwareSlotsActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/DevOtaPolicy.h"
#include "network/OtaBootSwitch.h"
#include "util/MemoryClaimGateDevice.h"

namespace fui = freeink::ui;

namespace {
// §20: what the resolved list props give every VALUE string — the theme's
// smallText, bound to the uiScale small font on every board (makeUiTarget).
// The label runs in the same font, so one measure serves the whole row.
int measureUiSmall(const void* ctx, const char* text) {
  return static_cast<const GfxRenderer*>(ctx)->getTextWidth(UI_10_FONT_ID, text ? text : "");
}
}  // namespace

const esp_partition_t* FirmwareSlotsActivity::slotPartition(const int index) const {
  return index == 0 ? ota_slots::stableSlot() : ota_slots::devSlot();
}

freeink::ui::Insets FirmwareSlotsActivity::contentInsets() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  return fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                     static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                     static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                     static_cast<int16_t>(safe.x)};
}

int FirmwareSlotsActivity::listBandWidth() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // contentInsets() leaves the safe area horizontally untouched (left =
  // safe.x, right = screen - (safe.x + safe.width)), so the rows draw in
  // the safe band. ui::list then takes the theme's listInset and reserves
  // the scroll strip for nav-managed lists (its draw path: strip = width +
  // inset + 2 of air; the token listScrollInset is pinned to 0).
  int band = safe.width - static_cast<int>(metrics.listInset) * 2;
  const int needed = static_cast<int>(metrics.listScrollWidth) + 2;
  if (metrics.listInset < needed) band -= needed - metrics.listInset;
  return band < 0 ? 0 : band;
}

int FirmwareSlotsActivity::valueSlotWidth(const int index) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = UITheme::getInstance();
  // Same resolved row geometry the list draws with: side padding inside the
  // band, then the row's own label (small text, single line) and the value
  // text gap (ListProps::textGap left at its 10 default). Value inset stays
  // 0 — this screen sets none.
  int width = listBandWidth() - static_cast<int>(metrics.listSidePadding) * 2;
  const char* label = rowItems[index].label;
  width -= renderer.getTextWidth(UI_10_FONT_ID, label ? label : "");
  width -= 10;
  return width < 0 ? 0 : width;
}

void FirmwareSlotsActivity::onEnter() {
  UiListActivity::onEnter();

  rowItems[0].label = tr(STR_SLOT_STABLE);
  rowItems[1].label = tr(STR_SLOT_DEV);
  const esp_partition_t* running = ota_slots::runningSlot();

  // §20 identity (owner report: the dev row printed the raw esp_app_desc
  // string, garbling the row, instead of the picker's "pr <num> <sha>"):
  // the slot's stamped version cross-references the CURRENT dev manifest.
  // The slots screen never owns the radio — no WifiSelectionActivity, no
  // silent restart on exit — so the fetch runs only when WiFi is already
  // up; otherwise the stamped version itself stays the build identity (its
  // trailing sha is what the manifest PR names carry). Fail-safe both
  // ways: a failed fetch leaves devEntries_ empty and the label falls
  // through to the stamp verbatim.
  ota_slots::SlotInfo probe;
  if (WiFi.status() == WL_CONNECTED && ota_slots::readSlotInfo(slotPartition(1), probe) &&
      dev_ota::parseDevStamp(probe.version, nullptr)) {
    memclaim::DeviceClaim claim(renderer, "SLOTS");
    claim.runCheap();
    static_cast<void>(dev_ota::fetchDevManifest(devEntries_.items, dev_ota::kMaxVersions, &devEntries_.count));
  }
  if (devEntries_.count <= 0) devEntries_ = {};

  for (int i = 0; i < kSlotCount; ++i) {
    ota_slots::SlotInfo info;
    valueText[i][0] = '\0';
    const bool isRunning = ota_slots::sameSlot(slotPartition(i), running);
    char suffix[32]{};
    if (isRunning) snprintf(suffix, sizeof(suffix), " · %s", tr(STR_SLOT_RUNNING));
    if (ota_slots::readSlotInfo(slotPartition(i), info)) {
      // The identity is composed to fit the value slot TOGETHER with the
      // suffix (the running marker) — slotRowValue reserves its width, the
      // append here is the composition's second half.
      int prNum = -1;
      dev_ota::slotRowValue(info.version, devEntries_.items, devEntries_.count, measureUiSmall, &renderer,
                            valueSlotWidth(i), suffix, prNum, valueText[i], sizeof(valueText[i]));
      strlcat(valueText[i], suffix, sizeof(valueText[i]));
      if (i == 1) {
        LOG_INF("SLOTS", "dev slot identity '%s' (pr=%d, manifest rows=%d)", valueText[i], prNum, devEntries_.count);
      }
    } else {
      snprintf(valueText[i], sizeof(valueText[i]), "%s", tr(STR_SLOT_EMPTY));
      strlcat(valueText[i], suffix, sizeof(valueText[i]));
    }
    rowItems[i].value = valueText[i];
    rowItems[i].actionValue = static_cast<int16_t>(i);
  }
}

const char* FirmwareSlotsActivity::headerTitle() const { return tr(STR_FIRMWARE_SLOTS); }

void FirmwareSlotsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(contentInsets());
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

bool FirmwareSlotsActivity::handleCustomInput() {
  // The confirm popup must see input before the list underneath (same pattern
  // as OpdsServerListActivity), otherwise Confirm re-opens the popup and the
  // boot callback never fires.
  return bootPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void FirmwareSlotsActivity::bootSlot(const int index) {
  const esp_partition_t* dest = slotPartition(index);
  ota_slots::SlotInfo info;
  // Re-checked here (not just when the list was built): the slot's contents
  // may have changed since onEnter, and otadata must not point at erased
  // flash with no valid app image.
  if (!dest || !ota_slots::readSlotInfo(dest, info)) {
    // No second OTA slot (single-slot boards) or no bootable image: the row
    // shows empty and must not offer a boot action.
    LOG_ERR("SLOTS", "boot requested for missing/empty slot %d", index);
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

  const esp_partition_t* dest = slotPartition(index);
  ota_slots::SlotInfo info;
  // No partition, no valid image, or already booted: no boot action. The row
  // value already says "empty" or "running" — booting an erased slot would
  // drop the device into the bootloader's fallback, and rebooting the running
  // slot accomplishes nothing.
  if (!dest || !ota_slots::readSlotInfo(dest, info) || ota_slots::sameSlot(dest, ota_slots::runningSlot())) {
    return;
  }

  // Confirm before the sticky otadata flip: a misclick on a 2-row list must
  // not reboot the device.
  const char* options[] = {tr(STR_CANCEL), tr(STR_CONFIRM)};
  bootPopup.show(tr(STR_SLOT_BOOT_PROMPT), options, 2, 0, [this, index](const int idx) {
    if (idx == 1) bootSlot(index);
  });
  requestUpdate();
}
