#include "XtcReaderActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#ifdef READING_STATS_ENABLED
#include "BookStatsActivity.h"
#include "FinishReadingPromptActivity.h"
#include "FinishReadingPromptPolicy.h"
#include "FinishedBooksIndex.h"
#include "ReadingStatsUtils.h"
#include "activities/util/ConfirmationActivity.h"
#endif

#include "CrossPointSettings.h"
#include "ProgressFile.h"
#include "ReaderActivity.h"
#include "ReaderUtils.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

bool XtcReaderActivity::loadBook() {
  auto loadedXtc = makeUniqueNoThrow<Xtc>(bookPath, "/.crosspoint");
  if (!loadedXtc) {
    LOG_ERR("XTR", "Failed to allocate XTC object");
    return false;
  }
  if (!loadedXtc->load()) {
    LOG_ERR("XTR", "Failed to load XTC");
    return false;
  }
  xtc = std::move(loadedXtc);
  xtc->setupCacheDir();
  loadProgress();
  return true;
}

#ifdef READING_STATS_ENABLED
void XtcReaderActivity::onEnter() {
  ReaderActivity::onEnter();
  if (xtc && SETTINGS.shouldTrackReadingStats()) {
    stats = BookReadingStats::load(xtc->getCachePath());
    globalStats = GlobalReadingStats::load();
    // ADR-5: the finish-reading prompt fires only on forward page movement,
    // never on open — the first settled evaluation seeds the baseline.
    lastPromptProgress = -1.0f;
    lastPromptPage = -1;
    // Session-scoped completion guard: a book reopened from a previously
    // completed state starts the session WITHOUT a completion confirmation, so
    // onExit() saves the reread position instead of re-pinning 100%.
    completedThisSession = false;
  }
}

void XtcReaderActivity::loop() {
#ifdef READING_STATS_ENABLED
  // Finish-reading prompt (ADR-5): settled-frame evaluation after committed
  // forward progress. Deferred while a gesture is in flight (inputPriority);
  // the helper itself gates overlays, the end-of-book menu, and builds.
  if (!mappedInput.rawInputPriority()) maybePromptFinishReading();
#endif
  ReaderActivity::loop();
}

#ifdef READING_STATS_ENABLED
void XtcReaderActivity::maybePromptFinishReading() {
  if (!xtc || !SETTINGS.shouldTrackReadingStats()) return;
  // ADR-3: the toggle gates only the new threshold modals (the existing 100%
  // prompt and the End-of-Book screen are untouched).
  if (!SETTINGS.finishReadingPrompt) return;
  if (stats.isCompleted) return;            // ADR-5: !stats.isCompleted
  if (stats.finishPromptDismissed) return;  // ADR-2: permanent per-book latch
  // Settled frame only: never at/past the final section boundary (the existing
  // end-of-book flow owns completion there).
  if (isAtEndOfBook()) return;

  // Performance guard: only re-evaluate the policy when the reading position
  // actually changed (progress is a pure function of currentPage/pageCount).
  if (currentPage == lastPromptPage) return;

  const float progress = getCurrentBookProgressPercent();
  // Clamp the unrounded percent to the valid range before any comparison —
  // the baseline must track real progress, not a display-rounded integer
  // (a rounded baseline can swallow a forward turn: 98.6% → 99.1% both round
  // to 99, and ≥99.5% then suppresses the 1% band forever).
  if (progress < 0.0f || progress > 100.0f) return;
  // Cache only evaluated positions: an invalid-percent frame must be retried
  // on the next settled frame instead of being swallowed by the guard.
  lastPromptPage = currentPage;

  // ADR-5: fire on forward progress only. A backward jump re-entering the
  // zone does not fire; firing resumes when progress moves forward again.
  // The first settled evaluation seeds the baseline and never fires — a book
  // opened already in the zone must not prompt without a real forward turn.
  if (lastPromptProgress < 0.0f) {
    lastPromptProgress = progress;
    return;
  }
  if (progress <= lastPromptProgress) {
    lastPromptProgress = progress;
    return;
  }
  lastPromptProgress = progress;

  // Unrounded progress: the policy's own boundaries (90/95/99/99.5) work on
  // the real percentage, not the display-rounded integer.
  const auto decision = finish_prompt::evaluate(progress, true, stats.isCompleted, stats.finishPromptDismissed);
  if (decision != finish_prompt::Decision::Fire10 && decision != finish_prompt::Decision::Fire5 &&
      decision != finish_prompt::Decision::Fire1) {
    return;
  }
  const uint8_t remaining =
      decision == finish_prompt::Decision::Fire1 ? 1 : (decision == finish_prompt::Decision::Fire5 ? 5 : 10);
  auto prompt = makeUniqueNoThrow<FinishReadingPromptActivity>(renderer, mappedInput, remaining);
  if (!prompt) {
    LOG_ERR("XTR", "OOM: finish-reading prompt");
    return;
  }
  startActivityForResult(std::move(prompt), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      // ADR-2: one dismissal silences the book permanently — no later
      // thresholds fire either. The latch clears only on a completion-state
      // change (setBookCompleted).
      stats.finishPromptDismissed = true;
      if (xtc) stats.save(xtc->getCachePath());
      requestUpdate();
      return;
    }
    // ADR-1: route through exactly the existing completion path.
    setBookCompleted(true);
    // Completed from the prompt: persist the terminal page like the turn
    // crossing does; loadProgress clamps it to the last page on reopen.
    if (xtc) saveProgress(xtc->getPageCount());
    goHomeOrShowCompletionAchievement();
  });
}
#endif

