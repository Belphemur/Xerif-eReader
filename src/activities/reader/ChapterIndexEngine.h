#pragma once

// ChapterIndexEngine — the single chapter-index (FIBP) build driver shared by
// the reader's synchronous rebuild path and the background prefetch worker.
// It owns no state: it drives a ChapterIndexTarget's build session and
// normalizes the per-chunk outcome, so callers stay free to show a popup,
// reschedule to the next tick, or cancel. The face set arrives through the
// LayoutParams; pacing/cancellation policy arrives through the yield hook.

#if defined(CROSSPOINT_TTF_READER)

#include <BookTypes.h>
#include <layout/ChapterLayout.h>

namespace freeink {
namespace book {

// The session-driving surface of TtfBookRuntime, narrowed so host tests can
// exercise the engine against a fake.
class ChapterIndexTarget {
 public:
  virtual ~ChapterIndexTarget() = default;
  virtual BookStatus beginChapterSession(uint16_t spineIndex, const LayoutParams& params, uint32_t generation) = 0;
  virtual BookStatus stepBuild(uint16_t minNewPages) = 0;
  virtual bool sessionFor(uint16_t spineIndex) const = 0;
  virtual bool sessionActive() const = 0;
  virtual bool sessionDone() const = 0;
  virtual uint32_t availablePageCount(uint16_t spineIndex) const = 0;
  virtual void abortSession() = 0;
};

// One stepBuild chunk's normalized outcome:
//   Progress — session still alive, more chunks expected
//   Complete — session finished and the cache committed
//   Failed   — build torn down (partial commit); no session remains
enum class ChapterPump : uint8_t { Progress, Complete, Failed };

// Outcome of a whole-spine run:
//   Completed — cache committed
//   Cancelled — yield hook stopped the run; partial commit is resumable
//   Failed    — build failed; nothing left running
enum class ChapterRun : uint8_t { Completed, Cancelled, Failed };

// Called between build chunks (and once before the first). Returning false
// stops the run: the session aborts with a resumable partial commit.
// pagesBuilt is the spine's cumulative page count so far.
struct ChapterIndexYield {
  void* ctx = nullptr;
  bool (*step)(void* ctx, uint16_t pagesBuilt) = nullptr;
};

// Begins the build session for spineIndex unless one is already running for
// it (a live session keeps laying out — callers must not restart it).
BookStatus ensureChapterSession(ChapterIndexTarget& target, uint16_t spineIndex, const LayoutParams& params,
                                uint32_t generation);

// Consecutive no-progress chunks tolerated before the pump gives up on the
// build: a chunk that neither advances the spine's page count nor finishes
// (hard failure, completion) nor is preceded by one is stalled. Legitimate
// slow builds always emit pages on an Ok chunk (the session's step() either
// delivers minNewPages or ends the parse), so 8 stalled chunks in a row can
// only be a session that consumes nothing while staying alive — torn down at
// the 9th with the resumable partial commit, not pumped forever.
constexpr uint16_t kMaxStalledPumpChunks = 8;

// Steps one build chunk and classifies the result. stOut (optional) carries
// the raw BookStatus for caller-side logging. stallChunks is the CALLER-OWNED
// consecutive-stall counter (must be provided; reset it whenever the pumped
// session is (re)begun) — it bounds the soft-failure/zero-page spin: without
// it, a step that fails while leaving the session active would be pumped
// forever with no pages and no terminal state.
ChapterPump pumpChapterChunk(ChapterIndexTarget& target, uint16_t spineIndex, uint16_t minNewPages, BookStatus* stOut,
                             uint16_t* stallChunks);

// Runs the full build session for one spine to completion, invoking the yield
// hook between chunks. A false hook result cancels: the session aborts with a
// resumable partial commit and Cancelled is returned.
ChapterRun runChapterBuild(ChapterIndexTarget& target, uint16_t spineIndex, const LayoutParams& params,
                           uint32_t generation, const ChapterIndexYield& yield, uint16_t chunkPages);

}  // namespace book
}  // namespace freeink

#endif  // CROSSPOINT_TTF_READER
