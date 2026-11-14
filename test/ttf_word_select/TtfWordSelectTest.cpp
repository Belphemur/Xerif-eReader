// Host tests for TtfWordSelect::buildTtfWordSelectData — token extraction
// from engine run geometry with a real TtfFont (Amazon Ember fixture), the
// seam behind the restored TTF dictionary entry point (PR #113 §17).
#include <FtFont.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

#include "TtfWordSelect.h"
#include "render/TtfFont.h"

namespace {

const std::string& emberBytes() {
  static const std::string bytes = [] {
    std::ifstream f(EMBER_FIXTURES_DIR "/Amazon_Ember_Regular.ttf", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }();
  return bytes;
}

freeink::book::Page makePage(const char* text, const uint16_t len) {
  static freeink::book::PageTextRun run{};
  run.text = text;
  run.charStart = 0;
  run.len = len;
  run.x = 10;
  run.baselineY = 40;
  run.sizePx = 20;
  run.charLen = len;
  run.styleFlags = freeink::book::StyleNone;
  run.layoutFlags = 0;
  static freeink::book::Page page{};
  page.runs = &run;
  page.runCount = 1;
  return page;
}

}  // namespace

// Backend-selective face (design D2/D9): the BackendFT target must exercise
// FreeType word selection, not merely compile it in. stb keeps its arena
// constructor; FtFont takes size/weight/italic and needs no arena.
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
using SelectFace = freeink::font::FtFont;
bool loadSelectFace(SelectFace& face, const std::string& bytes) {
  return face.init(reinterpret_cast<const uint8_t*>(bytes.data()), static_cast<uint32_t>(bytes.size()), 16, 400, false);
}
#else
using SelectFace = freeink::book::TtfFont;
bool loadSelectFace(SelectFace& face, const std::string& bytes) {
  static uint8_t arena[64 * 1024];
  freeink::book::Arena glyphArena(arena, sizeof(arena));
  return face.init(reinterpret_cast<const uint8_t*>(bytes.data()), static_cast<uint32_t>(bytes.size()), glyphArena);
}
#endif

TEST(TtfWordSelect, SplitsRunsIntoWhitespaceTokens) {
  static const char kText[] = "alpha beta";
  const auto page = makePage(kText, static_cast<uint16_t>(std::strlen(kText)));
  SelectFace font;
  const std::string& bytes = emberBytes();
  if (bytes.empty()) GTEST_SKIP() << "Missing Amazon Ember fixture";
  ASSERT_GE(bytes.size(), 16u);
  ASSERT_TRUE(loadSelectFace(font, bytes));
  freeink::book::FontChain chain;
  chain.add(&font, freeink::book::StyleNone);

  freeink::book::TtfWordSelectData data;
  ASSERT_TRUE(freeink::book::buildTtfWordSelectData(page, chain, data));
  ASSERT_EQ(data.boxes.size(), 2u);
  EXPECT_STREQ(data.boxes[0].text, "alpha");
  EXPECT_STREQ(data.boxes[1].text, "beta");
  EXPECT_EQ(data.boxes[0].textOffset, 0u);
  EXPECT_EQ(data.boxes[0].textLength, 5u);
  EXPECT_EQ(data.boxes[0].rawLength, 5u);
  // textOffset/textLength address the trim range INSIDE the copied token;
  // clean tokens have nothing to trim (offset 0, length == rawLength).
  EXPECT_EQ(data.boxes[1].textOffset, 0u);
  EXPECT_EQ(data.boxes[1].textLength, 4u);
  EXPECT_EQ(data.boxes[1].rawLength, 4u);
  // Geometry: y is the line-box top (baseline - ascent), boxes advance in x.
  EXPECT_LT(data.boxes[0].x, data.boxes[1].x);
  EXPECT_EQ(data.boxes[0].height, chain.lineHeight(20));
}

// Spacing-knob replay (kept scope-cut feature): token geometry must match the
// painted glyph positions — PagePaint adds word spacing on the space glyph and
// character spacing on everything else. Pin the delta between the 0-spacing
// walk and the spaced walk, and that spacing does not shift the FIRST token
// (its pen starts at run.x with no preceding glyph).
TEST(TtfWordSelect, SpacingKnobsReplayThePaintWalk) {
  static const char kText[] = "alpha beta";
  const auto page = makePage(kText, static_cast<uint16_t>(std::strlen(kText)));
  SelectFace font;
  const std::string& bytes = emberBytes();
  if (bytes.empty()) GTEST_SKIP() << "Missing Amazon Ember fixture";
  ASSERT_TRUE(loadSelectFace(font, bytes));
  freeink::book::FontChain chain;
  chain.add(&font, freeink::book::StyleNone);

  freeink::book::TtfWordSelectData plain;
  ASSERT_TRUE(freeink::book::buildTtfWordSelectData(page, chain, plain));
  freeink::book::TtfWordSelectData spaced;
  ASSERT_TRUE(freeink::book::buildTtfWordSelectData(page, chain, spaced, /*characterSpacingPx=*/2,
                                                    /*wordSpacingPx=*/4));
  ASSERT_EQ(plain.boxes.size(), spaced.boxes.size());
  // First token: no glyph precedes it, so both walks start at run.x.
  EXPECT_EQ(spaced.boxes[0].x, plain.boxes[0].x);
  // Second token: one character-spacing advance per glyph of "alpha" (2x5)
  // plus one word spacing on the inter-token space (4).
  EXPECT_EQ(spaced.boxes[1].x - plain.boxes[1].x, 2 * 5 + 4);
  // The token text/trim ranges are spacing-invariant.
  EXPECT_EQ(spaced.boxes[1].textLength, plain.boxes[1].textLength);
}
