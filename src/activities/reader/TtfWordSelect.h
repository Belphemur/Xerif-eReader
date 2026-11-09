#pragma once

// Word extraction over engine pages: the dictionary word selector on the
// TTF path. Splits each PageTextRun into whitespace-delimited tokens, joins
// the fragments layout split across run boundaries (the
// LayoutFirst/LastContinues pairing), and measures widths with the same
// FontChain that rendered the page — everything the legacy SD-font selector
// reads from TextBlock, rebuilt from run geometry + chapter char anchoring.

#if defined(CROSSPOINT_TTF_READER)

#include <Epub/FootnoteEntry.h>
#include <Memory.h>
#include <layout/ChapterLayout.h>
#include <render/TtfFont.h>
#include <stdint.h>

#include <vector>

namespace freeink {
namespace book {

// One selectable token. `text` points into the payload's arena
// (NUL-terminated, raw token — the trimmed range is textOffset/textLength).
// Geometry is page-logical screen coordinates with `y` the line-box TOP
// (baselineY - ascent), matching the legacy selector's drawText convention.
struct TtfWordBox {
  const char* text;
  int16_t x;
  int16_t y;
  int16_t width;
  int16_t height;  // FontChain line height for this run's size
  uint16_t textOffset;
  uint16_t textLength;
  uint16_t rawLength;  // pre-trim; footnote resolution needs the parens
  uint8_t styleFlags;
  uint32_t selectionGroup;  // per-page unique id of the logical word
  bool syntheticHyphen;
  // Chapter character anchor of the TRIMMED token: codepoints of extracted
  // text (the Page::charStart coordinate), charStart + charLen = half-open
  // end — clipping stores and TextBlock::SourceRange share the convention.
  // Consumer-side walk (no SDK change); charStart = UINT32_MAX when
  // unavailable: RTL runs are stored in visual order and their clips degrade
  // to text-only (design doc ttf/2026-11-09 §F3).
  uint32_t charStart;
  uint16_t charLen;
};

struct TtfWordSelectData {
  // Token text storage, outlives every box. PSRAM-backed on PSRAM builds
  // (poolMalloc); DRAM malloc on host/DRAM-only builds.
  PoolBytes arena;
  std::vector<TtfWordBox> boxes;
  std::vector<FootnoteEntry> footnotes;
};

// Builds the payload from a valid page. Run text is only read during the
// call — everything is copied out, so the reader's scratch mark can be
// released as soon as this returns. `footnotes` is filled by the caller.
// The spacing knobs replay the paint walk exactly (PagePaint adds them per
// decoded glyph, see the spacingFor lambda): token geometry must match painted
// glyph positions or highlight/hit boxes drift whenever the reader applies
// text spacing. Returns false on OOM (payload left empty).
bool buildTtfWordSelectData(const Page& page, FontChain& fonts, TtfWordSelectData& out, int16_t characterSpacingPx = 0,
                            int16_t wordSpacingPx = 0);

}  // namespace book
}  // namespace freeink

#endif  // CROSSPOINT_TTF_READER
