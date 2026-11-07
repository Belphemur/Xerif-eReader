#pragma once

#include <GfxRenderer.h>
#include <I18n.h>

#include <string>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "network/DevOtaClient.h"
#include "network/OtaJson.h"

class MappedInputManager;
class WifiSelectionActivity;

// Dev OTA channel UI (design §5). One state machine serves both entry points:
//
//  - Manual (CROSSPOINT_DEV_OTA builds only — the settings entry is gated):
//    lists the manifest's newest 3 dev versions and installs the chosen one.
//    Single-pass when running stable; from the dev slot it records the NVS
//    pending-install flag and reboots into stable, where the boot hook
//    completes the flow (the running slot is never a write target).
//  - Resume (every build): launched by the boot hook in main.cpp when a
//    pending install flag is set — connects WiFi and finishes the install.
//
// The install itself is dev_ota::installFromManifestEntry: download → SHA-256
// verify against the manifest → full image validation → FirmwareFlasher
// raw-write of app1 → otadata switch.
class DevOtaActivity : public Activity {
 public:
  explicit DevOtaActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool autoResume)
      : Activity("DevOta", renderer, mappedInput), autoResume(autoResume) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CHECKING || state == INSTALLING; }
  bool skipLoopDelay() override { return state == CHECKING || state == INSTALLING; }

 private:
  enum State {
    WIFI_SELECTION,
    CHECKING,
    LIST,
    CONFIRMING,
    INSTALLING,
    FAILED,
  };

  static constexpr int kMaxVersions = dev_ota::kMaxVersions;

  void onWifiSelectionComplete(bool success);
  void fetchManifest();
  void promptInstall(int index);  // manual path: confirm popup first
  void beginInstall(int index);
  void runInstall();
  void onInstallProgress(int percent, bool writing);

  const bool autoResume;
  State state = WIFI_SELECTION;
  // Fixed-size POD entries (design §9 lifetime rule); owned by value so the
  // install path holds no heap pointers across the blocking write.
  dev_ota::EntryBuffer entries{};
  int selectedIndex = 0;
  char pendingVersion[64] = {0};
  const char* failedDetail = nullptr;
  // Install progress, published from the blocking install via
  // onInstallProgress (percent edges only).
  int installPercent = -1;
  bool installWriting = false;
  OptionPopup<> confirmPopup;
};
