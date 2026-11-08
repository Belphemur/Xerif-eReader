#pragma once

#include <string>

#include "activities/Activity.h"
#include "network/FirmwareFlasher.h"

/**
 * SD-card based firmware update activity.
 *
 * Flow:
 *  1) onEnter -> push FileBrowserActivity in PickFirmware mode (only .bin files visible).
 *  2) On result: validate the .bin (header magic, size fits OTA partition).
 *  3) Push ConfirmationActivity ("Update firmware?").
 *  4) On confirm: stream the file into the OTA partition via the Arduino Update API,
 *     drawing a progress bar; on success ESP.restart().
 *
 * Used both from Settings -> System -> "SD Card Firmware Update", and as the only
 * activity launched in boot recovery mode (left side button + power on X3).
 *
 * §16 stable→stable dance (design §15/§16): the target-slot rule (design §15)
 * and the running slot never being a write target mean the TARGET slot is the
 * running slot. The dance completes the write of the TARGET slot from the
 * OTHER slot (the swap worker): the boot hook (main.cpp) launches this activity
 * with autoResume=true, the SD path is read from the pending-install flag.
 */
class SdFirmwareUpdateActivity : public Activity {
 public:
  enum class State {
    PICKING,
    VALIDATING,
    CONFIRMING,
    UPDATING,
    SUCCESS,
    FAILED,
  };

  explicit SdFirmwareUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool recoveryMode = false)
      : Activity("SdFirmwareUpdate", renderer, mappedInput), recoveryMode(recoveryMode) {}

  // §16 dance completion pass (design §15/§16): the boot hook (main.cpp)
  // launches this activity with autoResume=true after the dance restart; the
  // staged SD path is read from the pending-install flag and the pass writes
  // the TARGET slot (the non-running slot). Not for recovery mode.
  explicit SdFirmwareUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool recoveryMode,
                                    bool autoResume)
      : Activity("SdFirmwareUpdate", renderer, mappedInput), recoveryMode(recoveryMode), autoResume(autoResume) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::UPDATING || state == State::VALIDATING; }
  bool skipLoopDelay() override { return state == State::UPDATING; }

 private:
  State state = State::PICKING;
  bool recoveryMode = false;
  // §16 dance completion pass: the boot hook (main.cpp) launches this
  // activity with autoResume=true; the target slot is encoded by the
  // pending-install kind and the staged SD path is read from the flag.
  bool autoResume = false;

  std::string firmwarePath;
  size_t firmwareSize = 0;
  size_t writtenBytes = 0;
  unsigned int lastRenderedPercent = 101;
  std::string errorMessage;
  // §15 source rule: the picked file's classification (basename prefix) fixes
  // the target slot for this install. Set in onPickerResult.
  bool sdTargetIsStable = true;
  // §16 dance identity: SHA-256 of the picked file, computed BEFORE validation
  // so the recorded digest describes the image that was validated and
  // consented to, not whatever sits on the SD card at arm time. Set in
  // onPickerResult.
  uint8_t pickedDigest_[32] = {};
  bool pickedDigestValid_ = false;

  void launchPicker();
  void onPickerResult(const ActivityResult& result);
  bool validateFirmware();
  void promptConfirmation();
  void onConfirmationResult(const ActivityResult& result);
  void performUpdate();
  // §16 helpers (design §15/§16).
  bool sdSwapWorkerAvailable() const;
  void armSwapDance();
  // Unwind a failed completion pass: restore otadata to the TARGET slot, clear
  // the pending record, and drop the staged file so no boot re-enters the
  // installer and the device returns to its pre-dance firmware.
  void unwindDance();
  void finishFlash(firmware_flash::Result result);
};
