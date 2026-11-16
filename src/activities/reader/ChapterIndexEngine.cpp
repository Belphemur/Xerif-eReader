// ChapterIndexEngine — see ChapterIndexEngine.h.

#if defined(CROSSPOINT_TTF_READER)

#include "ChapterIndexEngine.h"

namespace freeink {
namespace book {

BookStatus ensureChapterSession(ChapterIndexTarget& target, const uint16_t spineIndex, const LayoutParams& params,
                                const uint32_t generation) {
  if (target.sessionFor(spineIndex)) return BookStatus::Ok;
  return target.beginChapterSession(spineIndex, params, generation);
}

// Consume one stall slot: Progress while the caller's bound still has head-
// room, then a bounded give-up — the session is torn down with the runtime's
// own abortSession semantics (resumable partial commit) and Failed returned,
// so callers take their existing failure paths instead of pumping a
// zero-page session forever. The counter resets so a replacement session
// starts clean.
static ChapterPump stalledChunk(ChapterIndexTarget& target, uint16_t* stallChunks) {
  if (stallChunks != nullptr && *stallChunks < kMaxStalledPumpChunks) {
    ++*stallChunks;
    return ChapterPump::Progress;
  }
  if (stallChunks != nullptr) *stallChunks = 0;
  target.abortSession();
  return ChapterPump::Failed;
}

ChapterPump pumpChapterChunk(ChapterIndexTarget& target, const uint16_t spineIndex, const uint16_t minNewPages,
                             BookStatus* stOut, uint16_t* stallChunks) {
  const uint32_t pagesBefore = target.availablePageCount(spineIndex);
  const BookStatus st = target.stepBuild(minNewPages);
  if (stOut != nullptr) *stOut = st;
  if (st == BookStatus::Ok) {
    if (!target.sessionActive()) {
      if (stallChunks != nullptr) *stallChunks = 0;
      return ChapterPump::Complete;
    }
    // Page progress resets the stall bound; a page-silent Ok chunk counts
    // against it (contractually impossible from the SDK's step(), but the
    // bound must not depend on that invariant holding).
    if (target.availablePageCount(spineIndex) != pagesBefore) {
      if (stallChunks != nullptr) *stallChunks = 0;
      return ChapterPump::Progress;
    }
    return stalledChunk(target, stallChunks);
  }
  // A soft failure keeps the session alive and remains pumpable — bounded:
  // kMaxStalledPumpChunks page-silent chunks in a row tear it down instead of
  // spinning the pump. A session that belongs to ANOTHER spine (the caller's
  // session was re-seeded under it) must NOT read as progress: the caller
  // would keep pumping the wrong chapter and report completion for a spine it
  // never built — re-seed instead. A done session still belongs to its spine
  // (sessionFor only compares the session spine), so the completion handshake
  // stays covered.
  if (target.sessionFor(spineIndex)) return stalledChunk(target, stallChunks);
  if (stallChunks != nullptr) *stallChunks = 0;
  return ChapterPump::Failed;
}

ChapterRun runChapterBuild(ChapterIndexTarget& target, const uint16_t spineIndex, const LayoutParams& params,
                           const uint32_t generation, const ChapterIndexYield& yield, const uint16_t chunkPages) {
  const BookStatus begin = ensureChapterSession(target, spineIndex, params, generation);
  if (begin != BookStatus::Ok) return ChapterRun::Failed;

  // One bound per run: page progress inside pumpChapterChunk resets it, so
  // only a run that keeps stalling gives up.
  uint16_t stalledChunks = 0;
  for (;;) {
    if (yield.step != nullptr && !yield.step(yield.ctx, static_cast<uint16_t>(target.availablePageCount(spineIndex)))) {
      // Resumable partial commit; the caller re-seeds on the next run.
      target.abortSession();
      return ChapterRun::Cancelled;
    }
    switch (pumpChapterChunk(target, spineIndex, chunkPages, nullptr, &stalledChunks)) {
      case ChapterPump::Progress:
        continue;
      case ChapterPump::Complete:
        return ChapterRun::Completed;
      case ChapterPump::Failed:
        return ChapterRun::Failed;
    }
    return ChapterRun::Failed;
  }
}

}  // namespace book
}  // namespace freeink

#endif  // CROSSPOINT_TTF_READER
