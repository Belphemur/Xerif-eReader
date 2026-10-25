#include "ActivityManager.h"

#include <BlePageTurner.h>
#include <BoardConfig.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalPowerManager.h>
#include <Memory.h>
#include <freertos/task.h>

#include <algorithm>
#include <ctime>

#include "Activity.h"
#include "CrossPointSettings.h"
#include "HapticFeedback.h"
#include "MemSentinel.h"
#include "OpdsServerStore.h"
#ifdef READING_STATS_ENABLED
#include "activities/reader/ReadingStatsMenuActivity.h"
#include "settings/GlobalStatsActivity.h"
#endif
#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "components/HeaderBackTapTarget.h"
#include "home/CrashActivity.h"
#include "home/FileBrowserActivity.h"
#include "home/HomeActivity.h"
#include "library/LibraryListActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "network/UsbDriveActivity.h"
#include "plugins/PluginCatalogActivity.h"
#include "reader/ReaderActivity.h"
#include "reader/RemoteTurnGate.h"
#include "settings/OpdsServerListActivity.h"
#include "settings/SettingsListActivity.h"
#include "util/BmpViewerActivity.h"
#include "util/FrontlightPanelActivity.h"
#include "util/FullScreenMessageActivity.h"

bool ActivityManager::isOnHomeScreen() const { return currentActivity && currentActivity->isHomeActivity(); }

bool ActivityManager::handleBackOnCurrent() {
  return currentActivity && pendingAction == PendingAction::None && currentActivity->handleHomeGesture();
}

