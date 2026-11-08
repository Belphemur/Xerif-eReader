#include "OtaUpdateActivity.h"

#include <BatteryMonitor.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include "HalPowerManager.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/DevOtaClient.h"
#include "network/OtaBootSwitch.h"
#include "network/OtaInstallRouting.h"
#include "network/OtaSlots.h"
#include "network/OtaUpdater.h"
#include "util/MemoryClaimGateDevice.h"

namespace {
// Stock firmware refuses to flash when the battery can't sustain the write +
// reboot (a brownout mid-OTA can brick the device). Gate the install the same
// way. Boards that report no battery telemetry (BatteryMonitor unsupported)
// skip the check rather than wrongly blocking the update.
constexpr uint16_t OTA_MIN_BATTERY_PCT = 20;

// §16 stable→stable dance staging path (the swap space, design §16). A fixed
// name: overwritten on every dance, removed by the completion pass after the
// flash (the pending kind tells the pass it is a staged file, not the user's).
constexpr char kStableStagingPath[] = "/.crosspoint/stable_ota.bin";

// Shared mapping from an OtaUpdaterError to the user-facing detail string, so
// both the check-time and install-time failure paths surface the same message.
const char* describeOtaError(OtaUpdater::OtaUpdaterError res) {
  switch (res) {
    case OtaUpdater::WRONG_DEVICE_ERROR:
      return tr(STR_FIRMWARE_WRONG_DEVICE);
    case OtaUpdater::SIGNATURE_ERROR:
      return tr(STR_OTA_SIGNATURE_FAILED);
    case OtaUpdater::JSON_PARSE_ERROR:
      return tr(STR_OTA_BAD_RELEASE);
    default:
      return nullptr;
  }
}
}  // namespace

void OtaUpdateActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_ERR("OTA", "WiFi connection failed, exiting");
    finish();
    return;
  }

  LOG_DBG("OTA", "WiFi connected, checking for update");

  {
    RenderLock lock(*this);
    state = CHECKING_FOR_UPDATE;
  }
  requestUpdateAndWait();

  // The cheap memory claim (docs/design/2026-11-03-memory-claims.md §4):
  // releases the rebuildable font caches and keeps the BLE-radio invariant
  // (radio down at these screens) under the plain tlsheap floor. The check's
  // own downloader guard stays the final arbiter — this claim never adds a
  // failure path here, only the [MEM] evidence lines.
  OtaUpdater::OtaUpdaterError res;
  {
    memclaim::DeviceClaim claim(renderer, "OTA");
    claim.runCheap();
    res = updater.checkForUpdate();
  }
  // NO_UPDATE here means the release carries no firmware asset for this board
  // (expected until per-board assets are published) — not a failure.
  if (res == OtaUpdater::NO_UPDATE) {
    LOG_DBG("OTA", "No firmware asset for this board in latest release");
    {
      RenderLock lock(*this);
      state = NO_UPDATE;
    }
    return;
  }
  if (res != OtaUpdater::OK) {
    LOG_DBG("OTA", "Update check failed: %d", res);
    {
      RenderLock lock(*this);
      failedDetail = describeOtaError(res);
      state = FAILED;
    }
    return;
  }

  if (!updater.isUpdateNewer()) {
    LOG_DBG("OTA", "No new update available");
    {
      RenderLock lock(*this);
      state = NO_UPDATE;
    }
    return;
  }

  // §16 (design §15/§16): running stable, a gh_release install dances through
  // the dev worker, and the dance's staging pass is fail-closed on the signed
  // manifest digest — without it the completion pass could never verify the
  // staged bytes are the ones that were checked. A release without a signed
  // manifest therefore cannot be installed from the stable slot; say so before
  // asking the user to confirm, not after staging 5 MB. (Running dev writes
  // app0 directly and keeps its pre-§16 no-manifest behavior.)
  if (ota_slots::sameSlot(ota_slots::runningSlot(), ota_slots::stableSlot()) && !updater.isManifestPinned()) {
    LOG_INF("OTA", "running stable and the release carries no signed manifest; refusing");
    {
      RenderLock lock(*this);
      failedDetail = tr(STR_OTA_SIGNATURE_FAILED);
      state = FAILED;
    }
    return;
  }

  {
    RenderLock lock(*this);
    state = WAITING_CONFIRMATION;
  }
  const char* options[] = {tr(STR_CANCEL), tr(STR_UPDATE)};
  // Default the selection to Update so the hardware Confirm button installs,
  // matching the pre-popup layout (Back = cancel, Confirm = update).
  confirmPopup.show(tr(STR_NEW_UPDATE), options, 2, 1, [this](const int idx) {
    if (idx == 1) {
      runUpdateInstall();
    } else {
      finish();
    }
  });
  requestUpdate();
}

