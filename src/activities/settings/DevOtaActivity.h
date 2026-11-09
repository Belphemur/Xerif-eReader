#pragma once

#include <FreeInkUICore.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <string>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "network/DevOtaClient.h"
#include "network/DevOtaPolicy.h"
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

  // FAILED screen for a resume the boot hook refused before dispatching
  // (record incomplete — no usable binding): shows the refusal state; the
  // record was already disarmed by the boot hook. Owner steer: a pending
  // resume that cannot proceed must never silently land on Home.
  void showResumeRefusal();

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
  // §16 refusal path for a resumed dance: clear the pending record and
  // restore otadata to the dev slot (the pre-dance firmware) unless the
  // write phase already started. See the definition for the full contract.
  void refuseDance();
  // Keep the scroll window (listScrollTop_..listVisibleRows_) containing
  // selectedIndex after a button move.
  void keepSelectionVisible();
  // Display row of a manifest entry in displayRows_ (-1 when absent).
  int displayRowForEntry(int entry) const;
  // Move the selection to the next/previous ENTRY row in display order
  // (header rows are skipped), wrapping. False when there is no entry row.
  bool stepSelection(int dir);
  void promptInstall(int index);  // manual path: confirm popup first
  void beginInstall(int index);
  void runInstall();
  void onInstallProgress(int percent, bool writing);

  const bool autoResume;
  bool resumeRefused = false;  // showResumeRefusal(): onEnter must not restart the flow
  State state = WIFI_SELECTION;
  // Fixed-size POD entries (design §9 lifetime rule); owned by value so the
  // install path holds no heap pointers across the blocking write.
  dev_ota::EntryBuffer entries{};
  int selectedIndex = 0;
  // Scroll window for the version picker: the full kMaxVersions manifest
  // cannot fit above the button hints, so the list scrolls around the
  // selection. listVisibleRows_ caches the row count the last render fit;
  // until the first LIST render it disables scrolling. Both count DISPLAY
  // rows (pickerRows output: entry rows + group headers), never entries.
  int16_t listScrollTop_ = 0;
  int16_t listVisibleRows_ = dev_ota::kMaxVersions;
  // §19 grouped display: PR group header + entries, then dev group header
  // + entries, recomputed after each manifest fetch. Header rows have
  // entry < 0 and are neither selectable nor touch targets.
  dev_ota::PickerRow displayRows_[kMaxVersions + 2];
  int16_t displayRowCount_ = 0;
  char pendingVersion[dev_ota::kPendingPayloadCap] = {0};
  const char* failedDetail = nullptr;
  // Install progress, published from the blocking install via
  // onInstallProgress (percent edges only).
  int installPercent = -1;
  bool installWriting = false;
  OptionPopup<> confirmPopup;

  // Touch for the hand-rendered LIST and FAILED screens (boards like the
  // X4 Pro drive Confirm/Back only through the GT911, so without this the
  // picker is a dead end): render() registers the hit rects on the render
  // task, loop() routes touch snapshots against the published generation —
  // the same cross-task pattern OptionPopup uses for the confirm dialog,
  // which routes its own buffer.
  static constexpr freeink::ui::ActionId TOUCH_ACTION_ROW = 1;      // value = entry index
  static constexpr freeink::ui::ActionId TOUCH_ACTION_DISMISS = 2;  // FAILED: tap anywhere leaves
  freeink::ui::InteractionBuffer<dev_ota::kMaxVersions + 1> touchInteractions;
  // Set by render() once the published table matches the on-screen state;
  // read by loop() so a tap never routes against a stale table.
  bool touchReady = false;

  // Consumes every touch frame while LIST/FAILED is on screen (a swallowed
  // tap must not also reach the button path) and acts on a release that
  // lands on a registered rect. Never blocks the input loop: routing is an
  // O(rows) hit test against the already-published table.
  bool handleHandRenderedTouch();
};
