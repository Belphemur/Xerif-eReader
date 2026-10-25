#include "TextSettingsPreview.h"

#include <EpdFontFamily.h>
#include <Epub/ParsedText.h>
#include <Epub/blocks/BlockStyle.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include "CrossPointSettings.h"
#include "fontIds.h"
#if defined(CROSSPOINT_TTF_READER)
#include <BookFontLoader.h>
#include <Memory.h>

#include "layout/ChapterLayout.h"
#include "render/TtfFont.h"
#endif

namespace textsettings {

PreviewLayout::PreviewLayout() = default;
PreviewLayout::~PreviewLayout() = default;

namespace {

// Map the paragraph-alignment setting to the engine's CssTextAlign (BOOK_STYLE = justified)
CssTextAlign toCssAlign(uint8_t align) {
  if (align == CrossPointSettings::BOOK_STYLE) return CssTextAlign::Justify;
  return static_cast<CssTextAlign>(align);
}

// Lay the sample text out through the reader engine into layout.lines
void relayout(PreviewLayout& layout, const GfxRenderer& renderer, int fontId, int textWidth) {
  layout.lines.clear();

  BlockStyle style;
  style.alignment = toCssAlign(SETTINGS.paragraphAlignment);
  style.textAlignDefined = true;  // honor the user's choice; RTL auto-detected from text

  ParsedText parsed(SETTINGS.hyphenationEnabled != 0, SETTINGS.focusReadingEnabled != 0, style,
                    SETTINGS.paragraphIndentSpaces);

  // Feed one space-separated word at a time; addWord handles NFC/CJK/RTL/focus splitting
  const char* text = I18N.get(StrId::STR_FONT_PREVIEW_TEXT);
  std::string word;
  for (const char* p = text;; p++) {
    if (*p == ' ' || *p == '\0') {
      if (!word.empty()) {
        parsed.addWord(word, EpdFontFamily::REGULAR);
        word.clear();
      }
      if (*p == '\0') break;
    } else {
      word.push_back(*p);
    }
  }

  parsed.layoutAndExtractLines(
      renderer, fontId, static_cast<uint16_t>(textWidth),
      [&layout](std::unique_ptr<TextBlock> line, uint32_t) { layout.lines.push_back(std::move(line)); }, true,
      SETTINGS.getCharacterSpacing(), SETTINGS.wordSpacing);
}

#if defined(CROSSPOINT_TTF_READER)

// Scratch budget for the preview's ChapterLayout pass. The engine's STANDARD
// profile peaks ~152KB on full chapters; the two-paragraph sample is tiny, so
// the fixed paragraph buffers + span tables dominate — same size class as the
// reader runtime's scratch, transient (freed when relayoutTtf returns).
constexpr size_t kPreviewScratchBytes = 256 * 1024;

// Run cap: the pane is a few lines tall; two paragraphs never exceed this.
constexpr size_t kMaxPreviewRuns = 24;

// RAM-backed BookSource over the sample text (layoutPlainText reads it whole).
class SampleBookSource final : public freeink::book::BookSource {
 public:
  explicit SampleBookSource(const std::string& data) : data_(data) {}
  int32_t readAt(const uint64_t offset, void* dst, const uint32_t len) override {
    if (offset >= data_.size()) return 0;
    const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(len, data_.size() - offset));
    memcpy(dst, data_.data() + offset, n);
    return static_cast<int32_t>(n);
  }
  uint64_t size() const override { return data_.size(); }

 private:
  const std::string& data_;
};

// Collects the engine's runs (copied — PageTextRun text points into scratch
// and dies with it).
class RunCollector final : public freeink::book::PageSink {
 public:
  std::vector<PreviewRun> runs;

