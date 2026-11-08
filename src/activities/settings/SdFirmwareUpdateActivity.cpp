#include "SdFirmwareUpdateActivity.h"

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_ota_ops.h>

#include <cstring>

#include "MappedInputManager.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/DevOtaClient.h"
#include "network/FirmwareFlasher.h"
#include "network/OtaBootSwitch.h"
#include "network/OtaInstallRouting.h"
#include "network/OtaSlots.h"

namespace {
// The unattended §16 dance completion pass writes a full slot image; a
// brownout mid-write can corrupt the TARGET slot (the worker itself is never
// touched, so the failure is recoverable — but the gate avoids the attempt,
// same rule as the §5 dance's completion pass). Boards without battery
// telemetry fall through.
constexpr uint16_t DANCE_MIN_BATTERY_PCT = 20;
}  // namespace

void SdFirmwareUpdateActivity::onEnter() {
  Activity::onEnter();
  // Build-identity marker — confirms which firmware build owns the SD update flow.
  LOG_INF("FW", "SdFirmwareUpdateActivity build=%s %s recovery=%d", __DATE__, __TIME__, recoveryMode ? 1 : 0);
  if (autoResume) {
    // §16 dance completion pass (design §15/§16): the swap worker boots after
    // the dance restart and completes the write of the TARGET slot. The staged
    // SD path is read from the pending-install flag; the pending kind fixes
    // the target slot, which for every dance arm is the non-running slot —
    // the same slot flashFromSdPath() writes.
    char dancePath[dev_ota::kPendingPayloadCap];
    if (!dev_ota::pendingInstallGet(dancePath, sizeof(dancePath))) {
      LOG_ERR("FW", "autoResume: no pending install; aborting");
      {
        RenderLock lock(*this);
        state = State::FAILED;
      }
      requestUpdate();
      return;
    }
    // The pending kind is the authoritative record of the target slot: it was
    // fixed by the §15 classification at dance-arm time. Deriving the target
    // from it (not from re-classifying the basename) keeps the completion
    // pass honest even if the staged file was renamed on SD.
    const dev_ota::PendingKind kind = dev_ota::pendingInstallKind();
    sdTargetIsStable = kind == dev_ota::PendingKind::StableStaged || kind == dev_ota::PendingKind::StablePicked;
    LOG_INF("FW", "autoResume: dance kind=%d target=%s path=%s", static_cast<int>(kind),
            sdTargetIsStable ? "stable" : "dev", dancePath);
    firmwarePath = dancePath;

    // Battery gate for the unattended completion pass (the §5 dance's rule):
    // a brownout mid-write can corrupt the TARGET slot. A resumed install
    // that fails here must not leave the flag armed — every boot would
    // re-enter the installer until the battery recovers.
    const BatteryMonitor battery;
    if (battery.readStatus().supported) {
      const uint16_t batteryPct = powerManager.getBatteryPercentage();
      if (batteryPct < DANCE_MIN_BATTERY_PCT) {
        LOG_ERR("FW", "Battery too low (%u%%) to resume the install", batteryPct);
        unwindDance();
        {
          RenderLock lock(*this);
          errorMessage = tr(STR_OTA_LOW_BATTERY);
          state = State::FAILED;
        }
        requestUpdate();
        return;
      }
    }

    {
      RenderLock lock(*this);
      state = State::VALIDATING;
    }
    requestUpdateAndWait();
    if (!validateFirmware()) {
      // The staged/picked file is gone, renamed, corrupt, or for the wrong
      // device: unwind so no boot re-enters the installer.
      unwindDance();
      {
        RenderLock lock(*this);
        state = State::FAILED;
      }
      requestUpdate();
      return;
    }
    // §16 dance identity check: the dance armed with the SHA-256 of the exact
    // image that was validated and consented to. A file swapped on the SD card
    // between the arm and this unattended boot must not reach flash.
    if (kind != dev_ota::PendingKind::DevChannel) {
      uint8_t expectedDigest[32];
      uint8_t actualDigest[32];
      if (!dev_ota::pendingInstallGetDigest(expectedDigest)) {
        LOG_ERR("FW", "autoResume: no recorded digest; refusing to flash");
        errorMessage = tr(STR_OTA_SIGNATURE_FAILED);
        unwindDance();
        {
          RenderLock lock(*this);
          state = State::FAILED;
        }
        requestUpdate();
        return;
      }
      if (!dev_ota::sha256OfFile(firmwarePath.c_str(), actualDigest) || memcmp(expectedDigest, actualDigest, 32) != 0) {
        LOG_ERR("FW", "autoResume: SD file differs from the consented image");
        errorMessage = tr(STR_OTA_FILE_CHANGED);
        unwindDance();
        {
          RenderLock lock(*this);
          state = State::FAILED;
        }
        requestUpdate();
        return;
      }
    }
    // The completion pass paints the same uninterruptible-flash UI as the
    // interactive confirmation path: progress bar + do-not-power-off line.
    // Staying in VALIDATING would repaint that static line for the whole
    // flash instead.
    {
      RenderLock lock(*this);
      state = State::UPDATING;
      writtenBytes = 0;
      lastRenderedPercent = 101;
    }
    requestUpdateAndWait();
    performUpdate();
    return;
  }
  state = State::PICKING;
  launchPicker();
}