ActivityManager::ActivityManager(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : renderer(renderer), mappedInput(mappedInput), renderingMutex(xSemaphoreCreateMutex()) {
  assert(renderingMutex != nullptr && "Failed to create rendering mutex");
  stackActivities.reserve(10);
}

ActivityManager::~ActivityManager() { assert(false); /* should never be called */ }

void ActivityManager::begin() {
  // All rendering runs on the Arduino loop task: the Adobe CFF engine needs
  // an unbounded, multi-KB caller stack (see the 48KB loopTask override in
  // main.cpp), and rebuilds are UX-modal anyway (the "Indexing" popup), so a
  // second render task only doubled the stack bill. Renders that must happen
  // while the main task is blocked in a tight loop (OTA/SD-flash progress)
  // are rendered synchronously from the progress callback — the flash driver
  // critical-sections its writes, so display work between writes is safe.
  mainTaskHandle = xTaskGetCurrentTaskHandle();
}

void ActivityManager::performRender() {
  // Acquire the lock before reading currentActivity so out-of-loop callers
  // (requestUpdate(true) from progress callbacks) stay serialized with any
  // RenderLock scope the activity itself holds.
  RenderLock lock;
  if (currentActivity) {
    HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
    // Night mode is a global output polarity applied to every activity.
    // The sleep screen forces normal polarity itself (SleepActivity).
    display.setInverted(SETTINGS.screenInverted != 0);
    const uint32_t renderingVisit = screenVisit;
    currentActivity->render(std::move(lock));
    // The Bluetooth page turner gates on "the home screen's own frame is on
    // the panel" (isForegroundHomeShown); the fork renders inline on the main
    // task instead of a render task, so the visit bookkeeping lives here.
    paintedVisit.store(renderingVisit, std::memory_order_release);
#if defined(CROSSPOINT_TTF_READER)
    // Render-stack and heap telemetry live in the book-render path
    // (EpubReaderActivity's TTF finish, where the deep frames actually run);
    // this generic wrapper only runs the sentinel.
    memSentinelCheck("main render");
#endif
  }
}

void ActivityManager::loop() {
  if (mappedInput.consumeSuppressedRelease()) return;

  if (currentActivity && currentActivity->requiresExclusiveStorageLoop()) {
    currentActivity->loop();
    // An exclusive-storage activity must restart rather than navigate away:
    // processing a pending action here could re-enable filesystem users while
    // the USB host still owns the raw SD card.
    if (requestedUpdate.exchange(false)) {
      performRender();
    }
    return;
  }

  if (currentActivity) {
    if (!currentActivity->isHomeActivity() && mappedInput.wasHomeGesture()) {
      if (currentActivity->handleHomeGesture()) {
        return;
      }
      goHome();
      return;
    }

    // Tap-first control-center entry: a tap on the status-bar band of the
    // top-level tab screens opens it, mirroring the top-edge swipe (which some
    // panels' etched glass makes unreliable). The reader keeps its clean page
    // (no status bar there to tap). Touch boards only, like the swipe itself.
    bool statusBarTap = false;
    if (mappedInput.hasTouch() && (currentActivity->name == "Home" || currentActivity->name == "FileBrowser" ||
                                   currentActivity->name == "Settings" || currentActivity->name == "SettingsList" ||
                                   currentActivity->name == "NetworkModeSelection")) {
      int tx = 0;
      int ty = 0;
      // The header back button shares this band; its taps stay Back.
      statusBarTap = mappedInput.wasScreenTapped(tx, ty) && ty < 44 && !HeaderBackTapTarget::contains(tx, ty);
    }
    // Only boards whose frontlight was detected get the panel.
    if (Frontlight.present() && currentActivity->name != "FrontlightPanel" &&
        (statusBarTap || mappedInput.wasLightPanelGesture())) {
      haptic_feedback::touchAction();
      pushActivity(std::make_unique<FrontlightPanelActivity>(renderer, mappedInput));
      return;
    }

    // Side-edge frontlight drag, before the activity's own input so the frames
    // it consumes never page-turn or tap. The activity's policy vetoes screens
    // whose edge gestures would conflict (reader chrome, the control center).
    if (SETTINGS.frontlightSideGestures && Frontlight.present() &&
        frontlightSwipe.update(mappedInput, renderer, currentActivity->allowsFrontlightSwipe())) {
      return;
    }

    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    currentActivity->loop();

    // Keep the header clock (BaseTheme::drawHeader) current: one redraw per
    // wall-clock minute. Reader screens are skipped — they render their own
    // status bar rather than the header, so a minute cadence there would only
    // add an e-ink refresh on top of every page turn.
    if (SETTINGS.headerClock != 0 && halClock.isAvailable() && !currentActivity->isReaderActivity()) {
      static time_t lastHeaderClockMinute = 0;
      const time_t nowMinute = time(nullptr) / 60;
      if (nowMinute != lastHeaderClockMinute) {
        lastHeaderClockMinute = nowMinute;
        requestUpdate();
      }
    }
  }

  while (pendingAction != PendingAction::None) {
    // The page turner's radio gives its heap back before the next screen allocates. Its teardown
    // can span loop passes and runs outside the render lock.
    if (pendingAction != PendingAction::ReplaceForSleep &&
        !bleturner::beforeScreenChange(0, pendingActivity && pendingActivity->isReaderActivity())) {
      return;
    }
    if (pendingAction == PendingAction::Pop) {
      RenderLock lock;

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction = PendingAction::None;
        continue;
      }

      ++screenVisit;
      ActivityResult pendingResult = std::move(currentActivity->result);

      // An activity popped without calling setResult() leaves its result in
      // the default-constructed state (hasResult=false). A parent handler that
      // reads a typed alternative via std::get<T>(data) would throw
      // std::bad_variant_access and abort; treat that case as cancelled so the
      // handler takes its safe branch. An explicit setResult() — including an
      // empty-data Confirm from ConfirmationActivity — is never rewritten.
      if (normalizeActivityResult(pendingResult)) {
        LOG_DBG("ACT", "Popped activity '%s' returned default-constructed result; treating as cancelled",
                currentActivity->name.c_str());
      }

      // Destroy the current activity
      exitActivity(lock);
      pendingAction = PendingAction::None;

      if (stackActivities.empty()) {
        LOG_DBG("ACT", "No more activities on stack, going home");
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

      } else {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        LOG_DBG("ACT", "Popped from activity stack, new size = %zu", stackActivities.size());
        // Handle result if necessary
        if (currentActivity->resultHandler) {
          LOG_DBG("ACT", "Handling result for popped activity");

          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        }

        // The handler above may have started a new activity, in which case it
        // already requested an update — skip the redundant one. cppcheck flags
        // the condition as always-true because it cannot see into the opaque
        // handler, but the handler is exactly what can change pendingAction.
        // cppcheck-suppress knownConditionTrueFalse
        if (pendingAction == PendingAction::None) {
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingActivity) {
      // Current activity has requested a new activity to be launched
      RenderLock lock;

      if (pendingAction == PendingAction::Replace || pendingAction == PendingAction::ReplaceForSleep) {
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
      } else if (pendingAction == PendingAction::Push) {
        // Move current activity to stack
        stackActivities.push_back(std::move(currentActivity));
        // The parent's header back rect must not route taps on the pushed
        // screen (which may draw no header of its own).
        HeaderBackTapTarget::clear();
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
      }
      ++screenVisit;
      pendingAction = PendingAction::None;
      currentActivity = std::move(pendingActivity);

      lock.unlock();  // onEnter may acquire its own lock
      currentActivity->onEnter();

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }

  if (requestedUpdate.exchange(false)) {
    // The loop task renders inline now — no render task to notify.
    performRender();
  }
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    currentActivity->onExit();
    currentActivity.reset();
  }
  // The outgoing screen's header back button must not eat taps on the next
  // screen; the next header draw re-records it.
  HeaderBackTapTarget::clear();
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  mappedInput.resetHomeButtonInput();
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    currentActivity = std::move(newActivity);
    currentActivity->onEnter();
  }
}

void ActivityManager::goToFileTransfer() {
  replaceActivity(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput));
}