void OtaUpdateActivity::onEnter() {
  Activity::onEnter();

  // Turn on WiFi immediately
  LOG_DBG("OTA", "Turning on WiFi...");
  WiFi.mode(WIFI_STA);

  // Launch WiFi selection subactivity
  LOG_DBG("OTA", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OtaUpdateActivity::onExit() {
  Activity::onExit();

  // Success path reboots via the SHUTTING_DOWN state's plain ESP.restart()
  // (loop() above) so the new firmware boots normally. Back-out paths land
  // here with wifi still active; silent-restart to free the LWIP/mbedTLS
  // fragmentation, same as the other wifi activities.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void OtaUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_UPDATE));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  float updaterProgress = 0;
  if (state == UPDATE_IN_PROGRESS) {
    LOG_DBG("OTA", "Update progress: %d / %d", updater.getProcessedSize(), updater.getTotalSize());
    updaterProgress = static_cast<float>(updater.getProcessedSize()) / static_cast<float>(updater.getTotalSize());
    // Only update every 2% at the most
    if (static_cast<int>(updaterProgress * 50) == lastUpdaterPercentage / 2) {
      return;
    }
    lastUpdaterPercentage = static_cast<int>(updaterProgress * 100);
  }

  if (state == CHECKING_FOR_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_CHECKING_UPDATE));
  } else if (state == WAITING_CONFIRMATION) {
    // Version info sits in the upper part of the screen so the centered
    // Cancel/Update popup doesn't cover it (same layout as ConfirmationActivity).
    const int infoTop = pageHeight / 6;
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, infoTop,
                      (std::string(tr(STR_CURRENT_VERSION)) + CROSSPOINT_VERSION).c_str());
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, infoTop + height + metrics.verticalSpacing,
                      (std::string(tr(STR_NEW_VERSION)) + updater.getLatestVersion()).c_str());

    if (confirmPopup.processRender(renderer, mappedInput)) return;
  } else if (state == UPDATE_IN_PROGRESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATING));

    int y = top + height + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(updaterProgress * 100), 100);

    y += metrics.progressBarHeight + metrics.verticalSpacing;
    // Percent label is drawn by BaseTheme::drawProgressBar; this slot is left intentionally empty
    // so the bytes line below stays at the same Y it was at when the activity drew its own percent.
    y += height + metrics.verticalSpacing;
    renderer.drawCenteredText(
        UI_10_FONT_ID, y,
        (std::to_string(updater.getProcessedSize()) + " / " + std::to_string(updater.getTotalSize())).c_str());
  } else if (state == NO_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NO_UPDATE), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    if (failedDetail != nullptr) {
      renderer.drawCenteredText(UI_10_FONT_ID, top + height + metrics.verticalSpacing, failedDetail);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FINISHED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
    const int hintY = top + height + metrics.verticalSpacing;
    const Rect hintBounds{metrics.contentSidePadding, hintY, pageWidth - metrics.contentSidePadding * 2,
                          pageHeight - hintY};
    UITheme::drawCenteredWrappedText(renderer, hintBounds, UI_10_FONT_ID, tr(STR_POWER_ON_HINT), 3, true,
                                     EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);
  }

  renderer.displayBuffer();
}

