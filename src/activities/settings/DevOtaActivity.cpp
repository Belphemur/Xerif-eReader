#include "DevOtaActivity.h"

#include <BatteryMonitor.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "HalPowerManager.h"
#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "network/DevOtaPolicy.h"
#include "network/OtaBootSwitch.h"
#include "network/OtaSlots.h"
#include "util/MemoryClaimGateDevice.h"

namespace fui = freeink::ui;

namespace {
// Same battery gate as the release OTA path (OtaUpdateActivity): a brownout
// mid-write can corrupt the written slot.
constexpr uint16_t DEV_OTA_MIN_BATTERY_PCT = 20;

// The dance's completion pass runs in the stable slot, so that image must
// contain the bootFinalize() resume hook (main.cpp). Images advertise the
// hook through the flash-scannable kResumeHookMarker, not through the
// esp_app_desc_t version (an opaque constant from the prebuilt Arduino core).
// An older stable image boots normally and never consumes the flag, leaving
// the device on a stable slot with no slot chooser. Refusing the dance here
// is the fail-safe direction.

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

// The picker's row dates are LOCAL (owner report 2026-11-09: the status bar
// read 15:02 while the rows showed the manifest's raw UTC 20:56).
// clockEffectiveOffsetMin() is the same resolution the status bar clock
// uses — the live IANA offset folds DST, falls back to the last-detected
// cache offline, and stays 0 (UTC) until a zone has been detected.
int localClockOffsetMinutes() { return SETTINGS.clockEffectiveOffsetMin(); }

const char* pickGroupLabel(uint8_t group) {
  return group == static_cast<uint8_t>(dev_ota::EntryGroup::Pr) ? tr(STR_DEV_GROUP_PRS) : tr(STR_DEV_GROUP_DEVS);
}
}  // namespace

void DevOtaActivity::showResumeRefusal() {
  // The record is gone (the boot hook cleared the unusable one); this is
  // pure feedback, no install can ever be dispatched from here. resumeRefused
  // survives into onEnter, which must not start the WiFi dance over a dead
  // record.
  resumeRefused = true;
  {
    RenderLock lock(*this);
    failedDetail = tr(STR_DEV_RESUME_REFUSED);
    state = FAILED;
  }
  requestUpdate();
}

void DevOtaActivity::onEnter() {
  Activity::onEnter();

  // The boot-hook refusal (showResumeRefusal before the manager activated
  // this activity): keep the FAILED state — no WiFi dance, no record to
  // re-read (the hook already disarmed the unusable one).
  if (resumeRefused) {
    requestUpdate();
    return;
  }

  if (autoResume) {
    // The dance's boot hook records the requested asset name in NVS but does
    // not carry it through the constructor; read it back so fetchManifest()
    // can resume the exact version instead of falling back to the newest.
    dev_ota::pendingInstallGet(pendingVersion, sizeof(pendingVersion));
  }

  // Same WiFi handshake as OtaUpdateActivity: launch the selection
  // subactivity; with saved credentials it auto-connects — the boot-hook
  // resume path depends on that, since nobody watches a picker at boot.
  WiFi.mode(WIFI_STA);
  // [TLS-PROBE] stage sample for the owner's per-phase allocation table
  // (docs/design/2026-11-10-tls-teardown.md §5).
  logMemAt("devota_enter");
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
      refuseDance();
    }
    finish();
    return;
  }

  LOG_DBG("DEVOTA", "[TLS-PROBE] wifi-up free=%u maxBlock=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  {
    RenderLock lock(*this);
    state = CHECKING;
  }
  requestUpdateAndWait();
  fetchManifest();
}

