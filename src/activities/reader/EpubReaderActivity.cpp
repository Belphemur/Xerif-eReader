#include "EpubReaderActivity.h"

#include <BlePageTurner.h>
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>
#include <TrustedTime.h>
#include <WiFi.h>
#include <esp_system.h>
#include <freertos/task.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <variant>

#include "../../util/BookmarkFile.h"
#include "BoardFeatures.h"
#include "BookmarkEntry.h"
#include "ChapterBuildGate.h"
#include "ClipSelectionActivity.h"
#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DictionaryWordSelectActivity.h"
#include "FrontlightControl.h"
#include "MemSentinel.h"
#include "PageTurnBatch.h"
#include "TouchLongPressRoute.h"
#if defined(CROSSPOINT_TTF_READER)
#include "FontPreviewActivity.h"
#include "QuickPageRelayout.h"
#include "TtfWordSelect.h"
#endif
#include "TtfResolvePolicy.h"
#include "activities/ActivityResult.h"
#include "activities/util/IntervalSelectionActivity.h"
#ifdef READING_STATS_ENABLED
#include "BookStatsActivity.h"
#include "FinishReadingPromptActivity.h"
#include "FinishReadingPromptPolicy.h"
#include "FinishedBooksActivity.h"
#include "FinishedBooksIndex.h"
#include "GlobalReadingStats.h"
#include "ReadingRhythmActivity.h"
#include "ReadingStatsMenuActivity.h"
#include "activities/settings/GlobalStatsActivity.h"
#include "activities/util/ConfirmationActivity.h"
#endif
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderClippingListActivity.h"
#include "EpubReaderFootnoteSelectActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderActivity.h"
#include "ReaderFontSizes.h"
#include "ReaderLineSpacing.h"
#include "ReaderToolbarUi.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFont.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "TtfUiFallback.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/settings/TextSettingsActivity.h"
#include "clippings/ClippingText.h"
#include "clippings/ClippingsManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"

#if defined(CROSSPOINT_TTF_READER)
#include <BookFontLoader.h>
#include <render/PageRenderer.h>

#include "activities/reader/ProgressRecord.h"
#include "adapters/FrameTargetFactory.h"
#include "adapters/PagePaint.h"
#endif

namespace {
// The X4 Pro and X4 Classic carry the X4's panel but sit outside isXteinkDevice()
// (that helper also gates power management). Overlay refresh choices are per-panel:
// this family runs the grayscale anti-aliasing pass, so chrome painted over a
// fresh page needs the HALF ghost-cleanup and closing re-renders the page.
bool xteinkClassPanel() {
  return gpio.isXteinkDevice() || BoardConfig::isX4Pro() || BoardConfig::isX4Classic() || BoardConfig::isEegoA4();
}

constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
// Indeterminate fill steps for the Indexing popup bar while a cover-driven
// restore is pending (see ttfShowIndexingPopup): visible motion, no percent.
constexpr int INDEX_BAR_INDETERMINATE[] = {20, 45, 70, 95};
// Cadence for re-rendering while a chapter build is delegated to the FIBP
// prefetch worker (worker owns the build; the reader just polls for commit).
constexpr unsigned long fibpDeferPollMs = 250;
// Grace before the reader takes a delegated build over from the worker: two
// page builds at full speed. Past it the user would otherwise sit on the
// Indexing popup for the rest of the spine (a 93-page spine measured 243 s
// at low power, tens of seconds even at full speed).
constexpr unsigned long fibpDeferTakeoverMs = 1500;
// How long the reader waits for the worker to pick up a resume claim
// before the margin fallback owns the extension (soak addendum).
constexpr unsigned long kFibpResumeClaimGraceMs = 2000;
constexpr float bookmarkProgressEpsilon = 0.0001f;
constexpr int CLIP_SELECTION_PAGE_WINDOW = 3;

// Word i's font: headings and CSS font-size runs can differ from the section font.
int clippingWordFont(const GfxRenderer& renderer, const int fontId, const TextBlock& block, const uint16_t i) {
  return TextBlock::renderFontId(renderer, block.wordFontId(i, fontId), fontId);
}

Rect clippingWordRect(const GfxRenderer& renderer, const int fontId, const PageLine& line, const uint16_t i,
                      const int top, const int left, const int wordWidth) {
  const auto& block = line.getBlock();
  const int wordFont = clippingWordFont(renderer, fontId, *block, i);
  const char* text = block->wordText(i);
  const auto style = static_cast<EpdFontFamily::Style>(block->wordStyle(i) & ~EpdFontFamily::UNDERLINE);
  const int skipX =
      clippingText::hasEmSpacePrefix(text) ? renderer.getTextAdvanceX(wordFont, "\xe2\x80\x83", style) : 0;
  int width = wordWidth - skipX;
  if (i + 1 < block->wordCount() && block->wordXpos(i + 1) > block->wordXpos(i)) {
    width = std::min(width, static_cast<int>(block->wordXpos(i + 1) - block->wordXpos(i) - skipX));
  }
  const int lineFont = TextBlock::renderFontId(renderer, block->blockFontId(fontId), fontId);
  return Rect{left + line.xPos + block->wordXpos(i) + skipX,
              top + line.yPos + block->getRubyShift(renderer.getFontAscenderSize(lineFont)) +
                  block->wordYOffset(renderer, fontId, i),
              width, renderer.getLineHeight(wordFont)};
}

#ifdef READING_STATS_ENABLED
// Minimum time a page must be shown before its dwell time counts toward
// reading statistics (matches CrossInk's MIN_READING_STATS_PAGE_MS).
constexpr unsigned long MIN_READING_STATS_PAGE_MS = 2000UL;
// Minimum forward-page dwell (seconds) before it is recorded as a pace sample.
constexpr uint32_t MIN_READING_PACE_SAMPLE_SECONDS = 2;
// Pages shown longer than this are treated as idle and excluded from stats.
constexpr uint32_t READING_IDLE_THRESHOLD_SECONDS = 600;
#endif

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

// Unrounded book-progress percent (0..100) for the page currently shown, used
// by both the reader menu and the stats exit snapshot so both report the same
// convention: chapterProgress = currentPage / estimatedTotalPages.
float computeBookProgressPercent(const Epub& epub, const Section* section, const int spineIndex) {
  if (epub.getBookSize() == 0 || !section || section->estimatedTotalPages() == 0) {
    return 0.0f;
  }
  const float chapterProgress =
      static_cast<float>(section->currentPage) / static_cast<float>(section->estimatedTotalPages());
  return epub.calculateProgress(spineIndex, chapterProgress) * 100.0f;
}

constexpr char READ_FOLDER[] = "/read";

bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!ClippingStore::moveBook(srcPath, dstPath)) {
    LOG_ERR("ERS", "Failed to move finished book and clippings to '/Read' folder");
    return;
  }

  // Protected-book sidecars travel with the book; a protected book separated
  // from its key no longer opens, so a failed move rolls everything back.
  static constexpr const char* SIDECARS[] = {".key", ".rights"};
  for (size_t i = 0; i < std::size(SIDECARS); i++) {
    const std::string from = srcPath + SIDECARS[i];
    if (!Storage.exists(from.c_str())) continue;
    const std::string to = dstPath + SIDECARS[i];
    if (Storage.rename(from.c_str(), to.c_str())) continue;
    LOG_ERR("ERS", "Failed to move sidecar %s -> %s", from.c_str(), to.c_str());
    for (size_t j = 0; j < i; j++) {
      Storage.rename((dstPath + SIDECARS[j]).c_str(), (srcPath + SIDECARS[j]).c_str());
    }
    if (!Storage.rename(dstPath.c_str(), srcPath.c_str())) {
      LOG_ERR("ERS", "Failed to restore epub after sidecar move failure: %s -> %s", dstPath.c_str(), srcPath.c_str());
    }
    return;
  }

  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  bool cacheMoved = false;
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    // No stats handling needed here: per-book stats live INSIDE the cache dir,
    // so renaming the dir moves the stats file with the book.
    cacheMoved = Storage.rename(oldCachePath.c_str(), newCachePath.c_str());
    if (!cacheMoved) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, cacheMoved ? newCachePath : oldCachePath);
#ifdef READING_STATS_ENABLED
  FinishedBooksIndex::migratePath(srcPath, dstPath);
#endif
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

// A button contact is closed right now. Reads the Xteink ADC ladder, power pin, and T5S3 expander
// key only; on other boards a press stops the build once the loop has debounced it.
bool buttonContactClosed() { return gpio.rawInputActive(); }

}  // namespace

EpubReaderActivity::~EpubReaderActivity() {
  ImageBlock::setExtractor(nullptr, nullptr);
#ifdef BOARD_HAS_PSRAM
  ImageBlock::setPsramExtractor(nullptr, nullptr);
#endif
  // ActivityManager destroys activities with its RenderLock already held;
  // taking another here self-deadlocks (renderingMutex is non-recursive).
  settleOverlayRefresh();
  discardOverlayPage();  // free the overlay's page snapshot if one is held
  // A radio still stopped for this book's build may start again in the next one.
  if (radioReleasedForBuild.load()) bleturner::afterPaint();

  // Design §4.4: exit flushing is the manager's job — closeBook() flushes
  // any unflushed change synchronously (bounded by one record write) and
  // resets the manager state. The manager stays valid after this; no
  // background tick can touch reader state.
  progressManager.closeBook();

  // Upstream #3764: reopen lands on the page being VIEWED (progress stays on
  // the current page), and the link stack gives Back a trace back to the
  // origin — replacing the old footnote-origin re-save that redirected reopen
  // to the origin page.
  if (footnoteDepth > 0 && epub) saveLinkStack();

  section.reset();
  CLIPPINGS.unload();
  nextSectionPrefetch.reset();
  nextSectionSpineIndex = -1;
#if defined(CROSSPOINT_TTF_READER)
  // Abort/suspend any open session (partial cache commit) and release the
  // arenas BEFORE the progress manager's state is gone but while epub is
  // still valid for logging.
  ttf_.reset();
#endif
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::onEnter() {
  ReaderActivity::onEnter();
#if defined(CROSSPOINT_TTF_READER)
  // Sync the FT faces' render mode with the persisted setting before any
  // render/worker session can use them (a Crisp mode saved in a previous
  // session must apply at boot, not only after a settings visit).
  if (ttf_) {
    freeink::book::fontLoader.applyRenderMode(SETTINGS.textRenderMode == CrossPointSettings::TEXT_RENDER_CRISP);
  }
#endif
#ifdef READING_STATS_ENABLED
  if (epub && SETTINGS.shouldTrackReadingStats()) {
    stats = BookReadingStats::load(epub->getCachePath());
    globalStats = GlobalReadingStats::load();
    sessionReadingSeconds = 0;
    sessionPageTurns = 0;
    hasSessionStartLocalDateTime = getCurrentLocalReadingStatsDateTime(sessionStartLocalDateTime);
    // ADR-5: the finish-reading prompt fires only on forward page movement,
    // never on open — the first settled evaluation seeds the baseline.
    lastPromptProgress = -1.0f;
    lastPromptPage = -1;
    lastPromptSpineIndex = -1;
    // Session-scoped completion guard: a book reopened from a previously
    // completed state starts the session WITHOUT a completion confirmation, so
    // onExit() saves the reread position instead of re-pinning 100%.
    completedThisSession = false;
  }
#endif
}

void EpubReaderActivity::onExit() {
#if defined(CROSSPOINT_TTF_READER)
  // R1: transient worker lifetime — cancel + join + free before teardown.
  stopFibpWorker();
#endif
#ifdef READING_STATS_ENABLED
  if (epub && SETTINGS.shouldTrackReadingStats()) {
    recordCurrentPageReadingTime();
    const uint32_t elapsedSecs = sessionReadingSeconds;
    if (elapsedSecs >= 60) {
      stats.sessionCount++;
      globalStats.totalSessions++;
    }
    if (elapsedSecs >= 10) {
      stats.totalReadingSeconds += elapsedSecs;
      globalStats.totalReadingSeconds += elapsedSecs;
      if (hasSessionStartLocalDateTime) {
        stats.recordReadingSpan(sessionStartLocalDateTime, elapsedSecs);
        globalStats.recordReadingSpan(sessionStartLocalDateTime, elapsedSecs);
      }
      if (elapsedSecs >= 120 && !stats.startDateManual && !stats.startDate.isValid() && hasSessionStartLocalDateTime) {
        stats.startDate = sessionStartLocalDateTime.date;
      }
    }
    if (elapsedSecs >= SESSION_MIN_SECONDS && sessionPageTurns >= SESSION_MIN_PAGE_TURNS) {
      // The Avg Session window has its own gates (lower time threshold, plus
      // an engagement check): it accepts sessions the Sessions counter
      // ignores, and rejects "book left open" sessions that turn no pages.
      stats.recordSession(elapsedSecs);
      globalStats.recordGlobalSession(elapsedSecs);
    }
    if (epub) {
      uint16_t chapterPages = section ? section->estimatedTotalPages() : 0;
#if defined(CROSSPOINT_TTF_READER)
      if (!section && ttf_) chapterPages = static_cast<uint16_t>(ttfPageCount);
#endif
      const uint64_t bookSize = epub->getBookSize();
      const uint64_t chapterEnd = epub->getCumulativeSpineItemSize(currentSpineIndex);
      const uint64_t chapterStart =
          currentSpineIndex >= 1 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
      const uint64_t chapterBytes = (chapterEnd > chapterStart) ? (chapterEnd - chapterStart) : 0;
#if defined(CROSSPOINT_TTF_READER)
      float bookProgressPercent;
      if (ttf_ && !section) {
        // Mirrors-based percent (computeBookProgressPercent needs a Section).
        const float chapterProgress =
            cachedChapterTotalPageCount > 0
                ? static_cast<float>(ttfPage) / static_cast<float>(cachedChapterTotalPageCount)
                : 0.0f;
        bookProgressPercent = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      } else {
        bookProgressPercent = computeBookProgressPercent(*epub, section.get(), currentSpineIndex);
      }
#else
      const float bookProgressPercent = computeBookProgressPercent(*epub, section.get(), currentSpineIndex);
#endif
      // A book completed in THIS session keeps the 100% the completion flow
      // pinned at the confirm site (setBookCompleted) — recomputing here would
      // report the pre-confirmation page's partial percent in the stats. A
      // book completed in an EARLIER session and reopened for a reread does
      // not: the reader must save the current position like any other book.
      const bool preservesProgress = stats.isCompleted && completedThisSession;
      if (bookSize > 0 && chapterBytes > 0 && chapterPages > 0 && bookProgressPercent >= 0.0f &&
          bookProgressPercent <= 100.0f) {
        const uint64_t bookPagesEstimate = static_cast<uint64_t>(chapterPages) * bookSize / chapterBytes;
        const float bookProgress = bookProgressPercent / 100.0f;
        const uint32_t remainingPages =
            static_cast<uint32_t>((1.0f - bookProgress) * static_cast<float>(bookPagesEstimate));
        auto timeLeft = estimateBookTimeLeftSeconds(stats, globalStats, remainingPages, currentPageWordsOnPage);
        // Cache the reading-status triple for the cover sleep/shutdown screens
        // on EVERY valid exit — the seeds are the sleep screens' only fresh
        // source for pages-left, so they must not be limited to completions.
        // Pages-left clamps to INT16_MAX (the persisted cache field is
        // int16_t); the time-left estimate only overwrites the cache when it
        // exists — a nullopt estimate keeps the previous value rather than
        // fabricating an unknown.
        APP_STATE.openBookPagesLeft = static_cast<int16_t>(remainingPages > INT16_MAX ? INT16_MAX : remainingPages);
        if (timeLeft) {
          APP_STATE.openBookTimeLeftSeconds = *timeLeft;
        }
        if (!preservesProgress) {
          // A book completed in THIS session keeps the 100% the completion
          // flow pinned at the confirm site (setBookCompleted) — recomputing
          // here would report the pre-confirmation page's partial percent in
          // the stats. A book completed in an EARLIER session and reopened
          // for a reread does not: the reader must save the current position
          // like any other book. Every other exit (normal reading sessions)
          // must refresh the record, or the home card shows stale values.
          if (timeLeft) {
            stats.estimatedTimeLeftSeconds = *timeLeft;
          }
          stats.lastBookProgressPercent =
              static_cast<uint8_t>(clampPercent(static_cast<int>(bookProgressPercent + 0.5f)));
        }
      }
    }
    // Two independent sequential-record writes (per-book + global). The global
    // save is atomic (tmp -> verify -> .bak rotate -> rename) on its own, so a
    // failure between them degrades gracefully: the book record may be ahead
    // of the aggregate until the next session commit.
    stats.save(epub->getCachePath());
    globalStats.save();
    if (stats.isCompleted) syncFinishedBookIndex();
  }
#endif
  ReaderActivity::onExit();
}

#ifdef READING_STATS_ENABLED
bool EpubReaderActivity::currentPageReadingSecondsForStats(uint32_t& seconds) const {
  if (!SETTINGS.shouldTrackReadingStats() || pageShownAtMs == 0) return false;
  const unsigned long elapsedMs = millis() - pageShownAtMs;
  if (elapsedMs < MIN_READING_STATS_PAGE_MS) return false;
  const unsigned long elapsedSeconds = elapsedMs / 1000;
  if (elapsedSeconds == 0 || elapsedSeconds > READING_IDLE_THRESHOLD_SECONDS) return false;
  seconds = static_cast<uint32_t>(elapsedSeconds);
  return true;
}

void EpubReaderActivity::recordCurrentPageReadingTime() {
  uint32_t seconds = 0;
  if (!currentPageReadingSecondsForStats(seconds)) {
    pageShownAtMs = 0;
    return;
  }
  if (UINT32_MAX - sessionReadingSeconds < seconds) {
    sessionReadingSeconds = UINT32_MAX;
  } else {
    sessionReadingSeconds += seconds;
  }
  pageShownAtMs = 0;
}

void EpubReaderActivity::recordForwardPagePaceSample(uint32_t seconds, uint16_t wordsOnPage) {
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  // Raw inputs of every pace sample (accepted or rejected): the WpmWindow
  // stores the floor-clamped value, so a stuck-at-80 device dump cannot show
  // whether words were undercounted or dwell inflated — this line can.
  const uint32_t rawWpm = (seconds > 0 && wordsOnPage > 0) ? (static_cast<uint32_t>(wordsOnPage) * 60U) / seconds : 0;
  LOG_DBG("RSDBG", "sample dwell=%us words=%u rawWpm=%lu", static_cast<unsigned>(seconds),
          static_cast<unsigned>(wordsOnPage), static_cast<unsigned long>(rawWpm));
#endif
  if (seconds < MIN_READING_PACE_SAMPLE_SECONDS) return;
  stats.recordForwardPageRead(seconds, wordsOnPage);
  globalStats.recordGlobalPageRead(seconds, wordsOnPage);
}

void EpubReaderActivity::applyBookStatsEditsFromDisk() {
  if (!epub || !SETTINGS.shouldTrackReadingStats()) return;

  // Only editable completion fields come back from the stats screen. Its copy
  // contains preview counters, so importing totals/sessions here would
  // double-count the in-flight reading session.
  const BookReadingStats diskStats = BookReadingStats::load(epub->getCachePath());
  stats.isCompleted = diskStats.isCompleted;
  stats.startDateManual = diskStats.startDateManual;
  stats.finishedDateManual = diskStats.finishedDateManual;
  stats.completionAchievementPending = diskStats.completionAchievementPending;
  stats.completionPromptDismissedAtHundred = diskStats.completionPromptDismissedAtHundred;
  // The stats screen's completion edits clear the latch on disk
  // (BookStatsActivity::applyCompletedState); import it so the reader's
  // in-memory gate matches the record instead of staying latched from before
  // the screen ran.
  stats.finishPromptDismissed = diskStats.finishPromptDismissed;
  stats.startDate = diskStats.startDate;
  stats.finishedDate = diskStats.finishedDate;

  globalStats.completedBooks = GlobalReadingStats::load().completedBooks;
}

void EpubReaderActivity::syncFinishedBookIndex() {
  // An entry without a title can never be loaded back (loadPath() skips it),
  // and would break the whole index rewrite's verify pass.
  if (epub && epub->getTitle().empty()) {
    LOG_ERR("ERS", "Skipping finished-book entry: empty title");
    return;
  }
  if (!epub || !FinishedBooksIndex::recordCanonical(epub->getPath(), epub->getCachePath(), epub->getTitle(),
                                                    epub->getAuthor(), stats)) {
    LOG_ERR("ERS", "Failed to synchronize finished-book entry");
  }
}

void EpubReaderActivity::handleBookStatsReturn() {
  const bool wasCompleted = stats.isCompleted;
  applyBookStatsEditsFromDisk();

  if (SETTINGS.removeReadBooksFromRecents) {
    if (!wasCompleted && stats.isCompleted) {
      RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (wasCompleted && !stats.isCompleted) {
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
    }
  }
  if (!wasCompleted && stats.isCompleted && SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath())) {
    pendingReadFolderMove = true;
  } else if (!stats.isCompleted) {
    pendingReadFolderMove = false;
  }
  syncFinishedBookIndex();
}

BookReadingStats EpubReaderActivity::achievementStatsPreview(uint32_t* pendingReadingSeconds) const {
  if (pendingReadingSeconds) *pendingReadingSeconds = 0;
  BookReadingStats preview = stats;
  if (!SETTINGS.shouldTrackReadingStats()) return preview;

  uint32_t pendingSeconds = sessionReadingSeconds;
  uint32_t currentPageSeconds = 0;
  if (currentPageReadingSecondsForStats(currentPageSeconds)) {
    pendingSeconds =
        pendingSeconds > UINT32_MAX - currentPageSeconds ? UINT32_MAX : pendingSeconds + currentPageSeconds;
  }
  if (pendingReadingSeconds) *pendingReadingSeconds = pendingSeconds;

  preview.totalReadingSeconds = preview.totalReadingSeconds > UINT32_MAX - pendingSeconds
                                    ? UINT32_MAX
                                    : preview.totalReadingSeconds + pendingSeconds;
  if (pendingSeconds >= 60 && preview.sessionCount < UINT16_MAX) ++preview.sessionCount;
  if (pendingSeconds >= 10 && hasSessionStartLocalDateTime) {
    preview.recordReadingSpan(sessionStartLocalDateTime, pendingSeconds);
  }
  return preview;
}

void EpubReaderActivity::maybePromptFinishReading() {
#ifdef READING_STATS_ENABLED
  if (!epub || !SETTINGS.shouldTrackReadingStats()) return;
  // ADR-3: the toggle gates only the new threshold modals (the existing 100%
  // prompt and the End-of-Book screen are untouched).
  if (!SETTINGS.finishReadingPrompt) return;
  if (stats.isCompleted) return;            // ADR-5: !stats.isCompleted
  if (stats.finishPromptDismissed) return;  // ADR-2: permanent per-book latch
  // Settled frame only: no overlay/other modal, no end-of-book menu, no
  // mid-build section, and never at/past the final section boundary (the
  // existing end-of-book flow owns completion there).
  if (overlay != Overlay::None || overlayPopup.isActive() || endOfBookMenuActive()) return;
  if (currentSpineIndex >= epub->getSpineItemsCount()) return;

  // Performance guard: skip the spine walk when the reading position is
  // identical to the last settled evaluation (progress only moves on a
  // committed page turn, so the percent cannot have changed).
  const int positionPage =
#if defined(CROSSPOINT_TTF_READER)
      (ttf_ && !section) ? ttfPage : (section ? section->currentPage : -1);
#else
      section ? section->currentPage : -1;
#endif
  if (currentSpineIndex == lastPromptSpineIndex && positionPage == lastPromptPage) return;

#if defined(CROSSPOINT_TTF_READER)
  float bookProgressPercent;
  if (ttf_ && !section) {
    // Mirrors-based percent (computeBookProgressPercent needs a Section).
    const float chapterProgress = cachedChapterTotalPageCount > 0
                                      ? static_cast<float>(ttfPage) / static_cast<float>(cachedChapterTotalPageCount)
                                      : 0.0f;
    bookProgressPercent = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  } else if (section && !section->isBuilding()) {
    bookProgressPercent = computeBookProgressPercent(*epub, section.get(), currentSpineIndex);
  } else {
    return;  // no section yet: defer to the next settled frame
  }
#else
  if (!section || section->isBuilding()) return;
  const float bookProgressPercent = computeBookProgressPercent(*epub, section.get(), currentSpineIndex);
#endif
  // Clamp the unrounded percent to the valid range before any comparison —
  // the baseline must track real progress, not a display-rounded integer
  // (a rounded baseline can swallow a forward turn: 98.6% → 99.1% both round
  // to 99, and ≥99.5% then suppresses the 1% band forever).
  if (bookProgressPercent < 0.0f || bookProgressPercent > 100.0f) return;
  // Cache only positions that were actually evaluated: an early return above
  // (section still building, no section yet, invalid percent) must be retried
  // on the next settled frame instead of being swallowed by the guard.
  lastPromptSpineIndex = currentSpineIndex;
  lastPromptPage = positionPage;

  // ADR-5: fire on forward progress only. A backward jump re-entering the
  // zone does not fire; firing resumes when progress moves forward again.
  // The first settled evaluation seeds the baseline and never fires — a book
  // opened already in the zone must not prompt without a real forward turn.
  if (lastPromptProgress < 0.0f) {
    lastPromptProgress = bookProgressPercent;
    return;
  }
  if (bookProgressPercent <= lastPromptProgress) {
    lastPromptProgress = bookProgressPercent;
    return;
  }
  lastPromptProgress = bookProgressPercent;

  // Unrounded progress: the policy's own boundaries (90/95/99/99.5) work on
  // the real percentage, not the display-rounded integer.
  const auto decision =
      finish_prompt::evaluate(bookProgressPercent, true, stats.isCompleted, stats.finishPromptDismissed);
  if (decision != finish_prompt::Decision::Fire10 && decision != finish_prompt::Decision::Fire5 &&
      decision != finish_prompt::Decision::Fire1) {
    return;
  }
  const uint8_t remaining =
      decision == finish_prompt::Decision::Fire1 ? 1 : (decision == finish_prompt::Decision::Fire5 ? 5 : 10);
  auto prompt = makeUniqueNoThrow<FinishReadingPromptActivity>(renderer, mappedInput, remaining);
  if (!prompt) {
    LOG_ERR("ERS", "OOM: finish-reading prompt");
    return;
  }
  startActivityForResult(std::move(prompt), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      // ADR-2: one dismissal silences the book permanently — no later
      // thresholds fire either. The latch clears only on a completion-state
      // change (setBookCompleted).
      stats.finishPromptDismissed = true;
      if (epub) stats.save(epub->getCachePath());
      requestUpdate();
      return;
    }
    // ADR-1: route through exactly the existing completion path.
    setBookCompleted(true);
    // Completed from the prompt: persist the terminal record here so 100%
    // survives on disk.
    if (!saveFinishedProgress()) LOG_ERR("ERS", "Failed to save finished-book progress");
    goHomeOrShowCompletionAchievement();
  });
#endif
}

void EpubReaderActivity::goHomeOrShowCompletionAchievement() {
  const int spineCount = epub ? epub->getSpineItemsCount() : 0;
  const bool atEndOfBook = epub && spineCount > 0 && currentSpineIndex >= spineCount;
  const bool onFinalPage = epub && section && spineCount > 0 && currentSpineIndex == spineCount - 1 &&
                           section->pageCount > 0 && section->currentPage >= section->pageCount - 1;
  const bool displaysHundredPercent =
      epub && computeBookProgressPercent(*epub, section.get(), currentSpineIndex) >= 99.5f;

  // Crossing past the final readable page is definitive completion evidence.
  // A final page or rounded 100% can also come from a jump, so ask first.
  if (!stats.isCompleted && atEndOfBook) {
    setBookCompleted(true);
  } else if (!stats.isCompleted && !stats.completionPromptDismissedAtHundred &&
             (onFinalPage || displaysHundredPercent)) {
    auto prompt = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_MARK_FINISHED_PROMPT), "");
    if (!prompt) {
      LOG_ERR("ERS", "OOM: completion prompt");
      // Without a prompt the reader asked to leave, not to complete: do not
      // silently record completion state they never confirmed.
      onGoHome();
      return;
    }
    startActivityForResult(std::move(prompt), [this](const ActivityResult& result) {
      if (result.isCancelled) {
        stats.completionPromptDismissedAtHundred = true;
        if (epub) stats.save(epub->getCachePath());
        onGoHome();
        return;
      }
      setBookCompleted(true);
      // Completed from the prompt: no page-turn crossing ran, so persist the
      // terminal record here or the pre-jump position survives on disk.
      if (!saveFinishedProgress()) LOG_ERR("ERS", "Failed to save finished-book progress");
      goHomeOrShowCompletionAchievement();
    });
    return;
  }

  if (!stats.isCompleted || !stats.completionAchievementPending || !epub) {
    onGoHome();
    return;
  }

  uint32_t pendingReadingSeconds = 0;
  const BookReadingStats achievementStats = achievementStatsPreview(&pendingReadingSeconds);
  GlobalReadingStats achievementGlobalStats = globalStats;
  if (pendingReadingSeconds >= 10) {
    achievementGlobalStats.totalReadingSeconds =
        achievementGlobalStats.totalReadingSeconds > UINT32_MAX - pendingReadingSeconds
            ? UINT32_MAX
            : achievementGlobalStats.totalReadingSeconds + pendingReadingSeconds;
  }

  auto achievement =
      makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, epub->getTitle(), epub->getAuthor(), achievementStats,
                                           "", achievementGlobalStats, BookStatsActivity::InitialPage::Achievement);
  if (!achievement) {
    LOG_ERR("ERS", "OOM: completion achievement screen");
    onGoHome();
    return;
  }
  stats.completionAchievementPending = false;
  stats.save(epub->getCachePath());
  activityManager.replaceActivity(std::move(achievement));
}

void EpubReaderActivity::onGoHomeRequested() {
  if (!epub || !SETTINGS.shouldTrackReadingStats()) {
    onGoHome();
    return;
  }
  recordCurrentPageReadingTime();
  goHomeOrShowCompletionAchievement();
}

void EpubReaderActivity::setBookCompleted(bool completed) {
  if (!epub || stats.isCompleted == completed) return;

  stats.isCompleted = completed;
  // Session-scoped completion guard: the 100% pin in onExit() only applies to
  // a completion confirmed during THIS reader session.
  completedThisSession = completed;
  stats.completionAchievementPending = completed;
  stats.completionPromptDismissedAtHundred = false;
  // ADR-2: the finish-reading latch clears only on a completion-state change
  // — never on a forward page turn (unlike completionPromptDismissedAtHundred).
  stats.finishPromptDismissed = false;
  if (completed && !stats.finishedDateManual && !stats.finishedDate.isValid()) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now)) stats.finishedDate = now.date;
  }

  if (completed) {
    // The home screen reads lastBookProgressPercent, but onExit() cannot
    // compute it at the end-of-book state (no section). A completed book is
    // done: pin the percent. Re-reading it later overwrites with real values.
    stats.lastBookProgressPercent = 100;
    // A finished book has no reading time left; without this the home card
    // would pair the pinned 100% with a stale "~2h" from the last session.
    stats.estimatedTimeLeftSeconds = 0;
    // Same triple for the cover sleep/shutdown screens: at 100% the onExit
    // seeds cannot run either (no section), so the pin site is the
    // authoritative writer — no stale "pages left" on a finished book.
    APP_STATE.openBookProgressPercent = 100;
    APP_STATE.openBookTimeLeftSeconds = 0;
    APP_STATE.openBookPagesLeft = 0;
    if (SETTINGS.removeReadBooksFromRecents) RECENT_BOOKS.removeByPath(epub->getPath());
    if (SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath())) pendingReadFolderMove = true;
    if (globalStats.completedBooks < UINT32_MAX) ++globalStats.completedBooks;
  } else {
    // Un-completing invalidates the completion cache: the sleep screens must
    // recompute on the next exit instead of showing 100% / no pages left.
    APP_STATE.openBookProgressPercent = -1;
    APP_STATE.openBookTimeLeftSeconds = 0;
    APP_STATE.openBookPagesLeft = -1;
    if (SETTINGS.removeReadBooksFromRecents) {
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
    }
    pendingReadFolderMove = false;
    if (globalStats.completedBooks > 0) --globalStats.completedBooks;
  }

  stats.save(epub->getCachePath());
  globalStats.save();
  syncFinishedBookIndex();
}
#endif

bool EpubReaderActivity::loadBook() {
  auto loadedEpub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
  if (!loadedEpub) {
    LOG_ERR("ERS", "Failed to allocate EPUB object");
    return false;
  }

  const bool uncached = !Storage.exists((loadedEpub->getCachePath() + "/book.bin").c_str());
  if (uncached) {
    disableFastInitialRefresh();
    GUI.drawPopup(renderer, tr(STR_INDEXING));
  }

  bool loaded;
  {
#ifdef BOOK_PROFILE
    uint32_t load_start_ms = millis();
    uint8_t core = xPortGetCoreID();
    uint32_t psram_free_before = ESP.getFreePsram();
    uint32_t heap_free_before = ESP.getFreeHeap();
#endif
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    if (uncached) loan.emplace(renderer);
    loaded = loadedEpub->load(true, SETTINGS.embeddedStyle == 0);
#ifdef BOOK_PROFILE
    uint32_t load_end_ms = millis();
    uint32_t psram_free_after = ESP.getFreePsram();
    uint32_t heap_free_after = ESP.getFreeHeap();
    LOG_INF("PROF", "phase=loadBook core=%d load_dur=%uus psram_free=%uB->%uB heap=%uB->%uB", core,
            load_end_ms - load_start_ms, psram_free_before, psram_free_after, heap_free_before, heap_free_after);
#endif
  }
  logMemAt("book_open");
  if (!loaded) {
    // Surfaced by handleLoadFailure() as a dialog; loadedEpub dies with this
    // scope, so carry the reason out in a member.
    loadProtectionError = loadedEpub->getProtectionError();
    LOG_ERR("ERS", "Failed to load EPUB%s%s", loadProtectionError.empty() ? "" : ": ", loadProtectionError.c_str());
    return false;
  }
  epub = std::move(loadedEpub);

  ImageBlock::clearRenderFailures();
  ImageBlock::setExtractor(epub.get(), [](void* ctx, const char* src, const char* dest) {
    return static_cast<Epub*>(ctx)->extractItemToFile(src, dest);
  });
#ifdef BOARD_HAS_PSRAM
  ImageBlock::setPsramExtractor(epub.get(), [](void* ctx, const char* src, size_t& size) {
    return static_cast<Epub*>(ctx)->extractItemToPsram(src, &size);
  });
#endif

  epub->setupCacheDir();

#if defined(CROSSPOINT_TTF_READER)
  // Native-TTF page source (design §3.5). readerFontEngine is the documented
  // rollback switch (CrossPointSettings.h): BITMAP selects the legacy Section
  // path, so the runtime is only created when TTF is selected. A failed open
  // falls back to the legacy path too — second kill switch.
  ttf_ = SETTINGS.readerFontEngine == CrossPointSettings::READER_ENGINE_TTF
             ? makeUniqueNoThrow<freeink::book::TtfBookRuntime>()
             : nullptr;
  if (ttf_) {
    const std::string ttfCacheDir = epub->getCachePath() + "/ficache";
    if (!ttf_->open(bookPath.c_str(), ttfCacheDir.c_str())) {
      LOG_ERR("ERS", "TTF runtime open failed — using legacy reader path");
      ttf_.reset();
    } else {
      // Sync the FT faces' render mode before the first render/worker session
      // (onEnter ran before ttf_ existed on a fresh open).
      freeink::book::fontLoader.applyRenderMode(SETTINGS.textRenderMode == CrossPointSettings::TEXT_RENDER_CRISP);
    }
  }
#endif

  // ProgressManager is the single source of truth: openBook() loads the
  // on-disk record, seeds the manager's state, and hands the position back.
  uint16_t savedSpine = 0;
  uint16_t savedPage = 0;
  uint16_t savedPageCount = 0;
  uint32_t savedOffset = 0;
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    bool ttfHasGeneration = false;
    const bool progressLoaded =
        progressManager.openBookTtf(epub->getCachePath().c_str(), savedSpine, savedPage, savedPageCount,
                                    ttfSavedCharOffset, ttfSavedGeneration, ttfHasGeneration);
    if (progressLoaded) {
      const int spineCount = epub->getSpineItemsCount();
      // Design §3, §10 amendment 11: the charOffset anchor is authoritative
      // and generation-independent; the page hint is advisory. Generation is
      // recorded metadata, never a gate. A stale hint on a record with a
      // valid anchor is clamped, not dropped — the count mirror can lag a
      // partial build.
      const auto verdict = ttf_resolve::judgeSavedTtfRecord(spineCount, savedSpine, savedPage, savedPageCount,
                                                            ttfHasGeneration, ttfSavedCharOffset);
      if (verdict == ttf_resolve::ProgressVerdict::Reject) {
        LOG_DBG("ERS", "Ignoring corrupt TTF progress: spine=%u page=%u/%u", savedSpine, savedPage, savedPageCount);
        savedSpine = 0;
        savedPage = 0;
        savedPageCount = 0;
        ttfHasGeneration = false;
        ttfSavedGeneration = 0;
        ttfSavedCharOffset = 0;
      } else if (verdict == ttf_resolve::ProgressVerdict::ClampPageHint) {
        const uint16_t clampedPage = static_cast<uint16_t>(savedPageCount - 1);
        LOG_DBG("ERS", "Clamping stale TTF page hint: page=%u -> %u/%u", savedPage, clampedPage, savedPageCount);
        savedPage = clampedPage;
        // The count mirror lagged the build: publish "unknown" (0) instead of
        // the stale total, or the status bar / KOReader sync would report a
        // wrong chapter length until the rebuild overwrites the mirror.
        savedPageCount = 0;
      }
      currentSpineIndex = savedSpine;
      nextPageNumber = savedPage == UINT16_MAX ? 0 : savedPage;
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = savedPageCount;
      ttfSavedSpine = savedSpine;
      // Restore latch: a judged record ALWAYS restores (owner directive,
      // design §10 amendment 11) — the charOffset anchor is authoritative and
      // generation-independent, and a page hint alone (charOffset 0, legacy
      // shape) restores by page: approximately right beats the chapter start.
      // Only a record with neither anchor nor hint (last-page sentinel
      // normalized to 0 above, or page 0 = chapter start anyway) has nothing
      // to restore; rejected records never latch.
      const bool hasRestoreAnchor =
          ttf_resolve::hasCharAnchor(ttfHasGeneration, ttfSavedCharOffset) || nextPageNumber != 0;
      ttfHasSavedPosition = verdict != ttf_resolve::ProgressVerdict::Reject && hasRestoreAnchor;
      if (!ttfHasSavedPosition) {
        nextPageNumber = 0;
        cachedChapterTotalPageCount = 0;
      }
      LOG_DBG("ERS", "Loaded TTF progress: spine %d, page %d, gen %u", currentSpineIndex, nextPageNumber,
              ttfSavedGeneration);
    }
  } else
