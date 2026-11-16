// ChapterIndexEngineTest — host suites for the shared chapter-index build
// driver (activities/reader/ChapterIndexEngine.*). Exercises the pump/run
// state machine against a scripted ChapterIndexTarget fake: begin/step/
// cancellation ordering, completion detection, and hard-failure mapping.
// The FIBP byte format itself is covered by TtfReaderRuntimeTest.

#include <gtest/gtest.h>

#include <vector>

#include "activities/reader/ChapterIndexEngine.h"

namespace book = freeink::book;

namespace {

constexpr uint16_t kNone = 0xFFFF;

// Scripted target: each stepBuild() call lays `pagesPerStep` pages; after
// `stepsToDone` steps the session finishes (no session state remains, like
// TtfBookRuntime after finishSession()). Hard failures tear the session down
// exactly like the runtime's abortSession() inside stepBuild().
class FakeTarget final : public book::ChapterIndexTarget {
 public:
  // ---- script knobs ----
  bool beginFails = false;
  bool hardFailNextStep = false;
  bool softFailNextStep = false;  // non-Ok status while the session stays alive
  bool softFailForever = false;   // every chunk: non-Ok, session stays alive
  bool silentStep = false;        // every chunk: Ok, session stays alive, no pages
  int stepsToDone = 3;
  uint16_t pagesPerStep = 4;

  // ---- observed calls ----
  int beginCalls = 0;
  int stepCalls = 0;
  int abortCalls = 0;
  uint16_t lastMinNewPages = 0;
  std::vector<uint16_t> beginSpines;

  bool sessionFor(uint16_t spineIndex) const override { return activeSpine_ == spineIndex; }
  bool sessionActive() const override { return activeSpine_ != kNone; }
  bool sessionDone() const override { return false; }
  // Spine-scoped: a count belongs to the session's spine; other spines get 0
  // so a caller pumping the wrong spine can never observe cross-chapter data.
  uint32_t availablePageCount(uint16_t spineIndex) const override { return sessionFor(spineIndex) ? pages_ : 0; }
  void abortSession() override {
    ++abortCalls;
    activeSpine_ = kNone;
  }

  book::BookStatus beginChapterSession(uint16_t spineIndex, const book::LayoutParams&, uint32_t) override {
    ++beginCalls;
    beginSpines.push_back(spineIndex);
    if (beginFails) return book::BookStatus::IoError;
    activeSpine_ = spineIndex;
    pages_ = 0;
    return book::BookStatus::Ok;
  }

  book::BookStatus stepBuild(uint16_t minNewPages) override {
    ++stepCalls;
    lastMinNewPages = minNewPages;
    if (activeSpine_ == kNone) return book::BookStatus::NotFound;
    if (hardFailNextStep) {
      hardFailNextStep = false;
      activeSpine_ = kNone;  // mirrors TtfBookRuntime's abort-on-step-failure
      return book::BookStatus::IoError;
    }
    if (softFailForever) return book::BookStatus::ParseError;  // session stays active
    if (softFailNextStep) {
      softFailNextStep = false;
      return book::BookStatus::ParseError;  // session stays active
    }
    if (silentStep) return book::BookStatus::Ok;  // session stays active, no pages
    pages_ += pagesPerStep;
    if (stepCalls >= stepsToDone) {
      activeSpine_ = kNone;  // finished + committed in this chunk
      return book::BookStatus::Ok;
    }
    return book::BookStatus::Ok;
  }

 private:
  uint16_t activeSpine_ = kNone;
  uint32_t pages_ = 0;
};

struct YieldRecorder {
  int calls = 0;
  std::vector<uint16_t> pagesAtCall;
  // Nth call that returns false (1-based); 0 = never cancel.
  int cancelAtCall = 0;

  bool step(void*, uint16_t pagesBuilt) {
    ++calls;
    pagesAtCall.push_back(pagesBuilt);
    return cancelAtCall == 0 || calls < cancelAtCall;
  }
};

}  // namespace

TEST(EnsureChapterSessionTest, BeginsWhenColdAndIsIdempotentWhenLive) {
  FakeTarget target;
  book::LayoutParams params;

  EXPECT_EQ(book::ensureChapterSession(target, 3, params, 0xABCU), book::BookStatus::Ok);
  ASSERT_EQ(target.beginCalls, 1);
  EXPECT_EQ(target.beginSpines[0], 3u);

  // A live session for the spine must NOT be restarted.
  EXPECT_EQ(book::ensureChapterSession(target, 3, params, 0xABCU), book::BookStatus::Ok);
  EXPECT_EQ(target.beginCalls, 1);
  EXPECT_EQ(target.stepCalls, 0);
}