// §16 unwind contract for a REFUSED dev-channel dance (the SD dances have
// SdFirmwareUpdateActivity::unwindDance). Target = the dev slot = the
// firmware the device ran before the dance. This boot is on the worker
// (stable); restoring otadata to the dev slot sends the next boot straight
// back to the pre-dance state. Guards:
//  - manual (non-resume) calls are inert: no dance exists to unwind;
//  - the §16 write latch true means the dev slot may already be partially
//    written — selecting it would boot-loop, so the worker stays selected
//    and the user exits through the FAILED screen (stable carries the slot
//    chooser).
void DevOtaActivity::refuseDance() {
  if (!autoResume) return;
  if (dev_ota::pendingInstallWriteStarted()) {
    LOG_ERR("DEVOTA", "dev slot may already be written; keeping the worker slot");
    dev_ota::pendingInstallClear();
    return;
  }
  if (!ota_boot::switchTo(ota_slots::devSlot())) {
    LOG_ERR("DEVOTA", "unwind: otadata restore to the dev slot failed");
  }
  dev_ota::pendingInstallClear();
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
    if (autoResume) refuseDance();
    {
      RenderLock lock(*this);
      failedDetail = describeDevError(res);
      state = FAILED;
    }
    requestUpdate();
    return;
  }

  if (autoResume) {
    // §16 dance identity: bind the recorded selection (name +, since the
    // consent fix, the arm-time SHA-256) to the freshly fetched manifest.
    // Missing or changed entries refuse — with the record bound as recorded
    // the newest entry must never wear the selection as a fallback (that
    // fallback once flashing an unrelated develop build into the dev slot).
    uint8_t digest[32];
    const bool haveDigest = dev_ota::pendingInstallGetDigest(digest);
    int target = -1;
    const dev_ota::ResumeBind bind =
        dev_ota::bindPendingEntry(entries.items, entries.count, pendingVersion, haveDigest ? digest : nullptr, &target);
    if (bind == dev_ota::ResumeBind::Exact) {
      beginInstall(target);
      return;
    }
    LOG_ERR("DEVOTA", "pending selection not bindable (%d, %s)", static_cast<int>(bind), pendingVersion);
    refuseDance();
    {
      RenderLock lock(*this);
      failedDetail = tr(STR_DEV_RESUME_REFUSED);
      state = FAILED;
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    // §19 grouped display: rebuild the display rows from the fetched
    // manifest (PR group, then dev group). Selection stays an ENTRY index.
    displayRowCount_ = static_cast<int16_t>(dev_ota::pickerRows(entries.items, entries.count, displayRows_));
    selectedIndex = 0;
    listScrollTop_ = 0;
    state = LIST;
  }
  probeListEnterMs_ = millis();
  probeSettleLogged_ = false;
  LOG_DBG("DEVOTA", "[TLS-PROBE] list-enter free=%u maxBlock=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  logMemAt("devota_list");
  requestUpdate();
}

void DevOtaActivity::promptInstall(const int index) {
  if (index < 0 || index >= entries.count) return;
  selectedIndex = index;

  char label[48];
  dev_ota::entryLabel(entries.items[index], label, sizeof(label));
  const char* options[] = {tr(STR_CANCEL), tr(STR_INSTALL)};
  // Default is Cancel: a stray confirm press must never land on Install.
  confirmPopup.show(tr(STR_DEV_INSTALL_PROMPT), label, options, 2, 0, [this, index](const int idx) {
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

  // A manifest entry without a digest can never be verified — refuse before
  // any dance is armed, instead of failing the whole dance in the worker
  // slot after the reboot (installFromManifestEntry refuses these too).
  if (!entries.items[index].hasSha) {
    LOG_ERR("DEVOTA", "manifest entry has no SHA-256; refusing %s", entries.items[index].name);
    {
      RenderLock lock(*this);
      failedDetail = tr(STR_DEV_SHA_MISSING);
      state = FAILED;
    }
    requestUpdate();
    return;
  }

  // Running-dev dance (design §5): the running slot is never a write target,
  // so record the request and reboot into stable; the boot hook finishes the
  // install from there (two reboots total).
  if (ota_slots::sameSlot(ota_slots::runningSlot(), ota_slots::devSlot())) {
    // The completion pass runs in the stable slot: refuse the dance unless
    // that image can consume the pending-install flag (kResumeHookMarker).
    // An auto-resumed record whose completion can never run is disarmed
    // here too: keeping it armed would replay this FAILED screen on every
    // boot until the flag is dead weight.
    if (!ota_slots::imageHasResumeMarker(ota_slots::stableSlot())) {
      LOG_ERR("DEVOTA", "stable slot lacks the resume-hook marker; refusing the dance");
      dev_ota::pendingInstallClear();
      {
        RenderLock lock(*this);
        failedDetail = tr(STR_DEV_STABLE_TOO_OLD);
        state = FAILED;
      }
      requestUpdate();
      return;
    }
    // The pending payload names the asset; the arm-time digest records the
    // exact bytes the user consented to (§16 dance identity). The resume
    // pass binds against both — an entry re-published under the same name
    // with different bytes, or a naming that has since been pruned, is a
    // refusal, never a different build.
    if (!dev_ota::pendingInstallSet(entries.items[index].name, dev_ota::PendingKind::DevChannel) ||
        !dev_ota::pendingInstallSetDigest(entries.items[index].sha256)) {
      // Either failure leaves the device on the dev slot with at most a
      // stale flag; clear it so no boot hook fires on a request nobody can
      // see, and never switch away — the dance never left the ground.
      dev_ota::pendingInstallClear();
      {
        RenderLock lock(*this);
        failedDetail = tr(STR_DEV_INSTALL_FAILED);
        state = FAILED;
      }
      requestUpdate();
      return;
    }
    // Turn otadata to the worker (stable) BEFORE the restart: the dance's
    // whole contract is that the next boot runs the completion pass from the
    // worker. A restart into the same (target) slot would leave a record the
    // boot hook can only classify as stale — it would silently land on Home
    // (owner report 2026-11-09: the update flow never resumed) and the
    // record would be gone. Same ||-chain shape as the other dance arms.
    // Anything that fails disarms: never switch away on a promise nobody can
    // see — the dance never left the ground.
    if (!ota_boot::switchTo(ota_slots::stableSlot())) {
      LOG_ERR("DEVOTA", "dance arm: otadata switch to the stable worker failed; clearing");
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
      if (autoResume) {
        // A resumed install that fails here must not leave the flag armed:
        // every boot would reconnect WiFi and fail again until the battery
        // recovers (the same no-retry-loop rule as the WiFi-failure path).
        refuseDance();
      }
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
    if (autoResume) refuseDance();
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

// Touch path for the hand-rendered states. X4 Pro-class boards expose no
// physical Confirm/Back (both arrive synthesized from the GT911 touch only),
// so without this the version picker cannot be operated at all.
bool DevOtaActivity::handleHandRenderedTouch() {
  if (state != LIST && state != FAILED) return false;
  const freeink::ui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
  if (!snap.touchPressed && !snap.touchHeld && !snap.touchReleased) return false;

  // Route only against a table this state published; while it is stale (the
  // state just changed and the next render has not run yet) the touch is
  // absorbed without action, never misfired.
  freeink::ui::ActionEvent event{};
  if (touchReady) event = touchInteractions.routePublished(snap);

  if (snap.touchPressed) {
    // Move the selection outline to the contacted row right away so the
    // contact is visibly alive before the release.
    const int16_t idx = touchInteractions.activeIndex();
    if (state == LIST && idx >= 0) {
      const freeink::ui::Interaction& hit = touchInteractions.publishedData()[idx];
      if (hit.action == TOUCH_ACTION_ROW && hit.value != selectedIndex) {
        selectedIndex = hit.value;
        requestUpdate();
      }
    }
    return true;
  }
  if (snap.touchHeld) return true;

  // Release: act only when the lift landed on a registered rect (a tap that
  // dragged off a row does nothing, matching OptionPopup's release rule).
  if (event && event.action == TOUCH_ACTION_ROW && state == LIST) {
    promptInstall(event.value);
    haptic_feedback::touchAction();
    return true;
  }
  if (event && event.action == TOUCH_ACTION_DISMISS && state == FAILED) {
    finish();
    haptic_feedback::touchAction();
    return true;
  }
  return true;
}

void DevOtaActivity::loop() {
  // Touch first: LIST/FAILED take taps through the interaction table, the
  // CONFIRMING popup routes its own touches inside OptionPopup::handleInput.
  if (handleHandRenderedTouch()) return;

  // [TLS-PROBE] one-shot settle sample 3 s after the list landed (docs/design/
  // 2026-11-10-tls-teardown.md §5): shows what (if anything) decays after the
  // fetch's TCP closes.
  if (state == LIST && probeListEnterMs_ != 0 && !probeSettleLogged_ &&
      static_cast<long>(millis() - (probeListEnterMs_ + 3000)) >= 0) {
    probeSettleLogged_ = true;
    LOG_DBG("DEVOTA", "[TLS-PROBE] list-settle+3s free=%u maxBlock=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    logMemAt("devota_settle");
  }

  if (state == CONFIRMING) {
    if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
    // Dismissed without a selection: back to the list.
    state = LIST;
    requestUpdate();
    return;
  }

  if (state == LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      // §19: buttons walk ENTRY rows in display order; header rows are
      // skipped (they are not selectable), so the traversal wraps over the
      // same modular cycle as before, minus the separator stops.
      stepSelection(-1);
      keepSelectionVisible();
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      stepSelection(1);
      keepSelectionVisible();
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // wasReleased, matching OptionPopup::handleButtons' confirm edge: the
      // popup also acts on the RELEASE, so opening on wasPressed would let
      // that same press's release fire the default option — one tap would
      // install without a deliberate confirmation.
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

void DevOtaActivity::keepSelectionVisible() {
  const int row = displayRowForEntry(selectedIndex);
  if (row < 0) return;
  if (row < listScrollTop_) {
    listScrollTop_ = static_cast<int16_t>(row);
  } else if (row >= listScrollTop_ + listVisibleRows_) {
    listScrollTop_ = static_cast<int16_t>(row - listVisibleRows_ + 1);
  }
}

int DevOtaActivity::displayRowForEntry(const int entry) const {
  for (int i = 0; i < displayRowCount_; ++i) {
    if (displayRows_[i].entry == entry) return i;
  }
  return -1;
}

bool DevOtaActivity::stepSelection(const int dir) {
  if (displayRowCount_ <= 0) return false;
  int row = displayRowForEntry(selectedIndex);
  if (row < 0) {
    // Off-table (a pruned manifest between renders): fall to the first
    // entry row rather than scrolling nowhere.
    row = -dir;  // so the wrap search below lands on the first entry
  }
  for (int step = 1; step <= displayRowCount_; ++step) {
    int cand = row + dir * step;
    cand = ((cand % displayRowCount_) + displayRowCount_) % displayRowCount_;
    if (displayRows_[cand].entry >= 0) {
      selectedIndex = displayRows_[cand].entry;
      return true;
    }
  }
  return false;
}

void DevOtaActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  // Closed unless the state below re-publishes a touch table that matches
  // what is on screen; loop() routes taps only while it is open.
  touchReady = false;
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
    // Settings-menu row geometry (design docs/design/2026-11-07-dev-ota-slots.md
    // §12): the settings list resolves its rows through
    // FreeInkApp::resolveListProps() — body font, theme touch row padding, then
    // the touch minimums — so its tap bands are finger-sized. Derive the same
    // values from the same shared tokens; the hit bands below reuse exactly
    // this geometry so they cannot drift from the rows.
    // Non-const: the touch publish Frame below binds a DrawTarget&.
    fui::GfxRendererTarget uiTarget = makeUiTarget(renderer);
    const fui::ThemeTokens& tokens = refreshSharedUiThemeTokens(uiTarget);
    const bool touch = uiTarget.deviceContext().hasTouch;
    const int rowFont = uiScaleSpec().bodyFontId;
    const int16_t labelLine = uiTarget.lineHeight(tokens.bodyText.font);
    const int16_t padY = touch ? tokens.listTouchRowPaddingY : tokens.listRowPaddingY;
    int16_t rowHeight = static_cast<int16_t>(labelLine + 2 * (padY < 0 ? 0 : padY));
    if (rowHeight < tokens.listMinRowHeight) rowHeight = tokens.listMinRowHeight;
    if (touch) {
      const int16_t deviceMin = uiTarget.deviceContext().minTouchSize;
      const int16_t touchMin = deviceMin > tokens.minTouchSize ? deviceMin : tokens.minTouchSize;
      if (rowHeight < touchMin) rowHeight = touchMin;
      if (rowHeight < tokens.listTouchMinRowHeight) rowHeight = tokens.listTouchMinRowHeight;
    }
    int16_t rowGap = tokens.listRowGap;
    if (touch && rowGap < tokens.listTouchRowGap) rowGap = tokens.listTouchRowGap;

    // Rows flow down from the running line like the settings list flows from
    // its header. The window is clamped to the space above the button hints:
    // the full CI window (kMaxVersions) cannot fit at this geometry, so the
    // list scrolls around the selection instead of drawing rows into the
    // button-hint band where they would be invisible and untappable. The
    // window counts DISPLAY rows (§19: entry rows + group headers).
    const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + height + rowGap;
    const int listBottom = pageHeight - metrics.buttonHintsHeight;
    const int rowCount_ = displayRowCount_;
    int visibleRows = 0;
    for (int used = 0; visibleRows < rowCount_; ++visibleRows) {
      const int next = used + rowHeight + (visibleRows > 0 ? rowGap : 0);
      if (next > listBottom - listTop) break;
      used = next;
    }
    if (visibleRows <= 0) visibleRows = 1;
    if (listScrollTop_ > rowCount_ - visibleRows) listScrollTop_ = rowCount_ - visibleRows;
    if (listScrollTop_ < 0) listScrollTop_ = 0;
    listVisibleRows_ = static_cast<int16_t>(visibleRows);
    const int lastRow = std::min(rowCount_, listScrollTop_ + visibleRows);

    const int localOffset = localClockOffsetMinutes();
    int y = listTop;
    fui::Rect rowRects[dev_ota::kMaxVersions];
    int touchRows = 0;
    for (int i = listScrollTop_; i < lastRow; ++i) {
      rowRects[touchRows] = fui::Rect{static_cast<int16_t>(side), static_cast<int16_t>(y),
                                      static_cast<int16_t>(pageWidth - side * 2), rowHeight};
      // Center the text's line box in the row like the fui list does.
      const int textY = y + (rowHeight - labelLine) / 2;
      if (displayRows_[i].entry < 0) {
        // Group header (§19): a bold separator above its group — never a
        // selection target and never a touch target (no rowRect above is
        // registered for it).
        renderer.drawText(rowFont, side, textY, pickGroupLabel(displayRows_[i].group), true, EpdFontFamily::BOLD);
        ++touchRows;  // occupied in the flow, just not touchable
        y += rowHeight + rowGap;
        continue;
      }
      const int entry = displayRows_[i].entry;
      char label[48];
      dev_ota::entryLabel(entries.items[entry], label, sizeof(label));
      // The selection outline is the row rect itself: the hit band registered
      // below is this exact rectangle, so the tap target never drifts.
      if (entry == selectedIndex) {
        renderer.drawRect(side, y, pageWidth - side * 2, rowHeight);
      }
      renderer.drawText(rowFont, side, textY, label);
      char compact[16];
      if (dev_ota::compactDevDate(entries.items[entry].date, localOffset, compact, sizeof(compact))) {
        const int dateX = pageWidth - side - renderer.getTextWidth(rowFont, compact);
        // Right-align the compact timestamp; drop it rather than overprint
        // the label on a narrow screen. Measured (UI_12 medium): a 28-char
        // version label is 352 px and the date 132 px, so a 480 px portrait
        // row (440 px usable) cannot hold both — the version wins (identity
        // beats the build instant); landscape's 760 px row keeps the date.
        if (dateX > side + renderer.getTextWidth(rowFont, label) + 4) {
          renderer.drawText(rowFont, dateX, textY, compact);
        }
      }
      ++touchRows;
      y += rowHeight + rowGap;
    }
    // Scroll indicator: a right-edge thumb when the manifest overflows the
    // window. The row rects end one side padding before the screen edge, so
    // the thumb cannot overlap a hit band.
    if (rowCount_ > visibleRows) {
      const int trackH = listBottom - listTop;
      const int thumbH = std::max(trackH * visibleRows / rowCount_, 12);
      const int thumbY = listTop + (trackH - thumbH) * listScrollTop_ / (rowCount_ - visibleRows);
      const int thumbX = pageWidth - metrics.listScrollWidth - metrics.scrollBarRightOffset;
      renderer.drawLine(thumbX, thumbY, thumbX, thumbY + thumbH - 1, metrics.listScrollWidth, true);
    }
    if (touchRows > 0) {
      // Publish on this (render) task; loop() routes against the published
      // generation, never the one being rebuilt — the OptionPopup handshake.
      // Only ENTRY rows register a hit; header rows sit between them and are
      // dead space (a tap there routes no action).
      touchInteractions.beginPublishCycle();
      const fui::DeviceContext device = uiTarget.deviceContext();
      const fui::InputSnapshot noInput{};
      fui::Frame<dev_ota::kMaxVersions + 1> frame(uiTarget, device, noInput, touchInteractions);
      for (int i = listScrollTop_; i < lastRow; ++i) {
        const int entry = displayRows_[i].entry;
        if (entry < 0) continue;
        frame.hit(rowRects[i - listScrollTop_], TOUCH_ACTION_ROW, static_cast<int16_t>(entry), fui::InputTouch);
      }
      touchInteractions.publish();
      touchReady = true;
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == CONFIRMING) {
    renderer.drawText(UI_10_FONT_ID, side, pageHeight / 6, runningLine);
    if (confirmPopup.processRender(renderer, mappedInput)) return;
  } else if (state == INSTALLING) {
    renderer.drawText(UI_10_FONT_ID, side, pageHeight / 6, runningLine);
    if (autoResume) {
      // §16 dance: this boot runs in the worker slot (stable) finishing the
      // dev install — say so instead of silently showing the worker slot's
      // version as if it were an ordinary install.
      renderer.drawText(UI_10_FONT_ID, side, pageHeight / 6 + height + metrics.verticalSpacing, tr(STR_DEV_RESUMING));
    }
    renderer.drawCenteredText(UI_10_FONT_ID, top, installWriting ? tr(STR_DEV_WRITING) : tr(STR_DEV_DOWNLOADING));
    const int barY = top + height + metrics.verticalSpacing;
    GUI.drawProgressBar(renderer, Rect{side, barY, pageWidth - side * 2, metrics.progressBarHeight},
                        installPercent < 0 ? 0 : installPercent, 100);
    // The download/write screens must say WHICH build is being installed
    // (brief 2026-11-09): the picker's row label (stamped version, "pr <num>"
    // / "dev <sha>" tag, or the asset name), the exact entry the user or the
    // dance resumed picked.
    char installTarget[48];
    dev_ota::entryLabel(entries.items[selectedIndex], installTarget, sizeof(installTarget));
    // The theme's drawProgressBar renders the percent centered at
    // bar.y + bar.height + 15 — place the install target one full line BELOW
    // that line so the two never overprint (device report 2026-11-09: the
    // name and the percent collided on the same y).
    renderer.drawCenteredText(UI_10_FONT_ID, barY + metrics.progressBarHeight + 15 + height, installTarget);
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    if (failedDetail != nullptr) {
      renderer.drawCenteredText(UI_10_FONT_ID, top + height + metrics.verticalSpacing, failedDetail);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    // Touch has no Back key on X4 Pro-class boards: the whole screen is the
    // dismiss target.
    touchInteractions.beginPublishCycle();
    {
      fui::GfxRendererTarget target = makeUiTarget(renderer);
      const fui::DeviceContext device = target.deviceContext();
      const fui::InputSnapshot noInput{};
      fui::Frame<dev_ota::kMaxVersions + 1> frame(target, device, noInput, touchInteractions);
      frame.hit(fui::Rect{static_cast<int16_t>(0), static_cast<int16_t>(0), static_cast<int16_t>(pageWidth),
                          static_cast<int16_t>(pageHeight)},
                TOUCH_ACTION_DISMISS, 0, fui::InputTouch);
    }
    touchInteractions.publish();
    touchReady = true;
  }

  renderer.displayBuffer();
}
