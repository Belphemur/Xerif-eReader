#include "DevOtaActivity.h"

#include <BatteryMonitor.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "HalPowerManager.h"
#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "network/OtaBootSwitch.h"
#include "network/OtaSlots.h"
#include "util/MemoryClaimGateDevice.h"

namespace fui = freeink::ui;

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

// The dance's completion pass runs in the stable slot, so that image must
// contain the bootFinalize() resume hook (main.cpp). Images advertise the
// hook through the flash-scannable kResumeHookMarker, not through the
// esp_app_desc_t version (an opaque constant from the prebuilt Arduino core).
// An older stable image boots normally and never consumes the flag, leaving
// the device on a stable slot with no slot chooser. Refusing the dance here
// is the fail-safe direction.

// Compact an ISO-8601 build date ("2026-11-07T14:03:45+00:00") down to
// "11-07 14:03" so the timestamp fits beside the row label. False when the
// field is not the shape the manifest generator emits; the caller then shows
// no date rather than an overflowing one.
bool compactDevDate(const char* iso, char* out, size_t cap) {
  if (!iso || strlen(iso) < 16) return false;
  const auto digit = [](char c) { return c >= '0' && c <= '9'; };
  if (iso[4] != '-' || iso[7] != '-' || iso[10] != 'T' || iso[13] != ':') return false;
  for (const int i : {5, 6, 8, 9, 11, 12, 14, 15}) {
    if (!digit(iso[i])) return false;
  }
  snprintf(out, cap, "%.2s-%.2s %.2s:%.2s", iso + 5, iso + 8, iso + 11, iso + 14);
  return true;
}

// Row/confirm label: the stamped version when the manifest carries one (build
// identity), else the compact "dev <sha>" label, else the raw asset name
// (older manifests, PR entries).
void rowLabel(const OtaDevVersionEntry& entry, char* out, size_t cap) {
  if (entry.version[0]) {
    snprintf(out, cap, "%s", entry.version);
  } else if (!devNameLabel(entry.name, out, cap)) {
    snprintf(out, cap, "%s", entry.name);
  }
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
    listScrollTop_ = 0;
    state = LIST;
  }
  requestUpdate();
}

void DevOtaActivity::promptInstall(const int index) {
  if (index < 0 || index >= entries.count) return;
  selectedIndex = index;

  char label[48];
  rowLabel(entries.items[index], label, sizeof(label));
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

  // Running-dev dance (design §5): the running slot is never a write target,
  // so record the request and reboot into stable; the boot hook finishes the
  // install from there (two reboots total).
  if (ota_slots::sameSlot(ota_slots::runningSlot(), ota_slots::devSlot())) {
    // The completion pass runs in the stable slot: refuse the dance unless
    // that image can consume the pending-install flag (kResumeHookMarker).
    if (!ota_slots::imageHasResumeMarker(ota_slots::stableSlot())) {
      LOG_ERR("DEVOTA", "stable slot lacks the resume-hook marker; refusing the dance");
      {
        RenderLock lock(*this);
        failedDetail = tr(STR_DEV_STABLE_TOO_OLD);
        state = FAILED;
      }
      requestUpdate();
      return;
    }
    if (!dev_ota::pendingInstallSet(entries.items[index].name, dev_ota::PendingKind::DevChannel) ||
        !ota_boot::switchTo(ota_slots::stableSlot())) {
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
      if (autoResume) {
        // A resumed install that fails here must not leave the flag armed:
        // every boot would reconnect WiFi and fail again until the battery
        // recovers (the same no-retry-loop rule as the WiFi-failure path).
        dev_ota::pendingInstallClear();
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
      keepSelectionVisible();
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      selectedIndex = (selectedIndex + 1) % entries.count;
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
  if (selectedIndex < listScrollTop_) {
    listScrollTop_ = selectedIndex;
  } else if (selectedIndex >= listScrollTop_ + listVisibleRows_) {
    listScrollTop_ = selectedIndex - listVisibleRows_ + 1;
  }
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
    // button-hint band where they would be invisible and untappable.
    const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + height + rowGap;
    const int listBottom = pageHeight - metrics.buttonHintsHeight;
    int visibleRows = 0;
    for (int used = 0; visibleRows < entries.count; ++visibleRows) {
      const int next = used + rowHeight + (visibleRows > 0 ? rowGap : 0);
      if (next > listBottom - listTop) break;
      used = next;
    }
    if (visibleRows <= 0) visibleRows = 1;
    if (listScrollTop_ > entries.count - visibleRows) listScrollTop_ = entries.count - visibleRows;
    if (listScrollTop_ < 0) listScrollTop_ = 0;
    listVisibleRows_ = static_cast<int16_t>(visibleRows);
    const int lastRow = std::min(entries.count, listScrollTop_ + visibleRows);

    int y = listTop;
    fui::Rect rowRects[dev_ota::kMaxVersions];
    int rowCount = 0;
    for (int i = listScrollTop_; i < lastRow; ++i) {
      char label[48];
      rowLabel(entries.items[i], label, sizeof(label));
      // The selection outline is the row rect itself: the hit band registered
      // below is this exact rectangle, so the tap target never drifts.
      if (i == selectedIndex) {
        renderer.drawRect(side, y, pageWidth - side * 2, rowHeight);
      }
      // Center the label's line box in the row like the fui list does.
      const int textY = y + (rowHeight - labelLine) / 2;
      renderer.drawText(rowFont, side, textY, label);
      char compact[16];
      if (compactDevDate(entries.items[i].date, compact, sizeof(compact))) {
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
      rowRects[rowCount++] = fui::Rect{static_cast<int16_t>(side), static_cast<int16_t>(y),
                                       static_cast<int16_t>(pageWidth - side * 2), rowHeight};
      y += rowHeight + rowGap;
    }
    // Scroll indicator: a right-edge thumb when the manifest overflows the
    // window. The row rects end one side padding before the screen edge, so
    // the thumb cannot overlap a hit band.
    if (entries.count > visibleRows) {
      const int trackH = listBottom - listTop;
      const int thumbH = std::max(trackH * visibleRows / entries.count, 12);
      const int thumbY = listTop + (trackH - thumbH) * listScrollTop_ / (entries.count - visibleRows);
      const int thumbX = pageWidth - metrics.listScrollWidth - metrics.scrollBarRightOffset;
      renderer.drawLine(thumbX, thumbY, thumbX, thumbY + thumbH - 1, metrics.listScrollWidth, true);
    }
    if (rowCount > 0) {
      // Publish on this (render) task; loop() routes against the published
      // generation, never the one being rebuilt — the OptionPopup handshake.
      touchInteractions.beginPublishCycle();
      const fui::DeviceContext device = uiTarget.deviceContext();
      const fui::InputSnapshot noInput{};
      fui::Frame<dev_ota::kMaxVersions + 1> frame(uiTarget, device, noInput, touchInteractions);
      for (int i = 0; i < rowCount; ++i) {
        frame.hit(rowRects[i], TOUCH_ACTION_ROW, static_cast<int16_t>(listScrollTop_ + i), fui::InputTouch);
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
    renderer.drawCenteredText(UI_10_FONT_ID, top, installWriting ? tr(STR_DEV_WRITING) : tr(STR_DEV_DOWNLOADING));
    const int barY = top + height + metrics.verticalSpacing;
    GUI.drawProgressBar(renderer, Rect{side, barY, pageWidth - side * 2, metrics.progressBarHeight},
                        installPercent < 0 ? 0 : installPercent, 100);
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
