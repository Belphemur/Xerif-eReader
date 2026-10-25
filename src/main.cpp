#include <Arduino.h>
#include <BlePageTurner.h>
#include <BoardConfig.h>
#include <BookFontLoader.h>
#include <BookTypes.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <TrustedTime.h>
#include <WiFi.h>
#include <XteinkDetect.h>
#include <builtinFonts/all.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/task.h>
#if FREEINK_CAP_TOUCH
#include <esp_sntp.h>
#endif
#include <time.h>

#include <atomic>
#include <cstdarg>
#include <cstring>

#include "BoardFeatures.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FrontlightControl.h"
#include "KOReaderCredentialStore.h"
#include "LcpPassphraseStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "OpdsTokenStore.h"
#include "ProgressManager.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "SleepFrameHash.h"
#include "TtfUiFallback.h"
#include "WifiCredentialStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "platform/UsbSerialJtagHandoff.h"
#include "util/ButtonNavigator.h"
#include "util/PluginEvents.h"
#include "util/ScreenshotUtil.h"

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;

// Profiling counters for Phase 1 memory instrumentation. g_psram_free_at_boot
// is the boot-time PSRAM baseline (defined here, extern'd in Logging.h). The
// central-directory and file-open counters live in ZipFile.cpp; do not duplicate
// them as static (internal-linkage) here or they will report 0 forever.
size_t g_psram_free_at_boot = 0;

// Defined in the book namespace so it matches the header's
// `extern BookFontLoader fontLoader;` declaration (design §3.2).
namespace freeink {
namespace book {
BookFontLoader fontLoader;
}  // namespace book
}  // namespace freeink

// TTF Phase 0 wiring proof: a link-time reference to the FreeInkBook engine.
// File scope + gnu::used defeats the optimizer on every build variant (valid
// for a namespace-scope variable on both the RISC-V and Xtensa GCC targets).
// gnu::used alone does not guarantee retention under --gc-sections; gnu::retain
// would, but the S3 framework's Xtensa GCC rejects it with -Wattributes at
// namespace scope, so the LOG_DBG reference in setup() anchors the section
// instead.
using BookStatusProbe = const char* (*)(freeink::book::BookStatus);
[[gnu::used]] static const BookStatusProbe bookStatusProbe = &freeink::book::bookStatusName;

// A wake hold must never become an in-app power-button action.  Boot may continue
// while the button is held; swallow the one release that ends that wake gesture.
static bool wakePowerReleasePending = false;

// Fonts
// 14 pt stays outside OMIT_FONTS: the always-available default reader font.
// (Formerly NotoSerif 14; the built-in family is Atkinson since upstream #4022.)
EpdFont atkinson_hn14RegularFont(&atkinson_hn_14_regular);
EpdFont atkinson_hn14BoldFont(&atkinson_hn_14_bold);
EpdFont atkinson_hn14ItalicFont(&atkinson_hn_14_italic);
EpdFont atkinson_hn14BoldItalicFont(&atkinson_hn_14_bolditalic);
EpdFontFamily atkinson_hn14FontFamily(&atkinson_hn14RegularFont, &atkinson_hn14BoldFont, &atkinson_hn14ItalicFont,
                                      &atkinson_hn14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont atkinson_hn12RegularFont(&atkinson_hn_12_regular);
EpdFont atkinson_hn12BoldFont(&atkinson_hn_12_bold);
EpdFont atkinson_hn12ItalicFont(&atkinson_hn_12_italic);
EpdFont atkinson_hn12BoldItalicFont(&atkinson_hn_12_bolditalic);
EpdFontFamily atkinson_hn12FontFamily(&atkinson_hn12RegularFont, &atkinson_hn12BoldFont, &atkinson_hn12ItalicFont,
                                      &atkinson_hn12BoldItalicFont);
EpdFont atkinson_hn16RegularFont(&atkinson_hn_16_regular);
EpdFont atkinson_hn16BoldFont(&atkinson_hn_16_bold);
EpdFont atkinson_hn16ItalicFont(&atkinson_hn_16_italic);
EpdFont atkinson_hn16BoldItalicFont(&atkinson_hn_16_bolditalic);
EpdFontFamily atkinson_hn16FontFamily(&atkinson_hn16RegularFont, &atkinson_hn16BoldFont, &atkinson_hn16ItalicFont,
                                      &atkinson_hn16BoldItalicFont);
EpdFont atkinson_hn18RegularFont(&atkinson_hn_18_regular);
EpdFont atkinson_hn18BoldFont(&atkinson_hn_18_bold);
EpdFont atkinson_hn18ItalicFont(&atkinson_hn_18_italic);
EpdFont atkinson_hn18BoldItalicFont(&atkinson_hn_18_bolditalic);
EpdFontFamily atkinson_hn18FontFamily(&atkinson_hn18RegularFont, &atkinson_hn18BoldFont, &atkinson_hn18ItalicFont,
                                      &atkinson_hn18BoldItalicFont);

#endif  // OMIT_FONTS

EpdFont smallFont(&atkinson_hn_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10MediumFont(&ubuntu_10_medium);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10MediumFont, &ui10BoldFont);

EpdFont ui12MediumFont(&ubuntu_12_medium);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12MediumFont, &ui12BoldFont);

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
RTC_NOINIT_ATTR uint32_t silentRebootPayload;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;
constexpr uint32_t SILENT_REBOOT_TARGET_SETTINGS = 2;
constexpr uint32_t SILENT_REBOOT_TARGET_JOIN_NETWORK = 3;
constexpr uint32_t SILENT_REBOOT_TARGET_MAX = SILENT_REBOOT_TARGET_JOIN_NETWORK;
constexpr uint32_t SILENT_REBOOT_LIGHT_ON = 1U << 0;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,          // cold boot, flash, panic, or plain reboot
  Silent,          // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  SplashlessWake,  // wake from deep sleep with the splash suppressed by the SD flag
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

// Latched true before enterPowerOff() tears down the current activity: WiFi
// activities call silentRestart() in onExit() to clear heap fragmentation,
// but a power-off is a full rail cut — rebooting here would power the device
// back up against the user's power gesture. Also makes main-loop sleep
// checks inert while the shutdown teardown runs.
static bool powerOffInProgress = false;

#if FREEINK_CAP_TOUCH
static bool finishWifiSessionWithoutRestart() {
  if (!BoardConfig::hasTouch()) return false;

  // A software reset does not cycle externally powered touch/frontlight rails.
  // Shut down the network stack in place so those peripherals retain state.
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  WiFi.mode(WIFI_OFF);
  delay(100);
  LOG_DBG("MAIN", "WiFi stopped without restart on touch device");
  return true;
}
#endif

// A silent restart is internal maintenance, so the light must come back exactly
// as the user left it. SETTINGS.frontlightOn is the saved preference and
// legitimately diverges from the live state (a wake with Restore Light on Wake
// off leaves the light off while the saved "was on" preference is kept), so
// carry the live state across the reboot instead of re-deriving it from
// settings. Cleared with the magic in setup().
static void armSilentReboot(const uint32_t target) {
  silentRebootTarget = target;
  silentRebootPayload = Frontlight.isOn() ? SILENT_REBOOT_LIGHT_ON : 0;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
}