  bool onPage(const freeink::book::Page& page) override {
    for (uint16_t r = 0; r < page.runCount && runs.size() < kMaxPreviewRuns; ++r) {
      const auto& run = page.runs[r];
      if (run.len == 0) continue;
      PreviewRun copy;
      copy.text.assign(run.text, run.len);
      copy.x = run.x;
      copy.baselineY = run.baselineY;
      copy.sizePx = run.sizePx;
      copy.styleFlags = run.styleFlags;
      runs.push_back(std::move(copy));
    }
    return runs.size() < kMaxPreviewRuns;  // false stops layout early
  }
};

// Lays the two-paragraph sample through the real engine with the pane's
// geometry; replaces layout.ttfRuns. On scratch OOM the previous runs are
// kept (stale preview for one frame, no crash).
bool relayoutTtf(PreviewLayout& layout, const int textWidth, const int previewHeight) {
  std::vector<PreviewRun> collected;
  bool ok = false;
  int16_t charSpacingPx = 0;
  int16_t wordSpacingPx = 0;
  do {
    // Sample text twice: the engine splits paragraphs on blank lines, so the
    // pane shows the real paragraph gap (the legacy preview drew twice).
    const char* text = I18N.get(StrId::STR_FONT_PREVIEW_TEXT);
    std::string sample;
    sample.reserve(strlen(text) * 2 + 2);
    sample += text;
    sample += "\n\n";
    sample += text;

    freeink::book::LayoutParams params;
    params.pageWidth = static_cast<int16_t>(textWidth);
    params.pageHeight = static_cast<int16_t>(previewHeight);
    params.marginLeft = 0;
    params.marginRight = 0;
    params.marginTop = 2;
    params.marginBottom = 0;
    params.baseSizePx = static_cast<uint16_t>(lroundf(static_cast<float>(SETTINGS.ttfFontPointSize) * 150.0f / 72.0f));
    params.lineSpacingPct = 100;
    switch (SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT) {
      case CrossPointSettings::TIGHT:
        params.lineSpacingPct = 95;
        break;
      case CrossPointSettings::WIDE:
        params.lineSpacingPct = 110;
        break;
      case CrossPointSettings::EXTRA_WIDE:
        params.lineSpacingPct = 120;
        break;
      default:
        break;
    }
    // Same §2.3 mapping as the reader (TtfBookRuntime::makeLayoutParams).
    params.paragraphSpacingPct = SETTINGS.extraParagraphSpacing != 0 ? 150 : 100;
    switch (SETTINGS.paragraphAlignment % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT) {
      case CrossPointSettings::LEFT_ALIGN:
        params.defaultAlign = freeink::book::TextAlign::Left;
        break;
      case CrossPointSettings::CENTER_ALIGN:
        params.defaultAlign = freeink::book::TextAlign::Center;
        break;
      case CrossPointSettings::RIGHT_ALIGN:
        params.defaultAlign = freeink::book::TextAlign::Right;
        break;
      case CrossPointSettings::JUSTIFIED:
      case CrossPointSettings::BOOK_STYLE:
      default:
        params.defaultAlign = freeink::book::TextAlign::Justify;
        break;
    }
    params.focusReading = SETTINGS.focusReadingEnabled != 0;
    // Mirrors the reader's mapping (TtfBookRuntime::makeLayoutParams) so the
    // preview renders with the same chapter CSS policy.
    params.embeddedStyles = SETTINGS.embeddedStyle != 0;
    params.hyphenator = nullptr;
    // Same spacing resolution as applyReaderLayoutParams, so the sample tracks
    // the reader when either spacing control changes.
    const int16_t bodySpacePx = static_cast<int16_t>(params.baseSizePx / 4);
    params.wordSpacingPx = static_cast<int16_t>(
        (static_cast<int32_t>(bodySpacePx) * static_cast<int32_t>(SETTINGS.wordSpacing)) / 100 - bodySpacePx);
    params.characterSpacingPx = SETTINGS.getCharacterSpacing();
    charSpacingPx = params.characterSpacingPx;
    wordSpacingPx = params.wordSpacingPx;
    freeink::book::FontChain* chain = freeink::book::fontLoader.getReaderFont();
    if (chain == nullptr || chain->styleCoverage() == 0) break;
    params.font = chain;

    const auto scratch = poolMakeBytes(kPreviewScratchBytes);
    if (!scratch) break;  // OOM: keep the previous runs
    freeink::book::Arena arena(scratch.get(), kPreviewScratchBytes);

    SampleBookSource source(sample);
    RunCollector sink;
    const auto st = freeink::book::ChapterLayout::layoutPlainText(source, params, arena, sink, nullptr, nullptr);
    // RunCollector's kMaxPreviewRuns early-stop still returns Ok — only a
    // real layout failure (OOM) must keep the previous runs and key.
    if (st == freeink::book::BookStatus::Ok) {
      collected = std::move(sink.runs);
      ok = true;
    }
  } while (false);

  // Only a complete relayout replaces the previous runs and lets the caller
  // advance the key: a failed pass keeps both, so the next frame retries
  // instead of blanking the preview.
  if (ok) {
    layout.ttfRuns = std::move(collected);
    layout.characterSpacingPx = charSpacingPx;
    layout.wordSpacingPx = wordSpacingPx;
  }
  return ok;
}

// UTF-8 decode (mirror of PageRenderer's TU-local decoder).
uint32_t decodeUtf8(const char* text, const uint32_t len, uint32_t& i) {
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

// Plots the laid-out runs into the preview pane at logical (textLeft, top):
// the glyph walk mirrors PageRenderer::renderText (kerning, synthetic-bold
// double-strike), but coverage lands through GfxRenderer::drawPixel so the
// renderer's orientation transform applies — a Phase 3.5 PagePaint preview.
void drawTtfRuns(const GfxRenderer& renderer, const std::vector<PreviewRun>& runs, const int textLeft, const int top,
                 const int bottom, const int right, const int16_t characterSpacingPx = 0,
                 const int16_t wordSpacingPx = 0) {
  freeink::book::FontChain* fonts = freeink::book::fontLoader.getReaderFont();
  if (fonts == nullptr) return;
  // The engine charges these on every advance; replay them or the sample paints
  // tighter than the text it is previewing.
  const auto spacingFor = [characterSpacingPx, wordSpacingPx](const uint32_t cp) {
    return cp == ' ' ? wordSpacingPx : characterSpacingPx;
  };
  for (const auto& run : runs) {
    // run.x is pane-relative (layout used pageWidth=textWidth, margins 0).
    int32_t penX = textLeft + run.x;
    uint32_t i = 0;
    uint32_t prev = 0;
    while (i < run.text.size()) {
      const uint32_t cp = decodeUtf8(run.text.data(), static_cast<uint32_t>(run.text.size()), i);
      if (prev != 0) penX += fonts->kerning(prev, cp, run.sizePx, run.styleFlags);
      uint8_t faceFlags = 0;
      freeink::book::RenderFont* font = fonts->fontFor(cp, run.styleFlags, &faceFlags);
      const freeink::book::GlyphBitmap* glyph = font != nullptr ? font->rasterize(cp, run.sizePx) : nullptr;
      if (glyph != nullptr) {
        const int strikes =
            (run.styleFlags & freeink::book::StyleBold) != 0 && (faceFlags & freeink::book::StyleBold) == 0 ? 2 : 1;
        for (int s = 0; s < strikes; ++s) {
          for (uint16_t gy = 0; gy < glyph->height; ++gy) {
            const int32_t y = top + run.baselineY + glyph->yoff + static_cast<int32_t>(gy);
            if (y < top || y >= bottom) continue;
            const uint8_t* srcRow = glyph->pixels + static_cast<uint32_t>(gy) * glyph->width;
            for (uint16_t gx = 0; gx < glyph->width; ++gx) {
              if (srcRow[gx] >= 96) {  // mid threshold: preview-grade ink
                const int px = static_cast<int>(penX + glyph->xoff + static_cast<int32_t>(gx) + s);
                if (px < textLeft || px >= right) continue;
                renderer.drawPixel(px, static_cast<int>(y), true);
              }
            }
          }
        }
      }
      penX += fonts->advance(cp, run.sizePx, run.styleFlags) + spacingFor(cp);
      prev = cp;
    }
  }
}
#endif  // CROSSPOINT_TTF_READER

}  // namespace

void renderPreview(const GfxRenderer& renderer, PreviewLayout& layout, int previewPadding, int labelGap, Rect bounds,
                   const char* caption) {
  const int top = bounds.y;
  const int height = bounds.height;
  const int left = bounds.x + previewPadding;
  const int width = bounds.width - (previewPadding * 2);
  if (width <= 0 || height <= 0) return;

  const int labelH = renderer.getTextHeight(UI_10_FONT_ID);
  const int labelReserved = labelH + labelGap + previewPadding;

  const int labelY = top + height - previewPadding - labelH;
  renderer.drawText(UI_10_FONT_ID, left, labelY, caption);

  const int textLeft = left + SETTINGS.screenMargin;
  const int textWidth = width - 2 * SETTINGS.screenMargin;
  if (textWidth <= 0) return;

#if defined(CROSSPOINT_TTF_READER)
  if (SETTINGS.readerFontEngine == CrossPointSettings::READER_ENGINE_TTF) {
    // Native-TTF pane: the sample is laid out and rasterized through the
    // active FontChain (§3.6). The engine chain is the preview's identity,
    // so the key carries the content fingerprint + continuous point size.
    // getReaderFont() first: it runs ensureLoaded, which recomputes the
    // fingerprint when a family change marked the loader dirty — the key
    // must be built from the fingerprint the relayout will render with.
    (void)freeink::book::fontLoader.getReaderFont();
    const PreviewKey key{.fontId = -1,
                         .fontPointSize = -1,
                         .screenMargin = SETTINGS.screenMargin,
                         .textWidth = textWidth,
                         .lineCompression = SETTINGS.getReaderLineCompression(),
                         .alignment = SETTINGS.paragraphAlignment,
                         .extraParagraphSpacing = SETTINGS.extraParagraphSpacing != 0,
                         .characterSpacing = SETTINGS.getCharacterSpacing(),
                         .wordSpacingPercent = SETTINGS.wordSpacing,
                         .focusReading = SETTINGS.focusReadingEnabled != 0,
                         .hyphenation = SETTINGS.hyphenationEnabled != 0,
                         .embeddedStyle = SETTINGS.embeddedStyle != 0,
                         .engine = SETTINGS.readerFontEngine,
                         .fingerprint = freeink::book::fontLoader.fontFingerprint(),
                         .ttfPointSize = SETTINGS.ttfFontPointSize,
                         .previewHeight = height};
    if (key != layout.key) {
      // getReaderFont first: ensureLoaded may recompute the fingerprint (a
      // family change marks the loader dirty), and the key must carry the
      // fingerprint the relayout actually rendered with.
      if (relayoutTtf(layout, textWidth, height)) {
        layout.key = key;
      }
    }
    const int top2 = top + previewPadding;
    const int bottom = top + height - labelReserved;
    drawTtfRuns(renderer, layout.ttfRuns, textLeft, top2, bottom, textLeft + textWidth, layout.characterSpacingPx,
                layout.wordSpacingPx);
    return;
  }
#endif

  const int fontId = SETTINGS.getReaderFontId();
  if (fontId == 0) return;

  const int lineH = renderer.getTextHeight(fontId);
  if (lineH <= 0) return;

  const float compression = SETTINGS.getReaderLineCompression();
  const int lineAdvance = std::max(1, renderer.getLineHeight(fontId, compression));
  const int paragraphGap = SETTINGS.extraParagraphSpacing ? lineAdvance / 2 : 0;

  // Re-lay-out (and re-prewarm glyphs) only when a layout-affecting setting or the
  // geometry changed; else reuse the cache. The prewarm inputs are (fontId, constant
  // sample text, styleMask<-focusReading), all of which are key fields, so a matching
  // key means an identical prewarm call. This relies on nothing else evicting the SD
  // glyph cache while this activity is up — true today: the only evictor is
  // FontCacheManager::PrewarmScope, used solely by the reader/dictionary activities.
  const PreviewKey key{.fontId = fontId,
                       .fontPointSize = SETTINGS.fontPointSize,
                       .screenMargin = SETTINGS.screenMargin,
                       .textWidth = textWidth,
                       .lineCompression = compression,
                       .alignment = SETTINGS.paragraphAlignment,
                       .extraParagraphSpacing = SETTINGS.extraParagraphSpacing != 0,
                       .paragraphIndentSpaces = SETTINGS.paragraphIndentSpaces,
                       .characterSpacing = SETTINGS.getCharacterSpacing(),
                       .wordSpacingPercent = SETTINGS.wordSpacing,
                       .focusReading = SETTINGS.focusReadingEnabled != 0,
                       .hyphenation = SETTINGS.hyphenationEnabled != 0,
                       .embeddedStyle = SETTINGS.embeddedStyle != 0};
  if (key != layout.key) {
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->prewarmCache(fontId, I18N.get(StrId::STR_FONT_PREVIEW_TEXT), SETTINGS.focusReadingEnabled ? 0x03 : 0x01);
    }
    relayout(layout, renderer, fontId, textWidth);
    layout.key = key;
  }

  // Draw the sample twice so the paragraph gap is visible
  int y = top + previewPadding;
  const int textBottomLimit = top + height - labelReserved;
  for (int paragraph = 0; paragraph < 2; paragraph++) {
    for (const auto& line : layout.lines) {
      if (y + lineH > textBottomLimit) return;
      line->render(renderer, fontId, textLeft, y);
      y += lineAdvance;
    }
    y += paragraphGap;
  }
}
}  // namespace textsettings