void ActivityManager::goToJoinNetwork() {
  // Post heap-defrag reboot: enter the web-server activity straight in Join
  // Network mode (skips mode selection, does not reboot again).
  replaceActivity(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput, /*startInJoinNetwork=*/true));
}

void ActivityManager::goToUsbDrive() {
#if FREEINK_CAP_USB_MSC
  auto activity = makeUniqueNoThrow<UsbDriveActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("ACT", "OOM: USB Drive activity");
    return;
  }
  replaceActivity(std::move(activity));
#else
  LOG_ERR("ACT", "USB Drive requested in a build without USB Drive capability");
#endif
}

void ActivityManager::goToSettings(const SettingsSection openSection) {
  replaceActivity(std::make_unique<SettingsListActivity>(renderer, mappedInput, openSection));
}

#ifdef READING_STATS_ENABLED
void ActivityManager::goToGlobalStats() {
  auto activity = makeUniqueNoThrow<GlobalStatsActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("ACT", "OOM: GlobalStatsActivity");
    return;
  }
  pushActivity(std::move(activity));
}

void ActivityManager::goToReadingStats(std::optional<BookReadingStats> bookStats, std::string bookTitle,
                                       std::string bookCachePath, std::string bookAuthor) {
  auto activity =
      makeUniqueNoThrow<ReadingStatsMenuActivity>(renderer, mappedInput, std::move(bookStats), std::move(bookTitle),
                                                  std::move(bookCachePath), std::move(bookAuthor));
  if (!activity) {
    LOG_ERR("ACT", "OOM: ReadingStatsMenuActivity");
    return;
  }
  pushActivity(std::move(activity));
}
#endif

void ActivityManager::goToFileBrowser(std::string path) {
  replaceActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToLibrary() {
  auto activity = makeUniqueNoThrow<LibraryListActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("ACT", "OOM: library activity");
    return;
  }
  replaceActivity(std::move(activity));
}

void ActivityManager::goToBrowser() {
  const auto& servers = OPDS_STORE.getServers();
  // Skip the server picker when there's only one server configured
  if (servers.size() == 1) {
    replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, servers[0]));
  } else {
    replaceActivity(std::make_unique<OpdsServerListActivity>(renderer, mappedInput, true));
  }
}

void ActivityManager::goToPlugins(bool showOpds) {
  replaceActivity(std::make_unique<PluginCatalogActivity>(renderer, mappedInput, showOpds, /*rootMode=*/true));
}

