#pragma once

#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/PageLink.h>
#include <Epub/Section.h>
#include <Memory.h>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include "BookmarkEntry.h"
#include "ChapterPosition.h"
#include "ClipSelectionActivity.h"
#include "EpubReaderMenuActivity.h"
#include "LinkNoteTitle.h"
#include "ProgressManager.h"
#include "ProgressMapper.h"
#include "ReaderActivity.h"
#include "ReaderToolbarUi.h"
#include "TouchLongPressMode.h"
#include "components/OptionPopup.h"
#if defined(CROSSPOINT_TTF_READER)
#include "FibpPrefetchWorker.h"
#include "QuickPageCapture.h"
#include "TtfBookRuntime.h"
#include "TtfResolvePolicy.h"
#endif
#ifdef READING_STATS_ENABLED
#include "BookReadingStats.h"
#include "GlobalReadingStats.h"
#include "ReadingStatsUtils.h"
#endif

// Turn-dispatch payload structs (ReaderUtils.h, included by the .cpp only).
namespace ReaderUtils {
struct PageTurnResult;
struct TouchPageTurn;
}  // namespace ReaderUtils

class EpubReaderActivity final : public ReaderActivity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  std::string pendingAnchor;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  std::optional<uint32_t> cachedVisibleTextOffset;
  std::optional<uint32_t> currentPageVisibleOffset;
  std::optional<uint32_t> pendingOffsetJump;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  int8_t pendingManualTurn = 0;
  bool pendingManualTurnTouch = false;
  bool pendingPercentJump = false;
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
  bool skipNextButtonCheck = false;
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool showDictionaryMessage = false;
  // When set, the dictionary popup shows the TTF-path "not available" string
  // instead of the no-dictionary one (kody, PR #113).
  bool dictionaryMessageTtf = false;
  unsigned long dictionaryMessageTime = 0UL;
  bool showFontDamagedMessage = false;
  unsigned long fontDamagedMessageTime = 0UL;
  bool showClippingMessage = false;
  StrId clippingMessage = StrId::STR_CLIPPING_SAVED;
  unsigned long clippingMessageTime = 0UL;
  bool currentPageBookmarked = false;
  int idlePrewarmSpine = -1;
  int idlePrewarmPage = -1;
  unsigned long idlePrewarmRetryAt = 0UL;
  std::unique_ptr<Section> nextSectionPrefetch = nullptr;
  int nextSectionSpineIndex = -1;
  unsigned long lastRenderCompleteMs = 0;
  bool bookmarkRemoved = false;
  std::vector<BookmarkEntry> cachedBookmarks;
  bool recentsEntryRemoved = false;
  // Latched by saveFinishedProgress() success: once the terminal record is on
  // disk this session, the EOB-entry save in onEndOfBookRendered is a no-op.
  bool eobTerminalProgressSaved = false;
  unsigned long bookmarkMessageTime = 0UL;
  bool pendingReadFolderMove = false;

  // Toolbar reader menu (SETTINGS.readerMenuStyle == READER_MENU_TOOLBAR): drawn
  // over the page instead of pushing the full-screen list menu. Select opens the
  // Toolbar; its tools open the Contents/Text/More bottom-sheet panels.
  enum class Overlay { None, Toolbar, Contents, Text, More, Stats };
  // Toolbar tool focus. 0=Contents, 1=Text, then the stats tile
  // (READING_STATS_ENABLED only) and More last — kTool* + kToolTileCount are
  // the source of truth, never bare literals.
  static constexpr int kToolContents = 0;
  static constexpr int kToolText = 1;
  static constexpr int kToolStats = 2;
  static constexpr int kToolMore =
#ifdef READING_STATS_ENABLED
      3;
#else
      2;
