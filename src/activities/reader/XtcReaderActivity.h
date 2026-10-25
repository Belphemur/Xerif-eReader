#pragma once

#include <Xtc.h>

#include <memory>
#include <string>

#ifdef READING_STATS_ENABLED
#include "BookReadingStats.h"
#include "GlobalReadingStats.h"
#endif

#include "ReaderActivity.h"

class XtcReaderActivity final : public ReaderActivity {
  std::shared_ptr<Xtc> xtc;
  uint32_t currentPage = 0;

  enum class StatusBarOverlayPosition { Bottom, Top };
  struct StatusBarInfo {
    int currentPage = 0;
    int pageCount = 0;
    std::string title;
  };

  void renderPage();
  void openChapterSelection();
  void renderStatusBarOverlay(GfxRenderer& renderer, StatusBarOverlayPosition position) const;
  StatusBarInfo getStatusBarInfo() const;
  // Writes `page` into progress.bin; pass xtc->getPageCount() for the
  // terminal record (past the final page, matching the turn crossing).
  void saveProgress(const uint32_t page) const;
  void loadProgress();
#ifdef READING_STATS_ENABLED
  BookReadingStats stats;
  GlobalReadingStats globalStats;
  // Finish-reading prompt (docs/design/2026-10-20-finish-reading-prompt.md):
  // UNROUNDED progress percent at the last settled evaluation (rounding to an
  // int would swallow a forward turn like 98.6% → 99.1%: both round to 99, and
  // ≥99.5% then suppresses the 1% band forever), so backward jumps never fire
  // and firing resumes when progress moves forward again (ADR-5).
  float lastPromptProgress = -1.0f;
  // Performance guard for maybePromptFinishReading(): only re-evaluate the
  // policy when the underlying reading position actually changed.
  int lastPromptSpineIndex = -1;
  int lastPromptPage = -1;
  // Session-scoped completion guard (ADR-5 contract): only a completion
  // confirmed in THIS reader session pins lastBookProgressPercent at 100 in
  // onExit(). A book completed in an earlier session and reopened for a
  // reread saves the reread position like any other book.
  bool completedThisSession = false;
  void maybePromptFinishReading();

  void onGoHomeRequested() override;
  void setBookCompleted(bool completed);
  void goHomeOrShowCompletionAchievement();
  void syncFinishedBookIndex();
  BookReadingStats achievementStatsPreview() const;
  float getCurrentBookProgressPercent() const;
#endif

  bool loadBook() override;
  void loop() override;
  std::string getBookTitle() const override { return xtc ? xtc->getTitle() : ""; }
  std::string getBookAuthor() const override { return xtc ? xtc->getAuthor() : ""; }
  std::string getBookThumbBmpPath() const override { return xtc ? xtc->getThumbBmpPath() : ""; }
  bool handleFormatInput() override;
  void renderBook() override;
  void applyInitialOrientation() override;

 public:
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                             bool allowFastInitialRefresh)
      : ReaderActivity("XtcReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  ~XtcReaderActivity() override = default;

#ifdef READING_STATS_ENABLED
  void onEnter() override;
  void onExit() override;
#endif

  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  ScreenshotInfo getScreenshotInfo() const override;
};