TEST(EnsureChapterSessionTest, PropagatesBeginFailure) {
  FakeTarget target;
  target.beginFails = true;
  book::LayoutParams params;

  EXPECT_EQ(book::ensureChapterSession(target, 0, params, 1u), book::BookStatus::IoError);
  EXPECT_EQ(target.beginCalls, 1);
  EXPECT_EQ(target.stepCalls, 0);
}

TEST(PumpChapterChunkTest, ClassifiesProgressCompleteAndFailed) {
  book::LayoutParams params;

  // Progress: session still active after the chunk.
  {
    FakeTarget target;  // stepsToDone=3, first step keeps the session alive
    book::LayoutParams params;
    ASSERT_EQ(target.beginChapterSession(1, params, 1u), book::BookStatus::Ok);
    book::BookStatus st = book::BookStatus::NotFound;
    uint16_t stall = 0;
    EXPECT_EQ(book::pumpChapterChunk(target, 1, 4, &st, &stall), book::ChapterPump::Progress);
    EXPECT_EQ(st, book::BookStatus::Ok);
    EXPECT_EQ(target.stepCalls, 1);
    EXPECT_EQ(target.lastMinNewPages, 4u);
  }

  // Complete: the chunk finished the session (no session state remains).
  {
    FakeTarget target;
    target.stepsToDone = 1;  // fresh target: the first step finishes it
    book::LayoutParams params;
    ASSERT_EQ(target.beginChapterSession(2, params, 1u), book::BookStatus::Ok);
    book::BookStatus st;
    uint16_t stall = 0;
    EXPECT_EQ(book::pumpChapterChunk(target, 2, 4, &st, &stall), book::ChapterPump::Complete);
    EXPECT_EQ(st, book::BookStatus::Ok);
    EXPECT_FALSE(target.sessionFor(2));
  }

  // Failed: hard step failure tore the session down.
  {
    FakeTarget target;
    target.hardFailNextStep = true;
    book::LayoutParams params;
    ASSERT_EQ(target.beginChapterSession(3, params, 1u), book::BookStatus::Ok);
    book::BookStatus st;
    uint16_t stall = 0;
    EXPECT_EQ(book::pumpChapterChunk(target, 3, 4, &st, &stall), book::ChapterPump::Failed);
    EXPECT_EQ(st, book::BookStatus::IoError);
  }

  // Soft failure: non-Ok status with a live session stays pumpable.
  {
    FakeTarget target;
    target.softFailNextStep = true;
    book::LayoutParams params;
    ASSERT_EQ(target.beginChapterSession(4, params, 1u), book::BookStatus::Ok);
    book::BookStatus st;
    uint16_t stall = 0;
    EXPECT_EQ(book::pumpChapterChunk(target, 4, 4, &st, &stall), book::ChapterPump::Progress);
    EXPECT_EQ(st, book::BookStatus::ParseError);
    EXPECT_TRUE(target.sessionFor(4));
  }
}

// The stuck-indexing regression: a chunk that neither advances pages nor
// finishes nor tears down must converge to Failed/abort within a bounded
// chunk count, not pump forever.
TEST(PumpChapterChunkTest, SoftFailureConvergesToBoundedFailed) {
  FakeTarget target;
  target.softFailForever = true;
  book::LayoutParams params;
  ASSERT_EQ(target.beginChapterSession(6, params, 1u), book::BookStatus::Ok);

  book::BookStatus st;
  uint16_t stall = 0;
  for (uint16_t chunk = 0; chunk < book::kMaxStalledPumpChunks; ++chunk) {
    EXPECT_EQ(book::pumpChapterChunk(target, 6, 4, &st, &stall), book::ChapterPump::Progress);
  }
  // The bound trips: the pump aborts the session (resumable partial commit)
  // and reports Failed. No chunk beyond the bound keeps the session alive.
  const int abortsBefore = target.abortCalls;
  EXPECT_EQ(book::pumpChapterChunk(target, 6, 4, &st, &stall), book::ChapterPump::Failed);
  EXPECT_EQ(target.abortCalls, abortsBefore + 1);
  EXPECT_FALSE(target.sessionFor(6));
  EXPECT_EQ(stall, 0u);
}