#endif
  {
    const bool progressLoaded =
        progressManager.openBook(epub->getCachePath().c_str(), savedSpine, savedPage, savedPageCount, savedOffset);
    if (progressLoaded) {
      const int spineCount = epub->getSpineItemsCount();
      if (spineCount <= 0 || savedSpine >= static_cast<uint16_t>(spineCount) ||
          (savedPageCount > 0 && savedPage >= savedPageCount)) {
        LOG_DBG("ERS", "Ignoring corrupt progress: spine=%u page=%u/%u", savedSpine, savedPage, savedPageCount);
        savedSpine = 0;
        savedPage = 0;
        savedPageCount = 0;
      }
      currentSpineIndex = savedSpine;
      nextPageNumber = savedPage;
      if (nextPageNumber == UINT16_MAX) {
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = savedPageCount;
      if (savedPageCount > 0) {
        cachedVisibleTextOffset = savedOffset;
      }
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
  }

  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      cachedVisibleTextOffset.reset();
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  loadLinkStack();
  loadCachedBookmarks();
  if (!CLIPPINGS.loadForBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), "epub")) {
    LOG_ERR("CLIP", "Failed to load clippings for %s", epub->getPath().c_str());
  }
  return true;
}

// Any reader chrome (toolbar sheet / panel / popup / open footnote) closes with
// the global back gesture instead of the gesture leaving the book; with no
// chrome the gesture falls through (caller pops / goes home as before).
bool EpubReaderActivity::handleHomeGesture() {
  if (!isChromeOpen()) return false;
  // Close only the topmost layer per gesture, matching the Back button's
  // stepped behavior: a sheet over an open footnote restores the sheet first
  // and leaves the footnote jump for the next gesture.
  if (overlay != Overlay::None) {
    closeOverlayToPage();
    return true;
  }
  if (footnoteDepth > 0) {
    restoreSavedPosition();
  }
  return true;
}

bool EpubReaderActivity::isChromeOpen() const {
  return overlay != Overlay::None || overlayPopup.isActive() || footnoteDepth > 0;
}

ChapterPosition EpubReaderActivity::chapterPosition() const {
  if (section) return {section->currentPage, section->estimatedTotalPages()};
  return {nextPageNumber, cachedChapterTotalPageCount};
}

int EpubReaderActivity::bookPercentFor(const ChapterPosition& position) const {
  if (!epub || epub->getBookSize() == 0 || !position.hasTotal()) return 0;
  // The page index can run past the chapter's estimated total while it is still
  // building, so the fraction is clamped before the cast.
  const float fraction = epub->calculateProgress(currentSpineIndex, position.chapterFraction());
  return static_cast<int>(std::clamp(fraction, 0.0f, 1.0f) * 100.0f + 0.5f);
}

void EpubReaderActivity::openReaderMenu() {
  pendingManualTurn = 0;
  if (usesToolbarMenu()) {
    // Reached from a child activity's result handler (footnotes, bookmarks,
    // go-to-percent... cancelled back to the menu), so the framebuffer holds
    // that screen, not the page: re-render the page and let renderBook() put
    // the toolbar on top. The in-reader fast path is openOverlay().
    overlay = Overlay::Toolbar;
    focusedTool = 0;
    panelHoldJumped = false;
    panelCursorShown = !mappedInput.hasTouch();
    if (!toolbarUi) toolbarUi = std::make_unique<ReaderToolbarUi>(renderer);
    toolbarUi->begin();
    discardOverlayPage();
    requestUpdate();
    return;
  }
#ifdef READING_STATS_ENABLED
  recordCurrentPageReadingTime();
  pageShownAtMs = 0;
#endif

  // Child screens (chapter list, text settings) release the section to free its
  // pagination buffers; chapterPosition() covers that with the cached position.
  const ChapterPosition position = chapterPosition();
  const int bookProgressPercent = bookPercentFor(position);

  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(
          renderer, mappedInput, epub->getTitle(), position.displayPage(), position.totalPages, bookProgressPercent,
          SETTINGS.orientation, !currentPageFootnotes.empty(), !cachedBookmarks.empty(), CLIPPINGS.hasClippings()),
      [this](const ActivityResult& result) {
        const auto& menu = std::get<MenuResult>(result.data);
        if (SETTINGS.orientation != menu.orientation) {
          applyOrientation(menu.orientation);
        }
        toggleAutoPageTurn(menu.pageTurnOption);
        if (!result.isCancelled) {
          onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
        }
      });
}

// Runs on the render task, under its lock, right before every chapter build. The page turner's
// radio holds about 50 KB while it runs, and no heap figure taken before a build tells whether the
// build fits next to it (X3: with the radio up, 55 KB free and a 47 KB block, a 6 KB chapter ran
// out of memory). So the radio stops for every build and starts again once the chapter is built
// and its page is shown. A radio that did not stop within the module's 3 s, or a start still
// settling, usually lets go soon after: the build waits up to BUILD_ROOM_WAIT_MS more, then is
// refused rather than run next to it. The stop finishes on the main loop (the module's tick) and
// the start on its own task; neither needs the render lock this task holds.
bool EpubReaderActivity::makeRoomForChapterBuild() {
  bool asked = false;
  const auto roomMade = [this, &asked] {
    // The first ask stops the radio. A later one asks again only once a start that was still
    // settling has left the radio up.
    if (!radioReleasedForBuild.load() && (!asked || !bleturner::status().starting)) {
      radioReleasedForBuild = bleturner::beforeChapterBuild() != bleturner::BuildRelease::NotHeld;
    }
    asked = true;
    return chapterBuildMayRun(bleturner::status());
  };
  const auto nowMs = [] { return static_cast<uint32_t>(millis()); };
  const auto nap = [](const uint32_t ms) { delay(ms); };
  if (waitForBuildRoom(roomMade, nowMs, nap, BUILD_ROOM_WAIT_MS, BUILD_ROOM_STEP_MS)) return true;
  LOG_ERR("ERS", "Bluetooth radio still holds the heap after %u ms; chapter build refused",
          static_cast<unsigned>(BUILD_ROOM_WAIT_MS));
  return false;
}

bool EpubReaderActivity::buildTickHeapGate() {
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t maxBlock = ESP.getMaxAllocHeap();
  buildHeapPaused = freeHeap < BACKGROUND_BUILD_MIN_FREE_HEAP || maxBlock < BACKGROUND_BUILD_MIN_MAX_ALLOC;
  return !buildHeapPaused;
}

void EpubReaderActivity::prefetchNextChapterDuringDisplay() {
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    ttfPrefetchTick();
    return;
  }
#endif
#if defined(BOARD_HAS_PSRAM) && defined(ESP_PLATFORM)
#ifdef BOOK_PROFILE
  const auto prefetchStart = millis();
#endif
  if (!epub) return;
  if (buildHeapPaused) return;
  const int nextSpine = currentSpineIndex + 1;
  if (nextSpine >= epub->getSpineItemsCount()) return;

  // Only prefetch if the next chapter hasn't been cached yet
  if (nextSectionPrefetch && nextSectionSpineIndex == nextSpine) {
    // Continue the existing prefetch build
    if (nextSectionPrefetch->isBuilding() && !nextSectionPrefetch->isBuildComplete()) {
      nextSectionPrefetch->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK);
#ifdef BOOK_PROFILE
      LOG_DBG("PROF", "phase=prefetch_continue spine=%d pages_built=%d dur=%lums core=%d", nextSpine,
              nextSectionPrefetch->pageCount, millis() - prefetchStart, xPortGetCoreID());
#endif
    }
    return;
  }

  // Start a new prefetch for the next chapter
  const ReaderRenderSpec renderSpec = SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight);
  nextSectionPrefetch = std::make_unique<Section>(epub, nextSpine, renderer);
  nextSectionSpineIndex = nextSpine;

  // Try loading existing cache first; if missing, start a build
  if (nextSectionPrefetch->loadSectionFile(renderSpec)) {
#ifdef BOOK_PROFILE
    LOG_DBG("PROF", "phase=prefetch_cache_hit spine=%d dur=%lums core=%d", nextSpine, millis() - prefetchStart,
            xPortGetCoreID());
#endif
  } else {
    nextSectionPrefetch->startBuild(renderSpec);
    nextSectionPrefetch->buildSomeMore(BUILD_PAGES_PER_CHUNK);
#ifdef BOOK_PROFILE
    LOG_DBG("PROF", "phase=prefetch_cold_build spine=%d pages_built=%d dur=%lums core=%d", nextSpine,
            nextSectionPrefetch->pageCount, millis() - prefetchStart, xPortGetCoreID());
#endif
  }
#endif
}

void EpubReaderActivity::showBuildPopup(GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  if (!buildPopupPending || !renderer.hasFrameBuffer()) return;
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  indexingPopupShown = true;
  pagesUntilFullRefresh = 1;
  buildPopupPending = false;
}

// Debounced state of every button. Covers the frame between a raw release and its debounced
// release edge, which would otherwise run a build step before the page turn.
bool EpubReaderActivity::anyButtonPressed() const {
  using Button = MappedInputManager::Button;
  for (const Button button :
       {Button::Back, Button::Confirm, Button::Left, Button::Right, Button::Up, Button::Down, Button::Power}) {
    if (mappedInput.isPressed(button)) return true;
  }
  return false;
}

void EpubReaderActivity::backgroundBuildTick() {
  RenderLock lock(RenderLock::Mode::Try);
  // Not under an open toolbar or panel: those repaint straight onto the page in the framebuffer.
  if (!lock.ownsLock() || overlay != Overlay::None || !backgroundBuildWanted()) return;
  // A button is down or a page turn is queued: the next loop frames act on it first.
  if (buttonContactClosed() || anyButtonPressed() || pendingManualTurn != 0 || !buildTickHeapGate()) return;
  // A build step can lend the framebuffer (image probes), which hands it back white while the
  // panel still shows the page; redraw so nothing is later painted over the blank buffer.
  const uint32_t loansBefore = renderer.frameBufferLoanCount();
  const bool built = section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK, &buttonContactClosed);
  if (renderer.frameBufferLoanCount() != loansBefore) {
    pageBufferStale = true;
    requestUpdate();
  }
  if (!built) {
    LOG_ERR("ERS", "Background section build failed");
    backgroundBuildFailedSpine = currentSpineIndex;
    nextPageNumber = section->currentPage;
    section.reset();
    requestUpdate();
  } else if (section->isBuildComplete()) {
    // A target the render pass left pending because the index was still
    // partial (the readable-page yield) is applied now that the finished
    // index can satisfy it; applyDeferredReposition() otherwise only guesses
    // a proportional page from the clamped one.
    bool repositioned = false;
    if (pendingPageJump.has_value()) {
      section->currentPage = *pendingPageJump == std::numeric_limits<uint16_t>::max()
                                 ? section->pageCount - 1
                                 : static_cast<int>(*pendingPageJump);
      pendingPageJump.reset();
      repositioned = true;
    }
    if (applyDeferredReposition() || repositioned) requestUpdate();
  }
}

void EpubReaderActivity::openDictionaryWordSelect(int touchX, int touchY, TouchLongPressMode mode) {
  if (mode == TouchLongPressMode::Dictionary && SETTINGS.dictionaryName[0] == '\0') {
    showDictionaryMessage = true;
    dictionaryMessageTtf = false;
    dictionaryMessageTime = millis();
    requestUpdate();
    return;
  }
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    // TTF word selection: prebuilt boxes from engine run geometry
    // (TtfWordSelect). Run text lives in the runtime's scratch arena only
    // for this block — buildTtfWordSelectData copies everything out, so the
    // mark is released before the child activity runs.
    RenderLock lock;  // the render task owns/uses the scratch arena
    const size_t scratchMark = ttf_->scratch().mark();
    const auto showTtfDictionaryError = [this] {
      showDictionaryMessage = true;
      dictionaryMessageTtf = true;
      dictionaryMessageTime = millis();
      requestUpdate();
    };
    freeink::book::Page page{};
    if (!ttf_->readPage(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(ttfPage), &page)) {
      ttf_->scratch().release(scratchMark);
      LOG_ERR("ERS", "TTF dictionary: page read failed (spine %d page %d)", currentSpineIndex, ttfPage);
      showTtfDictionaryError();
      return;
    }
    freeink::book::LayoutParams params;
    ttf_->makeLayoutParams(renderer, params, automaticPageTurnActive);
    freeink::book::TtfWordSelectData data;
    if (params.font == nullptr ||
        !freeink::book::buildTtfWordSelectData(page, *static_cast<freeink::book::FontChain*>(params.font), data)) {
      ttf_->scratch().release(scratchMark);
      LOG_ERR("ERS", "TTF dictionary: word extraction failed");
      showTtfDictionaryError();
      return;
    }
    // The page's footnote list was captured during the last render (§3.5
    // item 8); the selector resolves bare numeric markers against it.
    data.footnotes = currentPageFootnotes;
    ttf_->scratch().release(scratchMark);

    const DictionaryWordSelectActivity::PageRenderFn renderFn{this, &EpubReaderActivity::renderTtfSelectorPage};
    auto selector = makeUniqueNoThrow<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(data), renderFn,
                                                                    touchX, touchY, mode);
    if (!selector) {
      LOG_ERR("ERS", "OOM: dictionary word selector");
      showTtfDictionaryError();
      return;
    }
    startActivityForResult(std::move(selector), [this](const ActivityResult& result) {
      if (!result.isCancelled && std::holds_alternative<FootnoteResult>(result.data)) {
        navigateToHref(std::get<FootnoteResult>(result.data).href, /*savePosition=*/true);
      }
      requestUpdate();
    });
    return;
  }
#endif
  if (!section) return;
  auto page = section->loadPage(section->currentPage);
  if (!page) return;

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;

  auto selector = makeUniqueNoThrow<DictionaryWordSelectActivity>(
      renderer, mappedInput, std::move(page), orientedMarginLeft, orientedMarginTop, touchX, touchY, mode);
  if (!selector) {
    LOG_ERR("ERS", "OOM: dictionary word selector");
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(selector), [this](const ActivityResult& result) {
    if (!result.isCancelled && std::holds_alternative<FootnoteResult>(result.data)) {
      navigateToHref(std::get<FootnoteResult>(result.data).href, /*savePosition=*/true);
    }
    requestUpdate();
  });
}

void EpubReaderActivity::startClipSelection(const int initialX, const int initialY,
                                            const ClipSelectionActivity::Mode mode) {
  if (!section || !epub || section->currentPage < 0 || section->currentPage >= section->pageCount) return;

  const int pageNumber = section->currentPage;
  const uint16_t pageCount = section->pageCount;
  const uint16_t spineIndex = static_cast<uint16_t>(currentSpineIndex);
  std::vector<std::unique_ptr<Page>> pages;
  pages.reserve(CLIP_SELECTION_PAGE_WINDOW);
  for (int pageOffset = 0; pageOffset < CLIP_SELECTION_PAGE_WINDOW && pageNumber + pageOffset < pageCount;
       ++pageOffset) {
    auto page = section->loadPage(pageNumber + pageOffset);
    if (!page) break;
    if (pageOffset == 0 && initialX >= 0 && CLIPPINGS.hasClippings()) {
      RenderLock lock;
      const int clippingIndex = clippingAtPoint(*page, initialX, initialY);
      if (clippingIndex >= 0) {
        const bool removed = CLIPPINGS.removeClippingAt(static_cast<size_t>(clippingIndex));
        if (!removed) LOG_ERR("CLIP", "Failed to delete highlighted clipping %d", clippingIndex);
        clippingMessage = removed ? StrId::STR_CLIPPING_REMOVED : StrId::STR_CLIPPING_DELETE_FAILED;
        showClippingMessage = true;
        clippingMessageTime = millis();
        requestUpdate();
        return;
      }
    }
    pages.push_back(std::move(page));
  }
  if (pages.empty()) {
    LOG_ERR("CLIP", "Failed to load page %d for clipping selection", pageNumber);
    requestUpdate();
    return;
  }

  if (buildViewportWidth == 0 || buildViewportHeight == 0) {
    LOG_ERR("CLIP", "Cannot anchor clipping before the reader viewport is initialized");
    requestUpdate();
    return;
  }
  const uint32_t layoutSignature =
      readerRenderSpecSignature(SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight));

  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginTop += SETTINGS.screenMargin;
  marginLeft += SETTINGS.screenMargin;

  std::string chapterTitle;
  const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (tocIndex >= 0) chapterTitle = epub->getTocItem(tocIndex).title;
  std::string bookTitle = epub->getTitle();
  std::string author = epub->getAuthor();

  auto activity = makeUniqueNoThrow<ClipSelectionActivity>(renderer, mappedInput, std::move(pages), marginLeft,
                                                           marginTop, initialX, initialY, mode);
  if (!activity) {
    LOG_ERR("CLIP", "Failed to allocate clipping selection activity");
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(activity), [this, spineIndex, pageNumber, pageCount, layoutSignature,
                                               bookTitle = std::move(bookTitle), author = std::move(author),
                                               chapterTitle = std::move(chapterTitle)](const ActivityResult& result) {
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    const auto& clipping = std::get<ClippingResult>(result.data);
    const uint16_t startPage = static_cast<uint16_t>(pageNumber + clipping.startPageOffset);
    const uint16_t endPage = static_cast<uint16_t>(pageNumber + clipping.endPageOffset);
    if (section && currentSpineIndex == spineIndex && endPage < section->pageCount) {
      section->currentPage = endPage;
      currentPageVisibleOffset = section->getVisibleTextOffsetForPage(endPage);
    }
    if (clipping.action == ClippingResult::Action::Bookmark) {
      addBookmark();
      showBookmarkMessage = true;
      bookmarkMessageTime = millis();
      requestUpdate();
      return;
    }
    if (clipping.action == ClippingResult::Action::Lookup) {
      auto page = section ? section->loadPage(section->currentPage) : nullptr;
      if (!page) {
        LOG_ERR("CLIP", "Failed to load lookup page");
        requestUpdate();
        return;
      }
      int top, right, bottom, left;
      renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
      auto lookup = makeUniqueNoThrow<DictionaryWordSelectActivity>(
          renderer, mappedInput, std::move(page), left + SETTINGS.screenMargin, top + SETTINGS.screenMargin, -1, -1,
          TouchLongPressMode::Dictionary, clipping.text);
      if (!lookup) {
        LOG_ERR("CLIP", "OOM: dictionary lookup activity");
        requestUpdate();
        return;
      }
      startActivityForResult(std::move(lookup), [this](const ActivityResult& result) {
        if (!result.isCancelled) openDictionaryWordSelect();
      });
      return;
    }
    const uint16_t paragraphIndex =
        section ? section->getParagraphIndexForPage(startPage).value_or(UINT16_MAX) : UINT16_MAX;
    const auto addResult = CLIPPINGS.addClipping(
        spineIndex, startPage, endPage, pageCount, clipping.startWordIndex, clipping.endWordIndex, clipping.wordCount,
        chapterTitle.c_str(), paragraphIndex, clipping.text, layoutSignature, clipping.startOffset, clipping.endOffset);
    bool exported = false;
    if (addResult == ClippingStore::AddResult::Added) {
      exported = ClippingsManager::saveClipping(bookTitle, author, chapterTitle, startPage + 1, clipping.text);
    }
    clippingMessage = addResult == ClippingStore::AddResult::LimitReached ? StrId::STR_CLIPPING_LIMIT_REACHED
                      : exported                                          ? StrId::STR_CLIPPING_SAVED
                      : addResult == ClippingStore::AddResult::Added      ? StrId::STR_CLIPPING_EXPORT_FAILED
                                                                          : StrId::STR_CLIPPING_FAILED;
    showClippingMessage = true;
    clippingMessageTime = millis();
    requestUpdate();
  });
}

void EpubReaderActivity::openFootnoteSelect(const bool reopenMenuOnCancel) {
  if (!section || currentPageFootnotes.empty()) return;
  if (currentPageFootnotes.size() == 1) {
    navigateToHref(currentPageFootnotes[0].href, true);
    return;
  }

  auto page = section->loadPage(section->currentPage);
  if (!page) return;

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  auto selector = makeUniqueNoThrow<EpubReaderFootnoteSelectActivity>(renderer, mappedInput, std::move(page),
                                                                      orientedMarginLeft, orientedMarginTop);
  if (!selector) {
    LOG_ERR("ERA", "OOM: EpubReaderFootnoteSelectActivity");
    return;
  }
  startActivityForResult(std::move(selector), [this, reopenMenuOnCancel](const ActivityResult& result) {
    if (result.isCancelled) {
      if (reopenMenuOnCancel) {
        openReaderMenu();
      } else {
        requestUpdate();
      }
      return;
    }
    const auto& footnoteResult = std::get<FootnoteResult>(result.data);
    navigateToHref(footnoteResult.href, true);
    requestUpdate();
  });
}

void EpubReaderActivity::openClippings() {
  if (!CLIPPINGS.hasClippings()) {
    requestUpdate();
    return;
  }
  auto list = makeUniqueNoThrow<EpubReaderClippingListActivity>(renderer, mappedInput);
  if (!list) {
    LOG_ERR("CLIP", "OOM: clipping list");
    return;
  }
  startActivityForResult(std::move(list), [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      if (const auto* progress = std::get_if<ProgressChangeResult>(&result.data)) {
        applyProgressChange(*progress);
        return;
      }
    }
    requestUpdate();
  });
}

void EpubReaderActivity::loop() {
  if (loadFailurePopup.isActive()) {
    loadFailurePopup.handleInput(mappedInput, [this] { requestUpdate(); });
    return;
  }
  if (!epub) {
    finish();
    return;
  }

  rememberBookOnceRendered();
  retryDeferredOverlayPush();

  // The radio stopped for a chapter build: once the render task has let go of the lock, that page
  // is on screen; once the build tick below has finished the chapter, no parser is left alive and
  // the radio may start again.
  if (radioReleasedForBuild.load()) {
    RenderLock lock(RenderLock::Mode::Try);
    if (lock.ownsLock() && !(section && section->isBuilding())) {
      radioReleasedForBuild = false;
      bleturner::afterPaint();
    }
  }

  // The remote linked, or failed to, after the status bar showed its note: paint the page again.
  {
    RenderLock lock(RenderLock::Mode::Try);
    if (lock.ownsLock() && linkNoteTitle.repaint(bleturner::linkNote())) requestUpdate();
  }

  // Someone else turned the screen while this reader was stacked (the control
  // center's orientation tile). Reflow before the next render, or the page
  // would be drawn with a layout built for the previous frame size.
  if (appliedOrientation != SETTINGS.orientation) {
    applyOrientation(SETTINGS.orientation);
    requestUpdate();
    return;
  }

  // A loop-task build tick can occupy the RenderLock for hundreds of ms. Input
  // is only polled between loop passes, and the touch controller reports just
  // the current frame, so a gesture completed inside a tick stall never
  // reaches the reader. Defer background layout while input is active or an
  // edge is still pending.
  const bool inputPriority = mappedInput.rawInputPriority();

  constexpr unsigned long IDLE_PREWARM_DEBOUNCE_MS = 400;
  constexpr unsigned long IDLE_PREWARM_RETRY_BACKOFF_MS = 5000;
  if (!inputPriority && millis() >= idlePrewarmRetryAt) {
    RenderLock lock(RenderLock::Mode::Try);
    if (lock.ownsLock() && overlay == Overlay::None && section && !section->isBuilding() && renderer.hasFrameBuffer() &&
        lastRenderCompleteMs != 0 && millis() - lastRenderCompleteMs > IDLE_PREWARM_DEBOUNCE_MS &&
        ESP.getFreeHeap() > RENDER_MIN_FREE_HEAP && ESP.getMaxAllocHeap() > BACKGROUND_BUILD_MIN_MAX_ALLOC &&
        (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
      const int nextPage = section->currentPage + 1;
      if (nextPage >= static_cast<int>(section->pageCount)) {
        // Last page of the chapter: nothing to prewarm; stop re-checking.
        idlePrewarmSpine = currentSpineIndex;
        idlePrewarmPage = section->currentPage;
      } else if (const auto p = section->loadPage(nextPage)) {
        if (auto* fcm = renderer.getFontCacheManager()) {
          // The prewarm pass paints the NEXT page into the live framebuffer
          // only to scan its glyphs for the font cache; it never pushes the
          // panel. But sleep entry (the "Entering sleep" popup, Quick
          // Resume), a manual refresh, and screenshots all display the
          // framebuffer without a re-render — leave page N+1 in there and
          // the user sees the book flip a page as the device goes to sleep.
          // storeBwBuffer() snapshots the frame in 8 KB chunks (no
          // contiguous block) so it can be restored once the scan is done.
          // The overlay==None guard above also protects an open toolbar's
          // own stored snapshot from being freed by this store.
          if (renderer.storeBwBuffer()) {
            const auto t0 = millis();
            auto scope = fcm->createPrewarmScope();
            p->render(renderer, SETTINGS.getReaderFontId(), 0, 0);
            scope.endScanAndPrewarm();
            // Nothing was pushed to the panel since the store, so the
            // controller's differential baseline already matches the
            // restored frame — no baseline resync needed.
            renderer.restoreBwBuffer(false);
            // Mark only on success: a failed snapshot is retried after the
            // backoff instead of being silently skipped for this page.
            idlePrewarmSpine = currentSpineIndex;
            idlePrewarmPage = section->currentPage;
            idlePrewarmRetryAt = 0;
            LOG_DBG("ERS", "Idle prewarm: page %d in %lums", nextPage, millis() - t0);
          } else {
            idlePrewarmRetryAt = millis() + IDLE_PREWARM_RETRY_BACKOFF_MS;
          }
        } else {
          // No font cache manager: the prewarm cannot run at all this
          // session; mark done so we stop rechecking.
          idlePrewarmSpine = currentSpineIndex;
          idlePrewarmPage = section->currentPage;
        }
      } else {
        // loadPage() failed (SD hiccup): retry after the backoff instead of
        // re-attempting the SD load on every loop tick.
        idlePrewarmRetryAt = millis() + IDLE_PREWARM_RETRY_BACKOFF_MS;
      }
    }
  }

  if (!inputPriority) {
    RenderLock lock(RenderLock::Mode::Try);
    // Never next to the radio: once the reader reaches the watermark, the build starts in
    // renderBook(), which stops the radio first.
    if (lock.ownsLock() && section && !section->isBuilding() && section->isPartial() && buildViewportWidth > 0 &&
        !partialRebuildStartFailed && !bleturner::holdsHeap() &&
        section->currentPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(section->pageCount)) {
      const ReaderRenderSpec buildSpec = SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight);
      if (!section->startBuild(buildSpec)) {
        partialRebuildStartFailed = true;
        LOG_ERR("ERS", "Failed to start deferred partial extension build");
      } else {
        LOG_DBG("ERS", "Reader near partial watermark (%d/%d), resuming extension build", section->currentPage,
                section->pageCount);
      }
    }
  }

  // The legacy chapter build advances from the post-input idle path below
  // (the backgroundBuildTick() call after the page-turn detection), so one
  // bounded build step runs per idle loop frame, never two.

#if defined(CROSSPOINT_TTF_READER)
  if (!inputPriority) {
    RenderLock lock(RenderLock::Mode::Try);
    if (lock.ownsLock() && ttf_ && buildTickHeapGate()) {
      ttfBackgroundBuildTick();
    }
  }
#endif

  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();
  clearEndOfBookOptionsIfNeeded();

  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

#ifdef READING_STATS_ENABLED
  if (atEndOfBook || stats.isCompleted) {
#else
  if (atEndOfBook) {
#endif
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  // Finish-reading prompt (ADR-5): settled-frame evaluation after committed
  // forward progress. Deferred while a gesture is in flight (inputPriority);
  // the helper itself gates overlays, the end-of-book menu, and builds.
#ifdef READING_STATS_ENABLED
  if (!inputPriority) maybePromptFinishReading();
#endif

  // The open overlay owns the touch latch; page-turn tap detection must not
  // consume a tap that FUI will route to the sheet.
  const auto touch =
      overlay == Overlay::None
          ? ReaderUtils::detectTouchPageTurn(renderer, mappedInput, ReaderUtils::isRtlBookLanguage(epub->getLanguage()))
          : ReaderUtils::TouchPageTurn{false, false, 0};

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  if (showDictionaryMessage && (millis() - dictionaryMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showDictionaryMessage = false;
    requestUpdate();
  }

  if (showFontDamagedMessage && (millis() - fontDamagedMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showFontDamagedMessage = false;
    requestUpdate();
  }

  // The side-edge frontlight drag is dispatched globally by ActivityManager;
  // allowsFrontlightSwipe() vetoes it while the reader chrome owns input.

  // The toolbar reader menu owns all input while shown, ahead of the automatic page turn
  // below: the More panel's rate popup switches automatic turning on and leaves the panel
  // open, so the timer must neither flip the page under it nor eat the panel's next
  // Confirm/Back release.
  if (overlay != Overlay::None) {
    if (usesToolbarMenu()) {
      // Hold the interval at zero elapsed so closing the panel starts a fresh one.
      lastPageTurnTime = millis();
      handleOverlayInput();
      return;
    }
    // The style was switched off while an overlay was up (Settings reached via
    // the More panel); fall back to the clean page.
    overlay = Overlay::None;
    discardOverlayPage();
    requestUpdate();
    return;
  }

  // One setting decides the long-press route and only the matching consumer
  // reads wasScreenLongPress() (it suppresses the touch contact and stays true
  // for the rest of the frame -- InputManager clears the event next update).
  // Without this, upstream's clip gate shadowed the dictionary branch since
  // v2.6.0 because it ran first and returned after consuming the event.
  const auto longPressRoute = resolveTouchLongPressRoute(SETTINGS.touchLongPressAction);
  static_assert(CrossPointSettings::TOUCH_LP_DICTIONARY == 0 && CrossPointSettings::TOUCH_LP_IGNORE == 1 &&
                    CrossPointSettings::TOUCH_LP_FOOTNOTE == 2,
                "TouchLongPressRoute.h maps these persisted menu indexes");
  int selectionX = 0;
  int selectionY = 0;
  if (!atEndOfBook && SETTINGS.touchReaderControls && longPressRoute == TouchLongPressRoute::ClipSelection &&
      mappedInput.wasScreenLongPress(selectionX, selectionY)) {
    automaticPageTurnActive = false;
    pendingManualTurn = 0;
    startClipSelection(selectionX, selectionY);
    return;
  }

  switch (mappedInput.homeButtonAction()) {
    case HomeButtonAction::ReaderMenu:
    case HomeButtonAction::Bookmark:
    case HomeButtonAction::Sync:
    case HomeButtonAction::Dictionary:
    case HomeButtonAction::Footnotes:
    case HomeButtonAction::CreateClipping:
      automaticPageTurnActive = false;
      break;
    default:
      break;
  }

  if (automaticPageTurnActive) {
    const bool touchStopsAutoTurn = ReaderUtils::isTouchMenuGesture(renderer, mappedInput);
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) || touchStopsAutoTurn) {
      automaticPageTurnActive = false;
      if (touchStopsAutoTurn) haptic_feedback::touchAction();
      requestUpdate();
      return;
    }

    if (!section
#if defined(CROSSPOINT_TTF_READER)
        && !ttf_
#endif
    ) {
      requestUpdate();
      return;
    }

    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      LOG_DBG("ERS", "auto-turn deferred: render busy");
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      const bool succeeded = pageTurn(true);
      notePageTurn(true, succeeded);
      requestUpdate();
      return;
    }
  }

  if (showClippingMessage && (millis() - clippingMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showClippingMessage = false;
    requestUpdate();
  }

  // While the end-of-book suggestion menu is up it owns Confirm/Back/navigation, so it
  // gets this tick's input first and the long-press shortcuts below stay inert behind it
  // -- a hold there must not drop a bookmark onto the suggestion screen or paint the
  // dictionary word picker over it. Anything the menu does not handle (long-press Back to
  // the file browser, say) still falls through to the regular handlers.
  if (handleEndOfBookMenu()) {
    return;
  }

  if (SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF && mappedInput.hasTouch() &&
      (longPressRoute == TouchLongPressRoute::Dictionary || longPressRoute == TouchLongPressRoute::Footnote) &&
      !showDictionaryMessage) {
    int lx = 0, ly = 0;
    if (mappedInput.wasScreenLongPress(lx, ly)) {
      // Match the clip branch: the long press itself stops automatic turning
      // (the selector must not open over a page that is about to flip), and a
      // manual turn queued while the turn guard was busy must not fire on the
      // page left behind after the selector closes.
      automaticPageTurnActive = false;
      pendingManualTurn = 0;
      const auto mode = (longPressRoute == TouchLongPressRoute::Footnote) ? TouchLongPressMode::Footnote
                                                                          : TouchLongPressMode::Dictionary;
      openDictionaryWordSelect(lx, ly, mode);
      return;
    }
  }

  const bool endOfBookMenuOpen = endOfBookMenuActive();
#if FREEINK_CAP_MENU_BUTTON
  // Long-press Confirm runs the user-selected long-press function. Boards
  // without any Confirm button (physical or synthesized) drop this entirely.
  const unsigned long confirmHoldMs = confirmLongPressThreshold();
  // wasLongPressed() suppresses the release that follows it, so leave it unpolled while
  // the end-of-book menu owns Confirm -- otherwise the menu never sees that release.
  const bool confirmLongPressed = !endOfBookMenuOpen && confirmHoldMs != 0 &&
                                  mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, confirmHoldMs);
  if (confirmLongPressed) {
    switch (static_cast<HomeButtonAction>(SETTINGS.homeButtonLongPressAction)) {
      case HomeButtonAction::Bookmark:
        addBookmark();
        showBookmarkMessage = true;
        bookmarkMessageTime = millis();
        requestUpdate();
        break;
      case HomeButtonAction::Sync:
        if (launchKOReaderSync()) {
          return;
        }
        break;
      case HomeButtonAction::Dictionary:
        openDictionaryWordSelect();
        return;
      case HomeButtonAction::CreateClipping:
        startClipSelection();
        return;
      case HomeButtonAction::ReaderMenu:
      case HomeButtonAction::Ignore:
      default:
        break;
    }
  }
#endif

  if (!endOfBookMenuOpen) {
    switch (mappedInput.homeButtonAction()) {
      case HomeButtonAction::Bookmark:
        if (!showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        return;
      case HomeButtonAction::Sync:
        if (launchKOReaderSync()) return;
        break;
      case HomeButtonAction::Dictionary:
        if (!showDictionaryMessage) openDictionaryWordSelect();
        return;
      case HomeButtonAction::CreateClipping:
        startClipSelection();
        return;
      case HomeButtonAction::ReaderMenu:
        if (usesToolbarMenu() && (section
#if defined(CROSSPOINT_TTF_READER)
                                  || ttf_
#endif
                                  ))
          openOverlay(Overlay::Toolbar);
        else
          openReaderMenu();
        return;
      default:
        break;
    }
  }

  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);

  const bool touchMenu = ReaderUtils::isTouchMenuGesture(renderer, mappedInput);
  if (confirmReleased || touchMenu) {
    if (touchMenu) haptic_feedback::touchAction();
    // Toolbar style: the page is on screen and in the framebuffer, so paint the
    // toolbar over it (one refresh) instead of pushing a full-screen menu.
    if (usesToolbarMenu() && section) {
      pendingManualTurn = 0;
      openOverlay(Overlay::Toolbar);
    } else {
      openReaderMenu();
    }
    return;  // no build step before the menu shows
  }

  if (footnoteDepth > 0 && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_BACK_OR_HOME_MS) {
    restoreSavedPosition();
    return;
  }

  if (handleBackNavigation()) {
    return;
  }

  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::CREATE_CLIPPING &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    startClipSelection();
    return;
  }

  if ((!endOfBookMenuOpen && mappedInput.homeButtonAction() == HomeButtonAction::Footnotes) ||
      (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
       mappedInput.wasReleased(MappedInputManager::Button::Power) &&
       !mappedInput.wasReleased(MappedInputManager::Button::Down))) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      openFootnoteSelect(false);
    }
    return;
  }

  constexpr unsigned long kMinManualTurnGapMs = 200;
  const bool turnGuardActive = RenderLock::peek() || (millis() - lastPageTurnTime) < kMinManualTurnGapMs;
  if (pendingManualTurn != 0 && !turnGuardActive) {
    if (!section
#if defined(CROSSPOINT_TTF_READER)
        && !ttf_
#endif
    ) {
      pendingManualTurn = 0;
      return;
    }
    // Magnitude-carrying jump (queued-input aggregation §3.2/audit #4): the
    // parked magnitude applies ONCE, joined with this frame's fresh net so
    // presses that arrived while the gate was busy are not dropped.
    const auto turn = ReaderUtils::detectPageTurnSteps(mappedInput, touch);
    const int steps = turnnet::accumulate(pendingManualTurn, turn.steps);
    pendingManualTurn = 0;
    const bool succeeded = pageTurnBy(steps);
    notePageTurn(steps > 0, succeeded);
    if (succeeded && pendingManualTurnTouch) haptic_feedback::touchAction();
    pendingManualTurnTouch = false;
    requestUpdate();
    return;
  }

  const auto turn = ReaderUtils::detectPageTurnSteps(mappedInput, touch);
  if (!turn.prev && !turn.next) {
    // Nothing to act on in this frame: build more of the chapter.
    backgroundBuildTick();
    return;
  }

  if (handleEndOfBookPageTurn(turn.prev, turn.next)) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    return;
  }

  const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
  const bool longPress = !turn.fromTilt && heldMs >= ReaderUtils::SKIP_HOLD_MS;
  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    const bool succeeded = skipPages(turn.next ? 1 : -1);
    notePageTurn(false, succeeded);
    if (succeeded && (touch.prev || touch.next)) haptic_feedback::touchAction(true);
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        turn.next ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                  : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    if (touch.prev || touch.next) haptic_feedback::touchAction(true);
    requestUpdate();
    return;
  }

  if (!section
#if defined(CROSSPOINT_TTF_READER)
      && !ttf_
#endif
  ) {
    requestUpdate();
    return;
  }

  if (turnGuardActive) {
    // Park the frame's net magnitude (§3.2): a guarded frame's presses join
    // any already-parked jump instead of replaying one turn per refresh.
    pendingManualTurn = turnnet::accumulate(pendingManualTurn, turn.steps);
    pendingManualTurnTouch = touch.prev || touch.next;
    return;
  }

  const bool succeeded = pageTurnBy(turn.steps);
  notePageTurn(turn.steps > 0, succeeded);
  if (succeeded && (touch.prev || touch.next)) haptic_feedback::touchAction();
  requestUpdate();
}

