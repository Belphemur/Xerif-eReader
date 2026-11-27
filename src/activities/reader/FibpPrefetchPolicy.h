#pragma once

// FibpPrefetchPolicy — pure scheduling/stop policy for the FIBP prefetch
// worker (R3/R4). Header-only so host tests exercise it without device
// stubs; the worker class consumes it unchanged.

#if defined(CROSSPOINT_TTF_READER)

#include <stdint.h>

namespace freeink {
namespace book {
namespace fibp {

// "No chapter entered yet" sentinel for the queue planner.
constexpr uint16_t kNoChapter = 0xFFFF;

// Prefetch window: how many spines AHEAD of the entered chapter the worker
// indexes. 1 = next chapter only — whole-book prefetching measured as
// device-soak pathology (SD-write pressure through long background runs, and
// a task-watchdog abort on a 177-spine book) and burns battery/SD wear
// indexing chapters the user may never open. The window is (re)planned when
// the position trigger below fires for a new spine.
constexpr uint16_t kPrefetchLookaheadSpines = 1;

// Prefilter thresholds for the next-chapter prefetch trigger.
//
// Long chapters: fire only inside the last 15% of the chapter — building
// chapters the user may never reach wastes SD writes and battery (whole-book
// prefetch was the device-soak pathology PR #153 reverted). Image preparation
// is slow: the soak showed the boundary arriving while the build was still
// running, so 15% gives it lead time inside the same one-chapter window.
//
// Short chapters: a chapter with fewer than kShortChapterImmediatePrefetchPages
// pages has no reading-time budget to hide the next chapter's index behind — the
// %-trigger fires too late (for a 2-page chapter it fires at page 1 of 2, one
// turn from the spine boundary, so the worker cannot finish indexing before the
// reader turns in and hits the Indexing popup). Short chapters therefore fire at
// ENTRY (page 0) so the index has the whole chapter to complete.
// (Owner repro: 2-page chapter → Indexing popup on the turn into the next
// chapter; closing/reopening let the idle worker drain. 2026-10-04.)
constexpr uint16_t kShortChapterImmediatePrefetchPages = 10;
constexpr uint8_t kPrefetchRemainingPercent = 15;
// page == the 0-based current page inside the chapter; pageCount == 0 (unknown)
// never triggers. The remaining-threshold uses ceil so the 1-page-remaining edge
// fires even for tiny page counts.
//
// `lengthFinal` says whether pageCount is the chapter's SETTLED length or a
// still-growing build watermark: the reader reports the pages written so far, so
// a long chapter's first cold-start pass reports a 1..9 watermark. Treating that
// as a short chapter would fire the prefetch mid-build and reintroduce the
// premature SD-write pressure the percentage rule exists to prevent, so the
// short-chapter branch requires a finalized length.
inline bool shouldPrefetchNext(const uint16_t page, const uint16_t pageCount, const bool lengthFinal = true) {
  if (pageCount == 0) return false;  // unknown length: never treat as short
  if (lengthFinal && pageCount < kShortChapterImmediatePrefetchPages) return true;  // short: fire at entry
  const uint32_t thresholdPages = (static_cast<uint32_t>(pageCount) * kPrefetchRemainingPercent + 99) / 100;
  const uint32_t remaining = pageCount > page ? static_cast<uint32_t>(pageCount - page) : 0;
  return remaining <= thresholdPages;  // long: 15%-remaining rule
}

// R4 queue order: the chapter AFTER the entered one first, then onward
// (wrapping to the book start, ending at the entered chapter). With no
// chapter entered yet, the spine order is 0..spineCount-1. Writes at most
// `cap` indices into `out` and returns the count written.
inline uint16_t buildQueue(const uint16_t spineCount, const uint16_t notifiedSpine, uint16_t* out, const uint16_t cap) {
  if (out == nullptr || cap == 0 || spineCount == 0) return 0;
  const uint16_t start =
      (notifiedSpine == kNoChapter || notifiedSpine >= spineCount) ? 0 : static_cast<uint16_t>(notifiedSpine + 1);
  uint16_t n = 0;
  for (uint16_t i = 0; i < spineCount && n < cap; ++i) {
    out[n++] = static_cast<uint16_t>((start + i) % spineCount);
  }
  return n;
}

// Reader-side single-writer rule for a partial-prefix resume. While the
// worker holds the resume claim for the current spine, the reader leaves the
// extension to it as long as it can still show a built page; only once the
// reader has run out of built pages may it take the claim back and extend
// inline. Without this the reader hands the claim over and immediately takes
// it back (the resume churn), and both writers race the same FIBP file.
// `page` is the reader's clamped current page (0 when not yet laid out) and
// `available` the built prefix length.
inline bool readerYieldsExtension(const bool workerClaimActive, const uint16_t page, const uint16_t available) {
  return workerClaimActive && available > 0 && page < available;
}

// R3 yield-gate between build chunks: the chunk loop stops when the book is
// closing (cancelled) or the layout generation moved (settings change).
inline bool shouldStopChunk(const bool cancelled, const uint32_t currentGen, const uint32_t sessionGen) {
  return cancelled || currentGen != sessionGen;
}

}  // namespace fibp
}  // namespace book
}  // namespace freeink

#endif  // CROSSPOINT_TTF_READER