// Returns instead of rebooting when sleep or power-off supersedes the reboot;
// callers keep running in that case.
static void silentRestartTo(const uint32_t target, const char* targetName) {
  if (deepSleepInProgress || powerOffInProgress) return;  // sleeping/powering off supersedes the heap-defrag reboot
#if FREEINK_CAP_TOUCH
  if (finishWifiSessionWithoutRestart()) return;
#endif
  armSilentReboot(target);
  LOG_DBG("MAIN", "Silent restart (target=%s)", targetName);
  // E-ink retains the previous frame until the target's first paint lands
  // (~2-3s). Without an overlay, users don't see the reboot and fire input
  // through to the new activity. On Home, Select on the default
  // selectorIndex=0 opens the most-recent book, looking like a trampoline back
  // to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestart() { silentRestartTo(SILENT_REBOOT_TARGET_HOME, "home"); }

void silentRestartToReader() { silentRestartTo(SILENT_REBOOT_TARGET_READER, "reader"); }

void silentRestartToSettings() { silentRestartTo(SILENT_REBOOT_TARGET_SETTINGS, "settings"); }

void silentRestartToJoinNetwork() {
  if (deepSleepInProgress) return;
#if FREEINK_CAP_TOUCH
  // A software reset would cycle touch/frontlight rails; those boards proceed
  // into Join Network without the fresh-heap reboot (return, don't stop WiFi —
  // this runs on the way *in*, unlike the exit-time silentRestart()).
  if (BoardConfig::hasTouch()) return;
#endif
  armSilentReboot(SILENT_REBOOT_TARGET_JOIN_NETWORK);
  LOG_DBG("MAIN", "Silent restart (target=join-network)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void restartToHomeAfterStorageHandoff() {
  if (deepSleepInProgress || powerOffInProgress) return;  // sleeping/powering off supersedes the storage handoff reboot
  armSilentReboot(SILENT_REBOOT_TARGET_HOME);
  LOG_DBG("MAIN", "Restart after storage handoff (target=home)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  handoffUsbOtgToSerialJtag();
  ESP.restart();
}

void toggleFrontlight() {
#if FREEINK_CAP_FRONTLIGHT
  if (!Frontlight.present()) return;
  const bool lightOn = !Frontlight.isOn();
  Frontlight.setOn(lightOn);
  SETTINGS.frontlightOn = lightOn ? 1 : 0;
  SETTINGS.saveToFile();
  LOG_INF("LIGHT", "Frontlight toggled %s", lightOn ? "on" : "off");
#endif
}

// Run a configured capacitive Home-key action. Returns true when something ran.
// Reader-only actions (Reader Menu) no-op outside the reader; Sleep and
// Screenshot are global. GO_HOME uses goHome() so the home screen re-renders.
void enterDeepSleep(bool fromTimeout);
void enterPowerOff();

// Global Home-key actions run before activity input. Reader-scoped actions are
// consumed by EpubReaderActivity; a single tap is inert on the device home
// screen, while double-tap and hold deliveries remain available there.
bool dispatchGlobalHomeButtonAction() {
  const auto action = mappedInputManager.homeButtonAction();
  if (action == HomeButtonAction::Ignore) return false;
  if (mappedInputManager.homeButtonGesture() == HomeButtonGesture::Tap && activityManager.isOnHomeScreen()) {
    return true;  // consume the frame without delivering the tap action
  }

  switch (action) {
    case HomeButtonAction::ToggleFrontlight:
      toggleFrontlight();
      return false;  // the frame may still carry ordinary input
    case HomeButtonAction::Refresh:
      LOG_DBG("MAIN", "Manual screen refresh triggered");
      if (!activityManager.handleForcedRefresh()) {
        RenderLock lock;
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      return false;
    case HomeButtonAction::Sleep:
      LOG_INF("MAIN", "Sleep triggered by Home-key shortcut");
      enterDeepSleep(false);
      return true;
    case HomeButtonAction::Screenshot: {
      RenderLock lock;
      ScreenshotUtil::takeScreenshot(renderer);
      return true;
    }
    case HomeButtonAction::GoBack:
      // The current activity gets first claim: in the reader this closes an
      // open popup/panel/toolbar sheet instead of leaving the book. At the top
      // of the stack it falls back to the home screen (mirroring the X4's
      // left-edge back swipe).
      if (activityManager.handleBackOnCurrent()) return true;
      activityManager.popActivity();
      return true;
    default:
      return false;
  }
}

// Applies the slide switch's position to the bound function at boot, on each
// change, and when its binding changes; the physical position stays the source
// of truth, as on stock.
void handleSlideSwitch() {
  static int appliedAction = -1;
  static int appliedOn = -1;
  if (!gpio.hasToggleSwitch()) return;
  // Compare the position, not the change edge: transfer loops poll input and
  // clear the edge before this runs.
  const bool on = gpio.isToggleSwitchOn();
  if (appliedAction == SETTINGS.slideSwitchAction && appliedOn == static_cast<int>(on)) return;
  appliedAction = SETTINGS.slideSwitchAction;
  appliedOn = on;
  switch (SETTINGS.slideSwitchAction) {
    case CrossPointSettings::SLIDE_SWITCH_NIGHT_MODE:
      if ((SETTINGS.screenInverted != 0) == on) return;
      SETTINGS.screenInverted = on ? 1 : 0;
      break;
    case CrossPointSettings::SLIDE_SWITCH_TOUCH_CONTROLS:
      if ((SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF) == on) return;
      SETTINGS.touchReaderControls = on ? CrossPointSettings::TOUCH_READER_ON : CrossPointSettings::TOUCH_READER_OFF;
      break;
    default:
      if (!Frontlight.present() || Frontlight.isOn() == on) return;
      Frontlight.setOn(on);
      SETTINGS.frontlightOn = on ? 1 : 0;
      break;
  }
  SETTINGS.saveToFile();
  activityManager.requestUpdate();
  LOG_INF("SWITCH", "Slide switch %s", on ? "on" : "off");
}

bool handleX4ProFrontlightDoubleClick() {
  // Window ownership moved into MappedInputManager (soak-fix7 JFhK): the
  // manager holds the first release out of the served mask until the window
  // resolves, so Sleep/Force-Refresh handlers cannot fire on an ambiguous
  // click. This consumes only the resolved double-click verdict.
  if (mappedInputManager.consumePowerDoubleClick()) {
    toggleFrontlight();
    return true;
  }
  return false;
}

// Bluetooth page turner (lib/BlePageTurner): what the module may ask of this firmware. The module
// owns the radio; loop() tells it what is on screen, and the screen changes and sleep ask it to stop.
namespace pageturner {
std::atomic<bool> holdsCpu{false};

bleturner::Heap heap() {
  const auto h = HalMemory::getInternalHeap();
  return {h.freeBytes, h.largestBlockBytes};
}

// Runs on the radio's start task. Only TRIES the render lock: the render task can hold it through a
// chapter build that is waiting for this very radio.
bool releaseCaches() {
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return false;
  fontCacheManager.releaseSdFontCaches();
  return true;
}

bool deliver(const bleturner::Action action) {
  switch (action) {
    case bleturner::Action::NextPage:
      return activityManager.remoteTurn(true, false);
    case bleturner::Action::PrevPage:
      return activityManager.remoteTurn(false, false);
    case bleturner::Action::NextChapter:
      return activityManager.remoteTurn(true, true);
    case bleturner::Action::PrevChapter:
      return activityManager.remoteTurn(false, true);
    case bleturner::Action::Confirm:
      return activityManager.remoteClick(RemoteButton::Confirm);
    case bleturner::Action::Back:
      return activityManager.remoteClick(RemoteButton::Back);
    case bleturner::Action::ReaderMenu:
      return activityManager.remoteShortcut(true);
    case bleturner::Action::SaveQuote:
      return activityManager.remoteShortcut(false);
    case bleturner::Action::None:
      return false;
  }
  return false;
}

bool yieldForRadio() { return true; }        // the reader keeps no parser alive between pages
bool fileTransferActive() { return false; }  // file transfer is a screen of its own: the radio is off there
void restartIntoBook() { silentRestartToReader(); }

// The radio needs a steady clock while it is up or changing state.
void holdFullSpeed(const bool hold) {
  holdsCpu.store(hold);
  if (hold) powerManager.setPowerSaving(false);
}

void log(const bool error, const char* format, va_list args) {
#ifdef ENABLE_SERIAL_LOG
  if (!error && LOG_LEVEL < 1) return;
  char line[160];
  vsnprintf(line, sizeof(line), format, args);
  logPrintf(error ? "ERR" : "INF", "BLE", "%s", line);
#else
  (void)error;
  (void)format;
  (void)args;
#endif
}

const bleturner::Host host{heap,          releaseCaches, deliver, yieldForRadio, fileTransferActive, restartIntoBook,
                           holdFullSpeed, log,           nullptr};
}  // namespace pageturner

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";
constexpr char SLEEP_FRAME_TMP[] = "/.crosspoint/sleep_frame.bin.tmp";

static void saveSleepFrameBuffer() {
  const uint8_t* fb = renderer.getFrameBuffer();
  const size_t fbSize = renderer.getBufferSize();
  const uint32_t newHash = sleepFrameAdler32(fb, fbSize);

  // Skip the rewrite when the on-disk frame is unchanged (SD wear dedup).
  // A file of any other size (e.g. a pre-trailer file from before an OTA)
  // falls through to the full rewrite and self-heals.
  if (Storage.exists(SLEEP_FRAME_FILE)) {
    HalFile existing;
    if (Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, existing) && existing.size() == fbSize + sizeof(uint32_t) &&
        existing.seek(fbSize)) {
      uint8_t stored[sizeof(uint32_t)];
      if (existing.read(stored, sizeof(stored)) == static_cast<int>(sizeof(stored))) {
        uint32_t storedHash;
        memcpy(&storedHash, stored, sizeof(storedHash));
        if (storedHash == newHash) {
          LOG_DBG("SLP", "Sleep frame unchanged, skipping write");
          return;
        }
      }
    }
  }

  // Stage the full new frame (framebuffer + 4-byte Adler-32 trailer) beside
  // the current file, synced and closed, BEFORE touching the destination.
  // The panel already shows the new sleep screen, so the on-disk baseline
  // must never outlive its match: any detected staging failure (open,
  // short write, failed sync/close) drops the old frame as well, leaving
  // no-frame sleep — the safe direction. Publication is one replace
  // (remove + rename under the storage mutex), so a power cut can only
  // leave the old frame, the complete new temp, or neither — never a torn
  // file. A power cut DURING staging (undetectable afterwards) can leave a
  // stale-but-valid baseline that quick-resume restores; accepted, since
  // the alternative destroys the old frame before the new one exists.
  // Layout: [<framebuffer bytes> | <4-byte Adler-32 of the framebuffer>].
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_TMP, file)) {
    Storage.remove(SLEEP_FRAME_FILE);
    return;
  }
  const size_t fbWritten = file.write(fb, fbSize);
  uint8_t trailer[sizeof(uint32_t)];
  memcpy(trailer, &newHash, sizeof(trailer));
  const size_t trailerWritten = file.write(trailer, sizeof(trailer));
  file.flush();
  const bool synced = file.sync();
  const bool closed = file.close();
  if (fbWritten != fbSize || trailerWritten != sizeof(trailer) || !synced || !closed) {
    // With the trailer appended, up to 3 missing framebuffer bytes would read
    // back as trailer bytes; a failed sync/close could leave torn tmp data.
    // Never install such a file.
    LOG_ERR("SLP", "Bad sleep-frame tmp write (%u/%u + %u/4, sync=%d close=%d)", (unsigned)fbWritten, (unsigned)fbSize,
            (unsigned)trailerWritten, synced, closed);
    Storage.remove(SLEEP_FRAME_TMP);
    Storage.remove(SLEEP_FRAME_FILE);
    return;
  }
  if (!Storage.replaceFile(SLEEP_FRAME_TMP, SLEEP_FRAME_FILE)) {
    LOG_ERR("SLP", "Could not install sleep frame");
    Storage.remove(SLEEP_FRAME_TMP);
    return;
  }
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t fileSize = file.size();
  const bool hasTrailer = fileSize == bufferSize + sizeof(uint32_t);
  if (fileSize != bufferSize && !hasTrailer) {
    // Neither legacy (framebuffer-only) nor trailer format: treat as corrupt.
    file.close();
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  bool valid = bytesRead == bufferSize;
  if (valid && hasTrailer) {
    // Validate the trailer so a corrupted payload (bit rot, USB host edit)
    // is dropped instead of being restored on quick resume.
    uint8_t stored[sizeof(uint32_t)];
    valid = file.read(stored, sizeof(stored)) == static_cast<int>(sizeof(stored));
    if (valid) {
      uint32_t storedHash;
      memcpy(&storedHash, stored, sizeof(storedHash));
      valid = storedHash == sleepFrameAdler32(display.getFrameBuffer(), bufferSize);
    }
  }
  file.close();
  if (!valid) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  // The file is kept: the next saveSleepFrameBuffer() compares hashes against
  // it, and every non-quick-resume path removes it (see the stale-frame kill
  // sites around this file's other Storage.remove calls).
  return true;
}

// Stage the shutdown cover for the power off screen while the book is still
// available; at wake it is only read back and painted. requireTimerEnabled
// gates staging on the auto power off setting for deep-sleep entry; manual
// power off passes false. Only modes whose shutdown screen shows the staged
// cover (COVER, or COVER_CUSTOM from the reader) pay for cover generation;
// renderShutdownScreen falls back per sleepScreen otherwise.
static void stageAutoPowerOffCover(bool requireTimerEnabled) {
  APP_STATE.autoPowerOffCoverBmpPath.clear();
  if (requireTimerEnabled && SETTINGS.getAutoPowerOffMs() == 0) return;
  const auto sleepMode = static_cast<CrossPointSettings::SLEEP_SCREEN_MODE>(SETTINGS.sleepScreen);
  const bool coverShown =
      sleepMode == CrossPointSettings::SLEEP_SCREEN_MODE::COVER ||
      (sleepMode == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM && APP_STATE.lastSleepFromReader);
  if (!coverShown) return;
  if (APP_STATE.openEpubPath.empty()) return;
  std::string coverPath;
  if (SleepActivity::resolveCoverBmpPath(renderer, APP_STATE.openEpubPath, coverPath)) {
    APP_STATE.autoPowerOffCoverBmpPath = std::move(coverPath);
  }
}

// Plugin-event delivery on the way into deep sleep. sleep.enter is delivered
// now — over the live connection, or by bringing WiFi up when a plugin
// subscribes (e.g. fetching a fresh /sleep.bmp so THIS sleep shows it — the
// drain runs before goToSleep() renders the sleep screen). The connect path
// is bounded (join deadline + drain event budget), skipped on low battery,
// and sleep is never blocked on the network: a failed join or delivery just
// sleeps with the previous image and the queued events retry on the next
// drain (at-least-once). The caller's WiFi shutdown tears the radio down
// either way. Deferrable events already queued (reader.exit) ride along in
// the same drain.
static void deliverSleepPluginEvents(const bool allowNetwork) {
  // Activity-owned state must be queued before sleep.enter and before this
  // same-sleep drain. The hook is idempotent with ordinary activity teardown.
  activityManager.prepareForSleep();

  // Sleeping straight out of a book is the common flow, but the reader's own
  // reader.exit only fires later, inside goToSleep() — after this drain. Carry
  // the book and progress on sleep.enter itself so a sync handler bound to it
  // pushes current progress on THIS connection, not the next one.
  pluginevents::Var vars[2];
  size_t varCount = 0;
  char percent[8];
  const ScreenshotInfo info = activityManager.getScreenshotInfo();
  if (info.readerType != ScreenshotInfo::ReaderType::None && !APP_STATE.openEpubPath.empty()) {
    snprintf(percent, sizeof(percent), "%d", info.progressPercent);
    vars[varCount++] = {"book", APP_STATE.openEpubPath.c_str()};
    vars[varCount++] = {"percent", percent};
  }
  pluginevents::emit(pluginevents::Event::SleepEnter, vars, varCount);
  if (!allowNetwork) return;
  if (WiFi.status() == WL_CONNECTED) {
    pluginevents::drain(&renderer);
    return;
  }
  // Any connect-flagged queued event justifies the join, not only
  // sleep.enter: reader.session is queued while reading and delivered on this
  // same sleep, and a progress-sync plugin usually subscribes to it alone.
  if (!pluginevents::wantsConnectAny()) return;
  if (powerManager.getBatteryPercentage() < 20) return;
  // Sleep can be the first WiFi use since boot or deep-sleep wake.
  WIFI_STORE.loadFromFile();
  const auto cred = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!cred) return;

  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  WiFi.mode(WIFI_STA);
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  const unsigned long joinDeadline = millis() + 10000;
  while (WiFi.status() != WL_CONNECTED && millis() < joinDeadline) {
    delay(100);
  }
  if (WiFi.status() == WL_CONNECTED) {
    trustedtime::startSync();  // snap the clock floor while the network is up
    pluginevents::drain(&renderer);
  } else {
    LOG_DBG("MAIN", "Sleep-event WiFi join timed out; deferring delivery");
  }
}

#if FREEINK_DEVICE_PICCO
// Charge strip along the bottom of the retained sleep screen.
static void drawSleepChargeStrip(const bool charging, const uint16_t percent) {
  RenderLock lock;
  const int h = renderer.getLineHeight(SMALL_FONT_ID) + 12;
  const int y = renderer.getScreenHeight() - h;
  renderer.fillRect(0, y, renderer.getScreenWidth(), h, false);
  char text[48];
  snprintf(text, sizeof(text), "%s  %u%%", charging ? tr(STR_CHARGING) : tr(STR_CHARGED), percent);
  renderer.drawCenteredText(SMALL_FONT_ID, y + 6, text);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

// Shows the charge strip while the power HAL holds sleep on USB power, and
// removes it again if USB goes away. Returns true when the power button ended
// the hold.
static bool holdSleepWhileUsbPowered() {
  if (!gpio.isUsbConnected()) return false;
  // Keep the sleep frame so unplugging can remove the strip. 48 KB of PSRAM,
  // freed when the hold ends.
  const size_t frameSize = renderer.getBufferSize();
  auto* const sleepFrame = static_cast<uint8_t*>(heap_caps_malloc(frameSize, MALLOC_CAP_SPIRAM));
  if (sleepFrame) memcpy(sleepFrame, renderer.getFrameBuffer(), frameSize);
  const bool buttonPressed = powerManager.holdWhileUsbPowered(gpio, drawSleepChargeStrip);
  if (sleepFrame) {
    if (!buttonPressed) {
      RenderLock lock;
      memcpy(renderer.getFrameBuffer(), sleepFrame, frameSize);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
    heap_caps_free(sleepFrame);
  }
  return buttonPressed;
}
#endif

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout = false) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  // Sleep may end in a power-off (battery death, latch); persist the clock
  // floor now so a later cold boot resumes from it.
  trustedtime::note();

  // Stop Bluetooth before plugin delivery can start WiFi; a timeout defers delivery.
  deepSleepInProgress = true;
  const bool bluetoothStopped = bleturner::beforeSleep(2000);
  if (!bluetoothStopped) LOG_ERR("SLP", "Bluetooth did not stop; deferring sleep-event delivery");
  deliverSleepPluginEvents(bluetoothStopped);

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  // Every sleep mode leaves a complete retained frame on the e-ink panel. Keep
  // it visible until the first useful reader or home paint replaces it.
  APP_STATE.showBootScreen = false;

  stageAutoPowerOffCover(true);

  APP_STATE.saveToFile();
  // Frontlight changes are persisted at lifecycle boundaries only (never while
  // the user drags a slider): this is the sleep boundary, before the panel is
  // driven down and the SD card is torn down.
  frontlight::persistIfDirty();

  activityManager.goToSleep(fromTimeout);

  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  } else if (Storage.exists(SLEEP_FRAME_FILE)) {
    // A stale Quick Resume frame must not replace the selected sleep screen during wake.
    Storage.remove(SLEEP_FRAME_FILE);
  }

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

#if FREEINK_DEVICE_PICCO
  // The I2C LED driver keeps its state through deep sleep; switch it off here
  // (SETTINGS.frontlightOn keeps the preference for restore-on-wake).
  Frontlight.setOn(false);
  const bool wakeOnHeldButton = holdSleepWhileUsbPowered();
#else
  const bool wakeOnHeldButton = false;
#endif

  halTiltSensor.deepSleep();
  display.deepSleep();
  Storage.prepareForDeepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  uint64_t autoPowerOffUs = 0;
  if (const uint32_t apOffMs = SETTINGS.getAutoPowerOffMs(); apOffMs > 0) {
    autoPowerOffUs = static_cast<uint64_t>(apOffMs) * 1000ULL;
    // Stage the stock-parity shutdown marker at sleep entry so it survives
    // even if the timer wake crashes before the setup()-side re-stage.
    // Non-timer wakes suppress it below before takeLastShutdownKind().
    powerManager.stageAutoPowerOff();
  }
  powerManager.startDeepSleep(gpio, autoPowerOffUs, wakeOnHeldButton);
}