#endif
  // Number of tiles in the tool row (kToolMore is the last one).
  static constexpr int kToolTileCount = kToolMore + 1;
  // Stats panel rows: This Book / All Books / Reading Rhythm / Finished Books.
  static constexpr int kStatsPanelRows = 4;
  Overlay overlay = Overlay::None;
  int focusedTool = 0;  // toolbar tool focus: kToolContents..kToolMore
  int panelIndex = 0;   // selected row within the active panel
  // Panel list navigation: a tap steps one row, a hold jumps PANEL_HOLD_STEP rows in one go
  // (a contents list runs to hundreds of chapters). One jump per hold, not a repeat -- every
  // step repaints the panel, so repeating is bounded by the e-ink refresh anyway and reads as
  // sluggish. True once a hold has jumped, so the release that ends it is swallowed.
  static constexpr unsigned long PANEL_HOLD_MS = 1500;
  static constexpr int PANEL_HOLD_STEP = 10;
  bool panelHoldJumped = false;
  // Whether the panel draws its cursor row. Button boards always do; touch
  // boards only once a button has moved it, so a tapped row is not left inverted.
  bool panelCursorShown = false;
  // FreeInkUI chrome + tap targets for the overlay; created when it opens,
  // released when it closes.
  std::unique_ptr<ReaderToolbarUi> toolbarUi;
  // Modal option picker over the panel (same component the Settings screens
  // use), for enum rows: font family / size / line spacing / alignment /
  // orientation / auto page turn. Capacity 33 covers built-in + the TTF
  // scanner's 32-family cap; only the popup's visible page has touch targets.
  // Toggle rows stay one-tap toggles, as in Settings.
  OptionPopup<33, 8> overlayPopup;
#if defined(CROSSPOINT_TTF_READER)
  // Full-screen font preview (design 2026-09-27): the Text panel's size and
  // family rows push FontPreviewActivity; the close contract routes through
  // applyReaderTextSettings().
  void openFontPreview();
  // FontPreviewActivity host seam: runs before the preview's first loader
  // reload after a family change (borrowed-byte lifetime, issue #168).
  void onPreviewFontChanging();
#endif
  // True while a clean-page snapshot (renderer.storeBwBuffer) backs the open
  // overlay, letting panel->toolbar steps restore the page without a full
  // re-render. Discarded on close / whenever the page under the overlay changes.
  bool overlayPageStored = false;
  // A background build step lent the framebuffer: it came back white while the panel still shows the
  // page. Until renderBook() redraws, nothing may be painted straight onto it. Set by the loop task,
  // cleared by the render task.
  std::atomic<bool> pageBufferStale{false};
  // True while a deferred overlay chrome refresh (pushOverlayRefresh) may still
  // be running on the panel. settleOverlayRefresh() must run before the
  // framebuffer is touched or another differential refresh is pushed. Atomic:
  // written by the render/loop tasks and read in ~EpubReaderActivity().
  std::atomic<bool> overlayRefreshPending{false};
  // Set when a settle gave up after OVERLAY_REFRESH_SETTLE_TIMEOUT_MS: the
  // panel is wedged, so later settles only single-check refreshBusy() instead
  // of re-polling the full timeout, and blocking driver calls are skipped.
  // Only touched under the RenderLock, hence not atomic.
  bool overlaySettleTimedOut = false;
  // Throttle for retryDeferredOverlayPush()'s non-blocking re-checks.
  uint32_t overlayPushRetryMs = 0;
  void pushOverlayRefresh();
  void settleOverlayRefresh();
  void retryDeferredOverlayPush();
  int autoTurnOption = 0;  // current auto page-turn rate index (More panel)
  std::vector<EpubReaderMenuActivity::MenuItem> moreItems;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  std::vector<PageLink> currentPageLinks;
  int currentPageLinkMarginLeft = 0;
  int currentPageLinkMarginTop = 0;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;
  // The back-stack outlives the reader (sleep, home) in links.bin so Back
  // still returns to where a followed link was tapped.
  void saveLinkStack() const;
  void loadLinkStack();
  // Terminal progress record: spine == spineCount marks the book read past
  // its last section (reopen treats it as done and starts fresh; sync
  // reports a finished position). Every completion path writes it.
  bool saveFinishedProgress();