void XtcReaderActivity::onExit() {
  if (xtc && SETTINGS.shouldTrackReadingStats()) {
    const uint32_t pageCount = xtc->getPageCount();
    // A book completed in THIS session keeps the 100% pinned at the confirm
    // site (setBookCompleted) — recomputing here would report the
    // pre-confirmation page's partial percent in the stats. A book completed
    // in an EARLIER session and reopened for a reread does not: the reader
    // must save the current position like any other book.
    const bool preservesProgress = stats.isCompleted && completedThisSession;
    if (pageCount > 0 && preservesProgress) {
      const uint32_t clampedPage = std::min(currentPage, pageCount - 1);
      const int progress = std::clamp(
          static_cast<int>((static_cast<float>(clampedPage) + 1.0f) * 100.0f / static_cast<float>(pageCount) + 0.5f), 0,
          100);
      stats.lastBookProgressPercent = static_cast<uint8_t>(progress);
    }
    stats.save(xtc->getCachePath());
    globalStats.save();
    if (stats.isCompleted) syncFinishedBookIndex();
  }
  ReaderActivity::onExit();
}

void XtcReaderActivity::syncFinishedBookIndex() {
  if (xtc && xtc->getTitle().empty()) {
    LOG_ERR("XTR", "Skipping finished-book entry: empty title");
    return;
  }
  if (!xtc || !FinishedBooksIndex::recordCanonical(xtc->getPath(), xtc->getCachePath(), xtc->getTitle(),
                                                   xtc->getAuthor(), stats)) {
    LOG_ERR("XTR", "Failed to synchronize finished-book entry");
  }
}

BookReadingStats XtcReaderActivity::achievementStatsPreview() const {
  BookReadingStats preview = stats;
  if (!SETTINGS.shouldTrackReadingStats()) return preview;

  const uint32_t pageCount = xtc ? xtc->getPageCount() : 0;
  if (pageCount > 0) {
    const int progress = std::clamp(static_cast<int>(getCurrentBookProgressPercent() + 0.5f), 0, 100);
    preview.lastBookProgressPercent = static_cast<uint8_t>(progress);
  }
  return preview;
}

