#pragma once

// QuickPageRelayout — shared current-page relayout/paint seam for the quick
// font surfaces (FontPreviewActivity).
//
// quickRelayoutPage() re-lays ONLY the entry page's text span — the text the
// reader displays at open time (ADR 2026-10-14, page-local relayout). The
// engine's window mode (LayoutParams::windowStartChar, freeink-sdk) skips
// paragraphs that end before the anchor, lays the page containing it, and
// stops at the first page break, so a size step paints in O(one page layout)
// instead of a chapter scan. The delivered page is deep-copied out of the
// per-page arena with QuickPageCapture, trimmed to the entry span's end
// (`windowEndChar`) so a smaller font cannot pull later pages' text into the
// preview. The page's line breaks approximate the close reindex's (the engine
// starts at the paragraph boundary containing the anchor); the close reindex
// computes the true pagination.
//
// No committed cache is touched and no FIBP worker state changes; the caller
// owns invalidation, cursor bookkeeping, and the session capture buffer.

#include <GfxRenderer.h>
#include <Memory.h>

#include "QuickPageCapture.h"
#include "TtfBookRuntime.h"

#if defined(CROSSPOINT_TTF_READER)

#include <layout/ChapterLayout.h>

struct QuickRelayoutResult {
  // The window page was delivered and captured (ready to paint). False also
  // means the caller must keep the frame it opened over.
  bool reachedAnchor = false;
  bool captured = false;  // a page was deep-copied (same condition, kept explicit)
  void* font = nullptr;   // the FontChain the layout ran with (loader-owned; valid until a reload)
  // Spacing the capture was laid out with; the paint pass must replay them or
  // the painted glyphs drift from the stored run geometry.
  int16_t characterSpacingPx = 0;
  int16_t wordSpacingPx = 0;
};

// Relayout the entry page's text span (`anchorChar .. windowEndChar`, the
// reader's displayed page; windowEndChar 0 = unbounded) in `spineIndex` at the
// CURRENT settings into `capture`. `autoPageTurn` mirrors the reader's
// automaticPageTurnActive flag so the preview matches the layout a close
// reflow would produce. `sessionBuffer` is the caller's one-per-session
// pooled backing buffer: allocated on first use (PSRAM via poolMakeBytes),
// reused for every change, freed by the owner when its surface closes.
// Returns the outcome; painting (paintCapturedPage) is the caller's decision.
QuickRelayoutResult quickRelayoutPage(freeink::book::TtfBookRuntime& ttf, GfxRenderer& renderer,
                                      const uint16_t spineIndex, const uint32_t anchorChar,
                                      const uint32_t windowEndChar, const bool autoPageTurn, QuickPageCapture& capture,
                                      PoolBytes& sessionBuffer);

// Which pass of the page paint is running (design 2026-11-08 §5.1). Base is
// the pre-commit paint into the plain framebuffer. The absolute-gray image
// route re-walks the page content once per plane: the framebuffer is the
// plane source each pass (seeded 0xFF, cleared bit = dark), so text and
// image writes decide per pass which plane's bits they darken.
enum class TtfPaintPass : uint8_t { Base, PlaneLsb, PlaneMsb };

// Paint a transient engine Page (captured or freshly read) with the reader's
// TTF page pipeline: text via the shared uniform quantizer when AA gray
// parity holds or the page takes the absolute-gray image route, 1bpp engine
// path otherwise; images per the user's display policy. Extracted from
// EpubReaderActivity::paintTtfPage — keep this body and the reader member in
// lockstep (the member now delegates here).
// spacingPx are the values the page was laid out with; the engine charges them
// to every glyph advance, so the paint pass must replay them.
void paintCapturedPage(const freeink::book::Page& page, void* font, GfxRenderer& renderer,
                       freeink::book::TtfBookRuntime& ttf, int16_t characterSpacingPx = 0,
                       int16_t wordSpacingPx = 0, TtfPaintPass pass = TtfPaintPass::Base);

#endif  // CROSSPOINT_TTF_READER
