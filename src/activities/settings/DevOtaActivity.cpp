#include "DevOtaActivity.h"

#include <BatteryMonitor.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>

#include "HalPowerManager.h"
#include "MappedInputManager.h"
#include "OtaBootSwitch.h"
#include "OtaSlots.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "util/MemoryClaimGateDevice.h"

namespace {
// Same battery gate as the release OTA path (OtaUpdateActivity): a brownout
// mid-write can corrupt the written slot.
constexpr uint16_t DEV_OTA_MIN_BATTERY_PCT = 20;

// Display label for a dev asset name "xerif-dev-<sha>-x4pro.bin" → "dev <sha12>".
// Returns false when the name doesn't match the pattern; callers then show
// the raw asset name.
bool devNameLabel(const char* name, char* out, size_t cap) {
  constexpr char kPrefix[] = "xerif-dev-";
  const size_t prefixLen = sizeof(kPrefix) - 1;
  if (!name || strncmp(name, kPrefix, prefixLen) != 0) return false;
  const char* sha = name + prefixLen;
  const char* dash = strchr(sha, '-');
  if (!dash || dash == sha) return false;
  const size_t shaLen = static_cast<size_t>(dash - sha);
  const size_t shown = shaLen < 12 ? shaLen : 12;
  snprintf(out, cap, "dev %.*s", static_cast<int>(shown), sha);
  return true;
}

const char* describeDevError(const dev_ota::Error err) {
  switch (err) {
    case dev_ota::Error::SHA_MISMATCH:
      return tr(STR_DEV_HASH_MISMATCH);
    case dev_ota::Error::JSON_PARSE_ERROR:
    case dev_ota::Error::NO_MANIFEST:
    case dev_ota::Error::NO_VERSIONS:
      return tr(STR_OTA_BAD_RELEASE);
    default:
      return nullptr;
  }
}
}  // namespace

void DevOtaActivity::onEnter() {
  Activity::onEnter();

  // Same WiFi handshake as OtaUpdateActivity: launch the selection
  // subactivity; with saved credentials it auto-connects — the boot-hook
  // resume path depends on that, since nobody watches a picker at boot.
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void DevOtaActivity::onExit() {
  Activity::onExit();

  // The success path reboots via plain ESP.restart() from runInstall(); every
  // other exit lands here with WiFi still active — silent-restart to free the
  // LWIP/TLS fragmentation, same as the other WiFi activities.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void DevOtaActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    if (autoResume) {
      // The dance reboot expects this boot to finish the install; a WiFi
      // failure must not leave the flag armed for an every-boot retry loop.
      dev_ota::pendingInstallClear();
    }
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state = CHECKING;
  }
  requestUpdateAndWait();
  fetchManifest();
}

void DevOtaActivity::fetchManifest() {
  dev_ota::Error res;
  {
    memclaim::DeviceClaim claim(renderer, "DEVOTA");
    claim.runCheap();
    res = dev_ota::fetchDevManifest(entries.items, dev_ota::kMaxVersions, &entries.count);
  }

  if (res != dev_ota::Error::OK) {
    LOG_ERR("DEVOTA", "manifest fetch failed (%d)", static_cast<int>(res));
    if (autoResume) dev_ota::pendingInstallClear();
    {
      RenderLock lock(*this);
      failedDetail = describeDevError(res);
      state = FAILED;
    }
    requestUpdate();
    return;
  }

  if (autoResume) {
    // The dance's completion pass: install the version recorded in the flag,
    // or the newest one when the requested asset has since been pruned.
    int target = 0;
    if (pendingVersion[0] != '\0') {
      for (int i = 0; i < entries.count; ++i) {
        if (strcmp(entries.items[i].name, pendingVersion) == 0) {
          target = i;
          break;
        }
      }
    }
    beginInstall(target);
    return;
  }

  {
    RenderLock lock(*this);
    selectedIndex = 0;
    state = LIST;
  }
  requestUpdate();
}

void DevOtaActivity::promptInstall(const int index) {
  if (index < 0 || index >= entries.count) return;
  selectedIndex = index;

  char label[40];
  if (!devNameLabel(entries.items[index].name, label, sizeof(label))) {
    snprintf(label, sizeof(label), "%s", entries.items[index].name);
  }
  const char* options[] = {tr(STR_CANCEL), tr(STR_INSTALL)};
  confirmPopup.show(tr(STR_DEV_INSTALL_PROMPT), label, options, 2, 1, [this, index](const int idx) {
    if (idx == 1) {
      beginInstall(index);
    } else {
      state = LIST;
      requestUpdate();
    }
  });
  {
    RenderLock lock(*this);
    state = CONFIRMING;
  }
  requestUpdate();
}

void DevOtaActivity::beginInstall(const int index) {
  if (index < 0 || index >= entries.count) return;
  selectedIndex = index;

  // Running-dev dance (design §5): the running slot is never a write target,
  // so record the request and reboot into stable; the boot hook finishes the
  // install from there (two reboots total).
  if (ota_slots::sameSlot(ota_slots::runningSlot(), ota_slots::devSlot())) {
    if (!dev_ota::pendingInstallSet(entries.items[index].name) || !ota_boot::switchTo(ota_slots::stableSlot())) {
      // Either failure leaves the device on the dev slot with at most a stale
      // flag; clear it so no boot hook fires on a request nobody can see.
      dev_ota::pendingInstallClear();
      {
        RenderLock lock(*this);
        failedDetail = tr(STR_DEV_INSTALL_FAILED);
        state = FAILED;
      }
      requestUpdate();
      return;
    }
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    delay(50);
    ESP.restart();
    return;
  }

  runInstall();
}