void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) return;
  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) return;

  percent = clampPercent(percent);

  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) targetSize = bookSize - 1;

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) return;

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  const int found = epub->getSpineIndexForSize(targetSize);
  if (found >= 0) {
    targetSpineIndex = found;
    prevCumulative = (found > 0) ? epub->getCumulativeSpineItemSize(found - 1) : 0;
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  pendingSpineProgress = std::clamp(pendingSpineProgress, 0.0f, 1.0f);

  {
    RenderLock lock;
    clearDeferredReposition();
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::applyProgressChange(const ProgressChangeResult& sync) {
  if (sync.hasVisibleTextOffset && sync.spineIndex >= 0 && sync.spineIndex < epub->getSpineItemsCount()) {
    RenderLock lock;
    clearDeferredReposition();
    const auto page = section && currentSpineIndex == sync.spineIndex
                          ? section->getPageForVisibleTextOffset(sync.visibleTextOffset)
                          : std::nullopt;
    if (page) {
      section->currentPage = *page;
    } else {
      currentSpineIndex = sync.spineIndex;
      pendingOffsetJump = sync.visibleTextOffset;
      nextPageNumber = std::max(0, sync.page);
      section.reset();
    }
    requestUpdate();
    return;
  }

  int targetSpineIndex = sync.spineIndex;
  int targetPage = sync.page;
  const int activeTotalPages = section ? section->estimatedTotalPages() : 0;
  const bool cachedPageMatchesActiveSection = section && sync.totalPages > 0 && currentSpineIndex == sync.spineIndex &&
                                              sync.page >= 0 && sync.page < sync.totalPages &&
                                              activeTotalPages == sync.totalPages;

  if (!cachedPageMatchesActiveSection && sync.hasSavedProgress) {
    const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
    CrossPointPosition fallback =
        ProgressMapper::toCrossPoint(epub, {sync.xpath, sync.percentage}, renderer, currentSpineIndex, totalPages);
    targetSpineIndex = fallback.spineIndex;
    targetPage = fallback.pageNumber;
  }

  RenderLock lock;
  clearDeferredReposition();

  if (currentSpineIndex != targetSpineIndex) {
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = targetPage;
    section.reset();
  } else if (section && section->currentPage != targetPage) {
    const int clampedTargetPage = std::max(0, targetPage);
    section->currentPage = clampedTargetPage;
  } else if (!section) {
    nextPageNumber = targetPage;
  }
  requestUpdate();
}

int EpubReaderActivity::currentTocIndex() const {
  if (!epub) return 0;
  if (section) return std::max(0, section->getTocIndexForPage(section->currentPage));
  // Child screens may have released the section; its saved anchor map remains readable.
  Section cachedSection(epub, currentSpineIndex, renderer);
  return std::max(0, cachedSection.getTocIndexForPage(nextPageNumber));
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
#ifdef READING_STATS_ENABLED
  recordCurrentPageReadingTime();
  pageShownAtMs = 0;
#endif
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (result.isCancelled) {
      openReaderMenu();
    } else {
      const auto& sync = std::get<ProgressChangeResult>(result.data);

      applyProgressChange(sync);
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      int tocIndex;
      // Release the section while the chapter list is up (mirrors the
      // TEXT_SETTINGS path): picking a chapter resets it anyway, and its
      // tens-of-KB footprint is the difference between the chapter list
      // holding its CJK glyph arena (RAM-only repaints) and re-reading
      // glyphs from SD on every row step. Cancel restores via the same
      // cached-position rebuild TEXT_SETTINGS uses.
      {
        RenderLock lock;
        tocIndex = currentTocIndex();
        if (section) {
          rememberCurrentContentOffset();
          cachedSpineIndex = currentSpineIndex;
          cachedChapterTotalPageCount = section->pageCount;
          nextPageNumber = section->currentPage;
        }
        section.reset();
      }
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, tocIndex),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              openReaderMenu();
              return;
            }
            const auto& chapterResult = std::get<ChapterResult>(result.data);
            RenderLock lock;
            clearDeferredReposition();
            currentSpineIndex = chapterResult.spineIndex;
            pendingAnchor = chapterResult.anchor;
            nextPageNumber = 0;
            section.reset();
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      openFootnoteSelect(true);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TEXT_SETTINGS: {
      startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                    TextSettingsActivity::Tab::Family),
                             [this](const ActivityResult&) {
                               {
                                 RenderLock lock;
                                 if (section) {
                                   rememberCurrentContentOffset();
                                   cachedSpineIndex = currentSpineIndex;
                                   cachedChapterTotalPageCount = section->pageCount;
                                   nextPageNumber = section->currentPage;
                                 }
                                 section.reset();
                               }
                               openReaderMenu();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BLUETOOTH:
    case EpubReaderMenuActivity::MenuAction::NIGHT_MODE:
      // Handled in-place by EpubReaderMenuActivity so its On/Off value updates
      // without closing the menu.
      break;
    case EpubReaderMenuActivity::MenuAction::FRONTLIGHT:
      // Handled in-place by EpubReaderMenuActivity using the live frontlight HAL.
      break;
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      const int initialPercent = bookPercentFor(chapterPosition());
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              openReaderMenu();
            } else {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DICTIONARY: {
      openDictionaryWordSelect();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult&) { openReaderMenu(); });
          break;
        }
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHomeRequested();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock;
        if (epub) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = 0;
          uint16_t backupPageCount = 0;
#if defined(CROSSPOINT_TTF_READER)
          // TTF books hold no Section mirror: capture the generation-tagged
          // position BEFORE the runtime closes, so the clear saves a TTF
          // record instead of a legacy page=0 one (which would drop the
          // reader back to chapter start on reopen).
          bool ttfPosition = false;
          uint32_t ttfCharOffset = 0;
          uint32_t ttfGen = 0;
          if (ttf_ && ttfGenerationValid) {
            ttfPosition = true;
            backupPage = static_cast<uint16_t>(ttfPage);
            backupPageCount = static_cast<uint16_t>(ttfPageCount);
            ttfCharOffset = ttfCurrentCharStart;
            ttfGen = ttfGeneration;
            if (backupPageCount > 0 && backupPage >= backupPageCount) {
              // Count mirror can lag a partial build: never write an
              // internally inconsistent record.
              backupPage = static_cast<uint16_t>(backupPageCount - 1);
            }
          }
#endif
          if (section) {
            backupPage = section->currentPage;
            backupPageCount = section->pageCount;
            section.reset();
          }
          // Close every handle into the cache dir BEFORE removeDir: the
          // prefetch worker's own FIBP writer/reader and the TTF runtime keep
          // ficache files open, and a single open file fails the recursive
          // removal — leaving the whole epub_<hash> folder (ficache included)
          // behind on SD.
#if defined(CROSSPOINT_TTF_READER)
          stopFibpWorker();
          if (ttf_) {
            ttf_->close();
          }
#endif
          epub->clearCache();
          epub->setupCacheDir();
          // Single-writer rule (design §4.7): the cache-clear save also goes
          // through the saver so its state matches what is on disk.
          bool saved = false;
#if defined(CROSSPOINT_TTF_READER)
          if (ttfPosition) {
            saved = progressManager.saveNowTtf(epub->getCachePath().c_str(), backupSpine, backupPage, backupPageCount,
                                               ttfCharOffset, ttfGen);
          }
#endif
          if (!saved && !progressManager.saveNow(epub->getCachePath().c_str(), backupSpine, backupPage, backupPageCount,
                                                 false, 0)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
        }
      }
      onGoHomeRequested();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock;
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      launchKOReaderSync();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SAVE_CLIPPING: {
      startClipSelection();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::VIEW_CLIPPINGS: {
      openClippings();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      break;
    }
#ifdef READING_STATS_ENABLED
    case EpubReaderMenuActivity::MenuAction::READING_STATS: {
      recordCurrentPageReadingTime();
      BookReadingStats displayStats = stats;
      if (SETTINGS.shouldTrackReadingStats()) {
        displayStats.totalReadingSeconds += sessionReadingSeconds;
      }
      auto statsMenu = makeUniqueNoThrow<ReadingStatsMenuActivity>(
          renderer, mappedInput, displayStats, epub->getTitle(), epub->getCachePath(), epub->getAuthor());
      if (!statsMenu) {
        LOG_ERR("ERS", "OOM: reading stats menu");
        break;
      }
      startActivityForResult(std::move(statsMenu), [this](const ActivityResult& result) {
        if (epub && SETTINGS.shouldTrackReadingStats()) handleBookStatsReturn();
        if (std::holds_alternative<ClearPaceResult>(result.data) && epub) {
          stats.clearWpmStats();
          stats.save(epub->getCachePath());
        }
        openReaderMenu();
      });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_STATS: {
      startActivityForResult(
          std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_BOOK_STATS), epub->getTitle()),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              openReaderMenu();
              return;
            }
            if (SETTINGS.shouldTrackReadingStats() && epub && BookReadingStats::remove(epub->getCachePath())) {
              stats = BookReadingStats{};
              sessionReadingSeconds = 0;
              hasSessionStartLocalDateTime = getCurrentLocalReadingStatsDateTime(sessionStartLocalDateTime);
            }
            openReaderMenu();
          });
      break;
    }
#endif
  }
}

unsigned long EpubReaderActivity::confirmLongPressThreshold() const {
  switch (static_cast<HomeButtonAction>(SETTINGS.homeButtonLongPressAction)) {
    case HomeButtonAction::Bookmark:
    case HomeButtonAction::Dictionary:
    case HomeButtonAction::CreateClipping:
      return ReaderUtils::BOOKMARK_HOLD_MS;
    case HomeButtonAction::Sync:
      return KOREADER_STORE.hasCredentials() ? ReaderUtils::GO_HOME_MS : 0;
    case HomeButtonAction::ReaderMenu:
    case HomeButtonAction::Ignore:
    default:
      return 0;
  }
}

bool EpubReaderActivity::launchKOReaderSync() {
  flushReaderSession();

  // Without a KOReader server the sync activity only brings the device online
  // and drains that outbox; a failed join exits back to the reader with the
  // events still queued.
  if (!KOREADER_STORE.hasCredentials()) {
    activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
        renderer, mappedInput, epub->getPath(), getCurrentPosition(), SavedProgressPosition{}, "",
        /*pluginEventsOnly=*/true));
    return true;
  }

  RenderLock renderLock;

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;

  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos;
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  // The synchronous save below routes through the saver (single writer,
  // design §4.7); saveNow also records the position so the background tick
  // never rewrites it after the epub has been released (design §4.4 KOReader
  // exception).
  {
    std::optional<uint32_t> savedOffset;
    if (section && currentPage >= 0 && currentPage < section->pageCount) {
      savedOffset = (currentPage == section->currentPage && currentPageVisibleOffset.has_value())
                        ? currentPageVisibleOffset
                        : section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage));
    }
#if defined(CROSSPOINT_TTF_READER)
    // The TTF path records the generation-tagged char offset so the record
    // keeps its restore anchor; the legacy path keeps the visible-text offset.
    const bool saved = ttf_ ? progressManager.saveNowTtf(epub->getCachePath().c_str(), currentSpineIndex, currentPage,
                                                         totalPages, ttfCurrentCharStart, ttfGeneration)
                            : progressManager.saveNow(epub->getCachePath().c_str(), currentSpineIndex, currentPage,
                                                      totalPages, savedOffset.has_value(), savedOffset.value_or(0));
#else
    const bool saved = progressManager.saveNow(epub->getCachePath().c_str(), currentSpineIndex, currentPage, totalPages,
                                               savedOffset.has_value(), savedOffset.value_or(0));
#endif
    if (!saved) {
      LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
      pendingSyncSaveError = true;
      requestUpdate();
      return true;
    }
  }

  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    if (section) {
      nextPageNumber = section->currentPage;
    }
    discardOverlayPage();
    ImageBlock::releaseRenderCache();
    ImageBlock::setExtractor(nullptr, nullptr);
#ifdef BOARD_HAS_PSRAM
    ImageBlock::setPsramExtractor(nullptr, nullptr);
#endif
    section.reset();
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->releaseSdFontCaches();
    }
    // No rendering may run while the chapter mapper borrows the framebuffer.
    {
      GfxRenderer::FrameBufferLoan loan(renderer);
      localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
    }
    // The destructor can no longer save the back-stack once epub is gone.
    if (footnoteDepth > 0) saveLinkStack();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, localPos, std::move(localKoPos), std::move(localChapterName)
#if defined(CROSSPOINT_TTF_READER)
                                                                                 ,
      ttfGeneration, ttf_ != nullptr
#endif
      ));
  return true;
}

void EpubReaderActivity::applyInitialOrientation() {
  ReaderActivity::applyInitialOrientation();
  appliedOrientation = SETTINGS.orientation;
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // Also runs when SETTINGS already holds the new value but this layout was
  // built for the old one — that is what an external change looks like here.
  if (SETTINGS.orientation == orientation && appliedOrientation == orientation) {
    return;
  }

  RenderLock lock(*this);
  if (section) {
    rememberCurrentContentOffset();
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    // Geometry change -> new generation -> new caches; the position rides the
    // chapter char offset (design section 3.5).
    ttfInvalidateCaches();
  }
#endif

  if (SETTINGS.orientation != orientation) {
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();
  }
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  appliedOrientation = orientation;
  section.reset();
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  const bool wasActive = automaticPageTurnActive;
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
  } else {
    lastPageTurnTime = millis();
    pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
    automaticPageTurnActive = true;
  }

#if defined(CROSSPOINT_TTF_READER)
  if (ttf_ && automaticPageTurnActive != wasActive) {
    RenderLock lock;
    ttfInvalidateCaches();
  }
#endif

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    RenderLock lock;
    if (section) {
      rememberCurrentContentOffset();
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

bool EpubReaderActivity::pageTurn(bool isForwardTurn) {
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) return ttfPageTurn(isForwardTurn);
#endif
  if (!section) return false;
  // A "Bluetooth failed" note has been read: the page this turn paints shows the title again.
  bleturner::acknowledgeLinkNote();
  {
    RenderLock lock;
    clearDeferredReposition();
  }

#ifdef READING_STATS_ENABLED
  uint32_t dwellSeconds = 0;
  const bool haveDwell = currentPageReadingSecondsForStats(dwellSeconds);
  if (SETTINGS.shouldTrackReadingStats()) {
    recordCurrentPageReadingTime();
    if (isForwardTurn && haveDwell) {
      recordForwardPagePaceSample(dwellSeconds, currentPageWordsOnPage);
      if (stats.totalPagesTurned < UINT32_MAX) stats.totalPagesTurned++;
      if (globalStats.totalPagesTurned < UINT32_MAX) globalStats.totalPagesTurned++;
      if (sessionPageTurns < UINT16_MAX) sessionPageTurns++;
    }
  }
#endif

  if (isForwardTurn) {
#ifdef READING_STATS_ENABLED
    // A declined 100%-completion prompt applies only until the reader moves
    // forward again; otherwise it would suppress the prompt on a later exit.
    stats.completionPromptDismissedAtHundred = false;
#endif
    if (section->currentPage < section->pageCount - 1 || section->isBuilding()) {
      section->currentPage++;
      lastPageTurnTime = millis();
    } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex++;
      // Promote a prefetched next-section if it matches the new chapter
      if (nextSectionPrefetch && nextSectionSpineIndex == currentSpineIndex) {
        section = std::move(nextSectionPrefetch);
        nextSectionSpineIndex = -1;
      } else {
        section.reset();
      }
      lastPageTurnTime = millis();
    } else {
      currentSpineIndex = epub->getSpineItemsCount();
#ifdef READING_STATS_ENABLED
      // Crossing past the last readable page is definitive completion.
      if (SETTINGS.shouldTrackReadingStats() && !stats.isCompleted) setBookCompleted(true);
#endif
      // The EOB screen bypasses the section render path, so persist the
      // terminal position synchronously instead of waiting for the gate.
      if (!saveFinishedProgress()) {
        LOG_ERR("ERS", "Failed to save end-of-book progress");
      }
      lastPageTurnTime = millis();
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
      lastPageTurnTime = millis();
    } else if (currentSpineIndex > 0) {
      RenderLock lock;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      currentSpineIndex--;
      section.reset();
      // Clear prefetch if it doesn't match the new current chapter
      if (nextSectionSpineIndex != currentSpineIndex) {
        nextSectionPrefetch.reset();
        nextSectionSpineIndex = -1;
      }
      lastPageTurnTime = millis();
    } else {
      return false;
    }
  }

#ifdef READING_STATS_ENABLED
  pageShownAtMs = millis();
#endif
  logMemAt("page_turn");
  return true;
}

bool EpubReaderActivity::skipPages(int amount) {
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    ttfFrameRenderComplete.store(false, std::memory_order_release);
    if (amount > 0) {
      if (currentSpineIndex + 1 >= epub->getSpineItemsCount()) return false;
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex++;
      // Drops the chapter-scoped restore state, including any
      // ttfRestoreLastPage left by a previous back-step.
      clearDeferredReposition();
      return true;
    }
    if (ttfPage > 0) {
      ttfPage = 0;
      return true;
    }
    if (currentSpineIndex > 0) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex--;
      // Same for the backward skip.
      clearDeferredReposition();
      return true;
    }
    return false;
  }
#endif
  if (!section) return false;
  bleturner::acknowledgeLinkNote();
  if (amount > 0) {
#ifdef READING_STATS_ENABLED
    // A forward skip is still a forward turn: clear a declined 100% prompt.
    stats.completionPromptDismissedAtHundred = false;
#endif
    RenderLock lock;
    nextPageNumber = 0;
    currentSpineIndex++;
    // Promote prefetched section if it matches, otherwise discard stale prefetch
    if (nextSectionPrefetch && nextSectionSpineIndex == currentSpineIndex) {
      section = std::move(nextSectionPrefetch);
      nextSectionSpineIndex = -1;
    } else {
      nextSectionPrefetch.reset();
      nextSectionSpineIndex = -1;
    }
    section.reset();
#ifdef READING_STATS_ENABLED
    if (SETTINGS.shouldTrackReadingStats() && !stats.isCompleted && currentSpineIndex == epub->getSpineItemsCount())
      setBookCompleted(true);
#endif
    if (currentSpineIndex == epub->getSpineItemsCount()) {
      // The EOB screen bypasses the section render path, so persist the
      // terminal position synchronously when a skip crosses it.
      if (!saveFinishedProgress()) {
        LOG_ERR("ERS", "Failed to save end-of-book progress");
      }
    }
    return true;
  } else {
    if (section->currentPage > 0) {
      section->currentPage = 0;
      return true;
    } else if (currentSpineIndex > 0) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex--;
      section.reset();
      // Clear prefetch if it doesn't match the new current chapter
      if (nextSectionSpineIndex != currentSpineIndex) {
        nextSectionPrefetch.reset();
        nextSectionSpineIndex = -1;
      }
      return true;
    }
  }
  return false;
}

bool EpubReaderActivity::pageTurnBy(const int steps) {
  return pageturn::turnBy(steps, [this](const bool forward) {
    const int spineBefore = currentSpineIndex;
    if (!pageTurn(forward)) return pageturn::Step::Blocked;
    // A step that crossed into another chapter (or reached end of book) left
    // the section unloaded/pending — the next step needs this frame's render
    // to load it first, so the jump clamps at the boundary.
    return (isAtEndOfBook() || currentSpineIndex != spineBefore) ? pageturn::Step::Boundary : pageturn::Step::Turned;
  });
}

// Failed protected open: show the standard option dialog (wrapped message)
// instead of silently falling back to the previous screen. Exact error
// strings are set by openProtectedBook (ContentProtection.cpp).
bool EpubReaderActivity::handleLoadFailure() {
  if (loadProtectionError.empty()) return false;
  const std::string& perr = loadProtectionError;
  StrId msg = StrId::STR_DRM_PROTECTED_FILE;
  bool offerSync = false;
  if (perr == "access expired") {
    msg = StrId::STR_LOAN_EXPIRED;
  } else if (perr == "loan date unverified") {
    msg = StrId::STR_LOAN_TIME_UNVERIFIED;
    offerSync = true;
  }
  const char* options[2] = {I18N.get(offerSync ? StrId::STR_CLOCK_SYNC_NOW : StrId::STR_OK_BUTTON),
                            I18N.get(StrId::STR_OK_BUTTON)};
  loadFailurePopup.showMessage("", I18N.get(msg), options, offerSync ? 2 : 1, 0, [this, offerSync](const int index) {
    if (offerSync && index == 0) {
      beginLoanTimeSync();
      return;
    }
    finish();
  });
  requestUpdate();
  return true;  // stay alive; the popup's Back dismiss lands in loop()'s !epub finish
}

void EpubReaderActivity::beginLoanTimeSync() {
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    LOG_ERR("ERS", "OOM: Wi-Fi selection for loan time sync");
    finish();
    return;
  }
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
      finish();
      return;
    }
    GUI.drawPopup(renderer, tr(STR_SYNCING_TIME));
    const bool synced = trustedtime::syncNow(5000);
    WiFi.disconnect(false);
    delay(30);
    if (!synced) {
      // Reopening would hit the same unverified-time refusal: offer a retry.
      const char* options[2] = {I18N.get(StrId::STR_RETRY), I18N.get(StrId::STR_OK_BUTTON)};
      loadFailurePopup.showMessage("", I18N.get(StrId::STR_CLOCK_SYNC_FAIL), options, 2, 0, [this](const int index) {
        if (index == 0) {
          beginLoanTimeSync();
          return;
        }
        finish();
      });
      requestUpdate();
      return;
    }
    APP_STATE.openEpubPath = bookPath;
    // New book: the cached progress belongs to the previous one, so let the
    // cover screens recompute for this book.
    APP_STATE.openBookProgressPercent = -1;
    APP_STATE.openBookTimeLeftSeconds = 0;
    APP_STATE.openBookPagesLeft = -1;
    APP_STATE.openBookCacheLegacy = false;
    APP_STATE.saveToFile();
    // Reboot straight back into this book with a clean heap (no-op on touch
    // boards, which fall through to the in-place relaunch below).
    silentRestartToReader();
    activityManager.goToReader(bookPath);
  });
}

void EpubReaderActivity::render(RenderLock&& lock) {
  if (loadFailurePopup.isActive()) {
    renderer.clearScreen();
    loadFailurePopup.processRender(renderer, mappedInput);
    return;
  }
  ReaderActivity::render(std::move(lock));
}

bool EpubReaderActivity::remoteShortcut(const bool readerMenu) {
  if (!section) return false;
  automaticPageTurnActive = false;
  if (!readerMenu) {
    startClipSelection();
  } else if (usesToolbarMenu()) {
    openOverlay(Overlay::Toolbar);
  } else {
    openReaderMenu();
  }
  return true;
}

bool EpubReaderActivity::isAtEndOfBook() const { return epub && currentSpineIndex >= epub->getSpineItemsCount(); }

void EpubReaderActivity::onReturnFromEndOfBook() {
  if (epub && epub->getSpineItemsCount() > 0) {
    currentSpineIndex = epub->getSpineItemsCount() - 1;
    nextPageNumber = 0;
    pendingPageJump = std::numeric_limits<uint16_t>::max();
  }
}

// The look-ahead never builds next to the radio. While the radio is stopped for a build it runs to
// the end of the chapter, so the radio comes back with no parser alive.
bool EpubReaderActivity::preventAutoSleep() {
#if defined(CROSSPOINT_TTF_READER)
  // Live background builds are real work: the input-idle governor would
  // otherwise drop to LOW_POWER_FREQ mid-index (measured: 2.6 s/page spine
  // builds, and a task-WDT IDLE0 starvation reboot when one computation
  // stretch at low frequency exceeded the watchdog window). The worker
  // self-exits once every spine has been attempted, so this is bounded.
  if (fibpWorker_ != nullptr && fibpBegun_ && fibpWorker_->active()) return true;
  // A heap-paused session is stalled, not working: no pump runs below the
  // free-heap floors, so it must not hold low-power/auto-sleep hostage
  // (mirrors skipLoopDelay()).
  if (ttf_ != nullptr && ttf_->sessionActive() && !buildHeapPaused) return true;
#endif
  return section != nullptr && section->isBuilding();
}

// A started build runs to the end of the chapter, one bounded step per idle loop frame, so the
// parser and its page LUT are freed soon (and a radio stopped for the build comes back with no
// parser alive). Not while the radio holds the heap, and not in a spine whose background build
// failed.
bool EpubReaderActivity::backgroundBuildWanted() const {
  return section && section->isBuilding() && !bleturner::holdsHeap() && currentSpineIndex != backgroundBuildFailedSpine;
}

bool EpubReaderActivity::skipLoopDelay() {
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    return ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex)) && ttf_->sessionActive() && !buildHeapPaused;
  }
#endif
  // The main loop holds the render lock while querying this hint. It keeps yielding while a button
  // is down: a press that starts inside a long build step gets its second debounce sample ~5 ms
  // after the step instead of after a 10 ms sleep.
  return overlay == Overlay::None && !buildHeapPaused && backgroundBuildWanted();
}

void EpubReaderActivity::renderBook() {
#if defined(CROSSPOINT_TTF_READER)
  // Native-TTF page source (design §3.5): a fully separate render path so the
  // legacy Section pipeline below stays untouched.
  if (ttf_) {
    renderBookTtf();
    return;
  }
#endif
#ifdef BOOK_PROFILE
  uint32_t render_book_start_ms = millis();
  uint8_t core = xPortGetCoreID();
  uint32_t psram_free_before = ESP.getFreePsram();
  uint32_t heap_free_before = ESP.getFreeHeap();
#endif
  if (!epub) return;
  // A radio start in flight takes ~50 KB while the stack comes up: the page waits for it rather than
  // take the heap beside it (X3: abort() in a page restore at free 22,676 B). About 20 ms, once a visit.
  bleturner::settleStart();
  // Runs under the render task's RenderLock; catches every requestUpdate()
  // exit from the overlay while its deferred chrome refresh is still pending.
  settleOverlayRefresh();

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  const auto showBuildError = [this](const char* message = nullptr) {
    renderer.clearScreen();
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    GUI.drawPopup(renderer, message ? message : tr(STR_INDEX_FAILED));
    automaticPageTurnActive = false;
  };

  if (currentSpineIndex < 0) currentSpineIndex = 0;
  if (currentSpineIndex > epub->getSpineItemsCount()) currentSpineIndex = epub->getSpineItemsCount();

  if (currentSpineIndex == epub->getSpineItemsCount()) {
#ifdef BOOK_PROFILE
    LOG_INF("PROF", "phase=renderBook exit: no more spines core=%d", core);
#endif
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  buildViewportWidth = viewportWidth;
  buildViewportHeight = viewportHeight;

  const ReaderRenderSpec renderSpec = SETTINGS.readerRenderSpec(viewportWidth, viewportHeight);

  if (!section) {
    // Chapter changed or first load — clear stale prefetch
    nextSectionPrefetch.reset();
    nextSectionSpineIndex = -1;
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));
    partialRebuildStartFailed = false;

    const bool cacheLoaded = section->loadSectionFile(renderSpec);
#ifdef BOOK_PROFILE
    uint32_t psram_free_after = ESP.getFreePsram();
    uint32_t heap_free_after = ESP.getFreeHeap();
    LOG_INF("PROF", "phase=renderBook_cacheLoad core=%d cache_loaded=%s psram_free=%uB->%uB heap=%uB->%uB", core,
            cacheLoaded ? "yes" : "no", psram_free_before, psram_free_after, heap_free_before, heap_free_after);
#endif
    if (cacheLoaded) {
      cachedChapterTotalPageCount = 0;
      cachedVisibleTextOffset.reset();
    }
    const bool cacheComplete = cacheLoaded && !section->isPartial();
    const bool explicitOffsetJump = pendingOffsetJump.has_value();
    const std::optional<uint32_t> offsetJump =
        explicitOffsetJump ? pendingOffsetJump
        : (pendingPageJump.has_value() || !pendingAnchor.empty() || currentSpineIndex != cachedSpineIndex)
            ? std::nullopt
            : cachedVisibleTextOffset;
    if (!cacheComplete) {
      if (section->isPartial()) {
        LOG_DBG("ERS", "Partial cache found (%d pages), resuming build...", section->pageCount);
      } else {
        LOG_DBG("ERS", "Cache not found, building...");
      }

      const bool needsFullBuild = pendingPercentJump;
      if (needsFullBuild) {
        GUI.drawPopup(renderer, tr(STR_INDEXING));
        pagesUntilFullRefresh = 1;
        const auto popupFn = [this]() {
          if (renderer.hasFrameBuffer()) GUI.drawPopup(renderer, tr(STR_INDEXING));
        };
        if (!makeRoomForChapterBuild()) {
          section.reset();
          showBuildError(tr(STR_BT_BUILD_REFUSED));
          return;
        }
        GfxRenderer::FrameBufferLoan loan(renderer);
        if (!section->createSectionFile(renderSpec, popupFn)) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
          section.reset();
          loan.end();
          showBuildError();
          return;
        }
#ifdef BOOK_PROFILE
        uint32_t psram_free_after = ESP.getFreePsram();
        uint32_t heap_free_after = ESP.getFreeHeap();
        LOG_INF("PROF", "phase=renderBook_createSection core=%d create_psram_free=%uB->%uB heap_free=%uB->%uB", core,
                psram_free_before, psram_free_after, heap_free_before, heap_free_after);
#endif
        loan.end();
      } else {
        const int target = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber < 0 ? 0 : nextPageNumber);
        const bool anchorJump = !pendingAnchor.empty();

        if (section->isPartial() &&
            (anchorJump ? section->getPageForAnchor(pendingAnchor).has_value()
                        : target + PARTIAL_REBUILD_START_MARGIN < static_cast<int>(section->pageCount))) {
          LOG_DBG("ERS", "Partial covers target %d of %d; deferring extension build", target, section->pageCount);
        } else {
          const size_t spineBytes =
              epub->getCumulativeSpineItemSize(currentSpineIndex) -
              (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
          const bool willInflate = !section->hasHtmlCache();
          bool showPopup;
          if (anchorJump) {
            showPopup = !section->findAnchor(pendingAnchor).has_value() && spineBytes > BUILD_POPUP_BYTE_THRESHOLD;
          } else {
            const bool targetAvailable = target < static_cast<int>(section->pageCount);
            showPopup = !targetAvailable && ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate) ||
                                             target > BUILD_POPUP_PAGE_THRESHOLD);
          }
          if (showPopup) {
            GUI.drawPopup(renderer, tr(STR_INDEXING));
            indexingPopupShown = true;
            pagesUntilFullRefresh = 1;
          }
          buildPopupPending = !showPopup;
          // Section (re)builds are the heap-hungriest path (per-word
          // allocations for the whole section). Under TTF heap pressure, shed
          // every rebuildable font cache first — dropped glyphs re-fault on
          // demand after the build. Skipped for cpfont/builtin reading: the
          // release drops the persistent advance table and mini tables, which
          // would force SD metric re-reads on every fresh chapter for no gain.
          // (Fork: the TTF reader is the BookFontLoader runtime, not an
          // upstream GfxRenderer font list, so the gate is ttf_ != nullptr.
          // ttf_ exists only in CROSSPOINT_TTF_READER builds — guard the
          // call site with the same flag the member declaration uses.)
#if defined(CROSSPOINT_TTF_READER)
          if (ttf_ != nullptr) {
            if (auto* fcm = renderer.getFontCacheManager()) {
              fcm->releaseSdFontCaches();
            }
          }
#endif
          if (!makeRoomForChapterBuild()) {
            section.reset();
            buildPopupPending = false;
            showBuildError(tr(STR_BT_BUILD_REFUSED));
            return;
          }
          LOG_DBG("ERS", "Heap before section build: %u (max block %u)", (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap());
          const unsigned long buildStartMs = millis();
          bool started;
          {
            GfxRenderer::FrameBufferLoan loan(renderer);
            started = section->startBuild(renderSpec, [this] { showBuildPopup(renderer, pagesUntilFullRefresh); });
          }
          if (!started) {
            LOG_ERR("ERS", "Failed to start section build");
            section.reset();
            buildPopupPending = false;
            showBuildError();
            return;
          }
          while (!section->isBuildComplete() &&
                 (anchorJump               ? !section->findAnchor(pendingAnchor)
                  : offsetJump.has_value() ? !section->buildReachedVisibleTextOffset(*offsetJump)
                                           : static_cast<int>(section->pageCount) <= target)) {
            if (buildPopupPending && millis() - buildStartMs >= BUILD_POPUP_DEADLINE_MS) {
              showBuildPopup(renderer, pagesUntilFullRefresh);
            }
            if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
              LOG_ERR("ERS", "Failed during incremental section build");
              section.reset();
              buildPopupPending = false;
              indexingPopupShown = false;
              showBuildError();
              return;
            }
            // Enough pages are built to read: dismiss the Indexing popup AND stop waiting
            // on a target the partial index cannot satisfy yet. Clearing only
            // the latch is not enough — staying in this loop keeps building
            // synchronously, so the pass never paints a page. Breaking out
            // lets the caller paint an available page while
            // buildSomeMore() in the display-time overlap drains the rest.
            // Keyed on indexingPopupShown, not buildPopupPending — the latter
            // is already false once the popup is painted. A pending anchor or
            // text-offset jump keeps building to its target: yielding would
            // clear pendingAnchor below without resolving it, dropping a
            // TOC/bookmark jump. (2026-10-04.)
            if (indexingPopupShown && shouldClearBuildPopup(static_cast<int>(section->pageCount)) && !anchorJump &&
                !offsetJump.has_value()) {
              LOG_DBG("ERS", "Indexing popup cleared after %d pages built", section->pageCount);
              indexingPopupShown = false;
              pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
              break;
            }
          }
          buildPopupPending = false;
          indexingPopupShown = false;
        }
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    // A readable partial must not consume a pending numeric target: painting an
    // available page leaves currentPage clamped to the last built page, and
    // later passes reuse this section without re-reading nextPageNumber, so the
    // real target would never be applied (applyDeferredReposition() only guesses
    // proportionally from cachedChapterTotalPageCount). Keep the target pending
    // for the build-completion tick to apply against the finished index.
    if (pendingPageJump.has_value()) {
      // UINT16_MAX is the "land on the chapter's last page" sentinel (set when
      // turning back from the first page, and at end-of-book). While the index
      // is still partial the real last page is unknown, so leave the sentinel
      // pending instead of consuming it as a page number — otherwise the clamp
      // below pins the reader to the last BUILT page and it never re-resolves.
      if (*pendingPageJump != std::numeric_limits<uint16_t>::max() || !section->isPartial()) {
        section->currentPage = *pendingPageJump;
        pendingPageJump.reset();
      }
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) section->currentPage = 0;
    }

    if (offsetJump.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*offsetJump)) {
        section->currentPage = *offsetPage;
        clearDeferredReposition();
      }
    }
    if (explicitOffsetJump) {
      clearDeferredReposition();
    }
    pendingOffsetJump.reset();

    if (!pendingAnchor.empty()) {
      const auto page = section->findAnchor(pendingAnchor);
      if (page) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      }
      pendingAnchor.clear();
    }

    if (pendingPercentJump && section->pageCount > 0) {
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) newPage = section->pageCount - 1;
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  // A partial that already holds MIN_PAGES_TO_CLEAR_POPUP pages is readable: the
  // extension drains in the display-time overlap (buildSomeMore() after
  // displayBuffer), so painting Indexing and blocking here would only repeat
  // the wait the render pass just yielded.
  const bool partialReadable = shouldClearBuildPopup(static_cast<int>(section->pageCount));
  // The loops below build on until the page to show exists. The Bluetooth
  // radio must not hold its heap next to a build; only waits when a build is
  // actually needed (a readable partial drains in the display-time overlap).
  if (!partialReadable && section->currentPage >= static_cast<int>(section->pageCount) && !makeRoomForChapterBuild()) {
    section.reset();
    showBuildError(tr(STR_BT_BUILD_REFUSED));
    return;
  }
  if (!partialReadable && section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    pagesUntilFullRefresh = 1;
  }
  while (!partialReadable && section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    if (!section->isBuilding() && !section->startBuild(renderSpec)) {
      LOG_ERR("ERS", "Failed to start partial extension build");
      section.reset();
      showBuildError();
      return;
    }
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }
  if (section->isBuilding() && !partialReadable) {
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }

  // Clamp to the built range whenever the requested page is past the watermark.
  // The clamp used to require !isBuilding(), which left a still-building
  // section pointing past its last page — the render pass now yields while the
  // build continues, so the page to paint is the last BUILT one.
  if (section->pageCount > 0 && section->currentPage >= static_cast<int>(section->pageCount)) {
    section->currentPage = section->pageCount - 1;
  }

  applyDeferredReposition();

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    pageBufferStale = false;
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  updateBookmarkFlag();

  {
    auto p = section->loadPage(section->currentPage);
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      automaticPageTurnActive = false;
      const bool giveUp = ++pageLoadRetryCount > MAX_PAGE_LOAD_RETRIES;
      section->abandonBuild();
      section->clearCache();
      section.reset();
      if (giveUp) {
        LOG_ERR("ERS", "Page load retry limit reached, aborting");
        pageLoadRetryCount = 0;
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        showPendingSyncSaveError();
        return;
      }
      requestUpdate();
      showPendingSyncSaveError();
      return;
    }
    pageLoadRetryCount = 0;

    currentPageVisibleOffset = p->visibleTextOffset;
    currentPageFootnotes = std::move(p->footnotes);