// Manual power off: render the shutdown screen, then cut power. Unlike deep
// sleep there is no wake timer — the next power-button press is a normal cold
// boot (same next-boot state as the auto power off shutdown).
void enterPowerOff() {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for shutdown preparation
  // Snapshot the from-reader context BEFORE teardown: shutdown() empties the
  // activity stack, so isReaderActivity() would always be false afterwards
  // and COVER_CUSTOM / resume context would be lost (Copilot+CodeRabbit,
  // PR #107). Also latch the shutdown FIRST so a WiFi activity's onExit()
  // cannot silentRestart() its way into a reboot instead of a power-off.
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();
  powerOffInProgress = true;
  // Run the outgoing activity's onExit() — enterPowerOff() is reachable from
  // ANY activity, and this is what commits the reader's session state
  // (progress flush, stats commit) exactly like goToSleep() does before
  // replaceActivity() (review B2 / user directive, PR #107). The reader's
  // own exit path flushes progress via the saver.
  activityManager.shutdown();
  // Belt-and-braces: any capture that raced the exit above still lands.
  progressManager.flushNow();
  // COVER_CUSTOM branch reads this; the timer-wake path reads the value
  // persisted at deep-sleep entry instead.
  stageAutoPowerOffCover(false);
  {
    RenderLock lock;
    SleepActivity::renderShutdownScreen(renderer);  // Clears APP_STATE.autoPowerOffCoverBmpPath after paint
  }
  // Persist the post-off state: splashless wake on the next power-button
  // press, and no stale staged cover path in the settings file.
  APP_STATE.showBootScreen = false;
  APP_STATE.saveToFile();
  // Power-off boundary for the same deferred frontlight persistence as sleep.
  frontlight::persistIfDirty();
  // A stale Quick Resume frame must not replace the shutdown screen on the
  // next boot.
  Storage.remove(SLEEP_FRAME_FILE);

  // Same teardown as enterDeepSleep(): the modem power domain must not be
  // held alive across the rail cut.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  // Park the panel BEFORE the sink cuts its rail: the SSD1677 must receive its
  // deep-sleep command while the rail that powers it is still up (same ordering
  // rule as enterDeepSleep(); CodeRabbit finding).
  display.deepSleep();
  LOG_INF("MAIN", "Powering off");
  // Stock-parity shutdown marker (ghidra_poweroff_report.md): record WHY we are
  // powering off in RTC slow RAM; the next boot reports + clears it.
  powerManager.stageUserPowerOff();
  powerManager.enterPowerOffSleep(gpio);  // [[noreturn]]
}