void SdFirmwareUpdateActivity::launchPicker() {
  // Reuse the standard file browser, restricted to .bin files only.
  startActivityForResult(
      std::make_unique<FileBrowserActivity>(renderer, mappedInput, "/", FileBrowserActivity::Mode::PickFirmware),
      [this](const ActivityResult& result) { onPickerResult(result); });
}

void SdFirmwareUpdateActivity::onPickerResult(const ActivityResult& result) {
  if (result.isCancelled) {
    if (recoveryMode) {
      // Recovery mode: re-launch the picker so the user cannot escape into a half-initialised UI.
      launchPicker();
      return;
    }
    finish();
    return;
  }

  const auto* path = std::get_if<FilePathResult>(&result.data);
  if (!path) {
    LOG_ERR("FW", "Picker returned no path");
    finish();
    return;
  }
  firmwarePath = path->path;
  LOG_DBG("FW", "Selected: %s", firmwarePath.c_str());

  // §15 source rule: the picked file's basename classifies the target slot
  // (xerif-dev-/xerif-pr- → dev slot; every other .bin → stable slot).
  const auto base = firmwarePath.find_last_of('/');
  const char* basename = base == std::string::npos ? firmwarePath.c_str() : firmwarePath.c_str() + base + 1;
  const auto fileClass = ota_install_routing::classifySdFile(basename);
  sdTargetIsStable = ota_install_routing::targetForSdFile(fileClass) == ota_install_routing::InstallTarget::Stable;
  LOG_INF("FW", "SD install class=%d target=%s", static_cast<int>(fileClass), sdTargetIsStable ? "stable" : "dev");

  {
    RenderLock lock(*this);
    state = State::VALIDATING;
  }
  requestUpdateAndWait();

  if (!validateFirmware()) {
    RenderLock lock(*this);
    state = State::FAILED;
    requestUpdate();
    return;
  }

  promptConfirmation();
}