#ifdef READING_STATS_ENABLED
  BookReadingStats stats;
  GlobalReadingStats globalStats;
  ReadingStatsDateTime sessionStartLocalDateTime;
  bool hasSessionStartLocalDateTime = false;
  uint32_t sessionReadingSeconds = 0;
  // Forward page turns completed this session; the Avg Session window only
  // records sessions with real engagement (SESSION_MIN_PAGE_TURNS), not a
  // book merely left open.
  uint16_t sessionPageTurns = 0;
  unsigned long pageShownAtMs = 0UL;
  // Words on the page currently rendered, cached at render time for the pace
  // sample taken on the next forward turn.
  uint16_t currentPageWordsOnPage = 0;

  bool currentPageReadingSecondsForStats(uint32_t& seconds) const;
  void recordCurrentPageReadingTime();
  void recordForwardPagePaceSample(uint32_t seconds, uint16_t wordsOnPage);
  // Finish-reading prompt (docs/design/2026-10-20-finish-reading-prompt.md):
  // fires on settled frames after forward progress crosses the 10/5/1%
  // remaining bands. lastPromptProgress is the UNROUNDED progress percent at
  // the last settled evaluation (rounding to an int would swallow a turn like
  // 98.6% → 99.1%: both round to 99, and then ≥99.5% suppresses the 1% band
  // forever), so backward jumps never fire and firing resumes when progress
  // moves forward again (ADR-5).
  float lastPromptProgress = -1.0f;
  // Performance guard for maybePromptFinishReading(): the spine walk in
  // computeBookProgressPercent runs on every settled loop pass otherwise.
  // Only re-evaluate when the page/spine inputs actually changed.
  int lastPromptSpineIndex = -1;
  int lastPromptPage = -1;
  // Session-scoped completion guard (ADR-5 contract): only a completion
  // confirmed in THIS reader session pins lastBookProgressPercent at 100 in
  // onExit(). A book completed in an earlier session and reopened for a
  // reread saves the reread position like any other book.
  bool completedThisSession = false;
  void maybePromptFinishReading();
  // Completion/achievement flow: real end auto-completes, while a final-page
  // or 100% exit asks first and latches a declined prompt.
  void onGoHomeRequested() override;
  void setBookCompleted(bool completed);
  void goHomeOrShowCompletionAchievement();
  // Imports only user-editable completion state saved by BookStatsActivity;
  // live session counters stay authoritative in memory.
  void applyBookStatsEditsFromDisk();
  void syncFinishedBookIndex();
  void handleBookStatsReturn();
  BookReadingStats achievementStatsPreview(uint32_t* pendingReadingSeconds = nullptr) const;
#endif

  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  bool partialRebuildStartFailed = false;

