#include "ClockSyncActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalTimeZone.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ClockFormat.h"
#include "util/TlsHeapGuard.h"

void ClockSyncActivity::onEnter() {
  Activity::onEnter();
  state = SYNCING;
  syncedTime[0] = '\0';

  if (WiFi.status() == WL_CONNECTED) {
    requestUpdate();
    return;
  }

  shouldTearDownWifiOnExit = true;
  launchWifiSelection();
}

void ClockSyncActivity::onExit() {
  Activity::onExit();

  if (shouldTearDownWifiOnExit && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void ClockSyncActivity::launchWifiSelection() {
  LOG_INF("CLK", "Manual sync requested without WiFi, launching WiFi selection");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void ClockSyncActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    LOG_INF("CLK", "WiFi selection cancelled before manual clock sync");
    finish();
    return;
  }

  state = SYNCING;
  requestUpdate();
}

void ClockSyncActivity::runSync() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_INF("CLK", "Manual sync requested but WiFi is not connected after selection");
    state = NO_WIFI;
    requestUpdate();
    return;
  }

  LOG_INF("CLK", "NTP sync: free=%u maxblock=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  const bool ok = halClock.syncFromNTP();
  LOG_INF("CLK", "NTP sync done ok=%d free=%u", ok ? 1 : 0, static_cast<unsigned>(ESP.getFreeHeap()));
  if (!ok) {
    state = FAILED;
    requestUpdate();
    return;
  }

  // Mark as synced so the auto-sync hook stops firing on future WiFi connects.
  SETTINGS.clockHasBeenSynced = 1;

  // Re-detect the time zone from this device's public IP (e.g. after travel).
  // Store it even on manual sync; falls back to the previous/UTC display on
  // failure. The fetch is plain HTTP (no TLS on this path); the heap-guard
  // skip stays as defense in depth and the sync still reports success
  // (docs/design/2026-10-26-x3-wifi-connect-abort.md).
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  LOG_INF("CLK", "tz detect: free=%u maxblock=%u", static_cast<unsigned>(freeHeap),
          static_cast<unsigned>(maxAllocHeap));
  auto tz = freeink::TimeZoneInfo{};
  if (tlsheap::verdict(freeHeap, maxAllocHeap) == tlsheap::Verdict::Skip) {
    LOG_INF("CLK", "tz detect skipped: low heap (free=%u maxblock=%u)", static_cast<unsigned>(freeHeap),
            static_cast<unsigned>(maxAllocHeap));
    // Mark the detect owed so the next WiFi connect retries it (manual sync
    // must not leave first-time users on UTC either).
    SETTINGS.clockTzPending = 1;
  } else {
    tz = freeink::detectTimeZoneFromIp();
    SETTINGS.clockTzPending = 0;  // attempted once: valid or not, the retry is consumed
  }
  tzDetected_ = tz.valid;
  if (tz.valid) {
    strncpy(SETTINGS.clockTimeZoneId, tz.id, sizeof(SETTINGS.clockTimeZoneId) - 1);
    SETTINGS.clockTimeZoneId[sizeof(SETTINGS.clockTimeZoneId) - 1] = '\0';
    SETTINGS.clockTzOffsetMin = static_cast<int16_t>(tz.offsetMin);
    SETTINGS.clockTzIsDst = tz.isDst ? 1 : 0;
  }
  SETTINGS.saveToFile();

  // Full timestamp for the confirmation: the system clock was just set by SNTP,
  // so it carries seconds the RTC-backed status-bar clock does not.
  ClockFormat::formatTimestamp(syncedTime, sizeof(syncedTime), time(nullptr), SETTINGS.clockEffectiveOffsetMin(),
                               SETTINGS.clockTimeZoneId, SETTINGS.clockFormat == 1);
  state = SUCCESS;
  requestUpdate();
}

void ClockSyncActivity::loop() {
  if (state == SYNCING) {
    // First-tick: render the "Syncing..." screen, then perform the (blocking) sync.
    // requestUpdateAndWait below forces the render before we block on WiFi.
    requestUpdateAndWait();
    runSync();
    return;
  }

  int x = 0;
  int y = 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
}

void ClockSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CLOCK_SYNC));

  const int midY = pageHeight / 2;

  switch (state) {
    case SYNCING:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_CLOCK_SYNCING));
      break;
    case SUCCESS: {
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_CLOCK_SYNC_OK), true, EpdFontFamily::BOLD);
      if (syncedTime[0] != '\0') {
        // Worst case is 60 bytes: STR_CURRENT_TIME is 28 bytes in Kazakh (the
        // longest translation; UTF-8 Cyrillic/Arabic run 2 bytes per letter),
        // plus a separator and a full "2026-09-03 12:47:42 PM -04:00" (30).
        char line[80];
        snprintf(line, sizeof(line), "%s %s", tr(STR_CURRENT_TIME), syncedTime);
        renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, line);
      }
      if (!tzDetected_) {
        renderer.drawCenteredText(UI_10_FONT_ID, midY + 30, tr(STR_TIMEZONE_DETECT_FAILED));
      }
      break;
    }
    case NO_WIFI:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_CLOCK_SYNC_NO_WIFI), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, tr(STR_CLOCK_SYNC_NO_WIFI_HINT));
      break;
    case FAILED:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_CLOCK_SYNC_FAIL), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, tr(STR_CHECK_SERIAL_OUTPUT));
      break;
  }

  if (state != SYNCING) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