#ifdef READING_STATS_ENABLED
    currentPageWordsOnPage = p->wordCount();
#endif

    // The overlay and non-tiled grayscale renderer share the renderer's single
    // stored-BW slot. Release the old page snapshot before renderContents()
    // needs that slot, then snapshot the newly rendered page below.
    discardOverlayPage();

#ifdef BOOK_PROFILE
    uint8_t render_core = xPortGetCoreID();
    uint32_t render_start_ms = millis();
    uint32_t psram_free_before_r = ESP.getFreePsram();
    uint32_t heap_free_before_r = ESP.getFreeHeap();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    uint32_t psram_free_after_r = ESP.getFreePsram();
    uint32_t heap_free_after_r = ESP.getFreeHeap();
    uint32_t render_end_ms = millis();
    LOG_INF("PROF", "phase=renderContents core=%d render_dur=%uus psram_free=%uB->%uB heap=%uB->%uB", render_core,
            render_end_ms - render_start_ms, psram_free_before_r, psram_free_after_r, heap_free_before_r,
            heap_free_after_r);
    LOG_DBG("ERS", "Rendered page in %dms", render_end_ms - render_start_ms);
#else
    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
#endif
    lastRenderCompleteMs = millis();
#ifdef READING_STATS_ENABLED
    pageShownAtMs = millis();
#endif
    markPageRendered();
  }

  {
    // One call per render: the manager keeps the in-memory position current
    // and gates the disk write itself (changed + interval, or low battery —
    // §4.2/§4.5). Unchanged renders are no-ops inside the manager.
    LOG_DBG("PRG", "caller: save spine=%u page=%u/%u offset=%d", currentSpineIndex, section->currentPage,
            section->estimatedTotalPages(), currentPageVisibleOffset.has_value() ? 1 : 0);
    progressManager.save(currentSpineIndex, section->currentPage, section->estimatedTotalPages(),
                         currentPageVisibleOffset.has_value(), currentPageVisibleOffset.value_or(0));
  }
  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }

  if (showDictionaryMessage) {
    GUI.drawPopup(renderer, dictionaryMessageTtf ? tr(STR_DICT_TTF_UNSUPPORTED) : tr(STR_DICT_NO_DICT_SET));
  }

  // Glyphs of a damaged .cpfont draw as blanks; say why, once per loaded font.
  if (SdCardFont::takeCorruptGlyphNotice()) {
    showFontDamagedMessage = true;
    fontDamagedMessageTime = millis();
  }
  if (showFontDamagedMessage) {
    GUI.drawPopup(renderer, tr(STR_FONT_DAMAGED));
  }

  if (showClippingMessage) {
    GUI.drawPopup(renderer, I18N.get(clippingMessage));
  }

  pageBufferStale = false;  // the page is back in the framebuffer

  // Toolbar menu: overlay the toolbar / panel on top of the freshly rendered page.
  if (overlay != Overlay::None && usesToolbarMenu()) {
    // The page just re-rendered under the overlay: refresh the snapshot that
    // backs panel->toolbar restores (any previous copy is stale).
    discardOverlayPage();
    overlayPageStored = renderer.storeBwBuffer();
    renderOverlay();
    // An open option picker rides on top of the freshly drawn panel.
    if (overlayPopup.isActive()) overlayPopup.render(renderer);
    // FAST, same as openOverlay: HALF's inverting pass flashes the sheet
    // (white, in night mode) on every repaint under an open panel. Any AA
    // residue a FAST differential leaves under the chrome has not shown in
    // practice; restore a HALF cleanup here if text ever visibly ghosts
    // through the sheet (see #2190 for the mechanism).
    pushOverlayRefresh();
  }
}

// ── Native-TTF render path (design §3.5, CROSSPOINT_TTF_READER builds) ───────

#if defined(CROSSPOINT_TTF_READER)

// Estimated chapter total while a build is in flight (owner steer: the old
// engine's behavior — return after a few pages, let the user read, refine
// the count as indexing progresses). Exact once the chapter is complete.
uint32_t EpubReaderActivity::ttfEstimatedPageCount() const {
  if (!ttf_ || ttfSpine != currentSpineIndex) return ttfPageCount;
  uint64_t est = ttfPageCount;
  const bool building = ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex));
  const bool workerBuilding = fibpWorker_ != nullptr && fibpWorker_->buildingSpine() == currentSpineIndex;
  if (building) {
    // Live session: scale built pages by the layout session's input progress.
    const uint64_t consumed = ttf_->sessionBytesConsumed();
    const uint64_t total = ttf_->sessionBytesTotal();
    if (ttfPageCount > 0 && consumed > 0 && total > consumed) {
      est = static_cast<uint64_t>(ttfPageCount) * total / consumed;
    }
  } else if (ttf_->cacheReady() && ttf_->cachePartial()) {
    // Partial cache (worker may be extending it off-task): the cache
    // records its own input progress; while the worker holds the claim,
    // its live page count refines the extrapolation.
    const uint64_t consumed = ttf_->cacheBuildBytesConsumed();
    const uint64_t total = ttf_->cacheBuildBytesTotal();
    if (consumed > 0 && total > consumed) {
      const uint16_t workerPages = workerBuilding ? fibpWorker_->buildingProgressPages() : 0;
      // buildingProgressPages() is the spine's CUMULATIVE available-page count
      // (ChapterIndexEngine passes target.availablePageCount), so it already
      // includes the resumed prefix — take the greater of the two, never the
      // sum, or the prefix is counted twice while the worker extends it.
      const uint64_t pages = std::max<uint64_t>(ttfPageCount, workerPages);
      est = pages * total / consumed;
    }
  }
  if (est < ttfPageCount) est = ttfPageCount;     // never below what is servable
  constexpr uint64_t kMaxEstimatedPages = 60000;  // same clamp the old engine used
  return est > kMaxEstimatedPages ? static_cast<uint32_t>(kMaxEstimatedPages) : static_cast<uint32_t>(est);
}

void EpubReaderActivity::ttfSaveProgress() {
  // progressManager stays the single writer; this reports the
  // generation-tagged record shape (charOffset + generation) instead of a
  // visible-text offset.
  progressManager.saveTtf(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(ttfPage),
                          static_cast<uint16_t>(ttfPageCount), ttfCurrentCharStart, ttfGeneration);
}

void EpubReaderActivity::finishTtfPageRender() {
  // Book profiling trace: confirm the normal AA page turn performs no extra
  // application-level refresh after cleanup (the SDK's cleanup is RAM-only).
  const bool grayExtraDisplay = overlay != Overlay::None && usesToolbarMenu();
  LOG_DBG("GRS", "finishTtfPageRender: extraDisplay=%d", grayExtraDisplay);
  // P4: a chrome popup painted this frame must keep page N's context, so the
  // prerender stays unscheduled for this pass (the popup repaints every
  // render until its duration elapses, then the prerender resumes).
  ttfChromePopupShown = showBookmarkMessage || showDictionaryMessage;
  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }
  if (showDictionaryMessage) {
    GUI.drawPopup(renderer, dictionaryMessageTtf ? tr(STR_DICT_TTF_UNSUPPORTED) : tr(STR_DICT_NO_DICT_SET));
  }
  // P4 one-page-ahead prerender: schedule only for fully clean frames.
  if (!ttfChromePopupShown) ttfSchedulePreRender();
  if (overlay != Overlay::None && usesToolbarMenu()) {
    // The page just re-rendered under the overlay: refresh the snapshot that
    // backs panel->toolbar restores (any previous copy is stale).
    LOG_DBG("GRS", "ttf finish: overlay FAST after gray cleanup overlay=%d", static_cast<int>(overlay));
    if (renderer.hasFrameBuffer()) {
      if (overlayPageStored) {
        renderer.discardStoredBwBuffer();
        overlayPageStored = false;
      }
      overlayPageStored = renderer.storeBwBuffer();
    }
    renderOverlay();
    if (overlayPopup.isActive()) overlayPopup.render(renderer);
    pushOverlayRefresh();
  }
}

void EpubReaderActivity::ttfInvalidateCaches() {
  if (!ttf_) return;
  // Settings/orientation reflow: the anchor (ttfCurrentCharStart) is preserved
  // across the generation bump and resolved by the unified funnel (design §3.2).
  pendingRestoreTarget_ = ttf_resolve::reflowRestoreTarget(ttfCurrentCharStart);
  ttf_->abortSession();
  ttf_->dropPrefetch();
  ttf_->closeChapterCache();
  ttfPrefetchActive = false;
  ttfSpine = -1;
  ttfPage = -1;
  ttfPageCount = 0;
  ttfGenerationValid = false;
  ttfFrameRenderComplete.store(false, std::memory_order_release);
}

void EpubReaderActivity::ttfShowIndexingPopup() {
  // §3.4 (amended): Indexing popup + the theme's small progress bar. The popup
  // rect is kept for the pass; fillPopupProgress feeds the bar INSIDE the
  // popup frame (BaseTheme::popupProgressBarHeight, optional outline,
  // popupProgressClampPercent) and issues its own FAST_REFRESH. No percent
  // text, no separate rect math.
  //
  // Bar honesty: the byte metric (sessionBytesConsumed/Total) is only
  // truthful when the wait really is the whole chapter — normal forward
  // indexing, and Percent/LastPage restores, which resolve only from a
  // COMPLETE index. A cover-driven restore (CharOffset/Page kinds: Saved,
  // Reflow, OffsetJump, Anchor — an offset or page target maps as soon
  // as the built prefix covers it) finishes at an arbitrary fraction of the
  // chapter, so the same metric can read 25% one frame and 100% the next
  // right before the page paints. Those get a stepped indeterminate fill
  // instead of a number that lies.
  const Rect popupRect = GUI.drawPopup(renderer, tr(STR_INDEXING));
  const bool restorePending = pendingRestoreTarget_.kind != PendingTarget::None;
  const bool restoreNeedsComplete = restorePending && (pendingRestoreTarget_.kind == PendingTarget::Percent ||
                                                       pendingRestoreTarget_.kind == PendingTarget::LastPage);
  int percent;
  if (restorePending && !restoreNeedsComplete) {
    percent = INDEX_BAR_INDETERMINATE[indexingBarStep_ % 4];
    ++indexingBarStep_;
    // Per-pass line during a long restore: keep it off the serial floor.
    LOG_DBG("ERS", "Indexing popup: cover-driven restore, indeterminate bar step %d", percent);
  } else {
    const uint32_t consumed = ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex))
                                  ? ttf_->sessionBytesConsumed()
                                  : (ttf_->cacheReady() ? ttf_->cacheBuildBytesConsumed() : 0);
    const uint32_t total = ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex))
                               ? ttf_->sessionBytesTotal()
                               : (ttf_->cacheReady() ? ttf_->cacheBuildBytesTotal() : 0);
    percent = total > 0 ? static_cast<int>((static_cast<uint64_t>(consumed) * 100ULL) / total) : 0;
    LOG_DBG("ERS", "Indexing popup: restore=%d completeGated=%d consumed %lu/%lu bytes (%d%%)", restorePending,
            restoreNeedsComplete, static_cast<unsigned long>(consumed), static_cast<unsigned long>(total), percent);
  }
  GUI.fillPopupProgress(renderer, popupRect, percent);
  pagesUntilFullRefresh = 1;
  indexingPopupShown = true;
}

// Resolves the pending navigation state into a chapter-local target page.
// Returns false when more build output is needed first (jump states stay
// pending and are retried on the next render pass). `needFullBuild` asks for
// a complete build (percent jumps / last-page sentinels), not just enough
// pages to cover the target.
void EpubReaderActivity::consumeRestoreTarget() {
  // Origin-aware source-latch consumption (§3.2): the caller owns the
  // latches, so it clears exactly the one that produced the target.
  switch (pendingRestoreTarget_.origin) {
    case PendingTarget::Origin::Anchor:
      pendingAnchor.clear();
      break;
    case PendingTarget::Origin::Saved:
      ttfHasSavedPosition = false;
      break;
    case PendingTarget::Origin::OffsetJump:
      pendingOffsetJump.reset();
      break;
    case PendingTarget::Origin::Reflow:
      break;  // no extra latch: the invalidation anchor stays a CharOffset target
    case PendingTarget::Origin::OriginNone:
    default:
      break;  // no source latch (unexpected for a stored target)
  }
  LOG_INF("ERS", "TTF restore consumed: spine %d page %d origin %u", currentSpineIndex, ttfPage,
          static_cast<unsigned>(pendingRestoreTarget_.origin));
  pendingRestoreTarget_ = {};
}

bool EpubReaderActivity::ttfResolveTargetPage(int& targetOut, const freeink::book::LayoutParams& params,
                                              bool& needFullBuild) {
  (void)params;
  const int available = static_cast<int>(ttfPageCount);
  const uint16_t currentSpine = static_cast<uint16_t>(currentSpineIndex);
  const bool buildRunning = ttf_->sessionFor(currentSpine) && ttf_->sessionActive();
  const bool cacheMatchesGeneration = ttf_->cacheReady() && ttf_->cacheGeneration() == ttfGeneration;
  const bool sessionHasTotal =
      ttf_->sessionFor(currentSpine) && ttf_->sessionDone() && ttf_->sessionMatchesGeneration(ttfGeneration);
  const bool completeCache = cacheMatchesGeneration && !ttf_->cachePartial() && !buildRunning;
  const bool haveTotal = sessionHasTotal || completeCache;
  const auto offsetIsAvailable = [this, haveTotal, sessionHasTotal, completeCache](uint32_t offset) {
    if (!haveTotal) return false;
    return sessionHasTotal ? ttf_->sessionTotalChars() > offset : (completeCache && ttf_->cacheTotalChars() > offset);
  };
  const auto canMapCompleteOffset = [&offsetIsAvailable](uint32_t offset) { return offsetIsAvailable(offset); };

  if (pendingPageJump.has_value()) {
    const int jump = *pendingPageJump;
    if (jump == static_cast<int>(std::numeric_limits<uint16_t>::max())) {
      // Last-page sentinel: the end of the CHAPTER, so it may only resolve
      // once the total is known — a suspended partial prefix is not the end
      // (legacy behavior: wait for the full build).
      if (haveTotal) {
        pendingPageJump.reset();
        targetOut = std::max(0, available - 1);
        return true;
      }
      needFullBuild = true;
      return false;
    }
    // A normal jump beyond the built prefix stays pending on a partial
    // cache: consuming it would let a heap-gated background build fall back
    // to the old page. A complete cache clamps it below.
    if (!haveTotal && jump >= available) {
      needFullBuild = true;
      return false;
    }
    pendingPageJump.reset();
    targetOut = std::max(0, jump);
    return true;
  }

  if (ttfRestoreLastPage) {
    // Back from page 0 into the previous chapter (§3.5): last built page,
    // degrading to page 0 on a cold chapter. A partial prefix cannot report
    // the chapter's last page, so wait for the build to complete first.
    if (!haveTotal) {
      needFullBuild = true;
      return false;
    }
    ttfRestoreLastPage = false;
    targetOut = available > 0 ? available - 1 : 0;
    return true;
  }

  // ── Unified resolver funnel (design §3.2) ─────────────────────────────
  // Replaces four near-duplicate mapping blocks (anchor :2821, reflow :2877,
  // saved position :2901, offset jump :2959). Latch derivation and the
  // three-case policy (covered/terminal/pending) are delegated to the pure
  // ttf_resolve helpers; this function handles anchor resolution (SDK call)
  // and the origin-aware source-latch side effects.
  PendingTarget active = pendingRestoreTarget_;
  // An explicit navigation request (KOReader offset jump, TOC/footnote anchor)
  // supersedes an implicit restore target (saved record, reflow, preview
  // seed): drop the stored implicit target so the derivation below sees only
  // the explicit latch. Implicit latches are cleared with it — the position
  // they point at is re-saved from the new location on close.
  // The stored origin alone is not enough: the implicit restore can also exist
  // only as a raw latch (pendingRestoreTarget_ still None), and deriveTarget()
  // ranks hasSavedPosition ahead of the explicit jump/anchor — the surviving
  // latch would win the pass. Supersede on the stored implicit target OR either
  // implicit latch, then clear BOTH latches unconditionally (a Reflow
  // stored target can coexist with an armed saved-position latch).
  if ((pendingOffsetJump.has_value() || !pendingAnchor.empty()) &&
      (active.origin == PendingTarget::Origin::Saved || active.origin == PendingTarget::Origin::Reflow ||
       ttfHasSavedPosition)) {
    LOG_DBG("ERS", "Restore superseded by explicit navigation (stored origin %u)",
            static_cast<unsigned>(active.origin));
    ttfHasSavedPosition = false;
    pendingRestoreTarget_ = {};
    active = {};
  }
  if (active.kind == PendingTarget::None) {
    const ttf_resolve::LatchState latches{
        .hasSavedPosition = ttfHasSavedPosition,
        .savedSpine = ttfSavedSpine,
        .savedCharOffset = ttfSavedCharOffset,
        .savedGeneration = ttfSavedGeneration,
        .generation = ttfGeneration,
        .currentSpine = currentSpine,
        .savedPage = nextPageNumber > 0 ? static_cast<uint16_t>(nextPageNumber) : uint16_t{0},
        .offsetJump = pendingOffsetJump,
        .hasAnchor = !pendingAnchor.empty(),
        .anchorHash = pendingAnchor.empty() ? 0 : freeink::book::ZipCatalog::hashPath(pendingAnchor.c_str()),
    };
    const ttf_resolve::DerivedTarget derived = ttf_resolve::deriveTarget(latches);
    if (derived.degrade == ttf_resolve::Degrade::ChapterStart) {
      // §7 (amended §10.11): a saved record bound to another spine cannot
      // map — the anchor is chapter-scoped. A generation mismatch never
      // degrades; the anchor restores through the funnel.
      ttfHasSavedPosition = false;
      pendingRestoreTarget_ = {};
      targetOut = 0;
      return true;
    }
    if (derived.target.kind != PendingTarget::None) {
      active = derived.target;
      // Store the derived target: pendingRestoreTarget_ is the single source
      // of truth the §5.1/§5.2/§3.5 gates read, so a restore derived from a
      // source latch opens the gates exactly like one set by invalidation.
      pendingRestoreTarget_ = active;
    }
  }

  if (active.kind != PendingTarget::None) {
    // AnchorHash: resolve to char offset first (SDK call stays here), then
    // the CharOffset policy applies.
    if (active.kind == PendingTarget::AnchorHash) {
      uint32_t charOffset = 0;
      if (ttf_->charForAnchor(currentSpine, active.idHash, &charOffset)) {
        active.kind = PendingTarget::CharOffset;
        active.charOffset = charOffset;
        active.idHash = 0;
        pendingRestoreTarget_ = active;  // keep the stored target in sync
      } else if (completeCache) {
        pendingAnchor.clear();
        pendingRestoreTarget_ = {};
        LOG_DBG("ERS", "Anchor not found in built TTF chapter, opening at page 0");
        targetOut = 0;
        return true;
      } else {
        needFullBuild = true;
        return false;
      }
    }

    // Delegate the three-case policy to the pure policy function.
    auto pageForChar = [this, currentSpine](uint32_t offset, uint32_t& pageOut) -> bool {
      return ttf_->pageForChar(currentSpine, offset, &pageOut);
    };
    auto pageCharStart = [this, currentSpine](uint16_t pageIndex) -> uint32_t {
      return ttf_->pageCharStart(currentSpine, pageIndex);
    };
    ttf_resolve::ResolveOutcome outcome =
        ttf_resolve::evaluate(active, static_cast<uint32_t>(available), pageForChar, pageCharStart, haveTotal);
    targetOut = outcome.page;
    needFullBuild = outcome.needFullBuild;
    if (outcome.resolved) {
      // Consume-on-paint (§3.2): the source latches and the stored target stay
      // set until the target page is actually on screen (see the consume in
      // renderBookTtf after readPage). A resolve alone is not durable — the
      // partial build behind it can still be lost to a failed suspend/commit,
      // and the funnel must be able to re-derive the same target and rebuild.
      return true;
    }
    return false;
  }

  if (pendingPercentJump) {
    if (!haveTotal || available <= 0) {
      needFullBuild = true;
      return false;
    }
    int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(available));
    if (newPage >= available) newPage = available - 1;
    pendingPercentJump = false;
    targetOut = std::max(0, newPage);
    return true;
  }

  // Default: preserved page (re-render) or the pending default page set by
  // navigation (pageTurn/menu/KOReader).
  targetOut = ttfPage >= 0 ? ttfPage : std::max(0, nextPageNumber);
  return true;
}