#if defined(CROSSPOINT_TTF_READER)
  // Native-TTF page source (design §3.5). Null until loadBook() succeeds
  // (a failed open falls back to the legacy Section path — build-flag kill
  // switch). The legacy section members above stay the single position
  // mirror for chrome: nextPageNumber/cachedChapterTotalPageCount are kept
  // in sync with the TTF page state so renderStatusBar/KOReader/bookmarks
  // read the same values on both paths.
  std::unique_ptr<freeink::book::TtfBookRuntime> ttf_;
  // Index-ahead prefetch worker (R1): transient, per book. Created lazily on
  // the first TTF render (generation known), cancelled in onExit(). Inert on
  // non-PSRAM/stb builds (stub).
  std::unique_ptr<freeink::book::FibpPrefetchWorker> fibpWorker_;
  bool fibpBegun_ = false;
  char fibpFamily_[48] = {};  // family the worker's faces were built for
  uint32_t fibpNotifiedGen_ = 0;
  bool fibpDeferred_ = false;           // a build is delegated to the worker
  bool fibpDeferParkLatched_ = false;   // the delegation ever parked the reader
  unsigned long fibpDeferPollMs_ = 0;   // last deferred-build poll (throttle)
  unsigned long fibpDeferStartMs_ = 0;  // when the current delegation began
  // Soak addendum: the opened chapter's partial completion handed to the
  // worker (requestResumeClaim) while the reader paints from the partial.
  int16_t fibpResumeClaimedSpine_ = -1;
  bool fibpResumeSeenBuilding_ = false;  // worker observed holding the claim
  unsigned long fibpResumeRequestMs_ = 0;
  int ttfSpine = -1;          // spine the runtime's reader/session belong to
  int ttfPage = 0;            // chapter-local page index
  uint32_t ttfPageCount = 0;  // pages available for the current chapter
  // Estimated chapter total while a build is in flight: pages built scaled
  // by the layout session's input progress (the old Section engine's
  // extrapolation, owner steer: return after a few pages and let the user
  // read; the count refines as indexing progresses).
  uint32_t ttfEstimatedPageCount() const;
  uint32_t ttfGeneration = 0;
  bool ttfGenerationValid = false;
  bool ttfRestoreLastPage = false;                  // back-navigation into the previous chapter
  std::atomic<bool> ttfFrameRenderComplete{false};  // framebuffer holds the last rendered page
  uint32_t ttfCurrentCharStart = 0;                 // charStart of the last rendered page
  uint32_t ttfCurrentCharEnd = 0;                   // furthest run end of the last rendered page — the font
                                                    // preview's window bound (ADR 2026-10-14: the preview
                                                    // re-lays ONLY the entry page's span). Always paired
                                                    // with ttfCurrentCharStart (same gate, same pages).
  // Progress-record restore data (consumed on the chapter's first open).
  bool ttfHasSavedPosition = false;
  uint16_t ttfSavedSpine = 0;
  uint32_t ttfSavedCharOffset = 0;
  uint32_t ttfSavedGeneration = 0;
  bool ttfPrefetchActive = false;  // session building the NEXT spine
  // Unified pending target for the resolver funnel (design §3.2). Single
  // definition lives in TtfResolvePolicy.h (ttf_resolve::PendingTarget) —
  // aliased here so the funnel and the policy share one struct and one
  // Kind/Origin enum (no cross-type static_cast to drift). CharOffset targets
  // come from reflow (ttfCurrentCharStart), saved position, and KOReader
  // offset jumps; Page targets from page-anchored records; AnchorHash
  // targets resolve to a char offset via charForAnchor first.
  // The origin records which latch produced the target so the funnel clears
  // exactly that latch when the target resolves.
  using PendingTarget = ttf_resolve::PendingTarget;
  PendingTarget pendingRestoreTarget_{};
  // Cycles the Indexing popup's indeterminate fill while a cover-driven
  // restore is pending (see ttfShowIndexingPopup) — one step per drawn pass.
  uint8_t indexingBarStep_ = 0;

  void renderBookTtf();
  // Grayscale base refresh shared by both TTF gray transports: cleanup cycle
  // when due, otherwise the grayscale base waveform (§11 Q7).
  void ttfDisplayGrayBase();
  // Gray-parity transports (§11 Q7): strips where the panel supports them,
  // full-frame plane buffers otherwise (UC8279 X4). Both share the base/
  // cleanup contract; scratchMark releases after the caller's arena walk.
  void renderTtfGrayStrips(const freeink::book::Page& page, const freeink::book::LayoutParams& params,
                           size_t scratchMark);
  void renderTtfGrayFullFrame(const freeink::book::Page& page, const freeink::book::LayoutParams& params,
                              size_t scratchMark);
  // Rasterizes an engine page through the active render mode (gray parity or
  // 1bpp): text + rubies + rules + images/placeholder. Shared by the reader's
  // own page render and the TTF dictionary selector's repaint hook.
  void paintTtfPage(const freeink::book::Page& page, void* font, int16_t characterSpacingPx = 0,
                    int16_t wordSpacingPx = 0);
  // Render hook handed to DictionaryWordSelectActivity: repaints the page the
  // selector was opened on (same spine/page members, reader frozen beneath).
  static void renderTtfSelectorPage(void* ctx, GfxRenderer& renderer);
  bool ttfResolveTargetPage(int& targetOut, const freeink::book::LayoutParams& params, bool& needFullBuild);
  // Consume-on-paint (§3.2): clears the restore target and exactly the source
  // latch its origin names, plus any pending preview seed. Runs only after the
  // target page was actually read for paint (see renderBookTtf).
  void consumeRestoreTarget();
  void ttfBackgroundBuildTick();
  void ttfPrefetchTick();
  // Index-ahead worker wiring (R3/R4): (re)spawn/notify on the current
  // generation and chapter, stop on exit. Single-writer handoff: while the
  // worker builds the entered chapter, the sync path defers to its commit.
  void updateFibpWorker(uint32_t generation, const freeink::book::LayoutParams& params);
  void stopFibpWorker();
  void ttfInvalidateCaches();
  void ttfShowIndexingPopup();
  void ttfSaveProgress();
  void finishTtfPageRender();
  bool ttfPageTurn(bool isForwardTurn);
  // P4 one-page-ahead prerender (port of upstream 60079923, adapted to the
  // TTF engine): after a fully committed page-N frame, paint page N+1's
  // content-only (no status bar, no flush) into the framebuffer so the next
  // FORWARD turn skips layout+build+base-paint entirely. The prerender is
  // one RenderLock-protected transaction (renderBookTtf's prerender pass)
  // that starts only after the previous pass's waitRefreshComplete(); ready
  // is published only after the paint. Any other render pass invalidates it.
  // See the review contract in docs/design/2026-09-19-freetype-render-cache.md:
  // it is also invalidated (or the current page re-rendered) on any full-FB
  // flush outside the forward turn's own commit (deferred overlay pushes,
  // popup chrome opens).
  struct TtfPreRenderedPage {
    bool ready = false;
    int16_t spineIndex = -1;
    int16_t pageIndex = -1;
    // Validity axes the consume guard must match (soak finding #3: the
    // framebuffer content is only the target page when ALL of these are
    // unchanged since the prerender painted it).
    uint32_t generation = 0;     // layoutGenerationHash at prerender time
    uint8_t orientation = 0xFF;  // GfxRenderer::Orientation snapshot
    bool monochrome = false;     // effective text raster mode snapshot
    // Chunked-pump state (soak finding #5): the prerender paints in ≤
    // kPreRenderSliceBudgetMs slices, one per loop tick, so a button press
    // is never more than one slice away from being served. pumping marks an
    // unfinished paint (the framebuffer holds a PARTIAL page — never
    // consumable); nextRun/nextChar is the paint cursor across slices
    // (run index; nextChar = intra-run glyph offset — review r5).
    bool pumping = false;
    uint16_t nextRun = 0;
    uint32_t nextChar = 0;
    uint8_t slicesUsed = 0;
  };
  TtfPreRenderedPage ttfPreRendered;
  // millis() at renderBookTtf entry — busy-window telemetry (soak finding
  // #3 P2): input is polled, so presses fully inside a long render window
  // are dropped; the window length must be visible in the soak log.
  uint32_t ttfRenderBusyStartMs = 0;
  // Set by finishTtfPageRender() after a normal page render to request the
  // prerender pass; consumed and cleared by renderBookTtf() before any state
  // checks. Never set while an overlay/deferred overlay refresh is live.
  bool ttfPendingPreRender = false;
  // Set by the ttfPageTurn() fast path to tell renderBookTtf() the frame
  // buffer already holds the next page's content: only the status bar and
  // the display commit are needed.
  bool ttfUsePreRenderedBuffer = false;
  // True when finishTtfPageRender() painted a chrome popup this pass — such
  // a frame must stay page N's context, so the prerender stays unscheduled.
  bool ttfChromePopupShown = false;
  void ttfInvalidatePreRender(const char* reason);
  // Soak triage: log a fast-path miss reason once per distinct reason.
  void ttfLogPrerenderMiss(const char* reason);
  // The ACTIVE text raster mode (degrade-aware snapshot axis).
  bool ttfEffectiveMonochromeSnapshot() const;
  void ttfSchedulePreRender();
  // Both passes re-derive their target from live state (the flags only say a
  // pass was requested) and need the layout params for the paint font.
  void ttfRunPreRenderPass(const freeink::book::LayoutParams& params);
  bool ttfFastDisplayPass(const freeink::book::LayoutParams& params);
  // Extracts the engine page's internal (resolvable) footnote links into
  // currentPageFootnotes; shared by the normal and fast-display passes.
  void ttfExtractFootnotes(const freeink::book::Page& page);
  // Display commit + finalize tail shared by the normal and fast-display
  // passes: gray-route dispatch (strips/full-frame), 1bpp fallback with the
  // async-overlap window, telemetry, frame-complete publication, progress
  // save, chrome popups, and the prerender scheduling hook.
  void ttfCommitFrame(const freeink::book::Page& page, const freeink::book::LayoutParams& params, size_t scratchMark);
