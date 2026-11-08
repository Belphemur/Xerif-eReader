// PagePaint — see PagePaint.h. Gated to CROSSPOINT_TTF_READER: engine pages
// only exist on TTF builds (§14.1 compile-time split).

#if defined(CROSSPOINT_TTF_READER)

#include "PagePaint.h"

#include <Arduino.h>  // millis() for the slice budget; host targets provide a stub
#include <BitmapHelpers.h>
#include <GfxRenderer.h>

#include "GrayPlanes.h"

namespace freeink {
namespace book {
namespace {

// Mirror of PageRenderer's TU-local decoder (PageRenderer.cpp:96).
uint32_t decodeUtf8(const char* text, const uint32_t len, uint32_t& i) {
  if (i >= len) return 0;  // defensive: callers loop on i < len
  const auto b0 = static_cast<uint8_t>(text[i]);
  uint32_t cp = b0;
  uint32_t extra = 0;
  if (b0 >= 0xF0) {
    cp = b0 & 0x07;
    extra = 3;
  } else if (b0 >= 0xE0) {
    cp = b0 & 0x0F;
    extra = 2;
  } else if (b0 >= 0xC0) {
    cp = b0 & 0x1F;
    extra = 1;
  }
  ++i;
  while (extra > 0 && i < len && (static_cast<uint8_t>(text[i]) & 0xC0) == 0x80) {
    cp = (cp << 6) | (static_cast<uint8_t>(text[i]) & 0x3F);
    ++i;
    --extra;
  }
  return cp;
}

// Per-pixel sink: receives the page-logical pixel and its quantized tone.
using ToneSink = void (*)(void* ctx, int32_t x, int32_t y, uint8_t tone);
// Optional per-glyph cull (tiled plane bands): false skips the rasterize.
using GlyphFilter = bool (*)(void* ctx, int32_t x0, int32_t y0, int32_t x1, int32_t y1);

// The glyph walk shared by the base and plane passes — replicates
// PageRenderer::renderText exactly (kerning, fontFor fallback, synthetic-bold
// double strike, underline/strikethrough arms) but hands every sample to the
// caller's tone sink instead of writing into a FrameTarget.
void walkText(const Page& page, FontChain& fonts, void* ctx, const ToneSink sink, const GlyphFilter glyphFilter,
              const uint16_t startRun = 0, const uint32_t startChar = 0, uint16_t* nextRunOut = nullptr,
              uint32_t* nextCharOut = nullptr, const uint32_t budgetMs = 0, const int16_t characterSpacingPx = 0,
              const int16_t wordSpacingPx = 0) {
  // ChapterLayout::advanceFor() charges these on top of the font advance, so
  // the walk must replay them or painted glyphs drift left of the run geometry
  // the layout stored. Word spacing rides the space glyph, matching the engine.
  const auto spacingFor = [characterSpacingPx, wordSpacingPx](const uint32_t cp) {
    return cp == ' ' ? wordSpacingPx : characterSpacingPx;
  };
  const uint32_t start = budgetMs ? millis() : 0;
  for (uint16_t r = startRun; r < page.runCount; ++r) {
    const PageTextRun& run = page.runs[r];
    int32_t penX = run.x;
    uint32_t i = 0;
    uint32_t prev = 0;
    // Intra-run cursor: glyphs already painted in THIS pass when resuming
    // the run the previous slice yielded in (metrics-only replay — no
    // rasterize, no pixels — so the resume cannot double-plot ink).
    uint32_t painted = 0;
    const bool isCursorRun = (r == startRun && startChar > 0);
    while (i < run.len) {
      const uint32_t cp = decodeUtf8(run.text, run.len, i);
      if (prev != 0) penX += fonts.kerning(prev, cp, run.sizePx, run.styleFlags);
      if (isCursorRun && painted < startChar) {
        // Resume replay: advance-only, no rasterize, no pixel writes.
        penX += fonts.advance(cp, run.sizePx, run.styleFlags) + spacingFor(cp);
        prev = cp;
        ++painted;
        continue;
      }
      uint8_t faceFlags = 0;
      RenderFont* font = fonts.fontFor(cp, run.styleFlags, &faceFlags);
      // Cull from cheap metrics before rasterizing: paintPlanes() walks the
      // page once per strip, so a glyph the active band drops must not pay
      // pixel generation first (Copilot, PR #113). Faces without a bounds
      // implementation fall through to the rasterize-then-filter below.
      if (font != nullptr && glyphFilter != nullptr) {
        int16_t bx = 0;
        int16_t by = 0;
        uint16_t bw = 0;
        uint16_t bh = 0;
        if (font->glyphBounds(cp, run.sizePx, bx, by, bw, bh) &&
            !glyphFilter(ctx, penX + bx, run.baselineY + by, penX + bx + bw, run.baselineY + by + bh)) {
          penX += fonts.advance(cp, run.sizePx, run.styleFlags) + spacingFor(cp);
          prev = cp;
          continue;
        }
      }
      const GlyphBitmap* glyph = font != nullptr ? font->rasterize(cp, run.sizePx) : nullptr;
      if (glyph != nullptr) {
        if (glyphFilter != nullptr &&
            !glyphFilter(ctx, penX + glyph->xoff, run.baselineY + glyph->yoff, penX + glyph->xoff + glyph->width,
                         run.baselineY + glyph->yoff + glyph->height)) {
          // Outside the active plane band: skip the pixel walk, keep metrics.
          penX += fonts.advance(cp, run.sizePx, run.styleFlags) + spacingFor(cp);
          prev = cp;
          continue;
        }
        // Synthetic bold (double-strike, +1 px) when the run wants bold but
        // no bold face is registered; advance stays the measured one.
        const int strikes = (run.styleFlags & StyleBold) != 0 && (faceFlags & StyleBold) == 0 ? 2 : 1;
        for (int s = 0; s < strikes; ++s) {
          for (uint16_t gy = 0; gy < glyph->height; ++gy) {
            const uint8_t* srcRow = glyph->pixels + static_cast<uint32_t>(gy) * glyph->width;
            const int32_t dy = run.baselineY + glyph->yoff + static_cast<int32_t>(gy);
            for (uint16_t gx = 0; gx < glyph->width; ++gx) {
              sink(ctx, penX + glyph->xoff + static_cast<int32_t>(gx) + s, dy, srcRow[gx]);
            }
          }
        }
      }
      penX += fonts.advance(cp, run.sizePx, run.styleFlags) + spacingFor(cp);
      prev = cp;
      ++painted;
      // Intra-run yield (review r5): a long line must not blow the slice
      // budget — check after every glyph, cursor on the current run.
      if (budgetMs) {
        if (nextRunOut != nullptr) *nextRunOut = r;
        if (nextCharOut != nullptr) *nextCharOut = painted;
        if (millis() - start >= budgetMs) return;
      }
    }
    if (run.styleFlags & StyleUnderline) {
      const int32_t y = run.baselineY + (run.sizePx >= 24 ? 3 : 2);
      const int32_t thickness = run.sizePx >= 28 ? 2 : 1;
      for (int32_t t = 0; t < thickness; ++t) {
        for (int32_t x = run.x; x < penX; ++x) sink(ctx, x, y + t, 255);
      }
    }
    if (run.styleFlags & StyleStrikethrough) {
      const int32_t y = run.baselineY - (run.sizePx / 2);
      const int32_t thickness = run.sizePx >= 28 ? 2 : 1;
      for (int32_t t = 0; t < thickness; ++t) {
        for (int32_t x = run.x; x < penX; ++x) sink(ctx, x, y + t, 255);
      }
    }
    // Slice cursor: record progress after every completed run and yield when
    // the slice budget is spent (millis() granularity ~1ms).
    if (nextRunOut != nullptr) *nextRunOut = r + 1;
    if (nextCharOut != nullptr) *nextCharOut = 0;
    if (budgetMs && millis() - start >= budgetMs && r + 1 < page.runCount) return;
  }
}

// Ruby annotations: same glyph walk the engine's PageRenderer::renderRubies
// does (StyleNone, kerning, no underline/strike arms) but routed through the
// caller's tone sink so the gray-parity pipeline covers annotations too.
void walkRubies(const Page& page, FontChain& fonts, void* ctx, const ToneSink sink, const GlyphFilter glyphFilter,
                const uint16_t startRuby = 0, uint16_t* nextRubyOut = nullptr, const uint32_t budgetMs = 0) {
  const uint32_t start = budgetMs ? millis() : 0;
  for (uint16_t r = startRuby; r < page.rubyCount; ++r) {
    const PageRuby& ruby = page.rubies[r];
    const uint32_t tLen = static_cast<uint32_t>(strlen(ruby.text));
    int32_t penX = ruby.x;
    uint32_t i = 0;
    uint32_t prev = 0;
    while (i < tLen) {
      const uint32_t cp = decodeUtf8(ruby.text, tLen, i);
      if (prev != 0) penX += fonts.kerning(prev, cp, ruby.sizePx, StyleNone);
      RenderFont* font = fonts.fontFor(cp, StyleNone, nullptr);
      // Same pre-rasterize band cull as walkText (Copilot, PR #113).
      if (font != nullptr && glyphFilter != nullptr) {
        int16_t bx = 0;
        int16_t by = 0;
        uint16_t bw = 0;
        uint16_t bh = 0;
        if (font->glyphBounds(cp, ruby.sizePx, bx, by, bw, bh) &&
            !glyphFilter(ctx, penX + bx, ruby.baselineY + by, penX + bx + bw, ruby.baselineY + by + bh)) {
          penX += fonts.advance(cp, ruby.sizePx, StyleNone);
          prev = cp;
          continue;
        }
      }
      const GlyphBitmap* glyph = font != nullptr ? font->rasterize(cp, ruby.sizePx) : nullptr;
      if (glyph != nullptr) {
        if (glyphFilter != nullptr &&
            !glyphFilter(ctx, penX + glyph->xoff, ruby.baselineY + glyph->yoff, penX + glyph->xoff + glyph->width,
                         ruby.baselineY + glyph->yoff + glyph->height)) {
          penX += fonts.advance(cp, ruby.sizePx, StyleNone);
          prev = cp;
          continue;
        }
        for (uint16_t gy = 0; gy < glyph->height; ++gy) {
          const uint8_t* srcRow = glyph->pixels + static_cast<uint32_t>(gy) * glyph->width;
          const int32_t dy = ruby.baselineY + glyph->yoff + static_cast<int32_t>(gy);
          for (uint16_t gx = 0; gx < glyph->width; ++gx) {
            sink(ctx, penX + glyph->xoff + static_cast<int32_t>(gx), dy, srcRow[gx]);
          }
        }
      }
      penX += fonts.advance(cp, ruby.sizePx, StyleNone);
      prev = cp;
    }
    // Slice cursor: rubies yield with the same budget as text runs (review
    // r5); the ruby index encodes as cursor run ≥ runCount.
    if (nextRubyOut != nullptr) *nextRubyOut = r + 1;
    if (budgetMs && millis() - start >= budgetMs && r + 1 < page.rubyCount) return;
  }
}

struct BaseCtx {
  const GfxRenderer* renderer;
  int width;
  int height;
};

// BW base: tone >= 1 plots solid ink (the tone-1 boundary is the base rule).
void plotBase(void* ctx, const int32_t x, const int32_t y, const uint8_t coverage) {
  auto* self = static_cast<BaseCtx*>(ctx);
  if (pagepaint::grayTone(coverage) < 1) return;
  if (x < 0 || y < 0 || x >= self->width || y >= self->height) return;  // keep drawPixel's range quiet
  self->renderer->drawPixel(static_cast<int>(x), static_cast<int>(y), true);
}

struct PlaneCtx {
  const GfxRenderer* renderer;
};

// Absolute-gray plane pass (design 2026-11-08 §5.1): same walk as plotPlanes
// but for the absolute encoding — the pass runs full-frame on a 0xFF-seeded
// framebuffer (the plane source), so a dark tone's plane bit is CLEARED and
// white leaves the seed. Tone→plane via the shared grayPlanePixel absolute
// branch: tone n maps to level 3−n (PagePaint tones count up from white,
// plane levels count up from black).
struct AbsolutePlaneCtx {
  const GfxRenderer* renderer;
  int width;
  int height;
  bool msbPass;
  bool binaryText;  // Crisp mode: coverage past the ink threshold, no AA bands
};

// Mono1Sharp's ink threshold (PageRenderer.cpp): the crisp base plots
// coverage above this as solid ink — the plane passes must agree.
constexpr uint8_t kCrispInkThreshold = 120;

void plotAbsolutePlane(void* ctx, const int32_t x, const int32_t y, const uint8_t coverage) {
  auto* self = static_cast<AbsolutePlaneCtx*>(ctx);
  if (self->binaryText) {
    // Crisp text: solid ink or nothing — both planes carry the glyph the
    // same way the crisp base does (tone 3), no gray edge bands.
    if (coverage < kCrispInkThreshold) return;
  } else {
    const uint8_t tone = pagepaint::grayTone(coverage);
    if (tone == 0) return;  // white — the 0xFF seed already carries it
  }
  if (x < 0 || y < 0 || x >= self->width || y >= self->height) return;  // keep drawPixel's range quiet
  if (self->binaryText) {
    self->renderer->drawPixel(static_cast<int>(x), static_cast<int>(y), true);  // clear the bit
    return;
  }
  const auto pixel = grayPlanePixel(static_cast<uint8_t>(3 - pagepaint::grayTone(coverage)), self->msbPass,
                                    /*absolute=*/true);
  if (pixel.black) self->renderer->drawPixel(static_cast<int>(x), static_cast<int>(y), true);  // clear the bit
}

// Dual-plane pass: tone 1 flags MSB, tone 2 flags MSB+LSB; tone 3 is the
// base's job (GrayPlanes.h: plotting solid ink in the shared strip corrupts
// the base) and tone 0 stays untouched.
void plotPlanes(void* ctx, const int32_t x, const int32_t y, const uint8_t coverage) {
  auto* self = static_cast<PlaneCtx*>(ctx);
  const uint8_t tone = pagepaint::grayTone(coverage);
  if (tone < 1 || tone == 3) return;
  self->renderer->drawGrayDualPixel(static_cast<int>(x), static_cast<int>(y),
                                    /*msb=*/true, /*lsb=*/tone == 2);
}

}  // namespace

void PagePaint::paintText(const Page& page, FontChain& fonts, const GfxRenderer& renderer,
                          const int16_t characterSpacingPx, const int16_t wordSpacingPx) {
  BaseCtx ctx{&renderer, renderer.getScreenWidth(), renderer.getScreenHeight()};
  walkText(page, fonts, &ctx, plotBase, nullptr, 0, 0, nullptr, nullptr, 0, characterSpacingPx, wordSpacingPx);
  walkRubies(page, fonts, &ctx, plotBase, nullptr);
}

bool PagePaint::paintTextSliced(const Page& page, FontChain& fonts, const GfxRenderer& renderer,
                                const uint16_t firstRun, const uint32_t firstChar, uint16_t* nextRunOut,
                                uint32_t* nextCharOut, const uint32_t budgetMs, const int16_t characterSpacingPx,
                                const int16_t wordSpacingPx) {
  BaseCtx ctx{&renderer, renderer.getScreenWidth(), renderer.getScreenHeight()};
  if (firstRun < page.runCount) {
    walkText(page, fonts, &ctx, plotBase, nullptr, firstRun, firstChar, nextRunOut, nextCharOut, budgetMs,
             characterSpacingPx, wordSpacingPx);
    if (*nextRunOut < page.runCount) return false;  // budget spent mid-page: resume later
  }
  // Runs complete — rubies phase: the ruby index encodes as cursor run ≥
  // runCount (walkRubies yields on the same budget, review r5).
  const uint16_t firstRuby = firstRun >= page.runCount ? static_cast<uint16_t>(firstRun - page.runCount) : 0;
  walkRubies(page, fonts, &ctx, plotBase, nullptr, firstRuby, nextRunOut, budgetMs);
  if (*nextRunOut < page.rubyCount) {
    // walkRubies yielded (nextRubyOut = the pending ruby index) — encode as
    // cursor run ≥ runCount.
    *nextRunOut = static_cast<uint16_t>(page.runCount + *nextRunOut);
    return false;
  }
  return true;
}

void PagePaint::paintPlanes(const Page& page, FontChain& fonts, const GfxRenderer& renderer,
                            const int16_t characterSpacingPx, const int16_t wordSpacingPx) {
  PlaneCtx ctx{&renderer};
  // Band culling mirrors the legacy tiled walk (GfxRenderer.cpp): glyphs
  // entirely outside the active strip skip their rasterize entirely.
  const auto filter = [](void* c, const int32_t x0, const int32_t y0, const int32_t x1, const int32_t y1) {
    auto* self = static_cast<PlaneCtx*>(c);
    return self->renderer->glyphIntersectsStrip(static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1),
                                                static_cast<int>(y1));
  };
  walkText(page, fonts, &ctx, plotPlanes, filter, 0, 0, nullptr, nullptr, 0, characterSpacingPx, wordSpacingPx);
  walkRubies(page, fonts, &ctx, plotPlanes, filter);
}

void PagePaint::paintTextAbsolutePlanes(const Page& page, FontChain& fonts, const GfxRenderer& renderer,
                                        const bool msbPass, const bool binaryText, const int16_t characterSpacingPx,
                                        const int16_t wordSpacingPx) {
  // Full-frame walk (no strip bands): the caller seeded 0xFF and the plane
  // upload copies the framebuffer whole, so no band filter applies.
  AbsolutePlaneCtx ctx{&renderer, renderer.getScreenWidth(), renderer.getScreenHeight(), msbPass, binaryText};
  walkText(page, fonts, &ctx, plotAbsolutePlane, nullptr, 0, 0, nullptr, nullptr, 0, characterSpacingPx, wordSpacingPx);
  walkRubies(page, fonts, &ctx, plotAbsolutePlane, nullptr);
}

}  // namespace book
}  // namespace freeink

#endif  // CROSSPOINT_TTF_READER