bool SdFirmwareUpdateActivity::validateFirmware() {
  HalFile file;
  if (!Storage.openFileForRead("FW", firmwarePath.c_str(), file) || !file) {
    errorMessage = tr(STR_FIRMWARE_FILE_OPEN_FAILED);
    return false;
  }
  firmwareSize = file.fileSize();
  file.close();

  // Resolve the §15 target slot directly from the classification (§15 scope:
  // the SD install path no longer uses "the next update partition"). Recovery
  // mode keeps the legacy rule: it always writes the NON-running slot (§15
  // exception), which for every recovery boot is the same slot the §15 rule
  // would not pick, so resolve it the old way.
  const esp_partition_t* dest = nullptr;
  if (recoveryMode) {
    dest = esp_ota_get_next_update_partition(nullptr);
  } else {
    dest = sdTargetIsStable ? ota_slots::stableSlot() : ota_slots::devSlot();
  }
  if (!dest) {
    LOG_ERR("FW", "no target partition available");
    errorMessage = tr(STR_INVALID_FIRMWARE);
    return false;
  }
  const size_t partitionLimit = dest->size;
  if (firmwareSize > partitionLimit) {
    LOG_ERR("FW", "firmware (%u bytes) exceeds partition (%u bytes)", static_cast<unsigned>(firmwareSize),
            static_cast<unsigned>(partitionLimit));
    errorMessage = tr(STR_FIRMWARE_TOO_LARGE);
    return false;
  }

  // Run the same end-to-end integrity check (header / segment table / XOR checksum / SHA256
  // trailer) that the shared firmware-flasher applies right before raw-writing otadata. This
  // catches truncated or corrupted .bin files at confirmation time, before the user ever sees
  // the "Updating…" progress bar.
  const auto vr = firmware_flash::validateImageFile(firmwarePath.c_str(), partitionLimit);
  if (vr != firmware_flash::Result::OK) {
    LOG_ERR("FW", "image validation failed: %s", firmware_flash::resultName(vr));
    if (vr == firmware_flash::Result::TOO_LARGE) {
      errorMessage = tr(STR_FIRMWARE_TOO_LARGE);
    } else if (vr == firmware_flash::Result::TOO_SMALL) {
      errorMessage = tr(STR_FIRMWARE_TOO_SMALL);
    } else if (vr == firmware_flash::Result::BAD_CHIP || vr == firmware_flash::Result::WRONG_BOARD) {
      errorMessage = tr(STR_FIRMWARE_WRONG_DEVICE);
    } else {
      errorMessage = tr(STR_INVALID_FIRMWARE);
    }
    return false;
  }
  return true;
}

void SdFirmwareUpdateActivity::promptConfirmation() {
  {
    RenderLock lock(*this);
    state = State::CONFIRMING;
  }
  // Show "Update firmware?" with the file path as the body line.
  std::string heading = tr(STR_FIRMWARE_UPDATE_PROMPT);
  // Use the basename only to keep the body short.
  std::string body = firmwarePath;
  const auto pos = body.find_last_of('/');
  if (pos != std::string::npos) body = body.substr(pos + 1);

  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, body),
                         [this](const ActivityResult& result) { onConfirmationResult(result); });
}

void SdFirmwareUpdateActivity::onConfirmationResult(const ActivityResult& result) {
  if (result.isCancelled) {
    if (recoveryMode) {
      // Go back to the picker rather than exiting recovery.
      launchPicker();
      return;
    }
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state = State::UPDATING;
    writtenBytes = 0;
    lastRenderedPercent = 101;
  }
  requestUpdateAndWait();
  performUpdate();
}

void SdFirmwareUpdateActivity::performUpdate() {
  LOG_INF("FW", "SD update: %s (%u bytes)", firmwarePath.c_str(), static_cast<unsigned>(firmwareSize));

  auto progressCb = +[](size_t written, size_t total, void* ctx) {
    auto* self = static_cast<SdFirmwareUpdateActivity*>(ctx);
    self->writtenBytes = written;
    self->firmwareSize = total;
    // immediate=true: wake the render task directly. We're in a tight sync
    // loop so the main loop won't drain the requestedUpdate flag for us.
    self->requestUpdate(true);
  };

  // §16 routing (design §15/§16, normal mode only). Recovery mode is the §15
  // exception and keeps the legacy non-running-slot write; the autoResume
  // completion pass writes the TARGET slot, which for every dance arm is the
  // non-running slot — the same slot flashFromSdPath() writes.
  if (recoveryMode || autoResume) {
    const auto result = firmware_flash::flashFromSdPath(firmwarePath.c_str(), progressCb, this);
    finishFlash(result);
    return;
  }

  // Normal mode: the §16 matrix. The target slot comes from the §15 source
  // rule (basename classification); the running slot is never a write target.
  const bool runningIsStable = ota_slots::sameSlot(ota_slots::runningSlot(), ota_slots::stableSlot());
  const bool workerAvailable = sdSwapWorkerAvailable();
  const auto route = ota_install_routing::routeForInstall(runningIsStable, sdTargetIsStable, workerAvailable);
  switch (route) {
    case ota_install_routing::InstallRoute::DirectWrite: {
      // The target is not the running slot: write it directly (§16 matrix).
      const esp_partition_t* dest = sdTargetIsStable ? ota_slots::stableSlot() : ota_slots::devSlot();
      LOG_INF("FW", "§16 direct write → %s", dest->label);
      const auto result = firmware_flash::flashFromSdPathInto(firmwarePath.c_str(), dest, progressCb, this);
      finishFlash(result);
      return;
    }
    case ota_install_routing::InstallRoute::Dance:
      // The target IS the running slot: the §16 dance. armSwapDance() records
      // the pending install, switches otadata to the swap worker and reboots;
      // it never returns on success.
      armSwapDance();
      return;
    case ota_install_routing::InstallRoute::RefuseNoWorker:
    default:
      // §16: the swap worker is unavailable — REFUSE, never write the
      // wrong-role slot merely because it is free. The message names the
      // worker slot: running dev dances through the stable slot (the §5
      // dance's refusal), running stable dances through the dev slot.
      LOG_ERR("FW", "§16 swap worker unavailable; refusing the install");
      errorMessage = sdTargetIsStable ? tr(STR_SLOT_DEV_WORKER_TOO_OLD) : tr(STR_DEV_STABLE_TOO_OLD);
      {
        RenderLock lock(*this);
        state = State::FAILED;
      }
      requestUpdate();
      return;
  }
}

