#pragma once

// QuickPageCapture — deep-copies one engine Page so it can be painted after
// the transient ChapterLayout pass that produced it. Engine page data dies
// when PageSink::onPage() returns (the per-page arena resets), so the quick
// font sheet's paint-once preview must carry the candidate page out of the
// callback: only the last scanned page is ever displayed, and painting every
// scanned page made each +/- tap pay N-1 invisible page paints (issue #137).
//
// Page strings (run text, link targets/fragments, image hrefs, ruby text)
// all live in the engine's per-page arena; each is bumped into the capture
// buffer and the record's pointer is rebased to the copy. Record arrays are
// sized for the STANDARD build profile; a page that does not fit returns
// false and the caller paints inline instead.

#include <cstddef>
#include <cstdint>

#include "layout/ChapterLayout.h"  // Page + record types

namespace {
constexpr size_t alignUp(size_t offset, size_t alignment) { return (offset + alignment - 1) & ~(alignment - 1); }
}  // namespace

class QuickPageCapture {
 private:
  static constexpr uint16_t kMaxRuns = 768;   // STANDARD profile kMaxRunsPerPage
  static constexpr uint16_t kMaxLinks = 24;   // STANDARD profile kMaxLinksPerPage
  static constexpr uint16_t kMaxImages = 16;  // STANDARD profile kMaxImagesPerPage
  static constexpr uint16_t kMaxRules = 16;   // STANDARD profile kMaxRulesPerPage
  static constexpr uint16_t kMaxRubies = 24;  // STANDARD profile kMaxRubiesPerPage
  // Page strings come from the engine's per-page arena (24 KB cap on the
  // STANDARD profile); the copy region matches with slack.
  static constexpr size_t kStringCap = 28 * 1024;

 public:
  // Fixed buffer layout: record arrays first (typed offsets), then the
  // string bump region. Offsets are 8-aligned so pointer-sized members of
  // every record are aligned on both 32-bit firmware and 64-bit hosts.
  static constexpr size_t kRunsOff = 0;
  static constexpr size_t kLinksOff = alignUp(kRunsOff + kMaxRuns * sizeof(freeink::book::PageTextRun), 8);
  static constexpr size_t kImagesOff = alignUp(kLinksOff + kMaxLinks * sizeof(freeink::book::PageLink), 8);
  static constexpr size_t kRulesOff = alignUp(kImagesOff + kMaxImages * sizeof(freeink::book::PageImage), 8);
  static constexpr size_t kRubiesOff = alignUp(kRulesOff + kMaxRules * sizeof(freeink::book::PageRule), 8);
  static constexpr size_t kStringsOff = alignUp(kRubiesOff + kMaxRubies * sizeof(freeink::book::PageRuby), 8);

  // Total backing-buffer size (record arrays + string region). One
  // allocation serves every capture until the sheet closes.
  static constexpr size_t kBufferBytes = kStringsOff + kStringCap;

  // Installs the backing buffer (one allocation, reused across captures).
  // Must be max-aligned (malloc/poolMalloc) — the record arrays sit at
  // fixed offsets from it. Any previously captured page pointed into the
  // old buffer, so it is invalidated.
  void attach(uint8_t* buffer, size_t cap) {
    buffer_ = buffer;
    cap_ = cap;
    valid_ = false;
  }
  bool attached() const { return buffer_ != nullptr; }

  // Drops the captured page (the backing buffer stays attached).
  void reset() { valid_ = false; }

  // Chapter char end (codepoints) of `page`'s text: the furthest run end,
  // floored at the page's char start (the engine's own window-delivery
  // metric — a run-less page ends at its charStart).
  static uint32_t pageEndChar(const freeink::book::Page& page);

  // Deep-copies `page` into the buffer, overwriting any previous capture.
  // Returns false when the page does not fit; the capture state is then
  // unusable until the next successful capture().
  // `endChar` bounds the copy to the entry page's text span (ADR 2026-10-14:
  // the preview re-lays ONLY the entry page's span) — runs starting at or
  // past it are dropped and a run straddling it is truncated to its
  // leading codepoints (the synthetic-hyphen bit recomputed from the kept
  // bytes). 0 = no bound (untrimmed copy).
  bool capture(const freeink::book::Page& pageData, uint32_t endChar = 0);

  // True when the last capture() succeeded.
  bool ready() const { return valid_; }

  // The captured page. Valid only while `ready()` and no capture()/reset()
  // has run since.
  const freeink::book::Page& page() const { return page_; }

 private:
  // Copies a NUL-terminated string into the bump region and returns the
  // copy (nullptr when the region is exhausted).
  const char* copyString(const char* src);

  uint8_t* buffer_ = nullptr;
  size_t cap_ = 0;
  size_t stringUsed_ = 0;
  bool valid_ = false;
  freeink::book::Page page_{};
};

// The preview's PageSink for the window pass (ADR 2026-10-14): captures the
// window page — the first delivered page whose TEXT reaches past the anchor
// — bounded to the entry page's span, and stops the pass there. For an anchor
// of 0 the first delivered page IS the window page (windowStartChar 0
// disables the engine window; the sink cuts it after page 0).
//
// The anchor check makes the sink self-contained rather than trusting the
// engine's window suppression: a delivered page without text past the anchor
// (windowStartChar > 0 only delivers text pages past it, but a run-less
// image/rule page whose char position is already past the anchor can be
// delivered when its image forced the break) keeps the pass running so the
// anchor's text page is captured instead.
class QuickWindowSink final : public freeink::book::PageSink {
 public:
  QuickWindowSink(QuickPageCapture& captureRef, const uint32_t anchorChar, const uint32_t endChar)
      : capture_(captureRef), anchorChar_(anchorChar), endChar_(endChar) {}

  bool onPage(const freeink::book::Page& page) override {
    delivered_ = true;
    if (anchorChar_ > 0 && (page.runCount == 0 || QuickPageCapture::pageEndChar(page) <= anchorChar_)) {
      return true;  // no text past the anchor on this page — keep laying
    }
    // A page too large for the buffer is SKIPPED, not painted inline: an
    // inline speculative frame would be left on screen while the status bar
    // still reported the old page.
    if (capture_.capture(page, endChar_)) {
      reachedAnchor_ = true;
    } else {
      capture_.reset();
    }
    return false;  // the window page is the whole pass
  }

  bool delivered() const { return delivered_; }
  // The captured page contains the anchor and fits the buffer.
  bool reachedAnchor() const { return reachedAnchor_; }

 private:
  QuickPageCapture& capture_;
  uint32_t anchorChar_;
  uint32_t endChar_;
  bool delivered_ = false;
  bool reachedAnchor_ = false;
};