// Same bound for the Ok-but-zero-page shape: a chunk that stays Ok with the
// session active while laying no pages is stalled, not progress.
TEST(PumpChapterChunkTest, ZeroPageOkChunkConvergesToBoundedFailed) {
  FakeTarget target;
  target.silentStep = true;  // Ok, session stays alive, no pages laid
  book::LayoutParams params;
  ASSERT_EQ(target.beginChapterSession(7, params, 1u), book::BookStatus::Ok);

  book::BookStatus st;
  uint16_t stall = 0;
  for (uint16_t chunk = 0; chunk < book::kMaxStalledPumpChunks; ++chunk) {
    EXPECT_EQ(book::pumpChapterChunk(target, 7, 4, &st, &stall), book::ChapterPump::Progress);
    EXPECT_EQ(st, book::BookStatus::Ok);
  }
  EXPECT_EQ(book::pumpChapterChunk(target, 7, 4, &st, &stall), book::ChapterPump::Failed);
  EXPECT_FALSE(target.sessionFor(7));
}

// Progress resets the bound: alternating stalled and productive chunks never
// trips it, and a productive chunk after stalls restarts the full allowance.
TEST(PumpChapterChunkTest, ProgressResetsStallBound) {
  FakeTarget target;
  target.softFailForever = true;
  target.stepsToDone = 1000;  // the run itself must never complete here
  book::LayoutParams params;
  ASSERT_EQ(target.beginChapterSession(8, params, 1u), book::BookStatus::Ok);

  book::BookStatus st;
  uint16_t stall = 0;
  for (uint16_t round = 0; round < 5; ++round) {
    for (uint16_t chunk = 0; chunk < book::kMaxStalledPumpChunks; ++chunk) {
      EXPECT_EQ(book::pumpChapterChunk(target, 8, 4, &st, &stall), book::ChapterPump::Progress);
    }
    // A productive chunk (pages land) resets the counter; the next stalled
    // round gets the full allowance again.
    target.softFailForever = false;
    EXPECT_EQ(book::pumpChapterChunk(target, 8, 4, &st, &stall), book::ChapterPump::Progress);
    EXPECT_EQ(stall, 0u);
    target.softFailForever = true;
  }
  EXPECT_TRUE(target.sessionFor(8));
}

// The pump's bound must not fire for legitimate slow builds: one page per
// chunk with Ok status completes the run normally.
TEST(RunChapterBuildTest, OnePagePerChunkSlowBuildStillCompletes) {
  FakeTarget target;
  target.pagesPerStep = 1;
  target.stepsToDone = 64;
  YieldRecorder yield;
  book::LayoutParams params;

  const book::ChapterRun run = book::runChapterBuild(
      target, 2, params, 0x42U,
      {&yield, [](void* ctx, uint16_t pages) { return static_cast<YieldRecorder*>(ctx)->step(nullptr, pages); }}, 1);
  EXPECT_EQ(run, book::ChapterRun::Completed);
  EXPECT_EQ(target.beginCalls, 1);
  EXPECT_EQ(target.stepCalls, 64);
  EXPECT_EQ(target.abortCalls, 0);
}

// The run-level bound: a run whose every chunk stalls is Failed within a
// bounded number of chunks (and the session is torn down by the pump).
TEST(RunChapterBuildTest, StalledRunConvergesToFailedWithinBound) {
  FakeTarget target;
  target.softFailForever = true;
  book::LayoutParams params;
  YieldRecorder yield;

  const book::ChapterRun run = book::runChapterBuild(
      target, 3, params, 0x11U,
      {&yield, [](void* ctx, uint16_t pages) { return static_cast<YieldRecorder*>(ctx)->step(nullptr, pages); }}, 2);
  EXPECT_EQ(run, book::ChapterRun::Failed);
  // One pump per chunk plus the bound trip: bounded, not forever.
  EXPECT_EQ(target.stepCalls, static_cast<int>(book::kMaxStalledPumpChunks) + 1);
  EXPECT_EQ(target.abortCalls, 1);
  EXPECT_FALSE(target.sessionFor(3));
}