void XtcReaderActivity::goHomeOrShowCompletionAchievement() {
  const uint32_t pageCount = xtc ? xtc->getPageCount() : 0;
  const bool atEndOfBook = xtc && pageCount > 0 && currentPage >= pageCount;
  const bool onFinalPage = xtc && pageCount > 0 && currentPage == pageCount - 1;
  const bool displaysHundredPercent = getCurrentBookProgressPercent() >= 99.5f;

  // Crossing past the final readable page is definitive completion evidence.
  // A final page or rounded 100% can also come from a jump, so ask first.
  if (!stats.isCompleted && atEndOfBook) {
    setBookCompleted(true);
  } else if (!stats.isCompleted && !stats.completionPromptDismissedAtHundred &&
             (onFinalPage || displaysHundredPercent)) {
    auto prompt = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_MARK_FINISHED_PROMPT), "");
    if (!prompt) {
      LOG_ERR("XTR", "OOM: completion prompt");
      onGoHome();
      return;
    }
    startActivityForResult(std::move(prompt), [this](const ActivityResult& result) {
      if (result.isCancelled) {
        stats.completionPromptDismissedAtHundred = true;
        if (xtc) stats.save(xtc->getCachePath());
        onGoHome();
        return;
      }
      setBookCompleted(true);
      // Completed from the prompt: the turn crossing never ran, so push the
      // record past the final page like the crossing does; loadProgress
      // clamps it to the last page on reopen.
      if (xtc) saveProgress(xtc->getPageCount());
      goHomeOrShowCompletionAchievement();
    });
    return;
  }

  if (!stats.isCompleted || !stats.completionAchievementPending || !xtc) {
    onGoHome();
    return;
  }

  auto achievement = makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, xtc->getTitle(), xtc->getAuthor(),
                                                          achievementStatsPreview(), "", globalStats,
                                                          BookStatsActivity::InitialPage::Achievement);
  if (!achievement) {
    LOG_ERR("XTR", "OOM: completion achievement screen");
    onGoHome();
    return;
  }
  stats.completionAchievementPending = false;
  stats.save(xtc->getCachePath());
  activityManager.replaceActivity(std::move(achievement));
}

void XtcReaderActivity::setBookCompleted(bool completed) {
  if (!xtc || stats.isCompleted == completed) return;

  stats.isCompleted = completed;
  // Session-scoped completion guard: the 100% pin in onExit() only applies to
  // a completion confirmed during THIS reader session.
  completedThisSession = completed;
  stats.completionAchievementPending = completed;
  stats.completionPromptDismissedAtHundred = false;
  // A completed book is done: pin the percent so onExit()'s recomputation
  // skip (and the home screen) never report a partial pre-confirmation value.
  if (completed) stats.lastBookProgressPercent = 100;
  // ADR-2: the finish-reading latch clears only on a completion-state change
  // — never on a forward page turn (unlike completionPromptDismissedAtHundred).
  stats.finishPromptDismissed = false;
  if (completed && !stats.finishedDateManual && !stats.finishedDate.isValid()) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now)) stats.finishedDate = now.date;
  }

  if (completed) {
    if (globalStats.completedBooks < UINT32_MAX) ++globalStats.completedBooks;
  } else if (globalStats.completedBooks > 0) {
    --globalStats.completedBooks;
  }

  stats.save(xtc->getCachePath());
  globalStats.save();
  syncFinishedBookIndex();
}

void XtcReaderActivity::onGoHomeRequested() {
  if (!xtc || !SETTINGS.shouldTrackReadingStats()) {
    onGoHome();
    return;
  }
  goHomeOrShowCompletionAchievement();
}

float XtcReaderActivity::getCurrentBookProgressPercent() const {
  if (!xtc || xtc->getPageCount() == 0) return -1.0f;
  const uint32_t pageCount = xtc->getPageCount();
  const uint32_t clampedPage = std::min(currentPage, pageCount - 1);
  return (static_cast<float>(clampedPage) + 1.0f) * 100.0f / static_cast<float>(pageCount);
}
#endif

void XtcReaderActivity::openChapterSelection() {
  if (xtc && xtc->hasChapters() && !xtc->getChapters().empty()) {
    startActivityForResult(std::make_unique<XtcReaderChapterSelectionActivity>(renderer, mappedInput, xtc, currentPage),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               currentPage = std::get<PageResult>(result.data).page;
                               requestUpdate();
                             }
                           });
  }
}