void setupDisplayAndFonts(bool seamless = false) {
#if !FREEINK_MCU_C3
  // C3 resolves its controller in HalGPIO::begin() before SPI claims the
  // display pins. X4 Pro skips that C3-only path, so probe here before
  // display.begin() selects and initializes its panel driver.
  static bool controllerResolved = false;
  if (!controllerResolved) {
    controllerResolved = true;
    if (freeink::applyXteinkDisplayController()) {
      LOG_DBG("MAIN", "Panel controller: UltraChip UC81xx variant detected");
    }
  }
#endif

  display.begin(seamless);
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(ATKINSON_HN_14_FONT_ID, atkinson_hn14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(ATKINSON_HN_12_FONT_ID, atkinson_hn12FontFamily);
  renderer.insertFont(ATKINSON_HN_16_FONT_ID, atkinson_hn16FontFamily);
  renderer.insertFont(ATKINSON_HN_18_FONT_ID, atkinson_hn18FontFamily);
  // NOTOSANS_* are macro aliases of the ATKINSON_HN_* IDs (implicit migration
  // for existing settings), so the registrations above already cover them.
#endif  // OMIT_FONTS
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  // Discover and load SD card fonts
  sdFontSystem.begin(renderer);

  // Native-TTF reader font discovery (design §14.1): gated so PSRAM-less
  // builds never scan or allocate for the native-TTF path. MUST run before
  // the UI-fallback sync — update()'s ensureLoaded() needs a scanned
  // manifest, otherwise it records a failed load and the fallback never
  // registers until the next settings/reader lifecycle event.
#if defined(CROSSPOINT_TTF_READER)
  freeink::book::fontLoader.begin();
  // Select the reader family BEFORE the first ensureLoaded(): the UI-fallback
  // sync below triggers the session's first load, and an unselected loader
  // loads families_[0] — so the boot stack probe measured (and cached its
  // verdict for) a family the reader never opens. Every book open then
  // re-probed into the fragmented post-open heap (MaxAlloc ~45-63 KB < the
  // 57 KB probe stack) and failed closed = unhinted text for the whole
  // session (design 2026-10-22-hint-probe-boot-family.md, soak F1). Mirrors
  // the reader's own selection in TtfBookRuntime::makeLayoutParams.
  freeink::book::fontLoader.selectFamily(
      SETTINGS.readerFontEngine == CrossPointSettings::READER_ENGINE_TTF ? SETTINGS.ttfFontFamilyName : "");
#endif

#if CROSSPOINT_TTF_UI_FALLBACK
  // TTF-backed CJK/script UI fallback (design §14.6): register the active
  // TTF family at the UI sizes when it covers scripts the built-ins lack.
  // Re-synced from the settings screens / reader entry like sdFontSystem.
  freeink::book::ttfUiFallback.update(renderer);
#endif

  LOG_DBG("MAIN", "Fonts setup");
}

// loopTask override: ALL rendering runs on the loop task since the Adobe CFF
// engine (faster; chosen over the old "freetype" CFF engine, SDK PR #29/#30)
// interprets charstrings with an unbounded, multi-KB caller stack. 40KB is
// the EPub-InkPlate-proven size for the Adobe engine on ESP32; 48KB adds
// margin for the reader's ChapterLayout rebuild + paint on top of it.
// Replaces the ActivityManagerRender task (16KB) — net task-stack budget
// unchanged, one fewer task, no cross-task FreeType calls. Rebuilds are
// UX-modal anyway ("Indexing" popup), so blocking the loop is accepted.
SET_LOOP_TASK_STACK_SIZE(49152)

void setup() {
  BoardConfig::holdPowerRails();

#ifdef ENABLE_SERIAL_LOG
#ifdef CROSSPOINT_WAIT_FOR_USB_SERIAL
  // Development builds preserve reliable early CDC logs; release builds let
  // enumeration proceed asynchronously so users do not pay this startup cost.
  delay(250);
#endif
  Serial.begin(115200);
#if LOG_SERIAL_HAS_TX_TIMEOUT
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif
#endif

  HalSystem::begin();
#if defined(BOARD_HAS_PSRAM)
  // The ESP32-S3 Arduino framework's early system-init (priority 99) calls
  // psramInit() during boot, but on some X4 Pro boards it fails silently
  // (esp_psram_init returns an error), leaving spiramDetected=false and
  // psramFound()=false. The result is heap_caps_malloc(MALLOC_CAP_SPIRAM)
  // returns nullptr, which silently disables the ZipFileCache and other
  // PSRAM-backed buffers -- turning a 380 KB DRAM ceiling into a death by
  // a thousand SD re-reads during section building.
  // Retry psramInit() here, after the SDMMC peripheral clocks are up but
  // before any PSRAM allocation is attempted.
  if (!psramFound()) {
    const bool ok = psramInit();
    if (ok) {
      // psramInit() only sets the spiramDetected flag; psramAddToHeap()
      // actually registers PSRAM with the heap allocator so heap_caps_malloc
      // with MALLOC_CAP_SPIRAM can use it.
      psramAddToHeap();
    }
    LOG_INF("SYS", "PSRAM init: %s (free=%u bytes)", ok ? "OK" : "FAILED", ESP.getFreePsram());
  }
#endif
  g_psram_free_at_boot = ESP.getFreePsram();
  logMemAt("boot");
  // checkPanic() clears the watchdog capture marker after a successful SD
  // dump, so retain the boot classification for the later activity route.
  const bool rebootedFromPanic = HalSystem::isRebootFromPanic();

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_MAX) ? silentRebootTarget : 0;
  const bool silentRebootLightOn = isSilentReboot && (silentRebootPayload & SILENT_REBOOT_LIGHT_ON) != 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;
  silentRebootPayload = 0;

  gpio.begin();
  powerManager.begin();
  progressManager.begin();

  // Board TX-power ceiling. It can only be set once WiFi has started, so it is
  // applied on every STA/AP start rather than at each place that brings WiFi up.
  if (BoardConfig::ACTIVE.wifiMaxTxPower != 0) {
    const auto applyTxCap = [](arduino_event_id_t, arduino_event_info_t) {
      const esp_err_t err = esp_wifi_set_max_tx_power(static_cast<int8_t>(BoardConfig::ACTIVE.wifiMaxTxPower));
      if (err != ESP_OK) LOG_ERR("WIFI", "TX power cap failed: %d", err);
    };
    WiFi.onEvent(applyTxCap, ARDUINO_EVENT_WIFI_STA_START);
    WiFi.onEvent(applyTxCap, ARDUINO_EVENT_WIFI_AP_START);
  }

  // Determine the wake cause BEFORE consuming the shutdown marker: if the
  // previous session staged an auto-off marker at sleep entry but the user
  // interrupted the dwell (button wake), suppress the marker so the boot log
  // and downstream routing don't treat a user-interrupted sleep as a clean
  // power-off.
  const auto wakeupReason = gpio.getWakeupReason();
  if (wakeupReason != HalGPIO::WakeupReason::Timer && SETTINGS.getAutoPowerOffMs() > 0) {
    // Non-timer wake with auto-off configured: the staged auto-off marker is a
    // false positive — suppress it before takeLastShutdownKind() reads it.
    powerManager.clearShutdownMarker();
  }

  // Stock-parity shutdown marker (ghidra_poweroff_report.md): if the previous
  // session ended in a power off (manual or auto), RTC slow RAM carries the
  // magic + reason byte. Read + clear exactly once per boot and log it.
  const auto lastShutdown = HalPowerManager::takeLastShutdownKind();
  if (lastShutdown != HalPowerManager::ShutdownKind::None) {
    LOG_INF("MAIN", "Previous session ended in a clean power off (%s)",
            lastShutdown == HalPowerManager::ShutdownKind::AutoOff ? "auto-power-off" : "user power-off");
  }
  // Sample the wake hold now — a click wake is released within milliseconds of
  // boot — but defer the sleep-or-boot decision until SETTINGS is loaded below:
  // click-to-wake is a setting, and an X4 battery power-off cuts all power, so
  // only SD state survives to the next boot.
  //
  // Deep-sleep wakes skip verification: the EXT1/GPIO wake source armed at sleep
  // entry only fires on a real power-button press, but the press is already
  // released by the time verifyPowerButtonWakeup() samples (chip reset +
  // begin() work delay) — so a held-stability check would eat every short click
  // wake. Only the post-power-off (cold-boot) path actually has a held button to
  // verify, and that's the ghost-wake debounce the SETTINGS check covers.
  // The Metalio boots after the button is released, so it skips the check.
  const bool wakeHoldVerified = wakeupReason != HalGPIO::WakeupReason::PowerButton || gpio.wokeFromDeepSleep() ||
                                BoardConfig::isMetalioEInk4() || gpio.verifyPowerButtonWakeup();

  // X4 Pro and X4 Classic both map BTN_UP to GPIO0 — an ESP32-S3 boot strap — so
  // gate recovery on the non-strap Down key (GPIO7) to avoid a stuck-in-recovery loop.
  const auto recoveryButton = (BoardConfig::isX4Pro() || BoardConfig::isX4Classic()) ? MappedInputManager::Button::Down
                                                                                     : MappedInputManager::Button::Up;
  // The Picco has no second app slot to flash into (partitions_picco.csv).
  const bool recoveryFirmwareMode = wakeupReason == HalGPIO::WakeupReason::PowerButton && !BoardConfig::isPaperMono() &&
                                    !BoardConfig::isPicco() && mappedInputManager.isPressed(recoveryButton);

  halTiltSensor.begin();
  halClock.begin();

#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");
#else
  LOG_INF("MAIN", "Device: %s", BoardConfig::ACTIVE.name);
#endif

  // Surface RTC availability, the system epoch the zone resolver will use, and
  // the IANA id / cached offset driving the first render. The epoch here comes
  // straight from HalClock::begin()'s RTC seed, so it is wall-clock time on
  // boards with an RTC and a seconds-since-boot counter on boards without.
  {
    const time_t bootEpoch = time(nullptr);
    char rtcBuf[16] = "--:--";
    uint8_t rh = 0, rm = 0;
    if (halClock.getTime(rh, rm)) {
      snprintf(rtcBuf, sizeof(rtcBuf), "%02u:%02u", rh, rm);
    }
    LOG_INF("CLK", "boot: rtcAvailable=%d rtcLocal=%s sysEpoch=%lld tzId='%s' tzOffsetMin=%d hasBeenSynced=%u",
            halClock.isAvailable() ? 1 : 0, rtcBuf, static_cast<long long>(bootEpoch), SETTINGS.clockTimeZoneId,
            static_cast<int>(SETTINGS.clockTzOffsetMin), static_cast<unsigned>(SETTINGS.clockHasBeenSynced));
  }

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  APP_STATE.loadFromFile();
  const bool isSleepWake = wakeupReason == HalGPIO::WakeupReason::PowerButton;
  const bool isPersistedSleepWake = isSleepWake && !APP_STATE.showBootScreen;

  if (recoveryFirmwareMode) {
    LOG_INF("MAIN", "Recovery firmware mode (%s + POWER held at boot)",
            (BoardConfig::isX4Pro() || BoardConfig::isX4Classic()) ? "DOWN" : "UP");
  }

  // Touch boards default the reader menu to the toolbar overlay instead of the
  // full-screen list. Seeded before the load: fromJson() falls back to the
  // in-memory value only when the file carries no readerMenuStyle key, so a
  // user's saved choice (either style) still wins.
  if (gpio.hasTouch()) {
    SETTINGS.readerMenuStyle = CrossPointSettings::READER_MENU_TOOLBAR;
  }
  SETTINGS.loadFromFile();

  // Auto power off: the dwell timer woke us, so the sleep entry's staged marker
  // was consumed above as a genuine AutoOff shutdown. Re-stage it for the next
  // boot so the downstream timer-wake intercept (render shutdown screen + sink)
  // has the marker to report; the only re-stage that matters is on THIS wake
  // where the auto-off dwell actually elapsed.
  if (wakeupReason == HalGPIO::WakeupReason::Timer && SETTINGS.getAutoPowerOffMs() > 0) {
    powerManager.stageAutoPowerOff();
  }

  RECENT_BOOKS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  OPDS_TOKENS.loadFromFile();
  LCP_PASSPHRASES.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);
  pluginevents::refreshSubscriptions();
  // RTC seeding is handled by halClock.begin() above (seedSystemClockFromRtc):
  // it publishes the battery-backed RTC into the system clock before any
  // reader of time(), and trustedtime::init() below vetoes a lagging RTC by
  // restoring the persisted floor. (Upstream #3879 seeds here via a
  // HalClock::localTime() call that this fork's HalClock does not carry.)
  // Restore the monotonic clock floor before anything reads time() (event
  // timestamps, loan-expiry checks).
  trustedtime::init();
  bleturner::begin(pageturner::host, SETTINGS.ble);

  // TTF Phase 0 wiring proof: the FreeInkBook engine is linked but unused.
  // The reference lives at file scope (bookStatusProbe above); the log line
  // is the visible smoke trace only where logging is compiled in.
  LOG_DBG("MAIN", "Book engine linked: bookStatusName(Ok)=%s, vendor=%s",
          freeink::book::bookStatusName(freeink::book::BookStatus::Ok), freeink::book::vendorVersions());
  // Unconditional live reference: LOG_DBG compiles out below LOG_LEVEL 2, so
  // this volatile read is what anchors the probe section against linker GC
  // ([[gnu::used]] does not protect a section from garbage collection).
  [[maybe_unused]] const volatile BookStatusProbe keepAlive = bookStatusProbe;

  // Brightness and warmth are always restored. A normal wake starts with the
  // light off unless Restore Light on Wake is enabled; silent maintenance
  // reboots replay the live state captured at restart, so they neither go dark
  // nor light up against the user's wake preference.
  const bool restoreLightOn =
      isSilentReboot ? silentRebootLightOn : (SETTINGS.frontlightOn != 0 && SETTINGS.frontlightRestoreOnWake != 0);
  Frontlight.begin(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth, restoreLightOn);

  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      // With Short Power Button Press = Sleep, a single click wakes on any
      // device; otherwise the button must still be held (ghost-wake debounce).
      // Picco: plugging in USB pulses the wake line; power on, as stock does.
      if (!wakeHoldVerified && SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::SLEEP &&
          !(FREEINK_DEVICE_PICCO && gpio.isUsbConnected())) {
        LOG_DBG("MAIN", "Power-button wake not held through verification, sleeping");
        Storage.prepareForDeepSleep();
        powerManager.startDeepSleep(gpio);
      }
      wakePowerReleasePending = true;
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // Most devices return to sleep after a USB-powered cold boot.
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
#if FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_X4CLASSIC || FREEINK_DEVICE_PAPERMONO || FREEINK_DEVICE_EEGO_A4 || \
    FREEINK_DEVICE_METALIO_EINK4 || FREEINK_DEVICE_PICCO
      // X4 Pro must stay awake so USB Serial/JTAG remains available after leaving
      // USB Drive and reconnecting the cable. Paper Mono has no armable GPIO wake
      // (its button is behind the PMIC). Metalio also needs native USB available
      // after a USB-powered boot, as does the Picco. EEGO A4's post-flash reset reads as
      // POWERON (native-USB), so a flash would otherwise be misclassified as a
      // USB-power cold boot and sleep. Sleeping any of these here would strand
      // the device in a USB-replug boot loop (or sleep right after a flash).
      break;