void EpubReaderActivity::renderBookTtf() {
  if (!epub || !ttf_) return;
#if CROSSPOINT_TTF_UI_FALLBACK
  // makeLayoutParams() below can reload the shared font loader on this render
  // (first render after boot: the session's first selectFamily() dirties it),
  // freeing the resident bytes the registered UI-fallback faces borrow.
  // update() re-validates the fingerprint and borrowed byte addresses and
  // re-registers the faces BEFORE anything draws through the stale ones —
  // without it the status bar's FT_Get_Char_Index walks a freed cmap
  // (LoadProhibited, issue #168). Fast pointer-compare path when clean.
  freeink::book::ttfUiFallback.update(renderer);
#endif
  // Runs under the render task's RenderLock; catches every requestUpdate()
  // exit from the overlay while its deferred chrome refresh is still pending.
  settleOverlayRefresh();
  // P4 prerender: capture and clear the pass flags before any state checks.
  // - prerender pass:      paint the NEXT page's content only (no status bar,
  //                        no flush) into the framebuffer.
  // - fast-display pass:   the framebuffer already holds the next page's
  //                        content; skip the base paint, chrome + commit only.
  // - anything else:       a normal render — navigation, settings, popups,
  //                        rebuilds — and any stale prerender dies here.
  const bool isPreRenderPass = ttfPendingPreRender;
  const bool isBufferDisplayPass = ttfUsePreRenderedBuffer;
  ttfPendingPreRender = false;
  ttfUsePreRenderedBuffer = false;
  ttfRenderBusyStartMs = millis();
  if (!isPreRenderPass && !isBufferDisplayPass) ttfInvalidatePreRender("unflagged render pass");
  ttfChromePopupShown = false;
  // Any render attempt makes the previous framebuffer state provisional: only
  // a successful page+status render below may restore the fast-open flag.
  ttfFrameRenderComplete.store(false, std::memory_order_release);
  // The Indexing popup is per-pass paint, not sticky state: each pass decides
  // afresh whether the chapter is still too thin to read (see the !resolved
  // gate in renderBookTtf). Clearing the latch here is what lets a pass that
  // can serve pages paint the page instead of the popup.
  indexingPopupShown = false;

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };
  const auto showBuildError = [this]() {
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
    automaticPageTurnActive = false;
  };

  if (currentSpineIndex < 0) currentSpineIndex = 0;
  if (currentSpineIndex > epub->getSpineItemsCount()) currentSpineIndex = epub->getSpineItemsCount();
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    return;  // end of book — isAtEndOfBook() drives the end-of-book menu
  }

  // Render-path heap gate (§3.5): a low DRAM floor postpones the raster to
  // the next loop pass rather than fragmenting the hot path.
  if (ESP.getFreeHeap() < RENDER_MIN_FREE_HEAP) {
    requestUpdate();
    return;
  }

  // 1) Layout params + generation for the current geometry/settings state.
  freeink::book::LayoutParams params;
  ttf_->makeLayoutParams(renderer, params, automaticPageTurnActive);
  if (params.font == nullptr) {
    showBuildError();
    return;
  }
  // Retry backstop for the P2 hint-stack verdict. ensureLoaded() settles the
  // probe itself at its tail (shallow-heap window); an unmeasurable attempt
  // there arms exactly one re-probe, consumed HERE — still before the
  // generation hash and the worker start below consume the fingerprint and
  // before any hinted rasterization in this pass (a retry that succeeds
  // re-opens hinting and re-derives the fingerprint; a failed one leaves the
  // fail-closed degrade in effect). Measured deeper down the
  // settings/preview stack the loop task's own frames dominate the probe's
  // high-water delta and every face degrades.
  freeink::book::fontLoader.ensureHintProbeSettled();
  const uint32_t generation = freeink::book::layoutGenerationHash(params, freeink::book::fontLoader.fontFingerprint());
  ttfGeneration = generation;
  ttfGenerationValid = true;
  // Index-ahead worker: spawn/notify under the current generation + chapter.
  updateFibpWorker(generation, params);

  // 1b) P4 prerender passes. Both re-derive their target from live state (the
  // flags only say a pass was requested — navigation since the schedule shows
  // up here as a different spine/page) and both run with the panel quiet:
  // settleOverlayRefresh() above drained any deferred overlay waveform, and
  // the previous page's commit waited for its own refresh.
  if (isPreRenderPass) {
    ttfRunPreRenderPass(params);
    return;
  }
  if (isBufferDisplayPass) {
    if (ttfFastDisplayPass(params)) return;
    // Page read failed or an image page slipped through: the framebuffer no
    // longer matches the reading position — full render below.
    ttfInvalidatePreRender("fast pass precondition");
  }

  // 2) Chapter transition. A running session for the chapter we enter (a
  // prefetch build) keeps laying out; anything else aborts (partial commit).
  if (ttfSpine != currentSpineIndex) {
    ttfPrefetchActive = false;
    fibpDeferred_ = false;
    ttf_->dropPrefetch();
    ttfFrameRenderComplete.store(false, std::memory_order_release);
    if (ttfHasSavedPosition && currentSpineIndex != ttfSavedSpine) {
      ttfHasSavedPosition = false;
    }
    if (!ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex))) {
      ttf_->abortSession();
    }
    ttf_->closeChapterCache();
    ttfSpine = currentSpineIndex;
    ttfPage = -1;
    ttfPageCount = 0;
    if (pendingRestoreTarget_.origin != PendingTarget::Origin::Reflow) {
      // Position targets (saved record, jump, anchor, seed) are bound to the
      // chapter they were derived in; a reflow target survives the transition
      // because the char anchor below still maps in the rebuilt chapter.
      pendingRestoreTarget_ = {};
    }
    if (pendingRestoreTarget_.kind != PendingTarget::CharOffset) {
      // A settings/orientation reflow still needs the displayed page's char
      // anchor — dropping it here would resolve the reflow to offset 0.
      ttfCurrentCharStart = 0;
      ttfCurrentCharEnd = 0;
    }
    const freeink::book::BookStatus st = ttf_->openChapterCache(static_cast<uint16_t>(currentSpineIndex), generation);
    if (st == freeink::book::BookStatus::Stale) {
      LOG_DBG("ERS", "Stale TTF cache for spine %d — rebuilding", currentSpineIndex);
    } else if (st == freeink::book::BookStatus::Ok && ttf_->cachePartial()) {
      LOG_DBG("ERS", "Partial TTF cache (%u pages) — resuming build",
              ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex)));
    }
  }

  ttfPageCount = ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex));
  // The mirrors now belong to the entered chapter, so the worker's progress
  // report is finally meaningful. updateFibpWorker() ran BEFORE this transition
  // block and its notifyChapterProgress guard skipped the stale spine — and an
  // idle reader produces no further renders, so without this the entry trigger
  // for a short chapter would never fire at all.
  // fibpBegun_ mirrors updateFibpWorker's guard: a worker whose begin() failed
  // is non-null but unarmed (runtime_ null), and spawning its task would fault.
  if (ttfSpine == currentSpineIndex && fibpWorker_ != nullptr && fibpBegun_) {
    // Same settled-length test updateFibpWorker uses: while a session or a
    // partial cache owns the chapter the count is a watermark, and reporting it
    // as final would fire the short-chapter branch on a long chapter's first
    // cold-start pass. A committed complete cache IS the settled length, which
    // is the case this entry report exists to catch.
    const bool lengthFinal =
        !ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex)) && !(ttf_->cacheReady() && ttf_->cachePartial());
    fibpWorker_->notifyChapterProgress(static_cast<uint16_t>(currentSpineIndex), 0, static_cast<uint16_t>(ttfPageCount),
                                       lengthFinal);
  }

  // 2b) Soak addendum: a PARTIAL current chapter whose target page is already
  // servable is handed to the worker — the reader paints immediately from
  // the partial and the rest of the chapter finishes off-task. Idempotent
  // request; the commit pickup below reopens the complete cache when the
  // worker releases the claim. The reader never inline-builds while it
  // holds this claim (takeover guards below return the spine to the reader
  // when it actually needs pages the partial lacks).
  if (ttfSpine == currentSpineIndex && ttf_->cacheReady() && ttf_->cachePartial() && fibpWorker_ != nullptr &&
      fibpBegun_) {
    if (fibpResumeClaimedSpine_ != currentSpineIndex) {
      fibpResumeClaimedSpine_ = static_cast<int16_t>(currentSpineIndex);
      fibpResumeSeenBuilding_ = false;
      fibpResumeRequestMs_ = millis();
      fibpWorker_->requestResumeClaim(static_cast<uint16_t>(currentSpineIndex));
      // Start the task with the claim: only a live worker consumes it, and
      // updateFibpWorker's notifyChapterProgress may not run again until the
      // next user-driven render (an idle reader produces none).
      (void)fibpWorker_->ensureTask();
      LOG_INF("ERS", "Resume build handed to worker: spine %d (%u pages cached)", currentSpineIndex,
              static_cast<unsigned>(ttfPageCount));
    }
  } else if (fibpResumeClaimedSpine_ != -1 && fibpResumeClaimedSpine_ != currentSpineIndex) {
    fibpResumeClaimedSpine_ = -1;  // left the chapter
  }

  // 3) Single-writer handoff (R4 optional polish): while the worker builds
  // the chapter we are on, wait for its commit instead of racing a second
  // writer onto the same FIBP file; once it lands, reopen the cache so the
  // resolve/build decisions below see the worker's page count — no sync
  // rebuild, no popup stall beyond the wait.
  // A RESUME claim (2b) is exempt from the parking wait: the reader can
  // already serve its target page from the partial, so it renders normally
  // while the worker finishes in the background.
  // Set when the chapter is readable and the worker still owns this spine:
  // the reader may paint committed pages but must not open an inline session.
  bool workerOwnsSpine = false;
  // Park-decision inputs. The park/popup decision below runs AFTER the
  // resolver funnel because a restore the worker's partial already covers
  // must paint immediately — no Indexing popup, regardless of chapter size
  // (v2.6.0 regression owner rule). The funnel may already have run in the
  // park stage; its outcome is reused at the resolve site below (the resolver
  // clears source latches on resolve, so it must not run twice in one pass).
  bool needFullBuild = false;
  int target = -1;
  bool deferredProbed = false;      // park stage ran the funnel this pass
  bool deferredResolved = false;    // its outcome (valid when deferredProbed)
  bool deferredPopupArmed = false;  // this pass entered the delegation state
  const bool workerOnSpine = fibpWorker_ != nullptr && fibpWorker_->active() &&
                             fibpWorker_->buildingSpine() == static_cast<uint16_t>(currentSpineIndex);
  if (workerOnSpine && fibpResumeClaimedSpine_ != currentSpineIndex) {
    if (!fibpDeferred_) {
      fibpDeferred_ = true;
      fibpDeferStartMs_ = millis();
      deferredPopupArmed = true;
      LOG_DBG("ERS", "Chapter %d build delegated to prefetch worker", currentSpineIndex);
    }
    // The worker owns this build; re-polling with full renders every loop
    // tick flip-flops the power governor and burns the CPU. Re-check at a
    // slow cadence.
    if (millis() - fibpDeferPollMs_ >= fibpDeferPollMs) {
      fibpDeferPollMs_ = millis();
      requestUpdate();
    }
    // The worker commits pages while the reader is delegated here: reopen so
    // the park decision, the coverage probe and edge turns see its latest
    // partial (the readable-continue passes of the old flag toggle did this).
    ttf_->openChapterCache(static_cast<uint16_t>(currentSpineIndex), generation);
    ttfPageCount = ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex));
    // Past the readability threshold, stop deferring so the reader gets a page
    // instead of the popup for the rest of the worker's build. The worker keeps
    // draining the chapter off-task, so while it still holds this spine we must
    // NOT fall through into the inline build below: ensureChapterSession would
    // put a second writer on the same FIBP file.
    if (shouldClearBuildPopup(static_cast<int>(ttfPageCount))) {
      workerOwnsSpine = true;
    } else {
      // Park candidate. A restore the partial already covers is served right
      // now instead. Only restores probe: the resolver's shortcut kinds
      // (page jump, percent, last page) clear their latches on resolve, and a
      // probe-resolved jump would be lost to the default path on the next
      // pass. Explicit anchors and offset jumps keep the park behavior.
      const bool restoreLikely = pendingRestoreTarget_.kind != PendingTarget::None || ttfHasSavedPosition;
      if (restoreLikely) {
        deferredProbed = true;
        deferredResolved = ttfResolveTargetPage(target, params, needFullBuild);
      }
      const bool restoreCovered =
          deferredProbed && deferredResolved && target >= 0 && target < static_cast<int>(ttfPageCount);
      if (restoreCovered) {
        // Owner rule: the partial holds the restore target — paint it now and
        // let the worker finish the chapter in the background. No popup.
        workerOwnsSpine = true;
      } else if ((deferredPopupArmed || restoreLikely || fibpDeferParkLatched_) &&
                 millis() - fibpDeferStartMs_ >= fibpDeferTakeoverMs) {
        // A long delegated build parks the user on the Indexing popup for the
        // whole spine (the plan builds the entered chapter last, and a claim
        // that landed right before the page turn makes the wait the full
        // build). After a short grace period the reader takes the spine over:
        // stop the worker (it aborts at the next page boundary and commits
        // its partial), then the normal path below builds only up to the
        // target page at full speed. updateFibpWorker() respawns the worker
        // once the session drains; it skips this spine (partial/complete now
        // exists). The escape only fires for states that actually block the
        // reader — a restore pending since before the pass, or the popup this
        // episode armed. A reader already reading a committed page must never
        // kill the worker: a respawn skips a spine that has a partial, so the
        // chapter would then never finish in the background.
        LOG_DBG("ERS", "Delegated build too slow — taking over spine %d", currentSpineIndex);
        stopFibpWorker();
        fibpDeferred_ = false;
        requestUpdate();
      } else {
        // Park on the popup. Shown once per delegation episode (at the entry
        // transition); later parked passes leave the frame as painted. The
        // parked latch keeps the takeover escape reachable on later passes of
        // an episode that blocked the reader at its entry.
        fibpDeferParkLatched_ = true;
        if (deferredPopupArmed) ttfShowIndexingPopup();
        return;
      }
    }
  }
  if (fibpDeferred_ && (!workerOnSpine || fibpResumeClaimedSpine_ == currentSpineIndex)) {
    // The delegated build finished (or failed) — or a resume claim took the
    // spine over (its lifecycle reopens the cache below anyway): pick up the
    // committed cache so the resolve sees the worker's pages.
    fibpDeferred_ = false;
    fibpDeferParkLatched_ = false;
    ttf_->openChapterCache(static_cast<uint16_t>(currentSpineIndex), generation);
    ttfPageCount = ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex));
  }
  if (fibpResumeClaimedSpine_ == currentSpineIndex) {
    // Resume-claim lifecycle. The worker builds the claim with priority and
    // does not clear running_ between spines, so a finished commit may never
    // be caught by a render observing buildingSpine(). Probe the committed
    // cache whenever the worker is not on this spine: a complete cache means
    // the commit landed; a still-partial one keeps the claim while the task
    // is alive (it may not have reached the claim yet) and releases it once
    // the task is gone past the grace window.
    const bool workerHolds = fibpWorker_ != nullptr && fibpWorker_->active() &&
                             fibpWorker_->buildingSpine() == static_cast<uint16_t>(currentSpineIndex);
    if (workerHolds) {
      fibpResumeSeenBuilding_ = true;
    } else {
      const freeink::book::BookStatus st = ttf_->openChapterCache(static_cast<uint16_t>(currentSpineIndex), generation);
      ttfPageCount = ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex));
      const bool committed = st == freeink::book::BookStatus::Ok && !ttf_->cachePartial();
      const bool workerAlive = fibpWorker_ != nullptr && fibpWorker_->active();
      // The run loop claims the resume BEFORE the queue, so a worker seen on
      // another spine has already consumed this claim — or was gated off by an
      // earlier failure and will never build it. With no committed cache that
      // means the resume failed: release the claim so the margin fallback
      // resumes the extension instead of parking it for the worker's whole
      // look-ahead build window.
      const uint16_t workerBuilding =
          fibpWorker_ != nullptr ? fibpWorker_->buildingSpine() : freeink::book::fibp::kNoChapter;
      const bool busyElsewhere = workerBuilding != freeink::book::fibp::kNoChapter &&
                                 workerBuilding != static_cast<uint16_t>(currentSpineIndex);
      // busyElsewhere releases the claim on the same grace clock as the
      // dead-worker branch: an instant clear while resumeSpine_ is still
      // armed would let reader and worker build the same spine concurrently
      // (kody, PR #165). The grace window lets the worker pick the claim up
      // first; if it never does, the fallback resumes the extension.
      if (committed || fibpResumeSeenBuilding_ ||
          ((workerAlive && busyElsewhere || !workerAlive) &&
           millis() - fibpResumeRequestMs_ > kFibpResumeClaimGraceMs)) {
        fibpResumeClaimedSpine_ = -1;
        fibpResumeSeenBuilding_ = false;
        if (committed) {
          LOG_INF("ERS", "Worker resume build committed: %u pages", static_cast<unsigned>(ttfPageCount));
        }
      }
    }
  }

  // 4) Resolve the target page (jump states may need more build first). The
  // deferred-park stage may have already run the funnel this pass — reuse
  // that outcome instead of resolving twice.
  bool resolved = false;
  if (deferredProbed) {
    resolved = deferredResolved;
  } else {
    resolved = ttfResolveTargetPage(target, params, needFullBuild);
  }

  // §5.1: Session-begin gate must open for a pending restore. With a pending
  // unresolved restore (not covered yet), neither needFullBuild nor
  // (resolved && target >= ttfPageCount) holds — the first pass after reopen
  // would build nothing and paint the popup forever.
  const bool pendingRestore = pendingRestoreTarget_.kind != PendingTarget::None;
  // Consume-on-paint (§3.2) keeps pendingRestoreTarget_ set until the target
  // page paints, so a raw pendingRestore no longer means "a build is needed":
  // a restore the current index already covers (complete cache, or a partial
  // holding the anchor) resolves in the funnel and paints in THIS pass.
  // Gating the session/popup on the raw flag opened a fresh layout session
  // OVER a complete cache and flashed Indexing on every reopen — the v2.6.0
  // regression. Gate on "restore still uncovered" instead.
  const bool restoreNeedsBuild = pendingRestore && !resolved;

  // Reader needs pages the partial lacks while the worker holds the resume
  // claim: take the spine back (worker aborts at the next page boundary and
  // commits its partial), then build inline to the target.
  if (fibpResumeClaimedSpine_ == currentSpineIndex &&
      (restoreNeedsBuild || needFullBuild || (resolved && target >= static_cast<int>(ttfPageCount)))) {
    LOG_INF("ERS", "Reader takes resume claim over: spine %d (pending=%d needFull=%d target=%d pages=%u)",
            currentSpineIndex, pendingRestore, needFullBuild, target, static_cast<unsigned>(ttfPageCount));
    const unsigned long joinStartMs = millis();
    stopFibpWorker();
    LOG_INF("ERS", "Resume claim: worker stopped in %lu ms", millis() - joinStartMs);
    fibpResumeClaimedSpine_ = -1;
    fibpResumeSeenBuilding_ = false;
    ttf_->openChapterCache(static_cast<uint16_t>(currentSpineIndex), generation);
    ttfPageCount = ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex));
  }

  // 4) Build toward the target, synchronously like the legacy path. The heap
  // gate can defer the remainder to the background ticks.
  bool wasBuilding = false;
  // workerOwnsSpine: the worker is mid-build on this spine. Opening an inline
  // session here would be a second writer on the same FIBP file — serve the
  // pages it has already committed and let it finish.
  if (!workerOwnsSpine &&
      (restoreNeedsBuild || needFullBuild || (resolved && target >= static_cast<int>(ttfPageCount))) &&
      !ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex))) {
    const uint32_t spineBytes = ttf_->catalog().spineSize(static_cast<size_t>(currentSpineIndex));
    // §3.4 / §3.5: Show Indexing while a pending restore owns the pass —
    // a cold or stale cache and a settings-driven reflow always rebuild
    // across render passes, so show it; a warm top-up of a partial cache
    // stays threshold-gated to avoid a flash. No page is painted while the
    // restore is pending (§3.5). A restore the index already covers
    // (restoreNeedsBuild false) paints directly — see the gate above.
    const bool partialCache = ttf_->cacheReady() && ttf_->cachePartial();
    if (!ttf_->cacheReady() || restoreNeedsBuild || needFullBuild ||
        (partialCache && (spineBytes > BUILD_POPUP_BYTE_THRESHOLD || target > BUILD_POPUP_PAGE_THRESHOLD))) {
      // A chapter that already has MIN_PAGES_TO_CLEAR_POPUP pages is readable,
      // so painting Indexing over it would be a lie — the reader gets the page
      // and the remainder drains in the background ticks.
      if (!shouldClearBuildPopup(static_cast<int>(ttfPageCount))) ttfShowIndexingPopup();
    }
    const freeink::book::BookStatus st =
        freeink::book::ensureChapterSession(*ttf_, static_cast<uint16_t>(currentSpineIndex), params, generation);
    if (st != freeink::book::BookStatus::Ok) {
      LOG_ERR("ERS", "TTF session begin failed: %s", bookStatusName(st));
      showBuildError();
      return;
    }
    wasBuilding = true;
  }
  wasBuilding = wasBuilding || ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex));

  if (ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex))) {
    // Build one page chunk per render pass on a warm chapter; allow a short
    // cold-open burst so an empty chapter reaches a readable page faster.
    // Further chunks are driven by the next render/background tick so long
    // jumps still cannot freeze input.
    constexpr uint8_t kWarmSyncBuildChunks = 1;
    // Owner steer: return to the reader after ~one chunk (5-10 pages).
    constexpr uint8_t kColdStartSyncBuildChunks = 1;
    const uint8_t chunksPerPass = ttfPageCount == 0 ? kColdStartSyncBuildChunks : kWarmSyncBuildChunks;
    uint8_t chunksThisPass = 0;
    // §5.2: one chunk per pass toward the target. For a pending restore the
    // loop pumps while !resolved (cover check from ttfResolveTargetPage:
    // !resolved == !covered && !terminal), exactly one BUILD_PAGES_PER_CHUNK
    // per pass, requestUpdate() between passes (input polled, buttons alive).
    // needFullBuild stays true ONLY for Percent/LastPage kinds, preserving
    // their wait-for-total semantics. Normal forward reading keeps the existing
    // resolved && target >= available condition.
    while (ttf_->sessionActive() &&
           (pendingRestore  ? !resolved
            : needFullBuild ? true
                            : resolved && target >= static_cast<int>(ttf_->availablePageCount(
                                                        static_cast<uint16_t>(currentSpineIndex))))) {
      if (!buildTickHeapGate()) break;
      if (chunksThisPass++ >= chunksPerPass) {
        if (ttf_->sessionActive()) {
          ttfPageCount = ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex));
          requestUpdate();
          // Past the readability threshold, fall through and paint what exists
          // rather than returning before the readable-page gate below — with
          // needFullBuild the loop keeps running across passes, so returning
          // here would leave the reader on Indexing for the whole chapter. The
          // target stays pending and resolves on a later pass.
          if (!shouldClearBuildPopup(static_cast<int>(ttfPageCount))) return;
          break;
        }
        break;
      }
      freeink::book::BookStatus st = freeink::book::BookStatus::Ok;
      if (freeink::book::pumpChapterChunk(*ttf_, static_cast<uint16_t>(currentSpineIndex), BUILD_PAGES_PER_CHUNK,
                                          &st) == freeink::book::ChapterPump::Failed) {
        LOG_ERR("ERS", "TTF build failed: %s", bookStatusName(st));
        showBuildError();
        return;
      }
    }
    ttfPageCount = ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex));
    // Owner steer: after the first few pages, hand the rest to the worker —
    // suspend the inline session to a partial commit; the resume-claim
    // handoff (2b) picks it up next pass and the reader reads while the
    // chapter finishes off-task. Non-worker builds keep the session (the
    // background tick pump drives it with per-page progress logs).
    // Guards: only an ARMED worker may take over (an allocated-but-unbegun
    // worker object here would abort a healthy inline build), and a pending
    // restore is never handed off — the target page must be painted from this
    // build before the position can be consumed.
    if (ttf_->sessionActive() && fibpWorker_ != nullptr && fibpBegun_ &&
        pendingRestoreTarget_.kind == PendingTarget::None) {
      LOG_INF("ERS", "Owner steer: spine %d to worker at %u pages", currentSpineIndex,
              static_cast<unsigned>(ttfPageCount));
      ttf_->abortSession();
    }
  }
  if (wasBuilding && !ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex))) {
    // The build finished (or aborted) during this pass — reopen the cache so
    // serving switches from the writer index to the committed file.
    ttf_->openChapterCache(static_cast<uint16_t>(currentSpineIndex), generation);
    ttfPageCount = ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex));
  }

  // Set when this pass paints a placeholder because the real target is not
  // derivable yet. The page is readable but is NOT the reader's position, so
  // the position mirrors must not absorb it.
  bool servedPlaceholderPage = false;
  if (!resolved) {
    // §3.5: During a pending restore, no page is painted — the popup IS the
    // painted content until the anchor resolves. The readable-page gate
    // (shouldClearBuildPopup) does NOT fire while a restore target is pending;
    // painting page 0 of a partially built chapter would visually announce a
    // position loss even though the resolve later corrects it. Normal forward
    // reading indexing keeps today's yield behavior unchanged.
    if (pendingRestoreTarget_.kind != PendingTarget::None) {
      // §3.4: the popup IS the painted content while a restore owns the pass —
      // redraw it on EVERY blocked pass so the progress bar tracks the build
      // (a restore starting from a substantial partial cache never took the
      // session-start branch, and a shown popup must not freeze its bar).
      ttfShowIndexingPopup();
      if (millis() - fibpDeferPollMs_ >= fibpDeferPollMs) {
        fibpDeferPollMs_ = millis();
        requestUpdate();
      }
      return;
    }
    // The target is not derivable from the current index — a reflow position
    // only maps once the chapter index is COMPLETE, and a jump may need pages
    // past the watermark. Once MIN_PAGES_TO_CLEAR_POPUP pages exist there is
    // something worth reading, so serve the best available page rather than
    // holding the whole chapter on the Indexing popup; the background ticks
    // (ttfBackgroundBuildTick) keep draining the session and requestUpdate()
    // re-enters here when it lands. Below that threshold the popup is correct:
    // a one-page chapter gives the reader nothing to turn to.
    if (shouldClearBuildPopup(static_cast<int>(ttfPageCount))) {
      target = ttfPage >= 0 ? ttfPage : 0;
      servedPlaceholderPage = true;
      LOG_INF("ERS", "Serving page %d of %u mid-index (target not resolvable yet)", target,
              static_cast<unsigned>(ttfPageCount));
    } else {
      if (millis() - fibpDeferPollMs_ >= fibpDeferPollMs) {
        fibpDeferPollMs_ = millis();
        requestUpdate();
      }
      return;
    }
  }

  if (ttfPageCount == 0) {
    const bool buildInFlight = ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex));
    // Same spine/generation match the resolver's own cache checks use
    // (availablePageCount answers only the cache's own chapter,
    // TtfBookRuntime.cpp:176): a ready-but-stale or foreign-spine cache must
    // not count as complete, or a pending restore would be consumed as
    // terminal while the real chapter has not been built yet.
    const bool cacheComplete = ttf_->cacheReady() && ttf_->cacheSpine() == static_cast<uint16_t>(currentSpineIndex) &&
                               ttf_->cacheGeneration() == ttfGeneration && !ttf_->cachePartial() && !buildInFlight;
    const bool restorePending = pendingRestoreTarget_.kind != PendingTarget::None;
    if (buildInFlight || (restorePending && !cacheComplete)) {
      // A build is in flight (or the index can still grow) with nothing
      // committed yet: that is Indexing, not an empty chapter — with or
      // without a pending restore. Painting Empty here would reset
      // nextPageNumber and send the reader to the chapter start once the
      // rebuild lands. (cacheComplete already implies !buildInFlight, so the
      // terminal branch below only runs when no build can add pages.)
      LOG_INF("ERS", "Spine %d has no pages yet — restore pending=%d build active=%d, showing Indexing",
              currentSpineIndex, restorePending, buildInFlight);
      ttfShowIndexingPopup();
      requestUpdate();
      return;
    }
    if (restorePending) {
      // Terminal: the chapter is complete with zero pages, so the restore
      // target can never map. Consume it through the origin-aware path so the
      // source latch that produced it dies too — the funnel would otherwise
      // re-derive it on every later render (an offset jump can leak into
      // another chapter).
      LOG_INF("ERS", "Spine %d complete with 0 pages — restore cannot map, opening empty", currentSpineIndex);
      consumeRestoreTarget();
    }
    LOG_INF("ERS", "TTF: empty chapter (spine %d, no pages, no pending restore/build)", currentSpineIndex);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    nextPageNumber = 0;
    cachedChapterTotalPageCount = 0;
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (target < 0) target = ttfPage >= 0 ? ttfPage : 0;
  if (target >= static_cast<int>(ttfPageCount)) {
    if (ttf_->sessionActive()) {
      requestUpdate();  // build will cover it
      return;
    }
    target = static_cast<int>(ttfPageCount) - 1;
  }
  ttfPage = target;

  updateBookmarkFlag();

  // 5) Read + rasterize the page. Run text lives in the runtime's scratch
  // arena for exactly this block. A failed read must not expose the prior
  // frame through overlay fast paths.
  ttfFrameRenderComplete.store(false, std::memory_order_release);
  const size_t scratchMark = ttf_->scratch().mark();
  freeink::book::Page page{};
  if (!ttf_->readPage(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(ttfPage), &page)) {
    ttf_->scratch().release(scratchMark);
    LOG_ERR("ERS", "TTF page read failed (spine %d page %d)", currentSpineIndex, ttfPage);
    requestUpdate();  // transient SD failure; retry on the next pass
    return;
  }
  // A placeholder page's charStart is not the reader's position: it is merely
  // where the partial index happened to start. Keeping it would corrupt a later
  // reflow restore, and ttfCommitFrame() saves it on close — flushing a
  // provisional position to disk. Preserve the real offset until the target
  // resolves. (The fast-display path at ~3710 never serves a placeholder: it
  // only runs from a page the reader already turned to.)
  // A still-pending preview seed counts as unresolved for the same reason: the
  // page served is a clamp to the last built page, not the reader's position,
  // so its charStart must not overwrite the real offset either.
  // Consume-on-paint (§3.2): the restore target is only consumed once its
  // page is actually on screen. A resolver "resolve" alone is not durable —
  // the partial build behind it can be lost to a failed suspend/commit (the
  // funnel then re-derives the still-set latches and rebuilds). Placeholder
  // paints are not the reader's position, so they consume nothing. Consuming
  // BEFORE the mirror lets a seed target's own paint clear the seed guard and
  // establish the position in the same pass.
  const bool consumeRestore =
      !servedPlaceholderPage && resolved && ttfPage == target && pendingRestoreTarget_.kind != PendingTarget::None;
  if (consumeRestore) consumeRestoreTarget();
  if (!servedPlaceholderPage) {
    ttfCurrentCharStart = page.charStart;
    ttfCurrentCharEnd = QuickPageCapture::pageEndChar(page);
  }

  // §3.5 item 8: footnote list from the engine's PageLink substrate. Internal
  // (resolvable) targets only — external URLs never enter the reader flow.
  ttfExtractFootnotes(page);

  renderer.clearScreen(0xFF);
  paintTtfPage(page, params.font, params.characterSpacingPx, params.wordSpacingPx);
#ifdef READING_STATS_ENABLED
  // Engine-computed word count (ChapterLayout counts over the paragraph text
  // before run segmentation; justified runs carry no spaces — counting run
  // text collapsed a justified page to ~2 words).
  currentPageWordsOnPage = page.wordCount;
#endif

  // 6) Chrome after the page: keep the legacy position mirrors in sync so
  // renderStatusBar/KOreader/bookmark code reads the same values.
  // A page served mid-index (see the !resolved gate) is a placeholder, not the
  // reader's position — recording it would let the saved-position branch below
  // match it and land the reader at the start of the chapter.
  if (!servedPlaceholderPage) nextPageNumber = ttfPage;
  cachedChapterTotalPageCount = static_cast<int>(ttfEstimatedPageCount());
  renderStatusBar();
  // Do not mark the frame complete yet: the TTF gray/image-specific passes
  // below may still be modifying the display planes.
  ttfCommitFrame(page, params, scratchMark);
}

// Display commit + finalize tail shared by the normal render and the P4 fast
// turn (the framebuffer already holds the painted content in both cases —
// the fast pass skips only the base paint, not the commit).
void EpubReaderActivity::ttfCommitFrame(const freeink::book::Page& page, const freeink::book::LayoutParams& params,
                                        size_t scratchMark) {
  {
    const uint32_t busyMs = millis() - ttfRenderBusyStartMs;
    if (busyMs > 500) {
      LOG_DBG("ERS", "render busy window %lu ms — button edges inside it are unsampleable (polled input)",
              static_cast<unsigned long>(busyMs));
    }
  }
  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };
  // Same §11 Q7 predicate paintTtfPage used for the base pass above: images
  // keep the 1bpp engine path (no plane bits), so the dual-plane block runs
  // only for text-only AA pages on a panel whose controller supports the
  // 4-level gray mode at all.
  const bool pageHasImages = page.imageCount > 0 && SETTINGS.imageRendering == CrossPointSettings::IMAGES_DISPLAY;
  const auto grayCaps = renderer.grayscaleCapabilities();
  // Degrade-aware: a build without the mono module paints Smooth planes even
  // when the setting asks for Crisp (the loader degraded the faces).
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  const bool smoothText = !freeink::book::fontLoader.effectiveMonochrome();
#else
  const bool smoothText = SETTINGS.textRenderMode == CrossPointSettings::TEXT_RENDER_SMOOTH;
#endif
  const bool grayParity = smoothText && !pageHasImages && grayCaps.supported();
  LOG_DBG("GRS", "ttfGrayPath: aa=%d images=%d strip=%d cadence=%d/%d", grayParity, pageHasImages,
          grayCaps.stripUploads, pagesUntilFullRefresh, SETTINGS.getRefreshFrequency());
  if (grayParity) {
    // §11 Q7 construction (a): dual-plane gray parity. Base refresh ordering
    // mirrors the legacy AA path: cleanup cycle when due, otherwise the
    // grayscale base waveform; then the plane walks, the gray display, and
    // the baseline cleanup. Transport follows the panel: strips where the
    // driver supports them, full-frame plane buffers otherwise (UC8279 X4
    // advertises Overlay gray with stripUploads=false — the full planes go
    // through the same driver's full-plane upload, not a strip flag flip).
    LOG_DBG("GRS", "ttf gray route: transport=%s pagesUntilFullRefresh=%d refreshFrequency=%d images=%d",
            grayCaps.stripUploads ? "strips" : "full-frame", pagesUntilFullRefresh, SETTINGS.getRefreshFrequency(),
            pageHasImages);
    if (grayCaps.stripUploads) {
      renderTtfGrayStrips(page, params, scratchMark);
    } else {
      renderTtfGrayFullFrame(page, params, scratchMark);
    }
    // The page's run text lives in the scratch arena: release only after
    // the last plane pass has walked it.
    ttf_->scratch().release(scratchMark);
    lastRenderCompleteMs = millis();
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
    LOG_DBG("REND", "reader gray render stack high-water=%u bytes",
            static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    memSentinelCheck("reader gray render");
#endif
    // Cleanup has restored the framebuffer after the final gray-plane pass.
    ttfFrameRenderComplete.store(true, std::memory_order_release);
    // The frame is fully committed: allow rememberBookOnceRendered() to set
    // openEpubPath + recents. Missing here meant sleep-from-reader had no
    // staged cover path (blank sleep screen) and fresh books never entered
    // recents until a legacy/XTC/TXT reader session wrote them.
    markPageRendered();
#ifdef READING_STATS_ENABLED
    pageShownAtMs = millis();
#endif
    ttfSaveProgress();
    showPendingSyncSaveError();
    finishTtfPageRender();
    return;
  }

  // 1bpp path: the page's run text was last touched by paintTtfPage above.
  ttf_->scratch().release(scratchMark);

  const bool canAsyncDisplay = renderer.supportsAsyncRefresh();
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, canAsyncDisplay);
  if (canAsyncDisplay) {
    // E-ink refresh is running: overlap background work with it (legacy
    // renderContents pattern).
    prefetchNextChapterDuringDisplay();
    if (ttf_->sessionActive() && buildTickHeapGate()) {
      // Route through the tick so session completion reopens the committed
      // cache (a raw stepBuild would leave the reader without a source).
      ttfBackgroundBuildTick();
    }
    renderer.waitRefreshComplete();
  }
  lastRenderCompleteMs = millis();
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  // In-render stack probe: the post-render REND telemetry never ran for the
  // crashing reader renders — measure the reader path's own high-water HERE,
  // before render() returns (the deepest path: rebuild + FT paint + chrome).
  LOG_DBG("REND", "reader render stack high-water=%u bytes",
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  // Heap/PSRAM budget across book renders (moved here from the generic render
  // wrapper): catches FT-side leaks (faces, glyph backing) and DRAM pressure.
  LOG_DBG("REND", "render mem: HeapFree=%u HeapMin=%u MaxAlloc=%u PSRAMFree=%u PSRAMMin=%u",
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMinFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()), static_cast<unsigned>(ESP.getFreePsram()),
          static_cast<unsigned>(ESP.getMinFreePsram()));
  memSentinelCheck("reader bw render");
#endif
  // The B/W frame and status chrome are fully painted (and any async submit
  // has completed) before overlay opens may paint directly onto this frame.
  ttfFrameRenderComplete.store(true, std::memory_order_release);
  // Same rememberBookOnceRendered() contract as the gray path above.
  markPageRendered();
#ifdef READING_STATS_ENABLED
  pageShownAtMs = millis();
#endif

  ttfSaveProgress();
  showPendingSyncSaveError();
  finishTtfPageRender();
}

void EpubReaderActivity::ttfInvalidatePreRender(const char* reason) {
  if (ttfPreRendered.ready || ttfPreRendered.pumping) {
    // Rate-limit to once per distinct reason: the soak needs to know WHICH
    // guard kills a prerender, not a per-event log flood.
    static const char* lastReason = nullptr;
    if (reason != lastReason) {
      lastReason = reason;
      LOG_DBG("ERS", "TTF prerender invalidated: %s", reason);
    }
    if (ttfPreRendered.pumping) {
      LOG_DBG("ERS", "TTF prerender aborted after %u slice(s): %s", ttfPreRendered.slicesUsed, reason);
    }
  }
  ttfPreRendered.ready = false;
  ttfPreRendered.pumping = false;
  ttfPreRendered.spineIndex = -1;
  ttfPreRendered.pageIndex = -1;
  ttfPreRendered.nextRun = 0;
  ttfPreRendered.nextChar = 0;
  ttfPreRendered.slicesUsed = 0;
}

// Schedule the one-page-ahead prerender after a fully committed frame (see
// the header contract). Refuses to schedule while the panel could expose the
// prerendered content to the glass before the reading position moves there:
// an open overlay (chrome over the page), a pending deferred overlay push,
// or a chrome popup painted this pass (its context is page N).
void EpubReaderActivity::ttfSchedulePreRender() {
  if (ttfPendingPreRender || ttfPreRendered.ready) return;
  if (ttfChromePopupShown || overlay != Overlay::None) return;
  if (overlayRefreshPending.load(std::memory_order_acquire)) return;
  if (!ttf_ || ttfSpine != currentSpineIndex || !ttf_->cacheReady()) return;
  // A build in flight owns the FIBP writer state and re-renders anyway —
  // prerendering into that churn is wasted work (and it must not fight the
  // chapter prefetch for the same pages).
  if (ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex))) return;
  if (ttfPage < 0 || ttfPage + 1 >= static_cast<int>(ttfPageCount)) return;  // chapter end: prefetch owns it
  if (ESP.getFreeHeap() < RENDER_MIN_FREE_HEAP) return;
  ttfPendingPreRender = true;
  requestUpdate();
}

void EpubReaderActivity::ttfLogPrerenderMiss(const char* reason) {
  // Once per distinct reason (soak finding #3): enough to identify the
  // failing guard, no per-event flood.
  static const char* lastReason = nullptr;
  if (reason != lastReason) {
    lastReason = reason;
    LOG_DBG("ERS", "TTF prerender miss: %s (ready=%d spine=%d page=%d gen=%08x)", reason, ttfPreRendered.ready,
            ttfPreRendered.spineIndex, ttfPreRendered.pageIndex, ttfPreRendered.generation);
  }
}

bool EpubReaderActivity::ttfEffectiveMonochromeSnapshot() const {
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  return freeink::book::fontLoader.effectiveMonochrome();
#else
  return false;
#endif
}

// P4 prerender pass: paint the NEXT page's content-only into the framebuffer
// (no status bar, no flush) while the glass still shows page N. One
// RenderLock-protected transaction; the previous pass's commit already
// waited for the refresh (waitRefreshComplete in ttfCommitFrame), so the
// paint cannot expose a partial frame.
void EpubReaderActivity::ttfRunPreRenderPass(const freeink::book::LayoutParams& params) {
  // Re-derive the target from live state — the schedule flag alone must
  // never paint a page the reading position has left. A mid-pump slice
  // (pumping) revalidates the SAME axes before continuing.
  if (!ttf_ || ttfSpine != currentSpineIndex || !ttf_->cacheReady()) return;
  if (ttfPreRendered.ready) return;
  const int nextPage = ttfPage + 1;
  if (ttfPage < 0 || nextPage >= static_cast<int>(ttfPageCount)) return;
  if (ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex))) return;
  if (overlay != Overlay::None || overlayRefreshPending.load(std::memory_order_acquire)) return;
  if (ESP.getFreeHeap() < RENDER_MIN_FREE_HEAP) return;

  const bool resuming = ttfPreRendered.pumping;
  if (resuming && (ttfPreRendered.pageIndex != nextPage || ttfPreRendered.generation != ttfGeneration ||
                   ttfPreRendered.orientation != static_cast<uint8_t>(renderer.getOrientation()) ||
                   ttfPreRendered.monochrome != ttfEffectiveMonochromeSnapshot())) {
    // Reading position, layout, or raster mode changed mid-pump: the partial
    // paint is garbage — abort (the invalidate also clears the cursor).
    ttfInvalidatePreRender("stale mid-pump");
    return;
  }

  const size_t scratchMark = ttf_->scratch().mark();
  freeink::book::Page page{};
  if (!ttf_->readPage(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(nextPage), &page)) {
    ttf_->scratch().release(scratchMark);
    LOG_DBG("ERS", "TTF prerender: page %d read failed", nextPage);
    return;
  }
  if (page.imageCount > 0 && SETTINGS.imageRendering == CrossPointSettings::IMAGES_DISPLAY) {
    // Image pages keep the 1bpp engine path with image decode — excluded
    // from prerendering (the same exclusion upstream made).
    ttf_->scratch().release(scratchMark);
    return;
  }

  auto* chain = static_cast<freeink::book::FontChain*>(params.font);
  if (chain == nullptr) {
    ttf_->scratch().release(scratchMark);
    return;
  }
  // Chunked paint (soak finding #5): one ≤kPreRenderSliceBudgetMs slice per
  // loop tick. The gray-parity base pass supports a run cursor; the 1bpp
  // engine path paints in one slice (it is far cheaper — no plane math).
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  const bool smoothText = !freeink::book::fontLoader.effectiveMonochrome();
#else
  const bool smoothText = SETTINGS.textRenderMode == CrossPointSettings::TEXT_RENDER_SMOOTH;
#endif
  const bool pageHasImages = false;  // image pages already excluded above
  const bool grayParity = smoothText && !pageHasImages && renderer.grayscaleCapabilities().supported();
  if (!resuming) {
    renderer.clearScreen(0xFF);
    ttfPreRendered.spineIndex = static_cast<int16_t>(currentSpineIndex);
    ttfPreRendered.pageIndex = static_cast<int16_t>(nextPage);
    ttfPreRendered.generation = ttfGeneration;
    ttfPreRendered.orientation = static_cast<uint8_t>(renderer.getOrientation());
    ttfPreRendered.monochrome = ttfEffectiveMonochromeSnapshot();
    ttfPreRendered.nextRun = 0;
    ttfPreRendered.nextChar = 0;
    ttfPreRendered.slicesUsed = 0;
    ttfPreRendered.pumping = true;
    // The framebuffer no longer holds a clean displayed page: overlay opens
    // must not snapshot it (their hasRenderedPage gate reads this flag).
    ttfFrameRenderComplete.store(false, std::memory_order_release);
  }
  ttfPreRendered.slicesUsed++;
  bool complete = true;
  if (grayParity) {
    complete = freeink::book::PagePaint::paintTextSliced(
        page, *chain, renderer, ttfPreRendered.nextRun, ttfPreRendered.nextChar, &ttfPreRendered.nextRun,
        &ttfPreRendered.nextChar, kPreRenderSliceBudgetMs, params.characterSpacingPx, params.wordSpacingPx);
  } else {
    paintTtfPage(page, params.font, params.characterSpacingPx, params.wordSpacingPx);
  }
  ttf_->scratch().release(scratchMark);

  if (complete) {
    if (grayParity) {
      // paintTextSliced covers runs + rubies only — rules are the base pass's
      // last step (same ordering paintTtfPage uses).
      freeink::book::PageRenderer::renderRules(page, makeFrameTarget(renderer));
    }
    ttfPreRendered.pumping = false;
    ttfPreRendered.ready = true;
    LOG_DBG("ERS", "TTF prerendered page %d/%d (%u slice%s, %u runs)", nextPage, ttfPageCount - 1,
            ttfPreRendered.slicesUsed, ttfPreRendered.slicesUsed == 1 ? "" : "s", page.runCount);
  } else {
    // Resume next tick: the pass flag re-arms and requestUpdate schedules the
    // next slice. Input processed in between (the loop handles it before the
    // render) aborts the pump in ttfPageTurn — a press waits ≤ one slice.
    ttfPendingPreRender = true;
    requestUpdate();
  }
}

// P4 fast turn: the framebuffer holds the prerendered content of the page
// the reading position just moved to. Skip the base paint (layout/build/
// readPage-clearScreen/paintTtfPage), load the page for the mirrors and the
// gray plane walks, draw the status bar, and commit. Returns false when any
// precondition fails — the caller falls through to a full render.
bool EpubReaderActivity::ttfFastDisplayPass(const freeink::book::LayoutParams& params) {
  if (!ttf_ || !ttfPreRendered.ready) return false;
  if (ttfPreRendered.spineIndex != currentSpineIndex || ttfPreRendered.pageIndex != ttfPage) return false;
  if (ttfPreRendered.generation != ttfGeneration) {
    ttfLogPrerenderMiss("stale generation (display pass)");
    return false;
  }
  if (ttfPreRendered.orientation != static_cast<uint8_t>(renderer.getOrientation()) ||
      ttfPreRendered.monochrome != ttfEffectiveMonochromeSnapshot()) {
    ttfLogPrerenderMiss("mode/orientation changed (display pass)");
    return false;
  }
  if (ttfSpine != currentSpineIndex || ttfPage < 0 || ttfPage >= static_cast<int>(ttfPageCount)) return false;

  const size_t scratchMark = ttf_->scratch().mark();
  freeink::book::Page page{};
  if (!ttf_->readPage(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(ttfPage), &page)) {
    ttf_->scratch().release(scratchMark);
    LOG_ERR("ERS", "TTF fast turn: page read failed (spine %d page %d)", currentSpineIndex, ttfPage);
    return false;
  }
  ttfCurrentCharStart = page.charStart;
  ttfCurrentCharEnd = QuickPageCapture::pageEndChar(page);
  if (page.imageCount > 0 && SETTINGS.imageRendering == CrossPointSettings::IMAGES_DISPLAY) {
    // Image pages are never prerendered; a stale flag must not fast-path them.
    ttf_->scratch().release(scratchMark);
    return false;
  }
  // Consume the snapshot: every validity gate passed, this pass owns the
  // framebuffer. Clearing ready here (not in ttfPageTurn) keeps the display
  // pass reachable and lets ttfSchedulePreRender re-arm for the next page.
  ttfInvalidatePreRender("consumed");
  ttfExtractFootnotes(page);

  // Content is already in the framebuffer — chrome only, then commit.
  nextPageNumber = ttfPage;
  cachedChapterTotalPageCount = static_cast<int>(ttfEstimatedPageCount());
  renderStatusBar();
#ifdef READING_STATS_ENABLED
  currentPageWordsOnPage = page.wordCount;
#endif
  LOG_DBG("GRS", "ttf fast turn: page %d/%d displayed from prerender", ttfPage, ttfPageCount - 1);
  ttfCommitFrame(page, params, scratchMark);
  return true;
}