bool XtcReaderActivity::handleFormatInput() {
  if (!xtc) {
    return false;
  }

  // Enter chapter selection activity on Confirm release or touch menu gesture
  const bool touchMenu = ReaderUtils::isTouchMenuGesture(renderer, mappedInput);
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || touchMenu) {
    if (touchMenu && xtc->hasChapters() && !xtc->getChapters().empty()) haptic_feedback::touchAction();
    openChapterSelection();
    return true;
  }
  return false;
}

void XtcReaderActivity::applyInitialOrientation() { renderer.setOrientation(GfxRenderer::Orientation::Portrait); }

void XtcReaderActivity::renderBook() {
  if (!xtc) {
    return;
  }

  renderPage();
  saveProgress(currentPage);
}

XtcReaderActivity::StatusBarInfo XtcReaderActivity::getStatusBarInfo() const {
  const auto sb = SETTINGS.statusBarSpec();
  const int bookPageCount = static_cast<int>(xtc->getPageCount());
  const int bookPage = static_cast<int>(currentPage) + 1;
  std::string title = sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE ? xtc->getTitle() : "";

  if (!xtc->hasChapters()) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  const auto& chapters = xtc->getChapters();
  const auto chapterIt = std::find_if(chapters.begin(), chapters.end(), [this](const xtc::ChapterInfo& chapter) {
    return currentPage >= chapter.startPage && currentPage <= chapter.endPage;
  });

  if (chapterIt == chapters.end() || chapterIt->endPage < chapterIt->startPage) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = chapterIt->name.empty() ? tr(STR_UNNAMED) : chapterIt->name;
  }

  return StatusBarInfo{static_cast<int>(currentPage - chapterIt->startPage) + 1,
                       static_cast<int>(chapterIt->endPage - chapterIt->startPage) + 1, std::move(title)};
}

void XtcReaderActivity::renderStatusBarOverlay(GfxRenderer& renderer, const StatusBarOverlayPosition position) const {
  const auto sb = SETTINGS.statusBarSpec();
  const bool drawBottom = sb.xtcMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_BOTTOM &&
                          position == StatusBarOverlayPosition::Bottom;
  const bool drawTop = sb.xtcMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP &&
                       position == StatusBarOverlayPosition::Top;
  if (!drawBottom && !drawTop) {
    return;
  }

  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight <= 0) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  int clearY;
  int paddingBottom = 0;
  if (position == StatusBarOverlayPosition::Bottom) {
    clearY = renderer.getScreenHeight() - orientedMarginBottom - statusBarHeight - 4;
    if (clearY < 0) {
      clearY = 0;
    }
  } else {
    clearY = orientedMarginTop;
    paddingBottom = renderer.getScreenHeight() - statusBarHeight - orientedMarginBottom - orientedMarginTop - 4;
  }
  const int clearHeight = position == StatusBarOverlayPosition::Bottom
                              ? renderer.getScreenHeight() - orientedMarginBottom - clearY
                              : statusBarHeight + 4;
  if (clearHeight > 0) {
    renderer.fillRect(0, clearY, renderer.getScreenWidth(), clearHeight, false);
  }

  const int pageCount = static_cast<int>(xtc->getPageCount());
  const int displayPage = static_cast<int>(currentPage) + 1;
  const float progress = pageCount > 0 ? (static_cast<float>(displayPage) * 100.0f) / pageCount : 0.0f;
  const auto pageInfo = getStatusBarInfo();
  GUI.drawStatusBar(renderer, progress, pageInfo.currentPage, pageInfo.pageCount, pageInfo.title, paddingBottom, 0,
                    true, false, false, nullptr);
}