#else
      Storage.prepareForDeepSleep();
      powerManager.startDeepSleep(gpio);
      break;
#endif
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  // Only a verified deep-sleep wake may use the one-shot persisted flag.
  // Otherwise a stale flag could suppress the splash on a cold boot.
  const BootResume resume = isSilentReboot         ? BootResume::Silent
                            : isPersistedSleepWake ? BootResume::SplashlessWake
                                                   : BootResume::Splash;
  bool allowFastInitialReaderRefresh = false;
  bool needsWakeRefresh = false;

  setupDisplayAndFonts(resume != BootResume::Splash);

  // Auto power off: the dwell timer elapsed while in deep sleep. Render the
  // shutdown screen and cut power; the next power-button press is a normal
  // cold boot. Checked before any other wake routing; classification goes
  // through the HAL API (WakeupReason::Timer), not raw IDF sleep calls.
  if (gpio.getWakeupReason() == HalGPIO::WakeupReason::Timer && SETTINGS.getAutoPowerOffMs() > 0) {
    SleepActivity::renderShutdownScreen(renderer);
    // A stale Quick Resume frame must not replace the shutdown screen on the
    // next boot.
    Storage.remove(SLEEP_FRAME_FILE);
    // Park the panel before the sink cuts its rail (same ordering rule as
    // enterDeepSleep(); CodeRabbit finding).
    display.deepSleep();
    // The stock-parity marker was already staged earlier in setup()
    // (stageAutoPowerOff), so only the sink remains.
    powerManager.enterPowerOffSleep(gpio);
  }

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::SplashlessWake:
      // One-shot flag: re-arm the splash for the next ordinary boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a splashless-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (Storage.exists(SLEEP_FRAME_FILE) && loadSleepFrameBuffer()) {
        if (gpio.deviceIsX3()) {
          // begin() clears the X3 controller RAM, so restore the saved frame as
          // the baseline for the first reader paint without refreshing the panel.
          renderer.cleanupGrayscaleWithFrameBuffer();
          allowFastInitialReaderRefresh = true;
        }
      } else {
        // Clean the retained sleep image as part of the first Home paint.
        needsWakeRefresh = true;
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  // Output polarity is resolved per render by ActivityManager (night mode
  // inverts only the reading surfaces), so nothing to restore here.

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (rebootedFromPanic) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_JOIN_NETWORK) {
    // Rebooted on the way *into* File Transfer > Join Network for a fresh heap;
    // resume that flow directly instead of landing on home.
    activityManager.goToJoinNetwork();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_SETTINGS) {
    // Back out of the WiFi rows and the user is where they left off, not on Home.
    activityManager.goToSettings(SettingsSection::Network);
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome(HomeMenuItem::NONE, needsWakeRefresh);
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.openBookProgressPercent = -1;
    APP_STATE.openBookTimeLeftSeconds = 0;
    APP_STATE.openBookPagesLeft = -1;
    APP_STATE.openBookCacheLegacy = false;
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path, allowFastInitialReaderRefresh);
  }

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  allowSleepAt = millis() + 2000;
}