void EpubReaderActivity::ttfExtractFootnotes(const freeink::book::Page& page) {
  // The number label is the superscript run overlapping the link rect,
  // falling back to the link's ordinal position.
  currentPageFootnotes.clear();
  currentPageFootnotes.reserve(page.linkCount);
  for (uint16_t l = 0; l < page.linkCount; ++l) {
    const auto& link = page.links[l];
    // SDK contract fills both strings (dropped on alloc failure upstream);
    // guard anyway so future SDK drift cannot null-deref here.
    if (link.target == nullptr) continue;
    std::string href = link.target;
    if (link.fragment != nullptr && link.fragment[0] != '\0') href += std::string("#") + link.fragment;
    if (href.empty() || href.rfind("http", 0) == 0) continue;
    if (!epub || (link.target[0] != '\0' && epub->resolveHrefToSpineIndex(href) < 0)) continue;

    FootnoteEntry entry;
    std::string number;
    for (uint16_t r = 0; r < page.runCount; ++r) {
      const auto& run = page.runs[r];
      // Superscript marker runs are short; the label must sit inside the
      // link's rect to be the marker for THIS link.
      if (run.len == 0 || run.len > 4 || (run.styleFlags & freeink::book::StyleSuperscript) == 0) continue;
      const bool withinY = run.baselineY >= link.y && run.baselineY <= link.y + static_cast<int32_t>(link.height);
      const bool withinX = run.x >= link.x && run.x < link.x + static_cast<int32_t>(link.width);
      if (withinY && withinX) {
        number.assign(run.text, run.len);
        break;
      }
    }
    if (number.empty()) {
      char ordinal[8];
      snprintf(ordinal, sizeof(ordinal), "%u", static_cast<unsigned>(l + 1));
      number = ordinal;
    }
    strncpy(entry.number, number.c_str(), FOOTNOTE_NUMBER_LEN - 1);
    entry.number[FOOTNOTE_NUMBER_LEN - 1] = '\0';
    strncpy(entry.href, href.c_str(), FOOTNOTE_HREF_LEN - 1);
    entry.href[FOOTNOTE_HREF_LEN - 1] = '\0';
    currentPageFootnotes.push_back(entry);
  }
}

void EpubReaderActivity::ttfDisplayGrayBase() {
  // GRS: base-entry diagnostic — request mode, cadence state, and driver state flags.
  // The overlay base path is used for all TTF text AA pages: the owner's research
  // recommends Fast base + sparse overlay masks + stock short AA waveform. Direct
  // combined (absolute planes + quality bank) is reserved for full-screen images
  // and sleep covers; selecting it for text pages causes the jarring quality
  // waveform flash the research describes.
  //
  // Cadence knob: SETTINGS.refreshFrequency controls how many consecutive AA
  // pages run before the deliberate Half scrub. The first AA page and any
  // driver-invalid state still take a real B/W activation.
  if (pagesUntilFullRefresh <= 1) {
    LOG_DBG("GRS", "ttfDisplayGrayBase: cadence=full (pages<=1) requesting HALF+precondition");
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    renderer.preconditionGrayscale();
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    return;
  }
  LOG_DBG("GRS", "ttfDisplayGrayBase: cadence=fast pagesUntilFullRefresh=%d requesting overlay FAST base",
          pagesUntilFullRefresh);
  // Overlay mode: uses Fast base transition when the driver state allows
  // (_grayRefreshedOnce && _oldPlaneValid && !_needFullClear), otherwise falls
  // back to a real B/W display(). Always uses overlay masks — never Direct
  // combined for text pages (see SDK Uc8279X4Driver::displayGrayscaleBase).
  renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
  pagesUntilFullRefresh--;
}

void EpubReaderActivity::renderTtfGrayStrips(const freeink::book::Page& page, const freeink::book::LayoutParams& params,
                                             size_t scratchMark) {
  (void)scratchMark;  // released by the caller after this walk
#ifdef BOOK_PROFILE
  const unsigned long renderStartMs = millis();
#endif
  constexpr int STRIP_ROWS = 80;
  const int gh = renderer.getDisplayHeight();
  const int gwBytes = renderer.getDisplayWidthBytes();
  // Allocate both plane bands before mutating refresh state: if either
  // allocation fails, the B/W fallback runs from untouched cadence state.
  auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
  auto msbScratch = scratch ? makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS) : nullptr;
  if (!scratch || !msbScratch) {
    LOG_ERR("ERS", "OOM: TTF plane bands (%d bytes); displaying B/W page", gwBytes * STRIP_ROWS);
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, /*async=*/false);
    return;
  }

  ttfDisplayGrayBase();
#ifdef BOOK_PROFILE
  const unsigned long tBaseMs = millis();
#endif
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_DUAL);
  for (int y = 0; y < gh; y += STRIP_ROWS) {
    const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
    renderer.beginStripTarget(scratch.get(), y, rows, msbScratch.get());
    renderer.clearScreen(0x00);
    freeink::book::PagePaint::paintPlanes(page, *static_cast<freeink::book::FontChain*>(params.font), renderer,
                                          params.characterSpacingPx, params.wordSpacingPx);
    renderer.endStripTarget();
    renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
    renderer.writeGrayscalePlaneStrip(false, msbScratch.get(), y, rows);
  }
#ifdef BOOK_PROFILE
  const unsigned long tPlanesMs = millis();
#endif
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.displayGrayBuffer();
#ifdef BOOK_PROFILE
  const unsigned long tGrayMs = millis();
#endif
  renderer.cleanupGrayscaleWithFrameBuffer();
#ifdef BOOK_PROFILE
  LOG_DBG("PROF", "phase=ttf_gray transport=strips bands=%d base=%lums planes=%lums gray=%lums cleanup=%lums",
          (gh + STRIP_ROWS - 1) / STRIP_ROWS, tBaseMs - renderStartMs, tPlanesMs - tBaseMs, tGrayMs - tPlanesMs,
          millis() - tGrayMs);
#endif
}

void EpubReaderActivity::renderTtfGrayFullFrame(const freeink::book::Page& page,
                                                const freeink::book::LayoutParams& params, size_t scratchMark) {
  // Full-frame fallback for non-strip panels (UC8279 X4: Overlay gray with
  // stripUploads=false). The dual-plane walk targets two complete plane
  // buffers — the same bits the strip path produces, one DUAL walk instead
  // of bands — submitted through the driver's full-plane upload. The panel's
  // advertised restrictions are preserved: no strip flag is flipped.
  (void)scratchMark;  // released by the caller after this walk
#ifdef BOOK_PROFILE
  const unsigned long renderStartMs = millis();
#endif
  const int gh = renderer.getDisplayHeight();
  const size_t planeBytes = static_cast<size_t>(renderer.getDisplayWidthBytes()) * static_cast<size_t>(gh);
  // Size guard before the pool allocation (CWE-400 discipline); panel
  // geometry bounds this (800x480 needs 48000/plane), the check keeps a
  // future panel change honest.
  constexpr size_t kMaxFullFrameGrayBytes = 128 * 1024;
#if defined(BOARD_HAS_PSRAM)
  // PSRAM builds: two 48KB planes are trivial for the 8MB pool.
  const bool planesFit = planeBytes > 0 && planeBytes <= kMaxFullFrameGrayBytes;
#else
  // DRAM tier: the legacy reader's nontiled-dual heap gate.
  const bool planesFit = planeBytes > 0 && planeBytes <= kMaxFullFrameGrayBytes &&
                         ESP.getFreeHeap() >= planeBytes + 60000 && ESP.getMaxAllocHeap() >= planeBytes + 16 * 1024;
#endif
  auto lsbPlane = planesFit ? poolMakeBytes(planeBytes) : PoolBytes{};
  auto msbPlane = lsbPlane ? poolMakeBytes(planeBytes) : PoolBytes{};
  if (!lsbPlane || !msbPlane) {
    LOG_ERR("ERS", "OOM: TTF full-frame gray planes (%u bytes); displaying B/W page", (unsigned)planeBytes);
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, /*async=*/false);
    return;
  }

  ttfDisplayGrayBase();
#ifdef BOOK_PROFILE
  const unsigned long tBaseMs = millis();
#endif
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_DUAL);
  // One "full-frame strip" (origin 0, panel rows): drawGrayDualPixel lands
  // each tone's plane bits in the two private buffers, orientation-aware.
  renderer.beginStripTarget(lsbPlane.get(), 0, gh, msbPlane.get());
  renderer.clearScreen(0x00);
  freeink::book::PagePaint::paintPlanes(page, *static_cast<freeink::book::FontChain*>(params.font), renderer,
                                        params.characterSpacingPx, params.wordSpacingPx);
  renderer.endStripTarget();
#ifdef BOOK_PROFILE
  const unsigned long tPlanesMs = millis();
#endif
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.copyGrayscaleLsbBuffers(lsbPlane.get());
  renderer.copyGrayscaleMsbBuffers(msbPlane.get());
  renderer.displayGrayBuffer();
#ifdef BOOK_PROFILE
  const unsigned long tGrayMs = millis();
#endif
  renderer.cleanupGrayscaleWithFrameBuffer();
#ifdef BOOK_PROFILE
  LOG_DBG("PROF", "phase=ttf_gray transport=full_frame plane_bytes=%u base=%lums planes=%lums gray=%lums cleanup=%lums",
          (unsigned)planeBytes, tBaseMs - renderStartMs, tPlanesMs - tBaseMs, tGrayMs - tPlanesMs, millis() - tGrayMs);
#endif
}

void EpubReaderActivity::paintTtfPage(const freeink::book::Page& page, void* font, const int16_t characterSpacingPx,
                                      const int16_t wordSpacingPx) {
  if (!ttf_) return;
  paintCapturedPage(page, font, renderer, *ttf_, characterSpacingPx, wordSpacingPx);
}

void EpubReaderActivity::renderTtfSelectorPage(void* ctx, GfxRenderer& renderer) {
  auto* self = static_cast<EpubReaderActivity*>(ctx);
  if (!self->ttf_) return;
  const size_t scratchMark = self->ttf_->scratch().mark();
  freeink::book::Page page{};
  if (!self->ttf_->readPage(static_cast<uint16_t>(self->currentSpineIndex), static_cast<uint16_t>(self->ttfPage),
                            &page)) {
    self->ttf_->scratch().release(scratchMark);
    LOG_ERR("ERS", "TTF selector page read failed (spine %d page %d)", self->currentSpineIndex, self->ttfPage);
    return;
  }
  freeink::book::LayoutParams params;
  self->ttf_->makeLayoutParams(renderer, params, false);
  if (params.font != nullptr) {
    self->paintTtfPage(page, params.font, params.characterSpacingPx, params.wordSpacingPx);
  }
  self->ttf_->scratch().release(scratchMark);
}

void EpubReaderActivity::ttfBackgroundBuildTick() {
  if (!ttf_) return;
  if (ttf_->sessionActive()) {
    const uint16_t spine = ttf_->sessionSpine();
    const uint16_t before = ttf_->availablePageCount(spine);
    const uint32_t tickStart = millis();
    freeink::book::BookStatus st = freeink::book::BookStatus::Ok;
    if (freeink::book::pumpChapterChunk(*ttf_, spine, BACKGROUND_BUILD_PAGES_PER_TICK, &st) ==
        freeink::book::ChapterPump::Failed) {
      LOG_ERR("ERS", "Background TTF build failed: %s", bookStatusName(st));
      ttfPrefetchActive = false;
      return;
    }
    // Per-tick progress (soak finding #6): the 2-minute silent resume was
    // undiagnosable — every pump tick now names the spine, pages done, and
    // the tick cost so a soak log separates progress from a hang.
    const uint16_t after = ttf_->availablePageCount(spine);
    LOG_INF("ERS", "bg build spine %u: %u pages (%+d, %lu ms)", spine, after,
            static_cast<int32_t>(after) - static_cast<int32_t>(before),
            static_cast<unsigned long>(millis() - tickStart));
    powerManager.pokeNormalSpeed();
    if (!ttf_->sessionActive()) {  // finished (or failed) in this tick
      if (spine == static_cast<uint16_t>(currentSpineIndex)) {
        ttf_->openChapterCache(spine, ttfGeneration);
        ttfPageCount = ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex));
        requestUpdate();
      } else {
        ttfPrefetchActive = false;
        // The committed cache replaced the partial file the prefetch reader
        // describes — drop it so the next tick reopens the complete cache
        // instead of restarting a rebuild from the stale partial.
        ttf_->dropPrefetch();
      }
    }
    return;
  }
  // Partial cache below the read watermark: resume the extension build.
  if (ttf_->cacheReady() && ttf_->cachePartial() && ttfSpine == currentSpineIndex &&
      !ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex)) &&
      ttfPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(ttfPageCount)) {
    // Single-writer: yield the extension only while a LIVE worker is actually
    // building this spine (or the claim is still fresh — the worker may not
    // have reached it yet). A consumed-but-failed claim (the worker moved on to
    // the look-ahead spine) or a self-exited worker must not park the margin
    // extension forever: fall through and take over.
    const bool workerOnClaimSpine = fibpWorker_ != nullptr && fibpWorker_->active() &&
                                    fibpWorker_->buildingSpine() == static_cast<uint16_t>(currentSpineIndex);
    const bool claimStillFresh = millis() - fibpResumeRequestMs_ <= kFibpResumeClaimGraceMs;
    if (freeink::book::fibp::readerYieldsExtension(
            fibpResumeClaimedSpine_ == currentSpineIndex && (workerOnClaimSpine || claimStillFresh),
            static_cast<uint16_t>(ttfPage < 0 ? 0 : ttfPage), static_cast<uint16_t>(ttfPageCount))) {
      return;
    }
    // The reader needs pages NOW: take the claim back.
    if (fibpResumeClaimedSpine_ == currentSpineIndex) {
      LOG_INF("ERS", "Margin takeover: spine %d", currentSpineIndex);
      stopFibpWorker();
      fibpResumeClaimedSpine_ = -1;
      fibpResumeSeenBuilding_ = false;
    }
    freeink::book::LayoutParams params;
    ttf_->makeLayoutParams(renderer, params, automaticPageTurnActive);
    if (params.font == nullptr) return;  // no reader font chain: cannot lay out
    if (freeink::book::ensureChapterSession(*ttf_, static_cast<uint16_t>(currentSpineIndex), params, ttfGeneration) ==
        freeink::book::BookStatus::Ok) {
      freeink::book::BookStatus st = freeink::book::BookStatus::Ok;
      const uint16_t before = ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex));
      const uint32_t tickStart = millis();
      // One page per tick while extending a partial: a press waits at most
      // one page build between input polls (soak finding #6).
      const freeink::book::ChapterPump pump =
          freeink::book::pumpChapterChunk(*ttf_, static_cast<uint16_t>(currentSpineIndex), 1, &st);
      const uint16_t after = ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex));
      LOG_INF("ERS", "resume build spine %d: %u pages (%+d, %lu ms)", currentSpineIndex, after,
              static_cast<int32_t>(after) - static_cast<int32_t>(before),
              static_cast<unsigned long>(millis() - tickStart));
      powerManager.pokeNormalSpeed();
      if (pump == freeink::book::ChapterPump::Complete) {
        ttf_->openChapterCache(static_cast<uint16_t>(currentSpineIndex), ttfGeneration);
      }
      ttfPageCount = ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex));
      if (st != freeink::book::BookStatus::Ok) {
        LOG_DBG("ERS", "Partial TTF resume step failed: %s", bookStatusName(st));
      }
    }
  }
}

void EpubReaderActivity::updateFibpWorker(const uint32_t generation, const freeink::book::LayoutParams& params) {
  if (!ttf_ || !epub) return;
  const char* family = SETTINGS.ttfFontFamilyName;
  if (fibpWorker_ != nullptr && fibpBegun_ && strncmp(fibpFamily_, family, sizeof(fibpFamily_)) != 0) {
    // Family swap: the worker's face set must be rebuilt for the new bytes.
    stopFibpWorker();
  }
  if (fibpWorker_ == nullptr) {
    fibpWorker_ = makeUniqueNoThrow<freeink::book::FibpPrefetchWorker>();
    if (fibpWorker_ == nullptr) return;
  }
  if (!fibpBegun_) {
    // Single-writer: a legacy prefetch/sync session in flight owns its spine
    // — wait for it to drain before spawning the worker.
    if (ttf_->sessionActive()) {
      LOG_DBG("ERS", "Worker begin deferred: reader session active");
      return;
    }
    freeink::book::FibpPrefetchWorker::BeginContext ctx;
    snprintf(ctx.epubPath, sizeof(ctx.epubPath), "%s", bookPath.c_str());
    snprintf(ctx.cacheDir, sizeof(ctx.cacheDir), "%s/ficache", epub->getCachePath().c_str());
    snprintf(ctx.familyName, sizeof(ctx.familyName), "%s", family);
    ctx.spineCount = static_cast<uint16_t>(epub->getSpineItemsCount());
    ctx.generation = generation;
    ctx.layout = params;  // scalar fields used; the worker re-binds pointers
    fibpBegun_ = fibpWorker_->begin(ctx);
    if (fibpBegun_) {
      snprintf(fibpFamily_, sizeof(fibpFamily_), "%s", family);
      fibpNotifiedGen_ = generation;
    }
  } else if (generation != fibpNotifiedGen_) {
    fibpWorker_->notifyGeneration(generation, params);
    fibpNotifiedGen_ = generation;
  }
  if (fibpBegun_) {
    // Raw progress only — the worker applies the 10%-remaining policy. This
    // runs on every reader render (page turns included), so a mid-chapter
    // threshold crossing fires on the turn that crosses it, and a chapter
    // entered already past the threshold triggers immediately. The legacy
    // Section path has no FIBP worker (ttf_ is null there → inert stub), so
    // only the TTF mirrors apply here. ttfPage's -1 chapter-transition
    // sentinel (page not yet laid out) clamps to 0 — casting it raw would
    // read as 65535 and fire the trigger at ~0% progress.
    // Skip while the mirrors still belong to the previous spine (the
    // chapter-transition block has not refreshed them yet): reporting the old
    // (page, pageCount) against the new spine would fire the 10%-remaining
    // trigger at ~0% progress. The next render after the transition reports
    // the refreshed mirrors.
    if (ttfSpine != currentSpineIndex) return;
    const uint16_t page = ttfPage >= 0 ? static_cast<uint16_t>(ttfPage) : 0;
    // ttfPageCount mirrors availablePageCount(), which answers the live writer's
    // pageCount while a session is open or a partial cache is being extended —
    // a still-climbing watermark, not the chapter's settled length. The
    // short-chapter branch must not read that as "this chapter is short" (it
    // would fire the prefetch during the very first cold-start build of a long
    // chapter, the premature-indexing case the percentage rule prevents), so
    // report the length as final only when no build owns the chapter.
    const bool lengthFinal =
        !ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex)) && !(ttf_->cacheReady() && ttf_->cachePartial());
    fibpWorker_->notifyChapterProgress(static_cast<uint16_t>(currentSpineIndex), page,
                                       static_cast<uint16_t>(ttfPageCount), lengthFinal);
  }
}

void EpubReaderActivity::stopFibpWorker() {
  if (fibpWorker_ == nullptr) return;
  if (fibpWorker_->cancel()) {
    fibpWorker_.reset();
  } else {
    // Wedged join: the worker object owns state a live task still touches —
    // release ownership WITHOUT destroying (logged leak beats use-after-free).
    LOG_ERR("ERS", "FIBP worker did not stop — releasing without destroy");
    (void)fibpWorker_.release();
  }
  fibpBegun_ = false;
  fibpFamily_[0] = '\0';
  fibpDeferred_ = false;
  fibpDeferParkLatched_ = false;
  fibpResumeClaimedSpine_ = -1;
  fibpResumeSeenBuilding_ = false;
}

void EpubReaderActivity::ttfPrefetchTick() {
  if (!ttf_ || !epub || buildHeapPaused) return;
  // Single-writer: while the FIBP worker feature is active (begun) the worker
  // owns next-chapter prefetching — including its 10%-remaining trigger — so
  // the legacy in-activity build must not index ahead of it; it stays for
  // inert builds only (stub / begin failure).
  if (fibpWorker_ != nullptr && fibpBegun_) return;
  if (ttf_->sessionActive()) return;  // the current chapter's build wins

  const int nextSpine = currentSpineIndex + 1;
  if (nextSpine >= epub->getSpineItemsCount()) return;

  // Warm the next chapter's cache-reader index (cheap; serves nothing else).
  if (ttfGenerationValid) {
    ttf_->openPrefetch(static_cast<uint16_t>(nextSpine), ttfGeneration);
  }

  if (ttf_->sessionFor(static_cast<uint16_t>(nextSpine))) {
    if (ttf_->sessionActive()) {
      freeink::book::pumpChapterChunk(*ttf_, static_cast<uint16_t>(nextSpine), BACKGROUND_BUILD_PAGES_PER_TICK);
    } else {
      ttfPrefetchActive = false;
      ttf_->dropPrefetch();
      ttf_->openPrefetch(static_cast<uint16_t>(nextSpine), ttfGeneration);
    }
    return;
  }

  // Start building the next chapter only while the current one is fully
  // served from its cache AND the next spine has no complete cache yet —
  // otherwise a warm prefetch reader would be rewritten on every idle tick.
  // A suspended partial next-chapter cache still earns a build (§3.5).
  if (!ttfPrefetchActive && !(ttf_->prefetchFor(nextSpine) && !ttf_->prefetchPartial()) && ttf_->cacheReady() &&
      !ttf_->cachePartial() && !ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex))) {
    freeink::book::LayoutParams params;
    ttf_->makeLayoutParams(renderer, params, automaticPageTurnActive);
    if (params.font == nullptr) return;  // no reader font chain: cannot lay out
    if (freeink::book::ensureChapterSession(*ttf_, static_cast<uint16_t>(nextSpine), params, ttfGeneration) ==
        freeink::book::BookStatus::Ok) {
      freeink::book::pumpChapterChunk(*ttf_, static_cast<uint16_t>(nextSpine), BACKGROUND_BUILD_PAGES_PER_TICK);
      ttfPrefetchActive = true;
    }
  }
}

bool EpubReaderActivity::ttfPageTurn(const bool isForwardTurn) {
  if (!ttf_ || !epub) return false;
  // §5.4: Superseding navigation cancels the restore. A navigation press
  // processed between restore passes clears the pending restore and takes
  // effect as a normal navigation — the anchor no longer describes where the
  // user wants to be; their explicit navigation wins. The stored target is
  // cleared WITH the implicit source latches on every path that actually
  // navigates (below), never at entry: a backward-at-start no-op returns
  // false without navigating, and Reflow has no raw latch to re-derive from,
  // so an entry-level clear would lose the restore and drop the next render
  // at the chapter start. (reader-position-and-reindex.md: a superseding
  // navigation must cancel a pending one-shot — every cancel site is a
  // success path, so the fast/slow render branches cannot drift.)
  // The implicit source latches are cleared below on every path that actually
  // navigates (consume-on-paint leaves them armed until a paint, so the next
  // render would otherwise re-derive the cancelled restore and override the
  // turn). A backward-at-start no-op returns false WITHOUT clearing them: the
  // turn changed nothing, so a pending restore must still re-derive on the
  // next render. Explicit latches (offset jump / anchor) keep the current
  // behavior — they still apply after the turn.
  // Input-first rule (soak finding #5): a turn aborts any mid-pump prerender
  // immediately — the framebuffer holds a PARTIAL page that must neither be
  // consumed nor snapshot. The press is served within one slice of landing.
  if (ttfPreRendered.pumping) {
    ttfPendingPreRender = false;
    ttfInvalidatePreRender("input pending");
  }
  // Navigation invalidates the displayed frame until the next successful render.
  ttfFrameRenderComplete.store(false, std::memory_order_release);

#ifdef READING_STATS_ENABLED
  uint32_t dwellSeconds = 0;
  const bool haveDwell = currentPageReadingSecondsForStats(dwellSeconds);
  if (SETTINGS.shouldTrackReadingStats()) {
    recordCurrentPageReadingTime();
    if (isForwardTurn && haveDwell) {
      recordForwardPagePaceSample(dwellSeconds, currentPageWordsOnPage);
      if (stats.totalPagesTurned < UINT32_MAX) stats.totalPagesTurned++;
      if (globalStats.totalPagesTurned < UINT32_MAX) globalStats.totalPagesTurned++;
      if (sessionPageTurns < UINT16_MAX) sessionPageTurns++;
    }
  }
#endif

  // P4 fast path: the framebuffer already holds this next page's content.
  // Advance state here on the loop task, then hand the display commit to
  // renderBookTtf via ttfUsePreRenderedBuffer — all display work (status
  // bar, gray planes, flush) stays on the render task under its RenderLock.
  // Validation is spine+page exact PLUS the full validity axes (generation,
  // orientation, raster mode); anything else falls to the slow path, with
  // the miss reason logged once per distinct reason for soak triage.
  // A forward turn abandons any pending back-step: ttfResolveTargetPage()
  // consumes ttfRestoreLastPage ahead of every other target and can return
  // without clearing it when needFullBuild is set, so a flag left by an
  // earlier back-turn would silently redirect this forward turn to the
  // previous chapter's last page. Cleared here so it covers the prerender
  // fast path and the slow branch alike.
  if (isForwardTurn) ttfRestoreLastPage = false;

  if (isForwardTurn && ttfPreRendered.ready && ttfPage + 1 < static_cast<int>(ttfPageCount)) {
    if (ttfPreRendered.spineIndex != currentSpineIndex) {
      ttfLogPrerenderMiss("spine changed");
    } else if (ttfPreRendered.pageIndex != ttfPage + 1) {
      ttfLogPrerenderMiss("target page mismatch");
    } else if (ttfPreRendered.generation != ttfGeneration) {
      ttfLogPrerenderMiss("stale generation");
    } else if (ttfPreRendered.orientation != static_cast<uint8_t>(renderer.getOrientation())) {
      ttfLogPrerenderMiss("orientation changed");
    } else if (ttfPreRendered.monochrome != ttfEffectiveMonochromeSnapshot()) {
      ttfLogPrerenderMiss("raster mode changed");
    } else {
      ttfPage = ttfPreRendered.pageIndex;
      // ready stays set: the display pass revalidates it (spine/page/gen/
      // orientation/raster) and consumes the snapshot after the checks —
      // invalidating here would kill the fast pass's first gate (kody:
      // P4 never paid off) and block ttfSchedulePreRender from re-arming
      // the next page's prerender.
      ttfUsePreRenderedBuffer = true;
      lastPageTurnTime = millis();
      ttfHasSavedPosition = false;  // navigation succeeded: implicit latches die with the target
      pendingRestoreTarget_ = {};
#ifdef READING_STATS_ENABLED
      pageShownAtMs = millis();
#endif
      logMemAt("page_turn");
      return true;
    }
  }

  if (isForwardTurn) {
    const bool partialCurrent = ttf_->cacheReady() && ttf_->cachePartial() && ttfSpine == currentSpineIndex;
    if (ttfPage + 1 < static_cast<int>(ttfPageCount) ||
        (ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex)) && ttf_->sessionActive())) {
      ttfPage++;
      lastPageTurnTime = millis();
    } else if (partialCurrent) {
      // A partial FIBP is extendable, not the end of the chapter. Leave the
      // spine in place and let the next render pass resume the build.
      ttfPage = static_cast<int>(ttfPageCount);
      lastPageTurnTime = millis();
    } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex++;
      ttfPage = 0;
      // Drops the chapter-scoped restore state, including any
      // ttfRestoreLastPage left by a previous back-step.
      clearDeferredReposition();
      lastPageTurnTime = millis();
    } else {
      currentSpineIndex = epub->getSpineItemsCount();
      // Same terminal-record guarantee as the legacy crossings: nothing past
      // here renders a section page, so this save is the book's done marker.
      if (!saveFinishedProgress()) LOG_ERR("ERS", "Failed to save end-of-book progress");
      lastPageTurnTime = millis();
    }
  } else {
    if (ttfPage > 0) {
      ttfPage--;
      lastPageTurnTime = millis();
    } else if (currentSpineIndex > 0) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex--;
      ttfPage = 0;
      // Clear the chapter-scoped restore state first, then arm the back-step:
      // clearDeferredReposition() resets ttfRestoreLastPage, and the flag is
      // what tells the next resolve to open the previous chapter's last page
      // rather than its start.
      clearDeferredReposition();
      ttfRestoreLastPage = true;
      lastPageTurnTime = millis();
    } else {
      return false;
    }
  }

#ifdef READING_STATS_ENABLED
  pageShownAtMs = millis();
#endif
  logMemAt("page_turn");
  ttfHasSavedPosition = false;  // navigation succeeded: implicit latches die with the target
  pendingRestoreTarget_ = {};
  return true;
}

#endif  // CROSSPOINT_TTF_READER

bool EpubReaderActivity::saveFinishedProgress() {
  // Terminal record: spine == spineCount means the book is read past its
  // last section. The restore paths reject that spine and open fresh, so
  // reopen of a finished book starts at the beginning; KOReader/sleep sync
  // report a finished position instead of the last pre-finish page. Latching
  // eobTerminalProgressSaved on success lets the EOB-entry save skip once any
  // writer (crossing or prompt or first EOB render) has put the record down.
  if (!epub) return false;
  const bool saved =
      progressManager.saveNow(epub->getCachePath().c_str(), /*spineIndex=*/epub->getSpineItemsCount(), 0, 0, false, 0);
  if (saved) eobTerminalProgressSaved = true;
  return saved;
}

void EpubReaderActivity::onEndOfBookRendered() {
  automaticPageTurnActive = false;
  // The EOB menu may show without any page-turn crossing having run: persist
  // the terminal record the moment the screen is visible, once per session
  // (crossing saves latch eobTerminalProgressSaved, so no duplicate write).
  if (!eobTerminalProgressSaved) {
    if (!saveFinishedProgress()) LOG_ERR("ERS", "Failed to save end-of-book progress");
  }
  if (pendingSyncSaveError) {
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  }
}

bool EpubReaderActivity::applyDeferredReposition() {
  if ((!cachedVisibleTextOffset.has_value() && cachedChapterTotalPageCount == 0) || !section || section->isBuilding()) {
    return false;
  }
  bool changed = false;
  if (currentSpineIndex == cachedSpineIndex) {
    int newPage = section->currentPage;
    bool mappedOffset = false;
    if (cachedVisibleTextOffset.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*cachedVisibleTextOffset)) {
        newPage = *offsetPage;
        mappedOffset = true;
      }
    }
    if (!mappedOffset && cachedChapterTotalPageCount > 0 && section->pageCount != cachedChapterTotalPageCount) {
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
      newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
    }
    if (newPage < 0) newPage = 0;
    if (section->pageCount > 0 && newPage >= static_cast<int>(section->pageCount)) {
      newPage = section->pageCount - 1;
    }
    if (newPage != section->currentPage) {
      section->currentPage = newPage;
      changed = true;
    }
  }
  clearDeferredReposition();
  return changed;
}

void EpubReaderActivity::clearDeferredReposition() {
  cachedChapterTotalPageCount = 0;
  cachedVisibleTextOffset.reset();
#if defined(CROSSPOINT_TTF_READER)
  // Every spine transition and deferred reposition lands here, so this is where
  // the chapter-scoped restore state dies. All values describe the chapter
  // being left: pendingRestoreTarget_ is the unified funnel input (CharOffset
  // from reflow via ttfCurrentCharStart, saved position, offset jump, or
  // AnchorHash; see design §3.2/§4), ttfCurrentCharStart is a chapter-relative offset that renderBookTtf
  // retains, and ttfRestoreLastPage sends the reader to the previous chapter's
  // end, which is only meaningful when stepping back into that chapter. Carrying
  // any of them into a different spine resolves against the wrong chapter — the
  // position loss #195 describes. Saved-position fields (ttfSaved*) are NOT
  // cleared here — they are consumed by the funnel when resolved or unmappable,
  // not by the chokepoint (they describe the RECORD, consumed once).
  pendingRestoreTarget_ = {};
  ttfCurrentCharStart = 0;
  ttfCurrentCharEnd = 0;
  ttfRestoreLastPage = false;
#endif
}

void EpubReaderActivity::rememberCurrentContentOffset() {
  cachedVisibleTextOffset.reset();
  if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
    cachedVisibleTextOffset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(section->currentPage));
  }
}

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  const int fontId = SETTINGS.getReaderFontId();
  ImageBlock::clearRenderFailures();

  struct PxcSlotGuard {
    ~PxcSlotGuard() { ImageBlock::releaseRenderCache(); }
  } pxcSlotGuard;

  auto* fcm = renderer.getFontCacheManager();
  // BOOK_PROFILE: reset SD font overflow stats for this page turn.
  // The summary is logged at the end of the async display overlap block
  // below (phase=font_overflow). CROSSPOINT_TTF_READER excludes it: no
  // SdCardFont exists there, so the resetStats definition is compiled out
  // (docs/design/2026-10-trim-cpfont-x4pro.md).
#if defined(BOOK_PROFILE) && !defined(CROSSPOINT_TTF_READER)
  for (auto& [sdFontId, sdFont] : renderer.getSdCardFonts()) {
    sdFont->resetStats();
  }
#endif
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  // Scan the status bar too: a CJK book/chapter title redirected to the SD
  // fallback font joins the page's single batch prewarm instead of triggering
  // its own SD pass after the scope ends.
  renderStatusBar();
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  // Black & White images: the page turns as plain B/W, images dither in the
  // B/W pass and text keeps its usual anti-aliasing.
  const bool imageGrayscale = SETTINGS.imageRendering != CrossPointSettings::IMAGES_DISPLAY_BW;
  struct DitherScope {
    GfxRenderer& renderer;
    ~DitherScope() { renderer.setDitherImagesInBw(false); }
  } ditherScope{renderer};
  renderer.setDitherImagesInBw(!imageGrayscale);
  const bool pageHasImages = imageGrayscale && page->hasImages();
  const bool pageHasImagesNeedingDecode = pageHasImages && page->hasImagesNeedingDecode();
  const bool manualRefreshPending = forcedRefreshPending;
  forcedRefreshPending = false;
  const bool cleanImageBasePending = manualRefreshPending || pagesUntilFullRefresh <= 1;
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  const bool needsTextGrayscale = !freeink::book::fontLoader.effectiveMonochrome();
#else
  const bool needsTextGrayscale = SETTINGS.textRenderMode == CrossPointSettings::TEXT_RENDER_SMOOTH;