void DevOtaActivity::runInstall() {
  const BatteryMonitor battery;
  if (battery.readStatus().supported) {
    const uint16_t batteryPct = powerManager.getBatteryPercentage();
    if (batteryPct < DEV_OTA_MIN_BATTERY_PCT) {
      LOG_ERR("DEVOTA", "Battery too low (%u%%) to install", batteryPct);
      {
        RenderLock lock(*this);
        failedDetail = tr(STR_OTA_LOW_BATTERY);
        state = FAILED;
      }
      requestUpdate();
      return;
    }
  }

  {
    RenderLock lock(*this);
    installPercent = -1;
    installWriting = false;
    state = INSTALLING;
  }
  requestUpdateAndWait();

  dev_ota::Error res;
  {
    memclaim::DeviceClaim claim(renderer, "DEVOTA");
    claim.runCheap();
    res = dev_ota::installFromManifestEntry(
        entries.items[selectedIndex],
        [](void* ctx, const int percent, const bool writing) {
          static_cast<DevOtaActivity*>(ctx)->onInstallProgress(percent, writing);
        },
        this);
  }

  if (res != dev_ota::Error::OK) {
    LOG_ERR("DEVOTA", "install failed (%d)", static_cast<int>(res));
    if (autoResume) dev_ota::pendingInstallClear();
    {
      RenderLock lock(*this);
      failedDetail = describeDevError(res);
      state = FAILED;
    }
    requestUpdate();
    return;
  }

  // Flag first, then restart: the freshly booted dev slot must not see a
  // stale pending request and start the dance all over again.
  dev_ota::pendingInstallClear();
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void DevOtaActivity::onInstallProgress(const int percent, const bool writing) {
  {
    RenderLock lock(*this);
    installPercent = percent;
    installWriting = writing;
  }
  // immediate=true: the default deferred path is consumed by
  // ActivityManager::loop(), which never runs while installFromManifestEntry
  // blocks this task.
  requestUpdate(true);
}

void DevOtaActivity::loop() {
  if (state == CONFIRMING) {
    if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
    // Dismissed without a selection: back to the list.
    state = LIST;
    requestUpdate();
    return;
  }

  if (state == LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      selectedIndex = (selectedIndex + entries.count - 1) % entries.count;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      selectedIndex = (selectedIndex + 1) % entries.count;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      promptInstall(selectedIndex);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
  }
}

void DevOtaActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DEV_CHANNEL));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;
  const int side = metrics.contentSidePadding;

  // Running-slot line (design §5 header): what is running and from where.
  char runningLine[96];
  {
    ota_slots::SlotInfo info;
    const esp_partition_t* run = ota_slots::runningSlot();
    if (ota_slots::readSlotInfo(run, info)) {
      snprintf(runningLine, sizeof(runningLine), "%s %s (%s)", tr(STR_CURRENT_VERSION), info.version,
               ota_slots::sameSlot(run, ota_slots::devSlot()) ? tr(STR_SLOT_DEV_NAME) : tr(STR_SLOT_STABLE_NAME));
    } else {
      snprintf(runningLine, sizeof(runningLine), "%s %s", tr(STR_CURRENT_VERSION), CROSSPOINT_VERSION);
    }
  }

  if (state == CHECKING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_DEV_CHECKING));
  } else if (state == LIST) {
    renderer.drawText(UI_10_FONT_ID, side, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing,
                      runningLine);
    int y = top + height + metrics.verticalSpacing;
    for (int i = 0; i < entries.count; ++i) {
      char label[40];
      if (!devNameLabel(entries.items[i].name, label, sizeof(label))) {
        snprintf(label, sizeof(label), "%s", entries.items[i].name);
      }
      if (i == selectedIndex) {
        renderer.drawRect(side - 4, y - 2, pageWidth - (side - 4) * 2, height + 4);
      }
      renderer.drawText(UI_10_FONT_ID, side, y, label);
      renderer.drawText(UI_10_FONT_ID, pageWidth - side - 70, y, entries.items[i].date);
      y += height + metrics.verticalSpacing;
    }
    const auto labels =
        mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == CONFIRMING) {
    renderer.drawText(UI_10_FONT_ID, side, pageHeight / 6, runningLine);
    if (confirmPopup.processRender(renderer, mappedInput)) return;
  } else if (state == INSTALLING) {
    renderer.drawText(UI_10_FONT_ID, side, pageHeight / 6, runningLine);
    renderer.drawCenteredText(UI_10_FONT_ID, top, installWriting ? tr(STR_DEV_WRITING) : tr(STR_DEV_DOWNLOADING));
    const int barY = top + height + metrics.verticalSpacing;
    GUI.drawProgressBar(renderer,
                        Rect{side, barY, pageWidth - side * 2, metrics.progressBarHeight},
                        installPercent < 0 ? 0 : installPercent, 100);
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    if (failedDetail != nullptr) {
      renderer.drawCenteredText(UI_10_FONT_ID, top + height + metrics.verticalSpacing, failedDetail);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