void ActivityManager::goToReader(std::string path, const bool allowFastInitialRefresh) {
  if (path.empty()) {
    goToFileBrowser("/");
    return;
  }

  if (FsHelpers::hasImageExtension(path)) {
    auto activity = makeUniqueNoThrow<BmpViewerActivity>(renderer, mappedInput, std::move(path));
    if (!activity) {
      LOG_ERR("ACT", "OOM: bitmap viewer activity");
      return;
    }
    replaceActivity(std::move(activity));
    return;
  }

  auto activity = ReaderActivity::create(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  if (activity) {
    replaceActivity(std::move(activity));
  }
}

void ActivityManager::goToSleep(bool fromTimeout) {
  replaceActivity(std::make_unique<SleepActivity>(renderer, mappedInput, fromTimeout));
  if (pendingAction == PendingAction::Replace) pendingAction = PendingAction::ReplaceForSleep;
  loop();  // Important: sleep screen must be rendered immediately, the caller will go to sleep right after this returns
}

// Manual power off (review B2 / user directive, PR #107): run the current
// activity's onExit() exactly like goToSleep() does — enterPowerOff() is
// reachable from ANY activity, and the outgoing activity's onExit() is what
// commits its session state (reader progress/stats, settings staged for
// save) before the rail is cut. The outgoing activity is destroyed, its slot
// cleared, and no new activity is launched: the shutdown screen below is the
// last thing on the panel.
void ActivityManager::shutdown() {
  if (pendingActivity) {
    // A transition was already staged (e.g. the user held power while a
    // screen was opening): drop it, we are shutting down.
    pendingActivity.reset();
    pendingAction = PendingAction::None;
  }
  if (currentActivity) {
    RenderLock lock;
    exitActivity(lock);
  }
  while (!stackActivities.empty()) {
    stackActivities.back()->onExit();
    stackActivities.pop_back();
  }
}

void ActivityManager::goToBoot() { replaceActivity(std::make_unique<BootActivity>(renderer, mappedInput)); }

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivity(std::make_unique<FullScreenMessageActivity>(renderer, mappedInput, std::move(message), style));
}

void ActivityManager::goHome(HomeMenuItem initialMenuItem, bool cleanInitialRefresh) {
  if (initialMenuItem == HomeMenuItem::NONE && currentActivity) {
    const auto& activityName = currentActivity->name;
    if (activityName == "FileBrowser") {
      initialMenuItem = HomeMenuItem::FILE_BROWSER;
    } else if (activityName == "Library") {
      initialMenuItem = HomeMenuItem::LIBRARY;
    } else if (activityName == "OpdsBookBrowser" || activityName == "PluginCatalog") {
      initialMenuItem = HomeMenuItem::OPDS_BROWSER;
    } else if (activityName == "CrossPointWebServer") {
      initialMenuItem = HomeMenuItem::FILE_TRANSFER;
    } else if (activityName == "Settings" || activityName == "SettingsList") {
      initialMenuItem = HomeMenuItem::SETTINGS_MENU;
    }
  }
  replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, initialMenuItem, cleanInitialRefresh));
}
void ActivityManager::goToCrashReport() { replaceActivity(std::make_unique<CrashActivity>(renderer, mappedInput)); }

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  mappedInput.resetHomeButtonInput();
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
}

void ActivityManager::popActivity() {
  mappedInput.resetHomeButtonInput();
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  pendingAction = PendingAction::Pop;
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::requiresExclusiveStorageLoop() const {
  return currentActivity && currentActivity->requiresExclusiveStorageLoop();
}

bool ActivityManager::isReaderActivity() const {
  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity->isReaderActivity(); }) ||
         (currentActivity && currentActivity->isReaderActivity());
}

bool ActivityManager::isCurrentActivityReader() const { return currentActivity && currentActivity->isReaderActivity(); }

// Bluetooth page turner (lib/BlePageTurner): the book in front, whether it has painted a page, and
// a page or chapter turn from a remote. False when no book in front took the turn.
bool ActivityManager::isForegroundReader() const {
  return pendingAction == PendingAction::None && currentActivity && currentActivity->isReaderActivity();
}

bool ActivityManager::isRemoteReaderContext() const {
  return pendingAction == PendingAction::None && currentActivity && currentActivity->acceptsRemoteInput();
}

bool ActivityManager::remoteClick(const RemoteButton button) {
  if (!isRemoteReaderContext() || RenderLock::peek() || requestedUpdate.load()) return false;
  return mappedInput.injectRemoteClick(button);
}

bool ActivityManager::isForegroundHomeShown() const {
  return pendingAction == PendingAction::None && currentActivity && currentActivity->name == "Home" &&
         paintedVisit.load(std::memory_order_acquire) == screenVisit;
}