#endif
  const bool needsAnyGrayscale = needsTextGrayscale || (pageHasImages && !BoardConfig::isEegoA4());
  const bool absoluteImageGrayscale = pageHasImages && !gpio.deviceIsX3() &&
                                      display.getController() == HalDisplay::Controller::UC8279 &&
                                      renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported();
  const auto grayscale = renderer.grayscaleCapabilities(absoluteImageGrayscale ? HalDisplay::GrayscaleMode::Absolute
                                                                               : HalDisplay::GrayscaleMode::Overlay);
  const bool tiledGrayscale = needsAnyGrayscale && grayscale.stripUploads;
  // Paper Mono only (no other panel combines): defer the B/W base activation so
  // the gray planes join it in a single waveform. Displaying the base
  // separately makes the gray pass re-drive the whole text body — a visible
  // flash on every AA page.
  const bool combinedGrayscaleBase = tiledGrayscale && !pageHasImages && renderer.combinesGrayscaleBase();
  // supportsAsyncGrayscaleBase(): X3 must not use an async B/W refresh as the
  // grayscale base (upstream #3439); SSD1677-class panels keep the overlap.
  // The overlap window is also what lets the DUAL render walk fill both plane
  // buffers while the BW refresh is in flight (fork async-overlap work).
  const bool overlapRefresh = tiledGrayscale && renderer.supportsAsyncGrayscaleBase() && !pageHasImages;
  // GRAYSCALE_DUAL: one render walk flags both plane buffers. Gated to text
  // pages — image pages keep the two-pass walk until preserveImagePolarity
  // learns a dual target (design doc §8). Each path (tiled/nontiled) checks
  // its own buffer prerequisites before engaging. Absolute image pages
  // (upstream #3478) also always take the two-pass walk: dualPlane is false
  // when the page carries images, so the two feature paths never overlap.
  const bool dualPlane = !pageHasImages;
  auto renderGrayscalePass = [&]() {
    // A4 gray planes replace the full page, including its clipping highlights.
    if (BoardConfig::isEegoA4()) drawClippingHighlights(*page, fontId, orientedMarginTop, orientedMarginLeft);
    if (absoluteImageGrayscale || needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
    // A4 replaces the whole frame with its gray planes, including the status bar.
    if (absoluteImageGrayscale || BoardConfig::isEegoA4()) renderStatusBar();
  };

  if (pageHasImagesNeedingDecode) {
    page->renderWithImagePlaceholders(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    renderStatusBar();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    renderer.clearScreen();
  }

  drawClippingHighlights(*page, fontId, orientedMarginTop, orientedMarginLeft);
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  const auto tBwRender = millis();

  if (absoluteImageGrayscale) {
    const auto baseMode = cleanImageBasePending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
    if (!renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Absolute, baseMode)) {
      LOG_ERR("ERS", "Could not start absolute image page; displaying B/W");
      ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
      return;
    }
    LOG_DBG("ERS", "UC8279 image page: absolute quality waveform");
    pagesUntilFullRefresh = 1;
  } else if (pageHasImages) {
    // Image pages use one base refresh before the grayscale pass. FAST leaves
    // the panel receptive to the gray waveform; pending cleanup still honors
    // the scheduled/manual HALF refresh.
    renderer.displayBuffer(cleanImageBasePending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
    pagesUntilFullRefresh = 1;
  } else if (combinedGrayscaleBase) {
    // Stash the base without activating; displayGrayBuffer() below commits
    // base + grays as one waveform.
    ReaderUtils::displayBaseWithRefreshCycle(renderer, pagesUntilFullRefresh);
  } else if (needsAnyGrayscale) {
    if (pagesUntilFullRefresh <= 1) {
      // A cleanup refresh settles X3 correctly only when its grayscale
      // preconditioning waveform runs before the gray planes are written.
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      renderer.preconditionGrayscale();
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    } else if (overlapRefresh) {
      ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, /*async=*/true);
    } else {
      renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
      pagesUntilFullRefresh--;
    }
  } else {
    // Non-grayscale path: use async display when supported so we can overlap
    // the e-ink refresh (569-648ms) with background section builds and
    // next-chapter prefetch on the same core.
    const bool canAsyncDisplay = renderer.supportsAsyncRefresh() && !pageHasImages;
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, canAsyncDisplay);
    if (canAsyncDisplay) {
      // E-ink refresh is running. While it completes, run background work.
#ifdef BOOK_PROFILE
      const uint32_t overlapStartMs = millis();
      const uint8_t overlapCore = xPortGetCoreID();
#endif
      prefetchNextChapterDuringDisplay();
      if (section && section->isBuilding() && !section->isBuildComplete() && buildTickHeapGate()) {
        section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK);
      }
      renderer.waitRefreshComplete();
#ifdef BOOK_PROFILE
      LOG_DBG("PROF", "phase=async_display_overlap dur=%lums core=%d prefetch_active=%d build_active=%d",
              millis() - overlapStartMs, overlapCore, nextSectionPrefetch ? 1 : 0,
              section && section->isBuilding() ? 1 : 0);
#endif
    }
  }
  // BOOK_PROFILE: define a summary lambda for SD font overflow misses.
  // Each miss = SD read (~5-20ms); the goal of the PSRAM font cache feature
  // is to drive this to 0 for CJK books by raising MAX_PAGE_GLYPHS and expanding the ring.
  // Called at the end of renderContents() (after all gray paths) and before
  // the early return on storeBwBuffer() failure. Compiles on TTF boards too:
  // the body only touches header-inline accessors and TTF boards register no
  // SdCardFonts, so it logs zeros there (only resetStats() below is gated).
#ifdef BOOK_PROFILE
  auto logOverflowSummary = [&] {
    const auto& sdFonts = renderer.getSdCardFonts();
    uint32_t totalOverflowMisses = 0;
    uint32_t maxOverflowFill = 0;
    for (const auto& [sdFontId, sdFont] : sdFonts) {
      const auto& stats = sdFont->getStats();
      totalOverflowMisses += stats.overflowMisses;
      if (stats.overflowCountAtLog > maxOverflowFill) maxOverflowFill = stats.overflowCountAtLog;
    }
    LOG_DBG("PROF", "phase=font_overflow misses=%u max_fill=%u/%u", totalOverflowMisses, maxOverflowFill,
            SdCardFont::getOverflowCapacity());
  };
#endif
  const auto tDisplay = millis();
  // Path selection trace: which gray route this page takes and why.
  LOG_DBG("ERS", "Gray path: tiled=%d dual=%d textAA=%d images=%d overlap=%d", tiledGrayscale, dualPlane,
          needsTextGrayscale, pageHasImages, overlapRefresh);
  // Full-frame gray plane size for both dual paths (tiled async buffers and
  // nontiled dual): panel-stride bytes × full display height.
  const int dualHeight = renderer.getDisplayHeight();
  const size_t planeBytes = static_cast<size_t>(renderer.getDisplayWidthBytes()) * dualHeight;
  // Heap gate shared by every dual allocation (async overlap buffers, the
  // 8KB dual scratch, and the nontiled 2x48KB planes).
  constexpr size_t PLANE_BUF_HEADROOM = 60000;
  constexpr size_t PLANE_BUF_MAX_ALLOC_RESERVE = 16 * 1024;
  const auto planeBufFits = [planeBytes] {
    return ESP.getFreeHeap() >= planeBytes + PLANE_BUF_HEADROOM &&
           ESP.getMaxAllocHeap() >= planeBytes + PLANE_BUF_MAX_ALLOC_RESERVE;
  };

  if (tiledGrayscale) {
    constexpr int STRIP_ROWS = 80;
    const int gh = dualHeight;
    const int gwBytes = renderer.getDisplayWidthBytes();

    auto renderPlaneToBuffer = [&](const bool lsbPlane, uint8_t* buf) {
      renderer.setRenderMode(lsbPlane ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(buf + static_cast<size_t>(y) * gwBytes, y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
      }
    };

    auto lsbPlaneBuf = (overlapRefresh && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;
    auto msbPlaneBuf = (lsbPlaneBuf && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;

    if (lsbPlaneBuf) {
      if (msbPlaneBuf && dualPlane) {
        // Async overlap, both buffers live: one DUAL walk fills both planes
        // while the BW refresh is in flight.
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_DUAL);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(lsbPlaneBuf.get() + static_cast<size_t>(y) * gwBytes, y, rows,
                                    msbPlaneBuf.get() + static_cast<size_t>(y) * gwBytes);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
        }
      } else {
        renderPlaneToBuffer(true, lsbPlaneBuf.get());
        if (msbPlaneBuf) renderPlaneToBuffer(false, msbPlaneBuf.get());
      }
      const auto tGrayRender = millis();

      renderer.waitRefreshComplete();
      const auto tWait = millis();

      renderer.writeGrayscalePlaneStrip(true, lsbPlaneBuf.get(), 0, gh);
      if (msbPlaneBuf) {
        renderer.writeGrayscalePlaneStrip(false, msbPlaneBuf.get(), 0, gh);
      } else {
        renderPlaneToBuffer(false, lsbPlaneBuf.get());
        renderer.writeGrayscalePlaneStrip(false, lsbPlaneBuf.get(), 0, gh);
      }
      const auto tGrayWrite = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tEnd = millis();

      LOG_DBG("ERS",
              "Page render (tiled async): prewarm=%lums bw_render=%lums display=%lums gray_render=%lums "
              "wait=%lums gray_write=%lums gray_display=%lums cleanup=%lums total=%lums (planes buffered: %d)",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayRender - tDisplay, tWait - tGrayRender,
              tGrayWrite - tWait, tGrayDisplay - tGrayWrite, tEnd - tGrayDisplay, tEnd - t0, msbPlaneBuf ? 2 : 1);
    } else {
      // Dual needs both plane bands live for the whole walk; a shared 8 KB
      // scratch cannot hold two bands. Allocate the required scratch FIRST,
      // then attempt the optional MSB scratch — if memory only fits one band,
      // the two-pass fallback still runs instead of skipping AA.
      auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
      auto msbScratch = (dualPlane && scratch && planeBufFits())
                            ? makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS)
                            : nullptr;
      renderer.waitRefreshComplete();
      if (!scratch) {
        LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
        if (overlapRefresh || combinedGrayscaleBase) {
          // The BW refresh ran the shadow-free async path, so controller RAM's
          // differential baseline was never rebuilt. Even with AA skipped it must
          // be re-synced from the intact BW framebuffer, or the next differential
          // update diffs against stale contents. On the combined-base path the
          // base activation is still deferred; this cleanup commits it so the
          // page reaches the panel even without its grays.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }
      } else if (msbScratch) {
        // One DUAL walk per band: LSB bits land in `scratch`, MSB in `msbScratch`.
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_DUAL);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows, msbScratch.get());
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
          renderer.writeGrayscalePlaneStrip(false, msbScratch.get(), y, rows);
        }
        const auto tGrayBoth = millis();

        renderer.setRenderMode(GfxRenderer::BW);
        renderer.displayGrayBuffer();
        const auto tGrayDisplay = millis();

        renderer.cleanupGrayscaleWithFrameBuffer();
        const auto tCleanup = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render (tiled dual): prewarm=%lums bw_render=%lums display=%lums gray_both=%lums "
                "gray_display=%lums cleanup=%lums total=%lums",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayBoth - tDisplay,
                tGrayDisplay - tGrayBoth, tCleanup - tGrayDisplay, tEnd - t0);
      } else {
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
        }
        const auto tGrayLsb = millis();

        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
        }
        const auto tGrayMsb = millis();

        renderer.setRenderMode(GfxRenderer::BW);
        renderer.displayGrayBuffer();
        const auto tGrayDisplay = millis();

        renderer.cleanupGrayscaleWithFrameBuffer();
        const auto tCleanup = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
                "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
                tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
      }
    }
  } else {
    if (needsAnyGrayscale) {
      // Nontiled dual: the gray planes are rendered into two private
      // full-frame buffers and copied to the driver explicitly, so the BW
      // base can stay in the framebuffer for displayGrayBuffer(). Costs
      // 2x 48KB transient heap — gated by planeBufFits() like the async
      // overlap buffers; OOM falls back to the classic framebuffer passes.
      const bool nontiledDual = dualPlane && planeBufFits();
      auto lsbPlane = nontiledDual ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;
      auto msbPlane = (lsbPlane && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;
      if (!msbPlane) {
        lsbPlane = nullptr;
      }
      if (!nontiledDual || !msbPlane) {
        LOG_DBG("ERS", "Nontiled dual unavailable (heap); using framebuffer gray passes");
      }

      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
        if (absoluteImageGrayscale) renderer.setRenderMode(GfxRenderer::BW);
#ifdef BOOK_PROFILE
        logOverflowSummary();
#endif
        return;
      }
      const auto tBwStore = millis();

      if (lsbPlane && msbPlane) {
        // One DUAL walk over a "full-frame strip" (origin 0, panelHeight rows):
        // drawGrayDualPixel's rotate+clip then lands each tone's plane bits in
        // the two private buffers. The BW base stays in the framebuffer so
        // displayGrayBuffer() can stream it as usual.
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_DUAL);
        renderer.beginStripTarget(lsbPlane.get(), 0, dualHeight, msbPlane.get());
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        const auto tGrayBoth = millis();

        renderer.copyGrayscaleLsbBuffers(lsbPlane.get());
        renderer.copyGrayscaleMsbBuffers(msbPlane.get());
        const auto tGrayCopy = millis();

        renderer.displayGrayBuffer();
        const auto tGrayDisplay = millis();
        renderer.setRenderMode(GfxRenderer::BW);
        renderer.restoreBwBuffer();
        memSentinelCheck("ttf gray nontiled-dual");
        const auto tBwRestore = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render (nontiled dual): prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
                "gray_both=%lums gray_copy=%lums gray_display=%lums bw_restore=%lums total=%lums",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayBoth - tBwStore,
                tGrayCopy - tGrayBoth, tGrayDisplay - tGrayCopy, tBwRestore - tGrayDisplay, tEnd - t0);
      } else {
        // Two-pass fallback (also the absolute-image path — DUAL requires
        // dualPlane, which is false on image pages): absolute planes seed with
        // 0xFF so the base image's black/white bits survive in both planes.
        renderer.clearScreen(absoluteImageGrayscale ? 0xFF : 0x00);
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        renderGrayscalePass();
        renderer.copyGrayscaleLsbBuffers();
        const auto tGrayLsb = millis();

        renderer.clearScreen(absoluteImageGrayscale ? 0xFF : 0x00);
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        renderGrayscalePass();
        renderer.copyGrayscaleMsbBuffers();
        const auto tGrayMsb = millis();

        renderer.displayGrayBuffer();
        const auto tGrayDisplay = millis();
        renderer.setRenderMode(GfxRenderer::BW);
        renderer.restoreBwBuffer();
        const auto tBwRestore = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
                "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
                tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
      }
    } else {
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
#ifdef BOOK_PROFILE
  logOverflowSummary();
#endif
}

int EpubReaderActivity::clippingAtPoint(const Page& page, const int x, const int y) const {
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int fontId = SETTINGS.getReaderFontId();
  const uint32_t signature =
      readerRenderSpecSignature(SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight));
  uint16_t wordIndex = 0;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    const auto& block = line.getBlock();
    if (!block || !block->valid()) continue;
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      const char* text = block->wordText(i);
      if (!clippingText::hasVisibleText(text)) continue;
      const auto style = static_cast<EpdFontFamily::Style>(block->wordStyle(i) & ~EpdFontFamily::UNDERLINE);
      const int width = renderer.getTextAdvanceX(clippingWordFont(renderer, fontId, *block, i), text, style,
                                                 block->getBlockStyle().characterSpacing);
      if (width <= 0) continue;
      const uint16_t index = wordIndex++;
      const Rect rect =
          clippingWordRect(renderer, fontId, line, i, top + SETTINGS.screenMargin, left + SETTINGS.screenMargin, width);
      if (x < rect.x || x >= rect.x + rect.width || y < rect.y || y >= rect.y + rect.height) continue;
      const auto range = block->wordSourceRange(i);
      for (size_t j = 0; j < CLIPPINGS.clippingCount(); ++j) {
        if (clippingContainsWord(*CLIPPINGS.clippingAt(j), currentSpineIndex, section->currentPage, section->pageCount,
                                 signature, index, range.start, range.end))
          return static_cast<int>(j);
      }
    }
  }
  return -1;
}

void EpubReaderActivity::drawClippingHighlights(const Page& page, const int fontId, const int orientedMarginTop,
                                                const int orientedMarginLeft) const {
  if (!section || !CLIPPINGS.hasClippings() || section->currentPage < 0 || section->currentPage >= section->pageCount) {
    return;
  }

  size_t chapterClippings = 0;
  for (size_t i = 0; i < CLIPPINGS.clippingCount(); ++i) {
    if (CLIPPINGS.clippingAt(i)->spineIndex == currentSpineIndex) ++chapterClippings;
  }
  if (!chapterClippings) return;

  const auto started = millis();
  size_t highlightedWords = 0;
  const uint16_t currentPage = static_cast<uint16_t>(section->currentPage);
  const uint32_t layoutSignature =
      readerRenderSpecSignature(SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight));
  const auto isHighlighted = [&](const uint16_t wordIndex, const TextBlock::SourceRange range) {
    // ponytail: scan at most 256 C3 / 1024 S3 clippings per word; index by chapter if this becomes slow.
    for (size_t index = 0; index < CLIPPINGS.clippingCount(); ++index) {
      const Clipping& clipping = *CLIPPINGS.clippingAt(index);
      if (clippingContainsWord(clipping, static_cast<uint16_t>(currentSpineIndex), currentPage, section->pageCount,
                               layoutSignature, wordIndex, range.start, range.end))
        return true;
    }
    return false;
  };

  const auto fillHighlight = [&](const int x, const int y, const int width, const int height) {
    if (BoardConfig::isEegoA4() && renderer.getRenderMode() != GfxRenderer::BW) {
      // Gray planes start at zero and use set bits for ink, opposite to B/W.
      for (int py = y + (y & 1); py < y + height; py += 2) {
        for (int px = x + (x & 1); px < x + width; px += 2) renderer.drawPixel(px, py, false);
      }
    } else {
      renderer.fillRectDither(x, y, width, height, Color::LightGray);
    }
  };

  uint16_t pageWordIndex = 0;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    const auto& block = line.getBlock();
    if (!block || !block->valid()) continue;
    bool hasPreviousHighlight = false;
    int previousHighlightX = 0;
    int previousHighlightWidth = 0;
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      const char* text = block->wordText(i);
      if (!clippingText::hasVisibleText(text)) continue;
      const auto style = static_cast<EpdFontFamily::Style>(block->wordStyle(i) & ~EpdFontFamily::UNDERLINE);
      int width = renderer.getTextAdvanceX(clippingWordFont(renderer, fontId, *block, i), text, style,
                                           block->getBlockStyle().characterSpacing);
      if (width <= 0) continue;
      const uint16_t wordIndex = pageWordIndex++;
      if (!isHighlighted(wordIndex, block->wordSourceRange(i))) {
        hasPreviousHighlight = false;
        continue;
      }

      const Rect rect = clippingWordRect(renderer, fontId, line, i, orientedMarginTop, orientedMarginLeft, width);
      const int x = rect.x;
      const int y = rect.y;
      width = rect.width;
      if (width > 0) {
        const int highlightRight = x + width;
        const int previousHighlightRight = previousHighlightX + previousHighlightWidth;
        if (hasPreviousHighlight && previousHighlightRight < x) {
          fillHighlight(previousHighlightRight, y, x - previousHighlightRight, renderer.getLineHeight(fontId));
        } else if (hasPreviousHighlight && highlightRight < previousHighlightX) {
          fillHighlight(highlightRight, y, previousHighlightX - highlightRight, renderer.getLineHeight(fontId));
        }
        fillHighlight(x, y, width, renderer.getLineHeight(fontId));
        previousHighlightX = x;
        previousHighlightWidth = width;
        hasPreviousHighlight = true;
        ++highlightedWords;
      } else {
        hasPreviousHighlight = false;
      }
    }
  }
  LOG_DBG("CLIP", "Highlight pass: chapter_clips=%u highlighted_words=%u time=%lums",
          static_cast<unsigned>(chapterClippings), static_cast<unsigned>(highlightedWords), millis() - started);
}

void EpubReaderActivity::renderStatusBar() const {
  // Legacy path reads the Section; the TTF path keeps nextPageNumber /
  // cachedChapterTotalPageCount in sync as its mirrors.
  const int currentPage = section ? section->currentPage + 1 : nextPageNumber + 1;
  const float pageCount =
      section ? section->estimatedTotalPages() : (cachedChapterTotalPageCount > 0 ? cachedChapterTotalPageCount : 1);
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub ? (epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100) : 0;

  std::string title;
  int textYOffset = 0;
  const auto sb = SETTINGS.statusBarSpec();

  const auto note = linkNoteTitle.draw(sb.showsTitle(), bleturner::linkNote());
  if (note == bleturner::LinkNote::Connected) {
    const auto peer = bleturner::linked();
    char connected[64];
    snprintf(connected, sizeof(connected), tr(STR_BT_CONNECTED_TO), peer.name[0] != '\0' ? peer.name : peer.addr);
    title = connected;
  } else if (note == bleturner::LinkNote::LowMemory) {
    title = tr(STR_BT_LOW_MEMORY);
  } else if (note != bleturner::LinkNote::None) {
    title = note == bleturner::LinkNote::Connecting ? tr(STR_BT_CONNECTING) : tr(STR_BT_FAILED);
  } else if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }
  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    if (epub) {
      const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
      if (tocIndex != -1) {
        const auto tocItem = epub->getTocItem(tocIndex);
        title = tocItem.title;
      }
    }
  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub ? epub->getTitle() : "";
  }

#ifdef READING_STATS_ENABLED
  char chapterTimeLeftBuf[24];
  const char* chapterTimeLeft = nullptr;
  if (sb.showChapterTimeLeft) {
    if (SETTINGS.shouldTrackReadingStats() &&
        (section
#if defined(CROSSPOINT_TTF_READER)
         || ttf_
#endif
         ) &&
        (section ? section->estimatedTotalPages() > 0 : cachedChapterTotalPageCount > 0)) {
      const int chapterTotalPages =
          section ? static_cast<int>(section->estimatedTotalPages()) : cachedChapterTotalPageCount;
      const int chapterCurrentPage = section ? section->currentPage : nextPageNumber;
      const int pagesRemaining = std::max(0, chapterTotalPages - chapterCurrentPage - 1);
      auto timeLeft = estimateChapterTimeLeftSeconds(stats, globalStats, static_cast<uint16_t>(pagesRemaining),
                                                     currentPageWordsOnPage);
      if (timeLeft) {
        formatChapterTimeLeft(*timeLeft, chapterTimeLeftBuf, sizeof(chapterTimeLeftBuf));
        chapterTimeLeft = chapterTimeLeftBuf;
      } else {
        // Pace not learned yet (fewer than 50 global / 10 book page turns recorded).
        // Show a localized placeholder so the slot reads as active rather than broken.
        chapterTimeLeft = tr(STR_TIME_LEFT_UNAVAILABLE);
      }
    } else {
      // Tracking off but the element is enabled: keep the slot visible per the
      // user's chosen position so the setting is clearly doing something.
      chapterTimeLeft = tr(STR_TIME_LEFT_UNAVAILABLE);
    }
  }
#else
  const char* chapterTimeLeft = nullptr;
#endif

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked,
                    section ? section->isBuilding() : false, chapterTimeLeft);
}

// ---------------------------------------------------------------------------
// Toolbar reader menu
// ---------------------------------------------------------------------------

namespace {
constexpr StrId kTextRowNames[] = {StrId::STR_FONT, StrId::STR_FONT_SIZE, StrId::STR_LINE_SPACING,
                                   StrId::STR_PARA_ALIGNMENT, StrId::STR_FOCUS_READING};
constexpr StrId kAlignIds[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                               StrId::STR_BOOK_S_STYLE};
constexpr int kTextRowCount = static_cast<int>(std::size(kTextRowNames));
static_assert(std::size(kAlignIds) == CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT, "alignment labels");
}  // namespace

bool EpubReaderActivity::usesToolbarMenu() const {
  // Both board classes drive the same chrome: touch through the FreeInkUI tap
  // targets, buttons through the focused-tool pill and the panel cursor.
  return SETTINGS.readerMenuStyle == CrossPointSettings::READER_MENU_TOOLBAR;
}

std::string EpubReaderActivity::currentChapterTitle() const {
  if (!epub) return "";
  const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (tocIndex != -1) {
    return epub->getTocItem(tocIndex).title;
  }
  return tr(STR_UNNAMED);
}

std::string EpubReaderActivity::textRowName(int row) const {
  return row >= 0 && row < kTextRowCount ? I18N.get(kTextRowNames[row]) : "";
}

std::string EpubReaderActivity::textRowValue(int row) const {
  static constexpr StrId kFamily[] = {StrId::STR_ATKINSON_HN};
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    switch (row) {
      case 0:
        return SETTINGS.ttfFontFamilyName[0] != '\0' ? SETTINGS.ttfFontFamilyName : tr(STR_BUILTIN_FONT);
      case 1:
        return std::to_string(SETTINGS.ttfFontPointSize) + " pt";
      case 2:
        return I18N.get(LINE_SPACING_IDS[SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT]);
      case 3:
        return I18N.get(kAlignIds[SETTINGS.paragraphAlignment % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT]);
      default:
        return "";
    }
  }
#endif
  switch (row) {
    case 0:
      if (SETTINGS.sdFontFamilyName[0] != '\0') return SETTINGS.sdFontFamilyName;
      return I18N.get(kFamily[SETTINGS.fontFamily % CrossPointSettings::FONT_FAMILY_COUNT]);
    case 1:
      return std::to_string(SETTINGS.fontPointSize) + " pt";
    case 2:
      return I18N.get(LINE_SPACING_IDS[SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT]);
    case 3:
      return I18N.get(kAlignIds[SETTINGS.paragraphAlignment % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT]);
    default:
      return "";
  }
}

// Live apply: persist, re-paginate, and let renderBook() redraw the page with
// the open panel back on top -- the book itself is the preview.
void EpubReaderActivity::applyTextSettingLive() {
  applyReaderTextSettings();
  discardOverlayPage();  // the stored page is laid out with the old settings
  requestUpdate();
}

// Settings-style option pickers for the Text panel's enum rows. Every
// selection applies immediately to the page under the sheet.
#if defined(CROSSPOINT_TTF_READER)

// FontPreviewActivity host seam: the preview is about to switch families,
// whose next makeLayoutParams() reloads the loader's resident bytes. End the
// UI-fallback faces while the bytes they borrow are still valid (same
// sequence as applyReaderTextSettings()) and pause the FIBP worker whose own
// faces borrow the same bytes (issue #168 latent hazard); the next
// updateFibpWorker() re-begins it on the new family.
void EpubReaderActivity::onPreviewFontChanging() {
  if (fibpWorker_ != nullptr && fibpBegun_) stopFibpWorker();
#if CROSSPOINT_TTF_UI_FALLBACK
  freeink::book::ttfUiFallback.release(renderer);
#endif
}

// The Text panel's size/family rows push the full-screen FontPreviewActivity
// (design 2026-09-27): the captured current page fills the screen, every
// confirmed change re-lays it live, and the close contract routes through
// the reader's settings-driven clean reindex.
void EpubReaderActivity::openFontPreview() {
  if (!ttf_) return;
  // The preview's window bound is the displayed page's char end (recorded by
  // the last render pass next to ttfCurrentCharStart), so a smaller font in
  // the preview cannot pull later pages' text into the previewed span
  // (ADR 2026-10-14 amendment: bounded window capture).
  if (ttf_->sessionActive()) {
    // A live inline build owns the arenas quickLayoutPage needs. Suspend it
    // (the partial stays resumable) and reopen the current chapter cache so
    // the preview can re-lay the page at every change.
    ttf_->abortSession();
    ttf_->closeChapterCache();
    if (ttf_->openChapterCache(static_cast<uint16_t>(currentSpineIndex), ttfGeneration) !=
        freeink::book::BookStatus::Ok) {
      LOG_DBG("ERS", "font preview: chapter cache reopen failed — preview may keep the current frame");
    }
  }
  FontPreviewActivity::Host host;
  host.ctx = this;
  host.onFamilyChanging = [](void* ctx) { static_cast<EpubReaderActivity*>(ctx)->onPreviewFontChanging(); };
  auto preview = makeUniqueNoThrow<FontPreviewActivity>(renderer, mappedInput, ttf_.get(),
                                                        static_cast<uint16_t>(currentSpineIndex), ttfCurrentCharStart,
                                                        ttfCurrentCharEnd, automaticPageTurnActive, host);
  if (!preview) {
    LOG_ERR("ERS", "OOM: font preview");
    return;
  }
  {
    RenderLock lock;  // the render task shares the framebuffer
                      // The chrome was pushed deferred (pushOverlayRefresh) and this is a
                      // quick-menu launch that never settles: without the drain, the preview's
                      // first FAST diffs its clean full-screen layout against the pre-chrome
                      // baseline and the panel/page pixels are left baked on the glass. Same
                      // full-FB-flush contract as pushOverlayRefresh — a partial prerender must
                      // die before this framebuffer reaches the glass.
#if defined(CROSSPOINT_TTF_READER)
    ttfInvalidatePreRender("font preview launch");
#endif
    settleOverlayRefresh();
  }
  overlay = Overlay::None;
  overlayPopup.dismiss();
  discardOverlayPage();
  startActivityForResult(std::move(preview), [this](const ActivityResult& result) {
    if (!result.isCancelled && std::holds_alternative<QuickFontPreviewResult>(result.data)) {
      // The preview persisted the settings; this is the settings-driven clean
      // reindex a Text-settings font change lands. The position restores
      // through the chapter char offset the invalidation latches
      // (ttfInvalidateCaches keeps ttfCurrentCharStart as a CharOffset target)
      // — the preview's page-local relayout cannot supply a truthful page
      // index for the rebuilt layout (ADR 2026-10-14).
      applyReaderTextSettings();
      // The first frame of the reindexed page must reach the glass as a full
      // refresh (ADR §Decision 3): no FAST diff may land before it.
      pagesUntilFullRefresh = 1;
    }
    // Both close branches pop straight back to the book page; the chrome
    // overlay is torn down with the preview, no Text panel reopens.
    overlay = Overlay::None;
    discardOverlayPage();
    requestUpdate();
  });
}
#endif

void EpubReaderActivity::showTextRowPopup(const int row) {
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_ && (row == 0 || row == 1)) {
    // Size and family live in the full-screen font preview; the full-screen
    // settings activity remains the advanced Settings entry point.
    openFontPreview();
    return;
  }
#endif
  switch (row) {
    case 0: {
      // Non-TTF builds (and a TTF build whose runtime failed to open) keep
      // the full-screen family picker; only a live TTF runtime uses the sheet.
      auto settings = makeUniqueNoThrow<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                              TextSettingsActivity::Tab::Family);
      if (!settings) {
        LOG_ERR("ERS", "OOM: text settings activity");
        return;
      }
      {
        RenderLock lock;  // the render task shares the framebuffer
                          // Non-TTF fallback for the font row: same unsettled
                          // quick-menu launch as openFontPreview(), so it owes
                          // the same drain before handing over the framebuffer.
        settleOverlayRefresh();
      }
      overlay = Overlay::None;
      overlayPopup.dismiss();
      discardOverlayPage();
      startActivityForResult(std::move(settings), [this](const ActivityResult&) {
        applyReaderTextSettings();
        overlay = Overlay::Text;
        panelIndex = 0;
        if (toolbarUi) toolbarUi->begin();
        requestUpdate();
      });
      return;
    }
    case 1: {
      // The point sizes the active family actually ships.
      const auto sizes = readerFontPointSizes(&sdFontSystem.registry(), SETTINGS.sdFontFamilyName);
      if (sizes.empty()) return;
      std::vector<std::string> labels;
      labels.reserve(sizes.size());
      for (const uint8_t size : sizes) labels.push_back(std::to_string(size) + " pt");
      const uint8_t cur = snapToNearestPointSize(sizes, SETTINGS.fontPointSize);
      int curIdx = 0;
      for (size_t i = 0; i < sizes.size(); ++i) {
        if (sizes[i] == cur) curIdx = static_cast<int>(i);
      }
      overlayPopup.show(StrId::STR_FONT_SIZE, labels, curIdx, [this, sizes](int idx) {
        if (idx < 0 || idx >= static_cast<int>(sizes.size())) return;
        SETTINGS.fontPointSize = sizes[idx];
        applyTextSettingLive();
      });
      break;
    }
    case 2:
      overlayPopup.show(StrId::STR_LINE_SPACING, LINE_SPACING_IDS, static_cast<int>(std::size(LINE_SPACING_IDS)),
                        SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT, [this](int idx) {
                          SETTINGS.lineSpacing = static_cast<uint8_t>(idx);
                          applyTextSettingLive();
                        });
      break;
    case 3:
      overlayPopup.show(StrId::STR_PARA_ALIGNMENT, kAlignIds, static_cast<int>(std::size(kAlignIds)),
                        SETTINGS.paragraphAlignment % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT, [this](int idx) {
                          SETTINGS.paragraphAlignment = static_cast<uint8_t>(idx);
                          applyTextSettingLive();
                        });
      break;
    default:
      return;
  }
  paintOverlayPopup();
}

void EpubReaderActivity::discardOverlayPage() {
  if (!overlayPageStored) return;
  renderer.discardStoredBwBuffer();
  overlayPageStored = false;
}

// Upper bound on how long settleOverlayRefresh() polls the panel before giving
// up on a deferred overlay refresh: a healthy FAST waveform completes in
// ~0.3-2 s, so exceeding this means the controller is wedged, not slow.
static constexpr uint32_t OVERLAY_REFRESH_SETTLE_TIMEOUT_MS = 3000;