#endif

  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 2;
  static constexpr size_t BACKGROUND_BUILD_MIN_FREE_HEAP = 32 * 1024;
  static constexpr size_t BACKGROUND_BUILD_MIN_MAX_ALLOC = 16 * 1024;
  // Requires the render lock; heap admission is checked separately by the build tick.
  bool backgroundBuildWanted() const;
  bool buildTickHeapGate();
  bool buildHeapPaused = false;
  void prefetchNextChapterDuringDisplay();
  // One bounded build step on the loop task, in a frame whose input did nothing. Lays out about
  // BACKGROUND_BUILD_PAGES_PER_TICK pages (one 1 KB parse step may add more) and hands the loop
  // back as soon as a button is down, so a short press is seen before more pages are built.
  void backgroundBuildTick();
  bool anyButtonPressed() const;
  // A background build failed in this spine. The error would likely recur, so from then on the
  // spine is built only as far as the reader goes; pages from earlier build steps stay readable.
  int backgroundBuildFailedSpine = -1;
  // Bluetooth page turner: every chapter build stops the radio until the chapter is built and its
  // page is shown. False: the radio still holds or is taking the heap, and the build must not run.
  std::atomic<bool> radioReleasedForBuild{false};
  bool makeRoomForChapterBuild();
  static constexpr uint32_t BUILD_ROOM_WAIT_MS = 5000;
  static constexpr uint32_t BUILD_ROOM_STEP_MS = 50;
  bool inputOverPage() const override { return overlay != Overlay::None || ReaderActivity::inputOverPage(); }
  // "Connecting Bluetooth…" or "Bluetooth failed" in place of the status bar title.
  mutable LinkNoteTitle linkNoteTitle;
  static constexpr size_t RENDER_MIN_FREE_HEAP = 24 * 1024;
  // Chunked prerender slice budget (soak finding #5): one prerender slice per
  // loop tick, so a button press is served at most one slice after it lands
  // (the loop processes input before the render pass runs).
  static constexpr uint32_t kPreRenderSliceBudgetMs = 120;
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  static constexpr unsigned long BUILD_POPUP_DEADLINE_MS = 1000;
  // Once the Inline build (TTF cache-miss path) has laid out this many pages,
  // the Indexing popup must clear so the reader can start reading while the
  // rest of the chapter drains in the background (font-change open, and the
  // short-chapter next-chapter case). Distinct from BUILD_POPUP_PAGE_THRESHOLD
  // (which decides whether to SHOW the popup at all) and from the refresh
  // cadence that drives pagesUntilFullRefresh. (2026-10-04.)
  static constexpr int MIN_PAGES_TO_CLEAR_POPUP = 5;
  static constexpr bool shouldClearBuildPopup(const int pagesBuilt) { return pagesBuilt >= MIN_PAGES_TO_CLEAR_POPUP; }
  // buildPopupPending only means "the delayed Indexing popup has not been
  // painted yet" — showBuildPopup() clears it the moment it paints, and an
  // immediate show never sets it. The min-pages gate has to key off whether the
  // popup is actually ON SCREEN, which is this latch: set wherever the popup is
  // painted, cleared when the build finishes or errors out.
  bool indexingPopupShown = false;
  bool buildPopupPending = false;
  void showBuildPopup(GfxRenderer& renderer, int& pagesUntilFullRefresh);
  bool applyDeferredReposition();
  void clearDeferredReposition();
  void rememberCurrentContentOffset();
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  int currentTocIndex() const;
  // Live section position, or the values cached before a child screen
  // released the section.
  ChapterPosition chapterPosition() const;
  int bookPercentFor(const ChapterPosition& position) const;
  void openReaderMenu();
  // Toolbar reader menu (see Overlay above).
  bool usesToolbarMenu() const;
  // True while any reader chrome is up (toolbar sheet, a panel, or a popup
  // over a panel). The Home-key "Go Back" shortcut / home gesture close it
  // instead of leaving the book.
  bool isChromeOpen() const;
  bool handleHomeGesture() override;
  // The side-edge drag is vetoed while the toolbar/panel owns input, and on the
  // end-of-book menu — their own vertical-swipe handling would conflict.
  bool allowsFrontlightSwipe() const override { return overlay == Overlay::None && !endOfBookMenuActive(); }
  void openOverlay(Overlay target);
  void closeOverlayToPage();
  void discardOverlayPage();
  void handleOverlayInput();
  void renderOverlay();
  std::string currentChapterTitle() const;
  // Text panel rows (font, size, line spacing, alignment, focus reading).
  std::string textRowName(int row) const;
  std::string textRowValue(int row) const;
  void showTextRowPopup(int row);
  // Persist + re-paginate + re-render under the open panel (live preview).
  void applyTextSettingLive();
  void paintOverlayPopup();
  // Persist the reader text settings, (re)load the selected SD font, and
  // re-paginate the current chapter so changes apply without re-opening the book.
  void applyReaderTextSettings();
  // More panel rows.
  void buildMoreActions();
  std::string moreRowName(int row) const;
  std::string moreRowValue(int row) const;
  void activateMoreRow(int row);
  void openFootnoteSelect(bool reopenMenuOnCancel);
  // TouchLongPressMode (see TouchLongPressMode.h) is passed to
  // openDictionaryWordSelect so the activity never reads the mutable
  // SETTINGS.touchLongPressAction global.
  void openDictionaryWordSelect(int touchX = -1, int touchY = -1,
                                TouchLongPressMode mode = TouchLongPressMode::Dictionary);
  void startClipSelection(int initialX = -1, int initialY = -1,
                          ClipSelectionActivity::Mode mode = ClipSelectionActivity::Mode::Clip);
  int clippingAtPoint(const Page& page, int x, int y) const;
  void openClippings();
  void applyProgressChange(const ProgressChangeResult& progress);
  bool launchKOReaderSync();
  unsigned long confirmLongPressThreshold() const;
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void loadCachedBookmarks();
  void addBookmark();
  void updateBookmarkFlag();

  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void drawClippingHighlights(const Page& page, int fontId, int orientedMarginTop, int orientedMarginLeft) const;
  void renderStatusBar() const;
  void applyOrientation(uint8_t orientation);
  // Long-press turn dispatch shared by the fresh-turn and pending-turn
  // (Apply) paths: chapter skip / orientation change for a long press arming
  // this frame. Returns true when the frame was consumed by the dispatch.
  // See queued-input aggregation round-2: a long press arming in an Apply
  // frame must keep its configured action, and the parked magnitude stays
  // parked for a later frame.
  bool dispatchLongPressTurn(const ReaderUtils::PageTurnResult& turn, const ReaderUtils::TouchPageTurn& touch);
  void applyInitialOrientation() override;
  // The orientation the current layout was built for. The control center's
  // orientation tile can move SETTINGS.orientation while this reader sits on
  // the activity stack, and Pop restores it without onEnter(), so the drift has
  // to be noticed here rather than assumed away.
  uint8_t appliedOrientation = 0;

  // Modal shown when a protected book refuses to open (loan expired /
  // date unverified); OK exits, "Sync time" (when offered) verifies the
  // clock over Wi-Fi and reopens the book.
  OptionPopup<> loadFailurePopup;
  // Protection error captured by loadBook() for handleLoadFailure(); the
  // failed Epub itself does not outlive loadBook().
  std::string loadProtectionError;

  bool loadBook() override;
  bool handleLoadFailure() override;
  // Wi-Fi join + SNTP for a loan whose date could not be verified, then a
  // clean re-open of the book.
  void beginLoanTimeSync();
  std::string getBookTitle() const override { return epub ? epub->getTitle() : ""; }
  std::string getBookAuthor() const override { return epub ? epub->getAuthor() : ""; }
  std::string getBookThumbBmpPath() const override { return epub ? epub->getThumbBmpPath() : ""; }
  int getProgressBasisPoints() const override;
  void renderBook() override;
  void onEndOfBookRendered() override;

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                              bool allowFastInitialRefresh)
      : ReaderActivity("EpubReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  ~EpubReaderActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

  bool pageTurn(bool isForward) override;
  // Batched jump with the chapter-boundary clamp (queued-input aggregation
  // §3.2): a step that crossed into another chapter (or reached end of book)
  // needs a render/load before the next step applies state-safely, so the
  // jump stops there and lands on the boundary page in this frame's render.
  bool pageTurnBy(int steps) override;
  bool skipPages(int amount) override;
  bool remoteShortcut(bool readerMenu) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  bool skipLoopDelay() override;

  // True while a background build is live (FIBP worker indexing, TTF session,
  // or a section build): keeps the CPU out of low-power — the input-idle
  // governor would otherwise drop to LOW_POWER_FREQ mid-index (measured:
  // 2.6 s/page spine builds and a task-WDT IDLE0 starvation reboot) — and
  // holds the auto-sleep timer until the work drains. The worker self-exits
  // once every spine has been attempted, so this is bounded.
  bool preventAutoSleep() override;

  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