// True when the board can wake the parked GT911 through something other than
// the touch panel itself: any configured physical button GPIO or ADC ladder
// (edges + held-contact paths stay live while the controller sleeps). The
// IMU does NOT qualify: HalTiltSensor::update() early-returns outside the
// reader, so tilt activity cannot reset the idle timer (let alone wake)
// while the device idles on Home. Boards with no button/ladder pins —
// touch-only profiles like M5PaperS3 — must never park or their only input
// dies until a power cycle (kody/coderabbit PR #171).
static bool hasTouchIndependentWakeSource() {
  const BoardConfig::InputPins& in = BoardConfig::ACTIVE.input;
  return in.up >= 0 || in.down >= 0 || in.power >= 0 || in.back >= 0 || in.confirm >= 0 || in.left >= 0 ||
         in.right >= 0 || in.adcLadderPin >= 0;
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.setSharedConfirmPowerShortPressEmitsPower(SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
  mappedInputManager.update();

  // Bluetooth page turner, before the USB drive's early return: a card handed to USB stops the radio
  // before anything else.
  bleturner::Scene scene{};
  // Home counts only for builds whose radio may start there (the S3 profile, RadioPolicy.h).
  const bool homeShown = activityManager.isForegroundHomeShown();
  const bool remoteContext = activityManager.isRemoteReaderContext();
  scene.where = remoteContext ? bleturner::Where::Reader
                : homeShown   ? bleturner::Where::Home
                              : bleturner::Where::Elsewhere;
  scene.visit = activityManager.visit();
  scene.pageShown = homeShown || remoteContext;
  scene.storageBusy = activityManager.requiresExclusiveStorageLoop();
  scene.wifiOn = WiFi.getMode() != WIFI_MODE_NULL;
  const bool remoteActed = bleturner::tick(scene);

  if (activityManager.requiresExclusiveStorageLoop()) {
    // USB Drive handed the raw SD card to the host. Do not run screenshots,
    // sleep, shortcuts, or normal navigation while its filesystem is detached.
    activityManager.loop();
    if (activityManager.preventAutoSleep()) {
      powerManager.setPowerSaving(false);
      delay(10);
    } else {
      // No host is active, so a slower loop is safe. The activity itself times
      // out the raw-storage handoff rather than entering deep sleep detached.
      powerManager.setPowerSaving(true);
      delay(50);
    }
    return;
  }

  // After the exclusive-storage return: it saves settings to the SD card.
  handleSlideSwitch();
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());

  // Under the render lock, so a step never overlaps the themes' gauge reads.
  static bool gaugeCapacityPending = true;
  if (gaugeCapacityPending) {
    RenderLock lock(RenderLock::Mode::Try);
    if (lock.ownsLock()) gaugeCapacityPending = BatteryMonitor::loadDesignCapacity();
  }

  renderer.setFadingFix(SETTINGS.fadingFix);

#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT && defined(CROSSPOINT_MEM_SENTINEL) && \
    CROSSPOINT_MEM_SENTINEL
  // Memory sentinel (FT bring-up, PR #146): the on-device crash (IDLE0 stack
  // canary + corrupted TWDT entry, LoadProhibited at 0x10c/0x1c9) is a silent
  // DRAM corruption whose faulting frames point at IDLE0, not the culprit.
  // Census task stacks (a near-overflow task names itself) and walk the heap
  // block headers so the corrupting allocation is caught while still alive.
  static uint32_t lastMemSentinel = 0;
  if (millis() - lastMemSentinel >= 2000) {
    lastMemSentinel = millis();
    const UBaseType_t taskCount = uxTaskGetNumberOfTasks();
    auto snapshot = makeUniqueNoThrow<TaskStatus_t[]>(taskCount);
    if (snapshot) {
      const UBaseType_t got = uxTaskGetSystemState(snapshot.get(), taskCount, nullptr);
      for (UBaseType_t i = 0; i < got; ++i) {
        const UBaseType_t hwm = snapshot[i].usStackHighWaterMark;
        const char* name = snapshot[i].pcTaskName;
        // IDLE tasks sit near their small stack by design: a constant low HWM
        // is the steady-state baseline, not a leak — the DBG series below
        // tracks it, and a real smash trips the IDLE0 canary instead.
        if (hwm < 256 && (name == nullptr || strncmp(name, "IDLE", 4) != 0)) {
          LOG_ERR("SENT", "Task %s stack HWM=%u (overflow suspect)", name, static_cast<unsigned>(hwm));
        }
        // Victim-stack time series: IDLE0 (core 0) and the render task have
        // both carried wild-write scars during quick-font repros. Log their
        // HWM every tick so the write can be dated against the log phases.
        // FreeRTOS truncates names to configMAX_TASK_NAME_LEN (16), so match
        // the shared prefix instead of the full "ActivityManagerRender".
        if (name != nullptr && (strcmp(name, "IDLE0") == 0 || strncmp(name, "ActivityManager", 15) == 0)) {
          LOG_DBG("SENT", "%s stack HWM=%u", name, static_cast<unsigned>(hwm));
        }
      }
    }
    // print_errors=true: the whole point is naming the smashed block. The
    // full-heap walk is expensive — keep it on a slower cadence than the
    // cheap per-task census so renders aren't starved.
    static uint32_t lastHeapWalk = 0;
    if (millis() - lastHeapWalk >= 10000) {
      lastHeapWalk = millis();
      // print_errors=true: the whole point is naming the smashed block.
      if (!heap_caps_check_integrity_all(true)) {
        LOG_ERR("SENT", "Heap integrity check FAILED (see dump above)");
      }
    }
  }
#endif

  if (Serial && millis() - lastMemPrint >= 10000) {
    const auto heap = HalMemory::getInternalHeap();
    LOG_INF("MEM", "Free: %zu bytes, Total: %zu bytes, Min Free: %zu bytes, MaxAlloc: %zu bytes", heap.freeBytes,
            heap.totalBytes, heap.minFreeBytes, heap.largestBlockBytes);
#ifdef BOARD_HAS_PSRAM
    const auto psram = HalMemory::getPsramHeap();
    LOG_INF("MEM", "PSRAM: Free: %zu bytes, Total: %zu bytes, Min Free: %zu bytes, MaxAlloc: %zu bytes",
            psram.freeBytes, psram.totalBytes, psram.minFreeBytes, psram.largestBlockBytes);
#endif
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  // Shared by BOTH wake paths below (the mapped edge here, the held contact
  // just after it) so one contact cannot bypass the rate limit.
  static unsigned long lastTouchWakeAttempt = 0;
  // Contact seen by the idle trunk's 50 ms slice on the PREVIOUS iteration.
  // Consumed here, one iteration later, so the wake can never block ahead of
  // the update() that samples the button edge.
  static bool sliceSawContact = false;
  // Snapshot masks (soak-fix7): gpio.wasAny* reports only edges NOT yet
  // consumed by the manager's snapshot — after update() that's always
  // nothing, and the inactivity timer would never reset on buttons.
  const bool anyMappedEdge = mappedInputManager.wasAnyPressed() || mappedInputManager.wasAnyReleased();
  if (anyMappedEdge || gpio.wasTouchActivity() || halTiltSensor.hadActivity() || remoteActed ||
      activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
    // GT911 idle sleep (design 2026-09-24-gt911-idle-sleep.md §3 Tier B): the
    // controller is parked after GT911_IDLE_SLEEP_MS of silence; any activity
    // wakes it. Blocks until the controller ACKs again (≤ ~230 ms once).
    // isTouchAsleep() is the SDK's authoritative state — it also covers a
    // failed earlier wake attempt, which retries on the next activity edge
    // instead of leaving the chip parked forever. No-op on boards without a
    // GT911 (SDK returns immediately).
    // A FAILED wake keeps retrying here whenever preventAutoSleep() keeps this
    // branch hot (kody PR #171: rate-limit, 1 s) — each attempt blocks ~200 ms
    // on the poll-task handshake, so per-iteration retries collapse the loop.
    if (gpio.isTouchAsleep() && millis() - lastTouchWakeAttempt >= HalPowerManager::GT911_WAKE_RETRY_MS) {
      lastTouchWakeAttempt = millis();
      gpio.setTouchSleep(false);
    }
  }

  // A contact that broke the idle slice but produced no mapped edge — a button
  // already held when the park fired, or a stuck one — never enters the branch
  // above, so a parked controller would stay parked and every screen touch
  // would stay dead until release-and-press (kody PR #171 rounds 4-5).
  //
  // Deliberately AFTER update() and NOT in the idle trunk: waking from the
  // trunk blocks ~260 ms of handshake BEFORE update() samples the edge, which
  // drops any tap shorter than that — the round-1 regression. By now a fresh
  // press is already committed and owns its own wake above; only a contact
  // that produced nothing reaches this.
  //
  // lastActivityTime is intentionally NOT reset: a permanently stuck contact
  // would otherwise postpone the auto-sleep timeout forever. The inactivity
  // timer belongs to the mapped-edge path only (kody PR #171 round 5). The
  // contact therefore cannot re-park the controller either — the park side
  // refuses to run under a live contact.
  const bool heldContactPending = sliceSawContact;
  sliceSawContact = false;
  if (heldContactPending && !anyMappedEdge && gpio.isTouchAsleep() &&
      millis() - lastTouchWakeAttempt >= HalPowerManager::GT911_WAKE_RETRY_MS) {
    lastTouchWakeAttempt = millis();
    gpio.setTouchSleep(false);
  }

  // Let wake continue as soon as its hold has been verified. The release can
  // arrive after setup, so consume that one input frame rather than making it
  // a page turn, refresh, or other short power-button action.
  // Wake release swallow and the screenshot combo read LEVELS through the
  // manager (logical buttons map 1:1 for Power/Down); no raw BTN reads here.
  if (wakePowerReleasePending && !mappedInputManager.isPressed(MappedInputManager::Button::Power)) {
    wakePowerReleasePending = false;
    // The wake release armed the frontlight double-click window in this
    // tick's update() — its deferred expiry would republish the swallowed
    // release as a short-power action post-wake. The branch's contract is
    // "consume the wake input frame, dispatch nothing" (qodo T2).
    mappedInputManager.cancelPowerClickWindow();
    return;
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (mappedInputManager.isPressed(MappedInputManager::Button::Power) &&
      mappedInputManager.isPressed(MappedInputManager::Button::Down)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (mappedInputManager.isPressed(MappedInputManager::Button::Power)) return;
    if (mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    // The combo's tail Power release was swallowed by the click window
    // (manager armed + stripped it this tick): wasReleased(Power) above is
    // false, so this branch ends the combo. Cancel the window so the armed
    // release cannot resolve as a short-power click at expiry (audit F2).
    mappedInputManager.cancelPowerClickWindow();
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  // Consume the second X4 Pro power-button release so it does not also run a
  // configured short-power action after toggling the frontlight. The window
  // itself (arming, expiry → powerConfirmClickFrame) lives in the manager.
  if (handleX4ProFrontlightDoubleClick()) {
    return;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // A hold that woke the device must be released before it can count as a new
  // in-app long press. Otherwise a user who keeps holding after wake would put
  // the device straight back to sleep once allowSleepAt expires.
  static bool powerReleasedSinceWake = false;
  if (!mappedInputManager.isPressed(MappedInputManager::Button::Power)) powerReleasedSinceWake = true;

  // On X4 Pro with SLEEP, a press still within the click window is a
  // double-click candidate — let it be released and evaluated by the click
  // tracking below instead of powering off on button-down.
  const bool x4ProAwaitingClickWindow =
      mappedInputManager.isPowerClickHoldCandidate() && SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP;

  if (!x4ProAwaitingClickWindow && powerReleasedSinceWake && millis() >= allowSleepAt &&
      mappedInputManager.isPressed(MappedInputManager::Button::Power) &&
      gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't power off
    if (mappedInputManager.isPressed(MappedInputManager::Button::Down)) {
      return;
    }
    LOG_INF("MAIN", "Power button held %lums, powering off", gpio.getPowerButtonHeldTime());
    enterPowerOff();
    // This should never be hit as `enterPowerOff` never returns
    return;
  }

  // Short power click with the Sleep binding: sleep on release. This is the
  // only sleep trigger on Paper Mono (its PMIC reports the button as a
  // one-tick click, so the hold path above cannot fire); on the other boards
  // it complements the hold path, which is now exclusively the power off
  // gesture. allowSleepAt also covers the release of the press that woke the
  // device when wakePowerReleasePending missed it.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP && millis() >= allowSleepAt &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_INF("MAIN", "Sleep triggered by power-button click");
    enterDeepSleep();
    return;
  }

#if FREEINK_DEVICE_PAPERMONO
  // Paper Mono reports the PMIC power button as a one-tick click, so the held
  // path above cannot fire. With the Ignore binding, keep sleeping on the
  // click: the GPIO boards' Ignore short-click no-op has no Paper Mono
  // equivalent because a click is the only power-button event it can report.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::IGNORE && millis() >= allowSleepAt &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    enterDeepSleep();
    return;
  }
#endif

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    if (!activityManager.handleForcedRefresh()) {
      RenderLock lock;
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  // Not while reading: there a repaint is a full page re-render (visible
  // flash, the AA pass re-running, and a frontlight dip under the refresh
  // load); the reader's status bar picks the charging state up on the next
  // page turn instead.
  if (gpio.wasUsbStateChanged() && !activityManager.isReaderActivity()) {
    activityManager.requestUpdate();
  }

  if (dispatchGlobalHomeButtonAction()) {
    return;
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  bool skipLoopDelay = false;
  {
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) {
      // Let rendering advance without treating lock contention as idle.
      delay(10);
      return;
    }
    skipLoopDelay = activityManager.skipLoopDelay();
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (skipLoopDelay) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS && !pageturner::holdsCpu.load()) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      // Idle POF (EPD_IDLE_POF_MS, same lastActivityTime clock as the rest
      // of the trunk): after 30 s without input the EPD booster is powered
      // off while the panel retains its image (e-ink is bistable at zero
      // power) — nothing ever passes turnOffScreen=true today, so the
      // booster otherwise idles on through every pause. Waking costs only
      // the PON (~40 ms, owner trace "8279x4_PON (40 ms)") that precedes
      // every paint anyway, so no user-visible latency is added; the next
      // refresh's powerOnIfNeeded re-powers on both the sync and async
      // paths (single _isScreenOn guard, Uc8279X4Driver.cpp:400).
      // No latch: while the panel is off, powerOffScreen() is a cheap no-op
      // (the _isScreenOn guard short-circuits before any bus traffic), so
      // repeating it per idle iteration is harmless AND self-healing — any
      // paint (sync or async) re-powers the panel, and the next iteration
      // POFs again (kody: a refreshBusy()-observed re-arm misses sync
      // paints, which FreeInkDisplay::refreshBusy() never reports).
      // INFO 'power off (POF, idle hold)' logs only on an actual POF, so
      // the transition stays one line per idle window. refreshBusy()
      // first: never POF mid-refresh; between the check and the call no
      // refresh can start — every paint runs on this loop task (the FIBP
      // worker builds layouts, it never paints). Not USB-gated: USB power
      // is the owner's bench condition (see the park's
      // XERIF_BENCH_TOUCH_PARK escape for the GT911 equivalent).
      if (!display.refreshBusy() && millis() - lastActivityTime >= HalPowerManager::EPD_IDLE_POF_MS) {
        display.powerOffScreen();
      }
      // GT911 idle sleep rides this same idle trunk (DRY: one idle clock, no
      // third timer). Design §3 Tier B entry window ≈3–5 min; skip while USB
      // is attached (serial-monitor sessions must not fight sleep in the log),
      // when the user disabled the feature (SETTINGS.touchIdleSleep), and on
      // boards with no touch-independent wake source — parking those would
      // leave their only input unreachable until a power cycle.
      // BENCH ESCAPE (XERIF_BENCH_TOUCH_PARK): the USB gate makes the park
      // unobservable over serial — the owner's only verification path is the
      // serial monitor on USB power. Define the flag in a gitignored
      // platformio.local.ini to bypass JUST the USB gate on bench builds:
      //   [env:x4pro]
      //   build_flags = -DXERIF_BENCH_TOUCH_PARK=1
      // then `pio run -e x4pro -t upload`. Battery behaviour is unchanged.
#ifndef XERIF_BENCH_TOUCH_PARK
#define XERIF_BENCH_TOUCH_PARK 0
#endif
      const bool usbGate = !gpio.isUsbConnected() || XERIF_BENCH_TOUCH_PARK;
      // Parked state comes straight from the SDK (isTouchAsleep), so a failed
      // entry is retried after the backoff window instead of latched awake
      // until the next activity edge.
      static unsigned long lastParkAttempt = 0;
      if (SETTINGS.touchIdleSleep && hasTouchIndependentWakeSource() && !gpio.isTouchAsleep() &&
          millis() - lastActivityTime >= HalPowerManager::GT911_IDLE_SLEEP_MS && usbGate &&
          // Never park under a live contact: the held-contact wake above does
          // not reset the inactivity timer, so a parked controller would be
          // woken again on the next iteration and park/wake-thrash forever.
          // Mirrors the SDK's own refusal to park with a live contact
          // (InputManager::enterGt911Sleep, touchPressed/touchHomeKeyDown).
          !gpio.anyPhysicalButtonHeld() && millis() - lastParkAttempt >= HalPowerManager::GT911_PARK_RETRY_MS) {
        lastParkAttempt = millis();
        gpio.setTouchSleep(true);
      }
      // Sleep in short slices and wake the poll as soon as a button contact closes.
      // InputManager commits a press only when two consecutive polls agree, so a
      // press shorter than one 50 ms sleep could land in a single sample and be lost.
      const unsigned long idleStart = millis();
      while (millis() - idleStart < 50) {
        delay(10);
        if (gpio.anyPhysicalButtonHeld()) {
          // Button contact during the slice: record it and break out so the
          // trigger press is committed by the normal update() path at the TOP
          // of the next iteration (kody PR #171 round 2), not blocked behind
          // the ~200 ms wake handshake. The wake is the post-update check
          // there; nothing blocks in the trunk itself.
          // anyPhysicalButtonHeld(), not rawInputActive() alone: rawInputActive()
          // is the fresh hardware read (ADC ladder + power pin) so sync builds
          // (no poll task) still see a live sample every slice, but it does not
          // see digital-only nav-key banks that some variants expose via the
          // debounce latch — anyPhysicalButtonHeld() ORs both.
          sliceSawContact = true;
          break;
        }
      }
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