// Push freshly painted overlay chrome. Where the panel supports it the refresh
// is fired deferred: the loop keeps polling input while the waveform runs, so
// the chrome answers taps and buttons the moment it is visible instead of only
// after a blocking displayBuffer() returns. Caller must hold the RenderLock.
void EpubReaderActivity::pushOverlayRefresh() {
  // A full-FB flush outside the forward turn's own commit (review contract):
  // any mid-pump prerender's partial framebuffer must die here — the glass
  // is about to show this framebuffer's content.
#if defined(CROSSPOINT_TTF_READER)
  ttfInvalidatePreRender("overlay push");
#endif
  if (renderer.supportsAsyncRefresh()) {
    if (renderer.refreshBusy()) {
      // A previous deferred refresh is still running (its settle timed out on
      // a wedged panel): firing another waveform into it would re-enter the
      // driver's no-timeout busy wait. Skip; the pending flag stays set so a
      // later settle/push repaints once the panel is idle.
      LOG_ERR("ERS", "overlay refresh still busy, skipping push");
      return;
    }
    renderer.displayBufferAsync(HalDisplay::FAST_REFRESH);
    overlayRefreshPending.store(true, std::memory_order_release);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

// Wait out a pending deferred overlay refresh and reseed the panel's
// differential baseline from the framebuffer: the shadow-free async path skips
// the post-refresh resync, so without this the next FAST diff would run
// against the frame from before the chrome and leave stale pixels on the
// glass. The panel's own busy wait (UC8279 UcIdleHigh in the driver) has
// deliberately no timeout, so poll refreshBusy() here under a hard deadline
// instead: on timeout the controller is wedged, and we arm a driver resync
// (flag only, no bus traffic) for the next refresh rather than spin forever
// under the RenderLock (issue #143 freeze). After a timeout, subsequent
// settles only single-check refreshBusy() — polling another 3 s for every
// overlay interaction would stall input while the wedge lasts (#151). Caller
// must hold the RenderLock.
void EpubReaderActivity::settleOverlayRefresh() {
  if (!overlayRefreshPending.load(std::memory_order_acquire)) return;
#if defined(CROSSPOINT_TTF_READER)
  // The settle's baseline reseed re-flashes the framebuffer to the glass —
  // same full-FB-flush contract as pushOverlayRefresh: no partial prerender
  // may be on it.
  ttfInvalidatePreRender("overlay settle");
#endif
  if (overlaySettleTimedOut) {
    // A previous settle already burned the full deadline on this refresh: one
    // non-blocking check only, never re-poll under the RenderLock.
    if (renderer.refreshBusy()) return;
  } else {
    const uint32_t start = millis();
    while (renderer.refreshBusy()) {
      if (millis() - start > OVERLAY_REFRESH_SETTLE_TIMEOUT_MS) {
        LOG_ERR("ERS", "overlay refresh busy >%lu ms, arming panel resync",
                static_cast<unsigned long>(OVERLAY_REFRESH_SETTLE_TIMEOUT_MS));
        renderer.requestResync();
        overlaySettleTimedOut = true;
        // Leave the flag set: the next settle re-attempts (single check), and
        // pushOverlayRefresh() refuses to fire a new waveform while this one
        // is still running.
        return;
      }
      vTaskDelay(1);
    }
  }
  overlaySettleTimedOut = false;
  renderer.cleanupGrayscaleWithFrameBuffer();  // waits, then reseeds the baseline
  // Clear only after the baseline reseed: while it runs, the refresh is not
  // yet fully drained as far as the next settle/push is concerned.
  overlayRefreshPending.store(false, std::memory_order_relaxed);
}

// Recover a refresh that was skipped while the panel was wedged (timed-out
// settle): without this, a deferred overlay close would leave the chrome on
// the glass until the next page turn, because nothing else schedules a render
// once the panel drains (#151 review). Throttled non-blocking re-checks; the
// actual settle/push runs under the RenderLock like every other path.
void EpubReaderActivity::retryDeferredOverlayPush() {
  if (!overlayRefreshPending.load(std::memory_order_acquire) || !overlaySettleTimedOut) return;
  constexpr uint32_t OVERLAY_PUSH_RETRY_MS = 500;
  const uint32_t now = millis();
  if (now - overlayPushRetryMs < OVERLAY_PUSH_RETRY_MS) return;
  overlayPushRetryMs = now;
  if (renderer.refreshBusy()) return;  // still wedged; retry on a later tick
  RenderLock lock;
  settleOverlayRefresh();  // drains the flag and reseeds the baseline
  if (!renderer.refreshBusy()) renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void EpubReaderActivity::openOverlay(Overlay target) {
  mappedInput.resetHomeButtonInput();
  const Overlay previous = overlay;
  overlay = target;
  if (!toolbarUi) toolbarUi = std::make_unique<ReaderToolbarUi>(renderer);
  if (previous == Overlay::None) toolbarUi->begin();
#if defined(CROSSPOINT_TTF_READER)
  LOG_DBG("ERS", "overlay open target=%d previous=%d uiReady=%d", static_cast<int>(target), static_cast<int>(previous),
          toolbarUi->routingReady());
#endif
  // Buttons show a cursor from the start; touch boards only once a button moves it.
  panelCursorShown = !mappedInput.hasTouch();
  switch (target) {
    case Overlay::Toolbar:
      focusedTool = 0;
      break;
    case Overlay::Contents: {
      RenderLock lock;
      panelIndex = currentTocIndex();
      // Fresh viewport opening on the current chapter, cursor shown or not.
      toolbarUi->nav().reset(panelIndex);
      toolbarUi->nav().top = panelIndex;
      break;
    }
    case Overlay::Text:
      panelIndex = 0;
      toolbarUi->nav().reset();
      break;
    case Overlay::More:
      panelIndex = 0;
      buildMoreActions();
      toolbarUi->nav().reset();
      break;
    default:
      break;
  }
  panelHoldJumped = false;

  // The page is already on screen and still in the framebuffer, so paint the
  // chrome straight onto it and push one refresh. requestUpdate() would
  // re-render the whole page first: slow, and visibly wrong, since that repaint
  // lands before the overlay does.
  //
  // Refresh mode: FAST for every overlay paint, first open included. The AA
  // pass only grays glyph edges, and residue a FAST differential leaves under
  // the sheet has not shown in practice; it also self-heals on the
  // Xteink-class panels, whose close path re-renders the page. If text or
  // images ever visibly ghost through the chrome, restore a HALF cleanup on
  // the first open (see #2190 for the mechanism).
  bool hasRenderedPage = section != nullptr;
#if defined(CROSSPOINT_TTF_READER)
  // The TTF path keeps no Section mirror. Use only a frame that renderBookTtf
  // has completed for the current chapter; failed reads/navigation clear the
  // flag so a stale framebuffer can never be reused.
  hasRenderedPage = hasRenderedPage || (ttf_ && ttfFrameRenderComplete.load(std::memory_order_acquire) &&
                                        ttfSpine == currentSpineIndex && ttfPage >= 0 && ttfPageCount > 0);
#endif
  if (hasRenderedPage && !pageBufferStale) {
    // Serialize against the render task: renderBook may be mid-page (status
    // bar included) in the shared framebuffer, and painting the chrome from
    // the loop task at the same time interleaves the two frames.
    RenderLock lock;
    settleOverlayRefresh();
    if (previous == Overlay::None) {
      // Snapshot the clean page so stepping back from a panel to the toolbar
      // (and closing, where supported) can restore it without a re-render.
      overlayPageStored = renderer.storeBwBuffer();
    } else if (overlayPageStored) {
      // Overlay -> overlay: wipe the previous chrome (toolbar header, sheet,
      // progress row) back to the clean page so none of it shows around or
      // through the new sheet; re-store for the next transition. No baseline
      // resync: the glass still shows the old chrome, and the differential
      // must keep diffing against it to erase it.
      renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
      overlayPageStored = renderer.storeBwBuffer();
    }
    renderOverlay();
#if defined(CROSSPOINT_TTF_READER)
    LOG_DBG("ERS", "overlay rendered=%d uiReady=%d", static_cast<int>(overlay), toolbarUi->routingReady());
    memSentinelCheck("overlay render");
    LOG_DBG("ERS", "overlay mem: HeapFree=%u HeapMin=%u MaxAlloc=%u PSRAMFree=%u loopStackHW=%u",
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMinFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()), static_cast<unsigned>(ESP.getFreePsram()),
            static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#endif
    pushOverlayRefresh();
  } else {
    requestUpdate();  // no page yet: renderBook() draws the overlay once it is
  }
}

// Grayscale panels re-render to restore AA; other boards restore the B/W snapshot.
void EpubReaderActivity::closeOverlayToPage() {
  mappedInput.resetHomeButtonInput();
  overlay = Overlay::None;
  overlayPopup.dismiss();  // an option picker cannot outlive its panel
  toolbarUi.reset();       // ~1 KB of interaction table + props, only needed while open
  if (BoardConfig::isEegoA4()) pagesUntilFullRefresh = 1;
  if (!xteinkClassPanel() && overlayPageStored) {
    RenderLock lock;  // the render task shares the framebuffer
    settleOverlayRefresh();
    // No baseline resync: the glass shows the chrome, and erasing it needs
    // the differential to keep diffing against the last pushed frame.
    renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
    overlayPageStored = false;
    if (!renderer.refreshBusy()) {
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      // Timed-out settle on a wedged panel: entering the driver's no-timeout
      // display wait here would re-create the #143 freeze on a back gesture.
      // Skip the push; the pending flag keeps this close deferred and
      // retryDeferredOverlayPush() (activity loop) pushes the clean page once
      // the panel drains and reseeds the baseline.
      LOG_ERR("ERS", "overlay close deferred, panel still busy");
    }
    return;
  }
  discardOverlayPage();
  requestUpdate();  // redraw the clean page
}

void EpubReaderActivity::renderOverlay() {
#if defined(CROSSPOINT_TTF_READER)
  if (!epub || !toolbarUi || (!section && !ttf_)) return;
#else
  if (!epub || !section || !toolbarUi) return;
#endif

  ReaderToolbarUi::Model model;
  // The toolbar's tool pill is the button-navigation cursor: tap-first (same
  // convention as the panel lists), it only shows once a button has moved it.
  // Panels override below: there the pill marks the open panel on every board.
  model.activeTool = (overlay == Overlay::Toolbar && !panelCursorShown) ? -1 : focusedTool;
  // Strings the model points at live here until render() returns.
  std::string chapterTitle, pageInfo;

  if (overlay == Overlay::Toolbar) {
    chapterTitle = currentChapterTitle();
    // Legacy reads the Section; the TTF path reads its position mirrors.
    const int pageCount = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
    const int chapterCurrentPage = section ? section->currentPage + 1 : nextPageNumber + 1;
    const float chapterProgress =
        pageCount > 0 ? static_cast<float>(chapterCurrentPage) / static_cast<float>(pageCount) : 0.0f;
    const float bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress);
    pageInfo = std::to_string(chapterCurrentPage) + "/" + std::to_string(pageCount) + "   " +
               std::to_string(clampPercent(static_cast<int>(bookProgress * 100.0f + 0.5f))) + "%";
    model.chapterTitle = chapterTitle.c_str();
    model.pageInfo = pageInfo.c_str();
    model.progressPermille = static_cast<int>(bookProgress * 1000.0f + 0.5f);
    toolbarUi->setModel(model);
    toolbarUi->render();
    return;
  }

  // Panels (Contents / Text / More): a bottom sheet over the page.
  model.panel = true;
  if (!mappedInput.hasTouch()) {
    model.denseRows = true;
  }
  // Tap-first: the cursor is only drawn once a button has moved it, so a
  // tapped row does not stay inverted after its action.
  model.selectedIndex = panelCursorShown ? panelIndex : -1;
  if (overlay == Overlay::Contents) {
    model.panelTitle = tr(STR_TOOL_CONTENTS);
    model.itemCount = epub->getTocItemsCount();
    model.rowText = [this](int i) {
      const auto item = epub->getTocItem(i);
      const int depth = item.level > 1 ? (item.level - 1) * 2 : 0;
      return std::string(depth, ' ') + item.title;
    };
  } else if (overlay == Overlay::Text) {
    model.panelTitle = tr(STR_TOOL_TEXT);
    model.itemCount = kTextRowCount;
    model.rowText = [this](int i) { return textRowName(i); };
    model.rowValue = [this](int i) { return textRowValue(i); };
    model.rowCheckbox = [](void*, int i, freeink::ui::ListItem& item) {
      if (i == 4) GUI.setCheckboxRow(item, SETTINGS.focusReadingEnabled);
    };
  } else if (overlay == Overlay::Stats) {
    static constexpr StrId kStatsRowIds[] = {StrId::STR_STATS_SHOW_BOOK_STATS, StrId::STR_STATS_ALL_TIME,
                                             StrId::STR_STATS_READING_RHYTHM, StrId::STR_STATS_FINISHED_BOOKS};
    static_assert(std::size(kStatsRowIds) == kStatsPanelRows);
    model.panelTitle = tr(STR_READING_STATS);
    model.itemCount = kStatsPanelRows;
    model.rowText = [](int row) { return std::string(I18N.get(kStatsRowIds[row])); };
  } else {
    model.panelTitle = tr(STR_TOOL_MORE);
    model.itemCount = static_cast<int>(moreItems.size());
    model.rowText = [this](int i) { return moreRowName(i); };
    model.rowValue = [this](int i) { return moreRowValue(i); };
    model.rowCheckboxContext = this;
    model.rowCheckbox = [](void* ctx, int i, freeink::ui::ListItem& item) {
      const auto* self = static_cast<EpubReaderActivity*>(ctx);
      using MA = EpubReaderMenuActivity::MenuAction;
      const auto action = self->moreItems[i].action;
      if (action == MA::NIGHT_MODE) GUI.setCheckboxRow(item, SETTINGS.screenInverted);
      if (action == MA::BLUETOOTH) GUI.setCheckboxRow(item, SETTINGS.ble.enabled);
      if (action == MA::FRONTLIGHT) GUI.setCheckboxRow(item, Frontlight.isOn());
    };
  }
  toolbarUi->setModel(model);
  toolbarUi->render();

  if (!mappedInput.hasTouch()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}

void EpubReaderActivity::handleOverlayInput() {
  if (!toolbarUi) return;

  // A modal option picker over the panel owns all input while open.
  if (overlayPopup.isActive()) {
    overlayPopup.handleInput(mappedInput, [this] {
      if (overlayPopup.isActive()) {
        paintOverlayPopup();  // highlight moved
        return;
      }
      // Dismissed or selected: erase the dialog -- clean page back, then the
      // panel over it (the dialog can overhang the sheet onto the page).
      RenderLock lock;
      settleOverlayRefresh();
      if (overlayPageStored) {
        renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
        overlayPageStored = renderer.storeBwBuffer();
        renderOverlay();
        pushOverlayRefresh();
      } else {
        requestUpdate();
      }
    });
    return;
  }
  const auto fastRedraw = [this] {
    RenderLock lock;  // the render task shares the framebuffer
    settleOverlayRefresh();
    renderOverlay();
    pushOverlayRefresh();
  };

  // Jump to another spine item (chapter scrub). The overlay stays up and is
  // re-drawn over the new page by renderBook().
  const auto gotoSpine = [this](int target) {
    const int spineCount = epub->getSpineItemsCount();
    target = std::clamp(target, 0, spineCount - 1);
    if (target != currentSpineIndex) {
      RenderLock lock;
      clearDeferredReposition();
      nextPageNumber = 0;
      currentSpineIndex = target;
      section.reset();
    }
    requestUpdate();
  };
  const auto toolOverlay = [](int tool) {
    if (tool == EpubReaderActivity::kToolContents) return Overlay::Contents;
    if (tool == EpubReaderActivity::kToolText) return Overlay::Text;
#ifdef READING_STATS_ENABLED
    // Tracking-off builds keep the tile visible but gated, like the classic
    // menu's dimmed row: opening the panel is refused, so BookStatsActivity
    // can never launch from the toolbar.
    if (tool == EpubReaderActivity::kToolStats && SETTINGS.shouldTrackReadingStats()) return Overlay::Stats;
#endif
    return Overlay::More;
  };

  // Touch first: FreeInkUI routes the frame against the tap targets the last
  // render registered and hands back the action it mapped to.
  const auto routed = toolbarUi->route(mappedInput);
#if defined(CROSSPOINT_TTF_READER)
  if (routed.routed) {
    LOG_DBG("ERS", "overlay=%d uiReady=%d routed event=%d value=%d", static_cast<int>(overlay),
            toolbarUi->routingReady(), static_cast<int>(routed.event), routed.value);
  }
#endif

  // --- Toolbar ---
  if (overlay == Overlay::Toolbar) {
    switch (routed.event) {
      case ReaderToolbarUi::Event::Dismiss:
        closeOverlayToPage();
        return;
      case ReaderToolbarUi::Event::Tool:
        focusedTool = routed.value;
        openOverlay(toolOverlay(focusedTool));
        return;
      case ReaderToolbarUi::Event::PrevChapter:
        gotoSpine(currentSpineIndex - 1);
        return;
      case ReaderToolbarUi::Event::NextChapter:
        gotoSpine(currentSpineIndex + 1);
        return;
      case ReaderToolbarUi::Event::Scrub:
        gotoSpine(static_cast<int>((static_cast<float>(routed.permille) / 1000.0f) *
                                       static_cast<float>(epub->getSpineItemsCount() - 1) +
                                   0.5f));
        return;
      default:
        break;
    }
    if (routed.routed) return;  // a touch frame the chrome consumed (or dead space)

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeOverlayToPage();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      focusedTool = (focusedTool + EpubReaderActivity::kToolTileCount - 1) % EpubReaderActivity::kToolTileCount;
      panelCursorShown = true;
      fastRedraw();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      focusedTool = (focusedTool + 1) % EpubReaderActivity::kToolTileCount;
      panelCursorShown = true;
      fastRedraw();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openOverlay(toolOverlay(focusedTool));
      return;
    }
    const bool prev = mappedInput.wasReleased(MappedInputManager::Button::Up);
    const bool next = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (prev || next) {
      gotoSpine(currentSpineIndex + (next ? 1 : -1));
    }
    return;
  }

  // --- Panels (Contents / Text / More) ---
  const int count = overlay == Overlay::Contents ? epub->getTocItemsCount()
                    : overlay == Overlay::Text   ? kTextRowCount
                    : overlay == Overlay::Stats  ? kStatsPanelRows
                                                 : static_cast<int>(moreItems.size());
  const int pageRows = std::max(1, toolbarUi->visibleRows());

  // Activate the highlighted row: change a value / jump to a chapter / run an
  // action. Shared by the Confirm button and a row tap.
  const auto activateRow = [this, count, &fastRedraw] {
    if (panelIndex < 0 || panelIndex >= count) return;
    if (overlay == Overlay::Text) {
      if (panelIndex == 0) {
#if defined(CROSSPOINT_TTF_READER)
        // Family and size live in the full-screen font preview; the full
        // picker remains available from the Settings text screen.
        openFontPreview();
#endif
      } else if (panelIndex == 4) {
        // Focus Reading is a genuine on/off: a tap toggles and applies live.
        SETTINGS.focusReadingEnabled = SETTINGS.focusReadingEnabled ? 0 : 1;
        applyTextSettingLive();
      } else {
        // Enum rows open the Settings-style option picker.
        showTextRowPopup(panelIndex);
      }
    } else if (overlay == Overlay::Contents) {
      const auto item = epub->getTocItem(panelIndex);
      if (item.spineIndex != -1) {
        RenderLock lock;
        clearDeferredReposition();
        currentSpineIndex = item.spineIndex;
        pendingAnchor = item.anchor;
        nextPageNumber = 0;
        section.reset();
      }
      overlay = Overlay::None;
      discardOverlayPage();
      requestUpdate();
    } else if (overlay == Overlay::Stats) {
#ifdef READING_STATS_ENABLED
      {
        recordCurrentPageReadingTime();
        BookReadingStats displayStats = stats;
        if (SETTINGS.shouldTrackReadingStats()) {
          displayStats.totalReadingSeconds += sessionReadingSeconds;
        }
        // The panel lists the real destinations directly; no intermediate
        // ReadingStatsMenuActivity hop.
        std::unique_ptr<Activity> target;
        if (panelIndex == 0) {
          target = makeUniqueNoThrow<BookStatsActivity>(
              renderer, mappedInput, epub->getTitle(), epub->getAuthor(), displayStats, epub->getCachePath(),
              epub->getCachePath().empty() ? GlobalReadingStats{} : GlobalReadingStats::load());
        } else if (panelIndex == 1) {
          target = makeUniqueNoThrow<GlobalStatsActivity>(renderer, mappedInput);
        } else if (panelIndex == 2) {
          target = makeUniqueNoThrow<ReadingRhythmActivity>(renderer, mappedInput);
        } else {
          target = makeUniqueNoThrow<FinishedBooksActivity>(renderer, mappedInput);
        }
        if (!target) {
          LOG_ERR("ERS", "OOM: reading stats screen");
          return;
        }
        {
          RenderLock lock;  // the render task shares the framebuffer
                            // The chrome was pushed deferred (pushOverlayRefresh) and this is
                            // the only launch path that never settles: without the drain, the
                            // stats screen's first FAST diffs its clean layout against the
                            // pre-chrome baseline and bakes the panel/page into the glass.
                            // Same full-FB-flush contract as pushOverlayRefresh — a partial
                            // prerender must die before this framebuffer reaches the glass.
#if defined(CROSSPOINT_TTF_READER)
          ttfInvalidatePreRender("stats launch");
#endif
          settleOverlayRefresh();
        }
        overlay = Overlay::None;
        overlayPopup.dismiss();
        discardOverlayPage();
        startActivityForResult(std::move(target), [this](const ActivityResult& result) {
          if (epub && SETTINGS.shouldTrackReadingStats()) handleBookStatsReturn();
          if (std::holds_alternative<ClearPaceResult>(result.data) && epub) {
            stats.clearWpmStats();
            stats.save(epub->getCachePath());
          }
          requestUpdate();
        });
      }
#endif
    } else if (overlay == Overlay::More) {
      activateMoreRow(panelIndex);
    }
  };

  // Steps up to the toolbar -- the Back button and a tap on the page above
  // the sheet.
  const auto dismissPanel = [this, &fastRedraw] {
    overlay = Overlay::Toolbar;
    // Restore the snapshotted page under the toolbar instead of re-rendering
    // it (2+ refreshes -> one FAST). Re-store right away so another panel
    // round-trip can restore again.
    if (overlayPageStored) {
      {
        RenderLock lock;  // the render task shares the framebuffer
        settleOverlayRefresh();
        // No baseline resync: the glass shows the panel, and erasing it needs
        // the differential to keep diffing against the last pushed frame.
        renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
        overlayPageStored = renderer.storeBwBuffer();
      }
      fastRedraw();  // takes its own RenderLock
      return;
    }
    requestUpdate();
  };

  // Pages the list by one screen of rows through the nav (measured page size,
  // no-op at the ends). A shown cursor rides along so the buttons continue
  // from what is visible; on touch boards only the viewport moves.
  const auto pageList = [this, count, pageRows, &fastRedraw](int direction) {
    if (count <= 0) return;
    const bool moved = toolbarUi->nav().scrollBy(direction * pageRows, count);
    if (panelCursorShown) {
      panelIndex = std::clamp(panelIndex + direction * pageRows, 0, count - 1);
      fastRedraw();
      return;
    }
    if (moved) fastRedraw();
  };

  switch (routed.event) {
    case ReaderToolbarUi::Event::Dismiss:
      dismissPanel();
      return;
    case ReaderToolbarUi::Event::Tool: {
      // Sheet-bottom tool switcher: hop straight to another panel.
      const Overlay target = toolOverlay(routed.value);
      if (target != overlay) {
        focusedTool = routed.value;
        openOverlay(target);
      }
      return;
    }
    case ReaderToolbarUi::Event::Row:
      // A tap on the right-edge strip pages the sheet instead (upper half =
      // previous page, lower half = next): swipes are unreliable on etched
      // glass, and a long contents list needs a fast way through.
      if (routed.x >= renderer.getScreenWidth() - 44) {
        pageList(routed.y >= renderer.getScreenHeight() - (renderer.getScreenHeight() * 62) / 200 ? 1 : -1);
        return;
      }
      panelIndex = routed.value;
      panelCursorShown = false;
      activateRow();
      return;
    default:
      break;
  }
  // Swipe up/down pages the list. Checked before the routed-frame return:
  // FUI routes every touch frame over the sheet, so a swipe's frames count as
  // routed (without dispatching -- too much travel for a tap) and the gesture
  // would otherwise never be seen.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    pageList(swipe == MappedInputManager::SwipeDir::Up ? 1 : -1);
    return;
  }
  if (routed.routed) return;  // consumed by the chrome (title band, dead space)

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    dismissPanel();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateRow();
    return;
  }

  // Up/Down (side) and Left/Right (front) move the cursor: a tap steps one
  // row, holding past PANEL_HOLD_MS jumps PANEL_HOLD_STEP rows in one go, which
  // is how you cross a hundreds-of-chapters contents list without a press per
  // row. The jump fires once on the hold and swallows the release that ends it,
  // so it never doubles up with the tap step.
  if (count > 0) {
    const bool up = mappedInput.isPressed(MappedInputManager::Button::Up) ||
                    mappedInput.isPressed(MappedInputManager::Button::Left);
    const bool down = mappedInput.isPressed(MappedInputManager::Button::Down) ||
                      mappedInput.isPressed(MappedInputManager::Button::Right);
    if (!panelHoldJumped && (up || down) && mappedInput.getHeldTime() >= PANEL_HOLD_MS) {
      const int step = down ? PANEL_HOLD_STEP : -PANEL_HOLD_STEP;
      panelIndex = std::clamp(panelIndex + step, 0, count - 1);
      panelHoldJumped = true;
      panelCursorShown = true;
      fastRedraw();
      return;
    }

    const bool releasedUp = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool releasedDown = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                              mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (releasedUp || releasedDown) {
      if (!panelHoldJumped) {
        panelIndex = releasedUp ? ButtonNavigator::previousIndex(panelIndex, count)
                                : ButtonNavigator::nextIndex(panelIndex, count);
        panelCursorShown = true;
        fastRedraw();
      }
      panelHoldJumped = false;
    }
  }
}

// First paint of the option picker over the panel (and highlight repaints).
// The dialog draws over the current framebuffer without clearing; erasing it
// on dismissal is the popup gate's restore in handleOverlayInput().
void EpubReaderActivity::paintOverlayPopup() {
  RenderLock lock;
  settleOverlayRefresh();
  overlayPopup.render(renderer);
  pushOverlayRefresh();
}

void EpubReaderActivity::applyReaderTextSettings() {
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    SETTINGS.saveToFile();
#if CROSSPOINT_TTF_UI_FALLBACK
    // End the UI fallback faces FIRST, while the old loader bytes they
    // borrow are still resident — a family change reloads (and frees) those
    // bytes on the next ensureLoaded(), which must not happen while faces
    // still borrow them.
    freeink::book::ttfUiFallback.release(renderer);
#endif
    RenderLock lock;
    // ttfInvalidateCaches() below latches pendingRestoreTarget_ as a
    // CharOffset target from ttfCurrentCharStart, so the position restores
    // through the char offset. Armed under the lock: a render pass that
    // slipped in before this one would otherwise read the anchor against
    // the old-generation cache.
    freeink::book::fontLoader.markDirty();
    // Reflow in place: drop the caches; the new generation produces a fresh
    // build and the position restores through the page's char offset.
    ttfInvalidateCaches();
#if CROSSPOINT_TTF_UI_FALLBACK
    // ensureLoaded() inside update() reloads the newly selected family and
    // re-registers the fallback against the fresh resident bytes.
    freeink::book::ttfUiFallback.update(renderer);
#endif
    return;
  }
#endif
  {
    RenderLock lock;
    if (section) {
      rememberCurrentContentOffset();
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    // Release the old parser and page table before settings serialization needs heap.
    section.reset();  // force re-pagination with the new settings
  }
  SETTINGS.saveToFile();
  // (Re)load or unload the selected SD-card font for the current family/size.
  // The reader otherwise only loads SD fonts on book open, so without this an
  // in-reader font change wouldn't take effect until re-opening the book.
  // Locked because the render task can load or unload SD font variants too.
  RenderLock lock;
  sdFontSystem.ensureLoaded(renderer);
#if CROSSPOINT_TTF_UI_FALLBACK
  freeink::book::ttfUiFallback.update(renderer);
#endif
}

// The More panel carries everything the classic list menu offers except the
// entries with their own surface: chapters -> Contents tool, text -> Text
// tool, per-book stats -> its own tool tile (stats build only).
void EpubReaderActivity::buildMoreActions() {
  using MA = EpubReaderMenuActivity::MenuAction;
#ifdef READING_STATS_ENABLED
  EpubReaderMenuActivity::buildToolbarMoreItems(moreItems, !currentPageFootnotes.empty(), !cachedBookmarks.empty(),
                                                CLIPPINGS.hasClippings());
#else
  EpubReaderMenuActivity::buildMenuItems(moreItems, !currentPageFootnotes.empty(), !cachedBookmarks.empty(),
                                         CLIPPINGS.hasClippings());
  moreItems.erase(std::remove_if(moreItems.begin(), moreItems.end(),
                                 [](const auto& item) {
                                   return item.action == MA::SELECT_CHAPTER || item.action == MA::TEXT_SETTINGS;
                                 }),
                  moreItems.end());
#endif
}

std::string EpubReaderActivity::moreRowName(int row) const {
  return row >= 0 && row < static_cast<int>(moreItems.size()) ? I18N.get(moreItems[row].labelId) : "";
}

std::string EpubReaderActivity::moreRowValue(int row) const {
  using MA = EpubReaderMenuActivity::MenuAction;
  static constexpr StrId kOrient[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED,
                                      StrId::STR_LANDSCAPE_CCW};
  static_assert(std::size(kOrient) == CrossPointSettings::ORIENTATION_COUNT, "orientation labels");
  if (row < 0 || row >= static_cast<int>(moreItems.size())) return "";
  switch (moreItems[row].action) {
    case MA::ROTATE_SCREEN:
      return I18N.get(kOrient[SETTINGS.orientation % CrossPointSettings::ORIENTATION_COUNT]);
    case MA::AUTO_PAGE_TURN:
      return (autoTurnOption == 0 || autoTurnOption >= static_cast<int>(std::size(PAGE_TURN_RATES)))
                 ? std::string(tr(STR_STATE_OFF))
                 : std::to_string(PAGE_TURN_RATES[autoTurnOption]);
    default:
      return "";
  }
}

void EpubReaderActivity::activateMoreRow(int row) {
  using MA = EpubReaderMenuActivity::MenuAction;
  if (row < 0 || row >= static_cast<int>(moreItems.size())) return;
  const auto action = moreItems[row].action;
  {
    RenderLock lock;  // several actions launch screens that paint the framebuffer
    settleOverlayRefresh();
  }
  // In-place toggles keep the panel open and re-render the page beneath it.
  switch (action) {
    case MA::ROTATE_SCREEN: {
      static constexpr StrId kOrientIds[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW,
                                             StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
      static_assert(std::size(kOrientIds) == CrossPointSettings::ORIENTATION_COUNT, "orientation options");
      overlayPopup.show(StrId::STR_ORIENTATION, kOrientIds, static_cast<int>(std::size(kOrientIds)),
                        SETTINGS.orientation % CrossPointSettings::ORIENTATION_COUNT, [this](int idx) {
                          if (idx == SETTINGS.orientation) return;
                          applyOrientation(static_cast<uint8_t>(idx));
                          // The stored page is laid out for the old orientation.
                          discardOverlayPage();
                          requestUpdate();
                        });
      paintOverlayPopup();
      return;
    }
    case MA::AUTO_PAGE_TURN: {
      std::vector<std::string> labels;
      labels.reserve(std::size(PAGE_TURN_RATES));
      labels.emplace_back(tr(STR_STATE_OFF));
      for (size_t i = 1; i < std::size(PAGE_TURN_RATES); ++i) labels.push_back(std::to_string(PAGE_TURN_RATES[i]));
      overlayPopup.show(StrId::STR_AUTO_TURN_PAGES_PER_MIN, labels, autoTurnOption, [this](int idx) {
        autoTurnOption = idx;
        toggleAutoPageTurn(static_cast<uint8_t>(idx));
      });
      paintOverlayPopup();
      return;
    }
    case MA::BLUETOOTH:
      EpubReaderMenuActivity::toggleBluetooth();
      {
        RenderLock lock;
        renderOverlay();
        pushOverlayRefresh();
      }
      return;
    case MA::NIGHT_MODE:
      SETTINGS.screenInverted = SETTINGS.screenInverted == 0 ? 1 : 0;
      SETTINGS.saveToFile();
      discardOverlayPage();
      requestUpdate();
      return;
    case MA::FRONTLIGHT: {
      frontlight::setOn(!Frontlight.isOn());
      {
        RenderLock lock;  // the render task shares the framebuffer
        renderOverlay();
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      }
      return;
    }
    default:
      break;
  }
  // Leaf actions open their own screen / perform the action; close the overlay first.
  overlay = Overlay::None;
  if (action == MA::GO_TO_PERCENT && overlayPageStored) {
    // The percent dialog is a popup over the current frame: wipe the toolbar
    // chrome back to the clean page first so the dialog draws over the page,
    // not the sheet. No refresh push — the dialog's first frame carries it.
    RenderLock lock;
    settleOverlayRefresh();
    renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
    overlayPageStored = false;
  } else {
    discardOverlayPage();
  }
  if (action == MA::TOGGLE_BOOKMARK) {
    // No child activity here to trigger the re-render the list menu relies on:
    // show the same confirmation popup the long-press path does.
    addBookmark();
    showBookmarkMessage = true;
    bookmarkMessageTime = millis();
    requestUpdate();
    return;
  }
  onReaderMenuConfirm(action);
  // Child screens re-render the reader on Pop; GO_HOME/DELETE_CACHE leave the
  // reader entirely. In-place actions would otherwise leave the closed panel
  // on screen.
  if (action != MA::GO_HOME && action != MA::DELETE_CACHE) requestUpdate();
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';
  int targetSpineIndex = sameFile ? currentSpineIndex : epub->resolveHrefToSpineIndex(hrefStr);

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    return;
  }

  if (savePosition) {
    // Full: drop the oldest entry so Back always returns from the newest jump.
    if (footnoteDepth == MAX_FOOTNOTE_DEPTH) {
      std::copy(savedPositions + 1, savedPositions + MAX_FOOTNOTE_DEPTH, savedPositions);
      footnoteDepth--;
    }
    // A child screen (menu, footnote list) may have released the section;
    // nextPageNumber then holds the page it was on.
    const int page = section ? section->currentPage : nextPageNumber;
    savedPositions[footnoteDepth] = {currentSpineIndex, page};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, page);
  }

  {
    RenderLock lock;
    clearDeferredReposition();
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::saveLinkStack() const {
  // [depth] then depth x (spine u16 LE, page u16 LE)
  uint8_t data[1 + MAX_FOOTNOTE_DEPTH * 4];
  size_t size = 0;
  data[size++] = static_cast<uint8_t>(footnoteDepth);
  for (int i = 0; i < footnoteDepth; i++) {
    const SavedPosition& pos = savedPositions[i];
    data[size++] = pos.spineIndex & 0xFF;
    data[size++] = (pos.spineIndex >> 8) & 0xFF;
    data[size++] = pos.pageNumber & 0xFF;
    data[size++] = (pos.pageNumber >> 8) & 0xFF;
  }
  HalFile f;
  if (!Storage.openFileForWrite("ERS", epub->getCachePath() + "/links.bin", f)) return;
  if (f.write(data, size) != size) LOG_ERR("ERS", "Failed to write link stack");
}

void EpubReaderActivity::loadLinkStack() {
  const std::string path = epub->getCachePath() + "/links.bin";
  if (!Storage.exists(path.c_str())) return;
  {
    HalFile f;
    uint8_t data[1 + MAX_FOOTNOTE_DEPTH * 4];
    if (Storage.openFileForRead("ERS", path, f)) {
      const int size = f.read(data, sizeof(data));
      const int depth = size > 0 ? data[0] : 0;
      if (depth >= 1 && depth <= MAX_FOOTNOTE_DEPTH && size == 1 + depth * 4) {
        const int spineCount = epub->getSpineItemsCount();
        bool valid = true;
        for (int i = 0; i < depth; i++) {
          const uint8_t* p = data + 1 + i * 4;
          savedPositions[i] = {p[0] | (p[1] << 8), p[2] | (p[3] << 8)};
          valid = valid && savedPositions[i].spineIndex < spineCount;
        }
        if (valid) {
          footnoteDepth = depth;
          LOG_DBG("ERS", "Loaded link stack, depth %d", depth);
        }
      }
    }
  }
  // Consumed once: a later exit rewrites it, and an unclean shutdown must not
  // resurrect a stale stack.
  Storage.remove(path.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock;
    clearDeferredReposition();
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  BookmarkFile::load(epub->getPath(), cachedBookmarks);
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    if (!epub || ttfPage < 0 || ttfPage >= static_cast<int>(ttfPageCount)) return;
    LOG_DBG("ERS", "Toggle TTF bookmark at spine %d, page %d", currentSpineIndex, ttfPage);
    const int currentPage = ttfPage;
    const int pageCount = static_cast<int>(ttfPageCount);
    // Page-text summary from the cached page runs (cold path).
    std::string pageText;
    {
      RenderLock lock;  // render task owns/uses the scratch arena
      const size_t mark = ttf_->scratch().mark();
      freeink::book::Page page{};
      if (ttf_->readPage(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(currentPage), &page)) {
        for (uint16_t r = 0; r < page.runCount; ++r) {
          pageText.append(page.runs[r].text, page.runs[r].len);
        }
      }
      ttf_->scratch().release(mark);
    }
    SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
    const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);
    const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
    cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                         [&](const BookmarkEntry& b) {
                                           return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                          pageRange);
                                         }),
                          cachedBookmarks.end());
    if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
      bookmarkRemoved = true;
      currentPageBookmarked = false;
    } else {
      // Bookmarks keep spine + page under the TTF path (no visible-text
      // offset substrate in v1, §3.5).
      BookmarkEntry entry;
      entry.percentage = progress.percentage;
      entry.xpath = progress.xpath;
      entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
      entry.computedSpineIndex = currentSpineIndex;
      entry.computedChapterPageCount = pageCount;
      entry.computedChapterProgress = currentPage;
      cachedBookmarks.insert(cachedBookmarks.begin(), entry);
      bookmarkRemoved = false;
      currentPageBookmarked = true;
    }
    if (!BookmarkFile::save(epub->getPath(), cachedBookmarks)) {
      LOG_ERR("ERS", "Failed to save bookmarks");
    }
    requestUpdate();
    return;
  }
#endif
  if (!section || !epub) return;
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock;
    pageCount = section->estimatedTotalPages();
    currentPage = section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    const std::optional<uint32_t> offset =
        currentPageVisibleOffset.has_value() ? currentPageVisibleOffset
        : (currentPage >= 0 && currentPage < section->pageCount)
            ? section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))
            : std::nullopt;
    if (offset.has_value()) {
      entry.visibleTextOffset = *offset;
      entry.hasVisibleTextOffset = true;
    }
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  if (!BookmarkFile::save(epub->getPath(), cachedBookmarks)) {
    LOG_ERR("ERS", "Failed to save bookmarks");
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    if (!epub || cachedBookmarks.empty() || ttfPage < 0 || ttfPageCount == 0) {
      currentPageBookmarked = false;
      return;
    }
    const int pageCount = static_cast<int>(ttfPageCount);
    const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, ttfPage, pageCount);
    currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
      return bookmarkMatchesProgress(b, currentSpineIndex, ttfPage, pageCount, pageRange);
    });
    return;
  }
#endif
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const int pageCount = section->estimatedTotalPages();
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, section->currentPage, pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, pageCount, pageRange);
  });
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    info.currentPage = ttfPage + 1;
    info.totalPages = static_cast<int>(ttfPageCount);
    if (epub && epub->getBookSize() > 0 && info.totalPages > 0 && ttfPage >= 0) {
      const float chapterProgress = static_cast<float>(ttfPage) / static_cast<float>(info.totalPages);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
      info.progressKnown = true;
    }
    return info;
  }
#endif
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->estimatedTotalPages();
    if (epub && epub->getBookSize() > 0 && info.totalPages > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(info.totalPages);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
      info.progressKnown = true;
    }
  }
  return info;
}

int EpubReaderActivity::getProgressBasisPoints() const {
  if (isAtEndOfBook()) return 10000;
  if (!epub || !section || epub->getBookSize() == 0) return getProgressPercent() * 100;
  const int totalPages = section->estimatedTotalPages();
  if (totalPages <= 0) return getProgressPercent() * 100;
  const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(totalPages);
  const int basisPoints =
      static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 10000.0f + 0.5f);
  return std::clamp(basisPoints, 0, 10000);
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  localPos.hasResolvedSpineIndex = true;
  localPos.hasMappedPage = true;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    if (const auto offset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))) {
      localPos.visibleTextOffset = *offset;
      localPos.hasVisibleTextOffset = true;
    }
  }
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