bool SdFirmwareUpdateActivity::sdSwapWorkerAvailable() const {
  // The swap worker is the OTHER slot (§16): running stable dances through the
  // dev slot, running dev dances through the stable slot. It must understand
  // the §16 pending kinds — the §5 V1 hook marker alone also matches PR
  // #298-era images whose boot hook would drop the newer kinds.
  const esp_partition_t* worker = sdTargetIsStable ? ota_slots::devSlot() : ota_slots::stableSlot();
  return ota_slots::imageHasSwapKindsMarker(worker);
}

void SdFirmwareUpdateActivity::unwindDance() {
  // otadata still points at the swap worker; restore it to the TARGET slot —
  // the firmware the device ran before the dance — so the next boot is the
  // pre-dance state. Then clear the pending record and drop the staged file.
  if (!ota_boot::switchTo(sdTargetIsStable ? ota_slots::stableSlot() : ota_slots::devSlot())) {
    LOG_ERR("FW", "§16 unwind: otadata restore failed; device keeps booting the worker");
  }
  const bool staged = dev_ota::pendingInstallKind() == dev_ota::PendingKind::StableStaged;
  dev_ota::pendingInstallClear();
  if (staged) Storage.remove(firmwarePath.c_str());
}

void SdFirmwareUpdateActivity::armSwapDance() {
  // §16 dance arm: the target IS the running slot, so the OTHER slot (the swap
  // worker) completes the write after the restart. The user's file stays on SD
  // (the swap space); the pending kind fixes the target slot for the
  // completion pass. Fail-safe order: flag first, then otadata — a failed
  // switch leaves at most a stale flag, which the boot hook clears.
  const dev_ota::PendingKind kind = sdTargetIsStable ? dev_ota::PendingKind::StablePicked : dev_ota::PendingKind::SdDev;
  const esp_partition_t* worker = sdTargetIsStable ? ota_slots::devSlot() : ota_slots::stableSlot();
  // The pending record is read back into a kPendingPayloadCap buffer by the
  // boot hook and this completion pass; a longer path could never be resumed.
  if (firmwarePath.length() >= dev_ota::kPendingPayloadCap) {
    LOG_ERR("FW", "SD path too long for the pending-install record (%u >= %u)",
            static_cast<unsigned>(firmwarePath.length()), static_cast<unsigned>(dev_ota::kPendingPayloadCap));
    errorMessage = tr(STR_SD_PATH_TOO_LONG);
    {
      RenderLock lock(*this);
      state = State::FAILED;
    }
    requestUpdate();
    return;
  }
  // Pin the picked file's identity (§16 dance identity): the unattended
  // completion pass re-hashes the file and refuses to flash when the SD card's
  // contents changed between this consent and the completion boot.
  uint8_t pickedDigest[32];
  if (!dev_ota::sha256OfFile(firmwarePath.c_str(), pickedDigest) ||
      !dev_ota::pendingInstallSet(firmwarePath.c_str(), kind) || !dev_ota::pendingInstallSetDigest(pickedDigest) ||
      !ota_boot::switchTo(worker)) {
    LOG_ERR("FW", "§16 dance arm failed; clearing the flag");
    dev_ota::pendingInstallClear();
    errorMessage = tr(STR_FIRMWARE_WRITE_FAILED);
    {
      RenderLock lock(*this);
      state = State::FAILED;
    }
    requestUpdate();
    return;
  }
  LOG_INF("FW", "§16 dance armed → worker %s", worker->label);
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void SdFirmwareUpdateActivity::finishFlash(const firmware_flash::Result result) {
  if (result != firmware_flash::Result::OK) {
    LOG_ERR("FW", "flash failed: %s", firmware_flash::resultName(result));
    // BAD_CHIP / WRONG_BOARD here is the TOCTOU re-validation catching a
    // wrong-device image the pre-confirmation pass missed (e.g. the SD card
    // was swapped).
    errorMessage = result == firmware_flash::Result::BAD_CHIP || result == firmware_flash::Result::WRONG_BOARD
                       ? tr(STR_FIRMWARE_WRONG_DEVICE)
                       : tr(STR_FIRMWARE_WRITE_FAILED);
    if (autoResume) {
      // A failed completion pass must not leave the flag armed: every boot
      // would re-enter the installer (the same no-retry-loop rule as the
      // §5 dance's failure paths). Unwind fully: otadata back to the target
      // slot and the staged file removed.
      unwindDance();
    }
    RenderLock lock(*this);
    state = State::FAILED;
    requestUpdate();
    return;
  }

  LOG_INF("FW", "SD firmware update complete, restarting");
  if (autoResume) {
    // Flag first, then restart: the freshly booted slot must not see a stale
    // pending request and start the dance all over again. A staged gh_release
    // install also drops its 5 MB staging file here; a user-picked file is
    // never removed.
    const bool staged = dev_ota::pendingInstallKind() == dev_ota::PendingKind::StableStaged;
    dev_ota::pendingInstallClear();
    if (staged) Storage.remove(firmwarePath.c_str());
  }
  {
    RenderLock lock(*this);
    state = State::SUCCESS;
  }
  requestUpdateAndWait();
  delay(1500);
  ESP.restart();
}

void SdFirmwareUpdateActivity::loop() {
  if (state == State::FAILED) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      if (recoveryMode) {
        // Go back to picker so user can try a different .bin
        state = State::PICKING;
        launchPicker();
        return;
      }
      finish();
    }
  }
}

void SdFirmwareUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const char* headerText = recoveryMode ? tr(STR_RECOVERY_MODE) : tr(STR_SD_FIRMWARE_UPDATE);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerText);

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - lineHeight) / 2;

  if (state == State::VALIDATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_VALIDATING_FIRMWARE));
  } else if (state == State::UPDATING) {
    // Throttle redraws to once per percent.
    const unsigned int pct = firmwareSize > 0 ? static_cast<unsigned int>((writtenBytes * 100) / firmwareSize) : 0;
    if (pct == lastRenderedPercent) {
      return;
    }
    lastRenderedPercent = pct;

    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATING), true, EpdFontFamily::BOLD);

    int y = top + lineHeight + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(pct), 100);
    y += metrics.progressBarHeight + metrics.verticalSpacing;
    // Percent label is drawn by BaseTheme::drawProgressBar; this slot is left intentionally empty
    // so the do-not-power-off line below stays at the same Y as before.
    y += lineHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_FIRMWARE_UPDATE_DO_NOT_POWER_OFF));
  } else if (state == State::SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
    const int hintY = top + lineHeight + metrics.verticalSpacing;
    const Rect hintBounds{metrics.contentSidePadding, hintY, pageWidth - metrics.contentSidePadding * 2,
                          pageHeight - hintY};
    UITheme::drawCenteredWrappedText(renderer, hintBounds, UI_10_FONT_ID, tr(STR_RESTARTING_HINT), 3, true,
                                     EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);
  } else if (state == State::FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    if (!errorMessage.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing, errorMessage.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    // PICKING / CONFIRMING: a sub-activity is on top, nothing to draw.
    if (recoveryMode) {
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_RECOVERY_MODE_HINT));
    }
  }

  renderer.displayBuffer();
}