void XtcReaderActivity::renderPage() {
  const uint16_t pageWidth = xtc->getPageWidth();
  const uint16_t pageHeight = xtc->getPageHeight();
  const uint8_t bitDepth = xtc->getBitDepth();

  size_t pageBufferSize;
  if (bitDepth == 2) {
    pageBufferSize = static_cast<size_t>(pageWidth) * ((static_cast<size_t>(pageHeight) + 7) / 8) * 2;
  } else {
    pageBufferSize = ((pageWidth + 7) / 8) * pageHeight;
  }

  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(pageBufferSize));
  if (!pageBuffer) {
    LOG_ERR("XTR", "Failed to allocate page buffer (%lu bytes)", pageBufferSize);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  size_t bytesRead = xtc->loadPage(currentPage, pageBuffer, pageBufferSize);
  if (bytesRead == 0) {
    LOG_ERR("XTR", "Failed to load page %lu: bufferSize=%lu bitDepth=%u error=%s", currentPage, pageBufferSize,
            bitDepth, xtc::errorToString(xtc->getLastError()));
    free(pageBuffer);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderer.clearScreen();

  const uint16_t maxSrcY = pageHeight;

  if (bitDepth == 2) {
    const size_t planeSize = static_cast<size_t>(pageWidth) * ((static_cast<size_t>(pageHeight) + 7) / 8);
    const uint8_t* plane1 = pageBuffer;
    const uint8_t* plane2 = pageBuffer + planeSize;
    const size_t colBytes = (pageHeight + 7) / 8;

    auto getPixelValue = [&](uint16_t x, uint16_t y) -> uint8_t {
      const size_t colIndex = pageWidth - 1 - x;
      const size_t byteInCol = y / 8;
      const size_t bitInByte = 7 - (y % 8);
      const size_t byteOffset = colIndex * colBytes + byteInCol;
      const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
      const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
      return (bit1 << 1) | bit2;
    };

    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }

    if (pagesUntilFullRefresh <= 1) {
      // Periodic ghost cleanup: scrub via the normal path, then run the
      // settle flavor of the grayscale base pass (DTM planes are equal after
      // the display sync, so only the gentle reinforcement cells fire).
      // Combined-base panels (Paper Mono) instead defer the base so the gray
      // planes below join it in one waveform.
      if (renderer.grayscaleCapabilities().base == HalDisplay::GrayscaleBase::Combined) {
        renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
      } else {
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        renderer.preconditionGrayscale();
      }
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    } else {
      renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
      pagesUntilFullRefresh--;
    }

    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) == 1) {
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        const uint8_t pv = getPixelValue(x, y);
        if (pv == 1 || pv == 2) {
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();

    renderer.clearScreen();
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }

    renderer.cleanupGrayscaleWithFrameBuffer();

    free(pageBuffer);

    LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit grayscale)", currentPage + 1, xtc->getPageCount());
    markPageRendered();
    return;
  } else {
    const size_t srcRowBytes = (pageWidth + 7) / 8;

    for (uint16_t srcY = 0; srcY < maxSrcY; srcY++) {
      const size_t srcRowStart = srcY * srcRowBytes;

      for (uint16_t srcX = 0; srcX < pageWidth; srcX++) {
        const size_t srcByte = srcRowStart + srcX / 8;
        const size_t srcBit = 7 - (srcX % 8);
        const bool isBlack = !((pageBuffer[srcByte] >> srcBit) & 1);

        if (isBlack) {
          renderer.drawPixel(srcX, srcY, true);
        }
      }
    }
  }

  free(pageBuffer);

  if (SETTINGS.statusBarSpec().xtcMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP) {
    renderStatusBarOverlay(renderer, StatusBarOverlayPosition::Top);
  } else {
    renderStatusBarOverlay(renderer, StatusBarOverlayPosition::Bottom);
  }

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  LOG_DBG("XTR", "Rendered page %lu/%lu (%u-bit)", currentPage + 1, xtc->getPageCount(), bitDepth);
  markPageRendered();
}