bool ActivityManager::remoteShortcut(const bool readerMenu) {
  if (!isForegroundReader()) return false;
  auto* reader = static_cast<ReaderActivity*>(currentActivity.get());
  if (!remoteTurnAccepted(reader->pageRendered.load(std::memory_order_acquire), RenderLock::peek(),
                          reader->inputOverPage())) {
    return false;
  }
  return reader->remoteShortcut(readerMenu);
}

bool ActivityManager::remoteTurn(const bool forward, const bool chapter) {
  if (!isRemoteReaderContext() || mappedInput.hasRemoteClick()) return false;
  auto* reader = isForegroundReader() ? static_cast<ReaderActivity*>(currentActivity.get()) : nullptr;
  if (!reader || reader->inputOverPage()) {
    return !chapter && remoteClick(forward ? RemoteButton::Next : RemoteButton::Previous);
  }
  if (!remoteTurnAccepted(reader->pageRendered.load(std::memory_order_acquire), RenderLock::peek(),
                          reader->inputOverPage())) {
    return false;
  }
  const bool turned = chapter ? reader->skipPages(forward ? 1 : -1) : reader->pageTurn(forward);
  if (turned) requestUpdate();
  return turned;
}

bool ActivityManager::handleForcedRefresh() { return currentActivity && currentActivity->handleForcedRefresh(); }

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

ScreenshotInfo ActivityManager::getScreenshotInfo() const {
  if (currentActivity) {
    return currentActivity->getScreenshotInfo();
  }
  return {};
}

void ActivityManager::prepareForSleep() {
  RenderLock lock;
  for (const auto& activity : stackActivities) activity->prepareForSleep();
  if (currentActivity) currentActivity->prepareForSleep();
}

void ActivityManager::requestUpdate(bool immediate) {
  if (!immediate) {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being rendered in the same loop
    requestedUpdate = true;
    return;
  }
  // From the main task: render synchronously. Progress callbacks inside
  // tight loops (OTA/SD flash) block the loop task, so a deferred flag would
  // never drain — the caller relies on the render happening right here.
  // Consume any pending deferred request first: this render satisfies it,
  // and leaving it set would render the same state again next loop(). A
  // request created during the render stays pending for the next drain.
  // Re-entrant calls (already inside a render or a RenderLock scope) defer:
  // the mutex is not recursive, rendering inline would deadlock.
  if (xTaskGetCurrentTaskHandle() == mainTaskHandle &&
      xSemaphoreGetMutexHolder(renderingMutex) != xTaskGetCurrentTaskHandle()) {
    requestedUpdate.exchange(false);
    performRender();
  } else {
    requestedUpdate = true;
  }
}
void ActivityManager::requestUpdateAndWait() {
  // Main thread is the only renderer and the only supported caller: render
  // synchronously. The former cross-task waiter path had no callers (every
  // call site is onEnter()/main-thread flow) and carried registration/drain
  // races — removed instead of fixed. Misuse is loud, not fatal.
  if (xTaskGetCurrentTaskHandle() != mainTaskHandle) {
    LOG_ERR("ACT", "requestUpdateAndWait() called from a non-main task; ignoring");
    return;
  }
  assert(xSemaphoreGetMutexHolder(renderingMutex) != xTaskGetCurrentTaskHandle() &&
         "Cannot call requestUpdateAndWait() while holding RenderLock");
  requestedUpdate.exchange(false);
  performRender();
}

// RenderLock

RenderLock::RenderLock(Mode mode) {
  isLocked = xSemaphoreTake(activityManager.renderingMutex, mode == Mode::Try ? 0 : portMAX_DELAY) == pdTRUE;
  assert((mode == Mode::Try || isLocked) && "Blocking render lock acquisition failed");
}

RenderLock::RenderLock(Activity&) : RenderLock(Mode::Blocking) {}

RenderLock::~RenderLock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

void RenderLock::unlock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

/**
 *
 * Checks if renderingMutex is busy.
 *
 * @return true if renderingMutex is busy, otherwise false.
 *
 */
bool RenderLock::peek() { return xQueuePeek(activityManager.renderingMutex, NULL, 0) != pdTRUE; };
