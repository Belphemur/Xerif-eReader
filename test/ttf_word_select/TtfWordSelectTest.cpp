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

// Chapter char anchors (design doc ttf/2026-11-09 §F3): trimmed-token
// codepoint ranges derived consumer-side from run.charStart, gated to LTR
// tokens. `charLen` counts chapter CODEPOINTS, so multi-byte UTF-8 and baked
// ligature folds must be handled without assuming 1 byte = 1 codepoint.
namespace {

// Runs the anchor walkthrough on one synthetic run via the Ember fixture.
bool buildOnEmber(const char* text, freeink::book::TtfWordSelectData& data) {
  SelectFace font;
  const std::string& bytes = emberBytes();
  if (bytes.empty()) return false;
  if (!loadSelectFace(font, bytes)) return false;
  freeink::book::FontChain chain;
  chain.add(&font, freeink::book::StyleNone);
  const auto page = makePage(text, static_cast<uint16_t>(std::strlen(text)));
  return freeink::book::buildTtfWordSelectData(page, chain, data);
}

}  // namespace

TEST(TtfWordSelect, AsciiAnchorsCountTrimmedCodepoints) {
  freeink::book::TtfWordSelectData data;
  ASSERT_TRUE(buildOnEmber("(alpha) beta", data));
  ASSERT_EQ(data.boxes.size(), 2u);
  EXPECT_STREQ(data.boxes[0].text, "(alpha)");
  // Leading '(' is skipped by the trim but is a real chapter codepoint: the
  // anchor points at the trimmed token's first character.
  EXPECT_EQ(data.boxes[0].charStart, 1u);
  EXPECT_EQ(data.boxes[0].charLen, 5u);
  EXPECT_EQ(data.boxes[1].charStart, 8u);
  EXPECT_EQ(data.boxes[1].charLen, 4u);
}

TEST(TtfWordSelect, MultiByteUtf8AnchorsCountCodepointsNotBytes) {
  freeink::book::TtfWordSelectData data;
  ASSERT_TRUE(buildOnEmber("na\xc3\xafve caf\xc3\xa9", data));
  ASSERT_EQ(data.boxes.size(), 2u);
  // "naïve": 5 codepoints (ï is one 2-byte cp) over 6 UTF-8 bytes.
  EXPECT_EQ(data.boxes[0].charStart, 0u);
  EXPECT_EQ(data.boxes[0].charLen, 5u);
  // "café": 4 codepoints over 5 bytes; the anchor rides the 6 codepoints of
  // "naïve" plus the space.
  EXPECT_EQ(data.boxes[1].charStart, 6u);
  EXPECT_EQ(data.boxes[1].charLen, 4u);
}

TEST(TtfWordSelect, RtlTokensGetNoAnchor) {
  freeink::book::TtfWordSelectData data;
  ASSERT_TRUE(buildOnEmber("word \xd9\x83\xd8\xaa\xd8\xa8", data));  // كتب — Arabic
  ASSERT_EQ(data.boxes.size(), 2u);
  EXPECT_EQ(data.boxes[0].charStart, 0u);
  // RTL tokens are stored in visual order engine-side: no anchor, so the
  // clip degrades to text-only (design doc §F3).
  EXPECT_EQ(data.boxes[1].charStart, UINT32_MAX);
  EXPECT_EQ(data.boxes[1].charLen, 0u);
}

TEST(TtfWordSelect, LigatureCodepointWeightBakedCountsBothCodepoints) {
  // The layout re-encode folds ligature chains into one baked codepoint, so
  // the anchor walk must count a baked FB00-series cp as 2-3 chapter cps.
  // Ember folds ligatures (FontChain::ligature('f','i')==FB01), so a raw
  // FB01 weighing 2 here is exactly what the engine produces for "fi" — the
  // indistinguishable raw-occurrence residual is documented in the design
  // addendum as a bounded, per-run drift.
  freeink::book::TtfWordSelectData data;
  ASSERT_TRUE(buildOnEmber("a \xef\xac\x81 b", data));  // "a FB01 b"
  ASSERT_EQ(data.boxes.size(), 3u);
  EXPECT_EQ(data.boxes[1].charStart, 2u);
  EXPECT_EQ(data.boxes[1].charLen, 2u);  // baked fi covers f + i
  EXPECT_EQ(data.boxes[2].charStart, 5u);
}

TEST(TtfWordSelect, LigatureChainFinalWeighsThreeCodepoints) {
  // A chain final (FB03 = ffi baked from f+f+i) covers three chapter cps:
  // the walk per-baked-cp weight reproduces the engine's fold chain.
  freeink::book::TtfWordSelectData data;
  ASSERT_TRUE(buildOnEmber("\xef\xac\x83 x", data));  // "FB03 x"
  ASSERT_EQ(data.boxes.size(), 2u);
  EXPECT_EQ(data.boxes[0].charLen, 3u);
  EXPECT_EQ(data.boxes[1].charStart, 4u);  // 3 (FB03) + 1 (space)
}