bool XtcReaderActivity::pageTurn(bool isForward) {
  if (!xtc) return false;
  if (isForward) {
#ifdef READING_STATS_ENABLED
    // A declined 100%-completion prompt applies only until the reader moves
    // forward again; otherwise it would suppress the prompt on a later exit.
    stats.completionPromptDismissedAtHundred = false;
#endif
    if (currentPage < xtc->getPageCount()) {
      currentPage++;
#ifdef READING_STATS_ENABLED
      // Crossing past the last readable page is definitive completion.
      if (!stats.isCompleted && SETTINGS.shouldTrackReadingStats() && currentPage == xtc->getPageCount())
        setBookCompleted(true);
#endif
      // The EOB render path bypasses renderBook()'s save, so persist the
      // terminal page synchronously when the forward turn crosses it.
      if (currentPage == xtc->getPageCount()) saveProgress(currentPage);
      return true;
    }
  } else {
    if (currentPage > 0) {
      currentPage--;
      return true;
    }
  }
  return false;
}

bool XtcReaderActivity::skipPages(int amount) {
  if (!xtc) return false;
#ifdef READING_STATS_ENABLED
  if (amount > 0 && SETTINGS.shouldTrackReadingStats()) stats.completionPromptDismissedAtHundred = false;
#endif
  int newPage = static_cast<int>(currentPage) + amount;
  if (newPage < 0) newPage = 0;
  if (newPage > static_cast<int>(xtc->getPageCount())) newPage = static_cast<int>(xtc->getPageCount());
  if (newPage != static_cast<int>(currentPage)) {
    currentPage = static_cast<uint32_t>(newPage);
#ifdef READING_STATS_ENABLED
    if (SETTINGS.shouldTrackReadingStats() && !stats.isCompleted && currentPage == xtc->getPageCount())
      setBookCompleted(true);
#endif
    if (currentPage == xtc->getPageCount()) {
      // The EOB render path bypasses renderBook()'s save, so persist the
      // terminal page synchronously when a skip crosses it.
      saveProgress(currentPage);
    }
    return true;
  }
  return false;
}

bool XtcReaderActivity::isAtEndOfBook() const { return xtc && currentPage >= xtc->getPageCount(); }

void XtcReaderActivity::onReturnFromEndOfBook() {
  if (xtc && xtc->getPageCount() > 0) {
    currentPage = xtc->getPageCount() - 1;
  } else {
    currentPage = 0;
  }
}

void XtcReaderActivity::saveProgress(const uint32_t page) const {
  if (!xtc) return;
  uint8_t data[4];
  data[0] = page & 0xFF;
  data[1] = (page >> 8) & 0xFF;
  data[2] = (page >> 16) & 0xFF;
  data[3] = (page >> 24) & 0xFF;
  if (!ProgressFile::writeAtomic(xtc->getCachePath(), data, sizeof(data))) {
    LOG_ERR("XTC", "Failed to save progress: page %lu", static_cast<unsigned long>(page));
  }
}

void XtcReaderActivity::loadProgress() {
  if (!xtc) return;
  HalFile f;
  if (Storage.openFileForRead("XTC", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      if (currentPage >= xtc->getPageCount() && xtc->getPageCount() > 0) {
        currentPage = xtc->getPageCount() - 1;
      }
      LOG_DBG("XTC", "Loaded progress: page %lu/%lu", currentPage + 1, xtc->getPageCount());
    }
  }
}

ScreenshotInfo XtcReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Xtc;
  if (xtc) {
    const std::string t = xtc->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
    const uint32_t pageCount = xtc->getPageCount();
    info.totalPages = pageCount;
    uint32_t clampedPage = (pageCount > 0 && currentPage >= pageCount) ? pageCount - 1 : currentPage;
    info.progressPercent = pageCount > 0 ? xtc->calculateProgress(clampedPage) : 0;
    info.progressKnown = pageCount > 0;
    // XTC progress already counts the visible page as read.
    info.endProgressPercent = info.progressPercent;
    info.currentPage = static_cast<int>(clampedPage) + 1;
  } else {
    info.currentPage = currentPage + 1;
  }
  return info;
}