TEST(RunChapterBuildTest, CompletesWithOneBeginAndNoAbort) {
  FakeTarget target;
  YieldRecorder yield;
  book::LayoutParams params;

  const book::ChapterRun run = book::runChapterBuild(
      target, 5, params, 0x77U,
      {&yield, [](void* ctx, uint16_t pages) { return static_cast<YieldRecorder*>(ctx)->step(nullptr, pages); }}, 2);
  EXPECT_EQ(run, book::ChapterRun::Completed);
  EXPECT_EQ(target.beginCalls, 1);
  EXPECT_EQ(target.stepCalls, 3);
  EXPECT_EQ(target.abortCalls, 0);
  // Yield runs between chunks (and once before the first), receiving the
  // cumulative page count: each chunk lays pagesPerStep (4) pages.
  ASSERT_EQ(yield.pagesAtCall.size(), 3u);
  EXPECT_EQ(yield.pagesAtCall[0], 0u);
  EXPECT_EQ(yield.pagesAtCall[1], 4u);
  EXPECT_EQ(yield.pagesAtCall[2], 8u);
}

TEST(RunChapterBuildTest, CancelBetweenChunksAbortsToResumablePartial) {
  FakeTarget target;
  YieldRecorder yield;
  yield.cancelAtCall = 3;  // cancel before the 3rd chunk
  book::LayoutParams params;

  const book::ChapterRun run = book::runChapterBuild(
      target, 5, params, 0x77U,
      {&yield, [](void* ctx, uint16_t pages) { return static_cast<YieldRecorder*>(ctx)->step(nullptr, pages); }}, 4);
  EXPECT_EQ(run, book::ChapterRun::Cancelled);
  // Two chunks ran, then the hook stopped the run: exactly one abort (the
  // resumable partial commit) and no further stepping.
  EXPECT_EQ(target.stepCalls, 2);
  EXPECT_EQ(target.abortCalls, 1);
  EXPECT_EQ(target.stepCalls + 1, yield.calls);
}

TEST(RunChapterBuildTest, CancelBeforeFirstChunkWritesNothing) {
  FakeTarget target;
  YieldRecorder yield;
  yield.cancelAtCall = 1;
  book::LayoutParams params;

  const book::ChapterRun run = book::runChapterBuild(
      target, 9, params, 0x77U,
      {&yield, [](void* ctx, uint16_t pages) { return static_cast<YieldRecorder*>(ctx)->step(nullptr, pages); }}, 4);
  EXPECT_EQ(run, book::ChapterRun::Cancelled);
  EXPECT_EQ(target.beginCalls, 1);
  EXPECT_EQ(target.stepCalls, 0);
  EXPECT_EQ(target.availablePageCount(9), 0u);  // no pages committed
  EXPECT_EQ(target.abortCalls, 1);
}

TEST(RunChapterBuildTest, BeginFailureFailsWithoutSteppingOrAborting) {
  FakeTarget target;
  target.beginFails = true;
  YieldRecorder yield;
  book::LayoutParams params;

  const book::ChapterRun run = book::runChapterBuild(
      target, 1, params, 1u,
      {&yield, [](void* ctx, uint16_t pages) { return static_cast<YieldRecorder*>(ctx)->step(nullptr, pages); }}, 4);
  EXPECT_EQ(run, book::ChapterRun::Failed);
  EXPECT_EQ(target.stepCalls, 0);
  EXPECT_EQ(target.abortCalls, 0);
  EXPECT_EQ(yield.calls, 0);
}

TEST(RunChapterBuildTest, HardStepFailureMapsToFailedWithoutExtraAbort) {
  FakeTarget target;
  target.hardFailNextStep = true;  // the runtime tears the session down itself
  YieldRecorder yield;
  book::LayoutParams params;

  const book::ChapterRun run = book::runChapterBuild(
      target, 2, params, 1u,
      {&yield, [](void* ctx, uint16_t pages) { return static_cast<YieldRecorder*>(ctx)->step(nullptr, pages); }}, 4);
  EXPECT_EQ(run, book::ChapterRun::Failed);
  EXPECT_EQ(target.stepCalls, 1);
  // The engine must not double-abort: the target already tore the session
  // down inside the failed stepBuild().
  EXPECT_EQ(target.abortCalls, 0);
}

TEST(RunChapterBuildTest, NullYieldRunsToCompletion) {
  FakeTarget target;
  book::LayoutParams params;

  const book::ChapterRun run = book::runChapterBuild(target, 0, params, 1u, {}, 4);
  EXPECT_EQ(run, book::ChapterRun::Completed);
  EXPECT_EQ(target.stepCalls, 3);
  EXPECT_EQ(target.abortCalls, 0);
}