void OtaUpdateActivity::runUpdateInstall() {
  // Stock firmware gates OTA behind a battery threshold: a brownout mid-write
  // can corrupt the running partition and brick the device. Gate only when the
  // board actually has battery telemetry — cover ALL backends (I2C gauge, ADC,
  // M5PM1), not just gauge profiles. Boards with no battery path fall through
  // and are never wrongly blocked.
  const BatteryMonitor battery;
  if (battery.readStatus().supported) {
    const uint16_t batteryPct = powerManager.getBatteryPercentage();
    if (batteryPct < OTA_MIN_BATTERY_PCT) {
      LOG_ERR("OTA", "Battery too low (%u%%) to update; aborting", batteryPct);
      {
        RenderLock lock(*this);
        failedDetail = tr(STR_OTA_LOW_BATTERY);
        state = FAILED;
      }
      requestUpdate();
      return;
    }
  }

  LOG_DBG("OTA", "New update available, starting download...");
  {
    RenderLock lock(*this);
    state = UPDATE_IN_PROGRESS;
  }
  requestUpdateAndWait();

  // §16 routing (design §15/§16): the gh_release source ALWAYS targets the
  // stable slot. When the running slot IS the stable slot, the §16 dance
  // applies: stage the asset on SD, record the pending install, restart into
  // the dev slot (the swap worker), and let the boot hook complete the write
  // of app0. Running dev writes app0 directly (the intended role replacement).
  const bool runningIsStable = ota_slots::sameSlot(ota_slots::runningSlot(), ota_slots::stableSlot());
  // The §16 dance worker must understand the §16 pending kinds: the §5 V1 hook
  // marker alone also matches PR #298-era images whose boot hook would drop
  // the StableStaged request (design §16, swap-worker capability).
  const auto route = ota_install_routing::routeForInstall(runningIsStable, /*targetIsStable=*/true,
                                                          ota_slots::imageHasSwapKindsMarker(ota_slots::devSlot()));
  if (route == ota_install_routing::InstallRoute::Dance) {
    armStableDance();
    return;
  }
  if (route == ota_install_routing::InstallRoute::RefuseNoWorker) {
    // §16: the swap worker (dev slot) is empty or lacks the resume hook —
    // REFUSE, never write the wrong-role slot merely because it is free.
    LOG_ERR("OTA", "§16 swap worker (dev slot) unavailable; refusing the install");
    {
      RenderLock lock(*this);
      failedDetail = tr(STR_SLOT_DEV_WORKER_TOO_OLD);
      state = FAILED;
    }
    requestUpdate();
    return;
  }

  // The cheap memory claim for the download itself (§4): same floor and
  // non-arbiter contract as the update check above. The claim ends at the
  // block scope, records-only (the deferred radio restore rule, §3.3).
  OtaUpdater::OtaUpdaterError res;
  {
    memclaim::DeviceClaim claim(renderer, "OTA");
    claim.runCheap();
    res = updater.installUpdate(
        [](void* ctx) {
          // immediate=true notifies the render task directly. The default deferred path only
          // sets a flag consumed at the end of ActivityManager::loop(), which never runs while
          // installUpdate() blocks this task.
          static_cast<OtaUpdateActivity*>(ctx)->requestUpdate(true);
        },
        this);
  }

  if (res != OtaUpdater::OK) {
    LOG_DBG("OTA", "Update failed: %d", res);
    {
      RenderLock lock(*this);
      failedDetail = describeOtaError(res);
      state = FAILED;
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = FINISHED;
  }
  requestUpdateAndWait();
  // Hold the completion screen briefly so the user sees it, then restart.
  delay(3000);
  {
    RenderLock lock(*this);
    state = SHUTTING_DOWN;
  }
}

void OtaUpdateActivity::loop() {
  if (state == WAITING_CONFIRMATION) {
    if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
    // Popup dismissed without a selection (Back button or tap outside): cancel.
    finish();
    return;
  }

  if (state == FAILED) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
      finish();
    }
    return;
  }

  if (state == NO_UPDATE) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
      finish();
    }
    return;
  }

  if (state == SHUTTING_DOWN) {
    ESP.restart();
  }
}

void OtaUpdateActivity::armStableDance() {
  // §16 stable→stable dance (design §15/§16): the running slot is never a
  // write target, so the gh_release asset is staged on SD (the swap space),
  // the pending install is recorded, and the device restarts into the dev
  // slot — the swap worker — whose boot hook completes the write of app0.
  // Fail-safe order: stage → flag → otadata. A failed otadata switch leaves
  // at most a stale flag (cleared by the boot hook) plus the staging file.
  OtaUpdater::OtaUpdaterError res;
  {
    memclaim::DeviceClaim claim(renderer, "OTA");
    claim.runCheap();
    res = updater.stageUpdateToSd(
        kStableStagingPath,
        [](void* ctx) {
          // immediate=true: the deferred path is consumed by
          // ActivityManager::loop(), which never runs while the download
          // blocks this task.
          static_cast<OtaUpdateActivity*>(ctx)->requestUpdate(true);
        },
        this);
  }
  if (res != OtaUpdater::OK) {
    LOG_ERR("OTA", "§16 staging failed: %d", static_cast<int>(res));
    {
      RenderLock lock(*this);
      failedDetail = describeOtaError(res);
      state = FAILED;
    }
    requestUpdate();
    return;
  }

  // Pin the staged file's identity (§16 dance identity): the unattended
  // completion pass re-hashes the staging file and refuses to flash when the
  // SD card's contents changed between this arm and the completion boot.
  uint8_t stagedDigest[32];
  if (!dev_ota::sha256OfFile(kStableStagingPath, stagedDigest) ||
      !dev_ota::pendingInstallSet(kStableStagingPath, dev_ota::PendingKind::StableStaged) ||
      !dev_ota::pendingInstallSetDigest(stagedDigest) || !ota_boot::switchTo(ota_slots::devSlot())) {
    LOG_ERR("OTA", "§16 dance arm failed; clearing the flag");
    dev_ota::pendingInstallClear();
    Storage.remove(kStableStagingPath);
    {
      RenderLock lock(*this);
      failedDetail = tr(STR_FIRMWARE_WRITE_FAILED);
      state = FAILED;
    }
    requestUpdate();
    return;
  }
  LOG_INF("OTA", "§16 dance armed → dev worker");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}
