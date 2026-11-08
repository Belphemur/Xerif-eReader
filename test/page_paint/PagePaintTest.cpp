// PagePaint host tests: the shared tone quantizer, base/plane consistency,
// and strip-band vs full-frame plane equivalence (the two gray transports
// must produce identical plane bits for the same page — E-Ink AA research,
// 2026-09, "full-frame versus strip" host test row).
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "Arduino.h"  // host stub: g_fakeMillis clock control
#include "GfxRenderer.h"
#include "PagePaint.h"

namespace {

// A 4x4 glyph whose pixels hit every quantizer boundary; persistent storage
// (rasterize results must outlive the walk).
constexpr uint16_t kW = 4;
constexpr uint16_t kH = 4;
const uint8_t kCoverage[kW * kH] = {
    0,   42,  43,  127,  // white / light boundaries
    128, 212, 213, 255,  // dark / black boundaries
    1,   96,  144, 100,  // mid-band samples
    255, 0,   43,  128,
};

class FakeRenderFont final : public freeink::book::RenderFont {
 public:
  // Each metric step advances the fake clock (see the Arduino.h stub) so the
  // sliced-paint tests can force deterministic yields — 130 ms per glyph vs
  // a 120 ms budget yields after every painted glyph.
  int16_t advance(uint32_t, uint16_t, uint8_t) override {
    g_fakeMillis += 130;
    return static_cast<int16_t>(kW);
  }
  int16_t lineHeight(uint16_t) override { return 16; }
  int16_t ascent(uint16_t) override { return 12; }
  bool hasGlyph(uint32_t) const override { return true; }
  bool glyphBounds(uint32_t, uint16_t, int16_t& xoff, int16_t& yoff, uint16_t& width, uint16_t& height) const override {
    xoff = 0;
    yoff = -static_cast<int16_t>(kH);
    width = kW;
    height = kH;
    return true;
  }
  const freeink::book::GlyphBitmap* rasterize(uint32_t, uint16_t) override {
    glyph_ = {kCoverage, kW, kH, 0, -static_cast<int16_t>(kH), static_cast<int16_t>(kW)};
    return &glyph_;
  }

 private:
  freeink::book::GlyphBitmap glyph_{};
};

// One two-glyph run: every sample of the coverage table lands on the page.
freeink::book::Page makeTestPage() {
  static const char kText[] = "AA";
  static freeink::book::PageTextRun run{};
  run.text = kText;
  run.charStart = 0;
  run.len = 2;
  run.x = 8;
  run.baselineY = 24;
  run.sizePx = 16;
  run.charLen = 2;
  run.styleFlags = freeink::book::StyleNone;
  run.layoutFlags = 0;
  static freeink::book::Page page{};
  page.runs = &run;
  page.runCount = 1;
  return page;  // POD struct; run/text outlive the test scope
}

// Three runs at distinct x — enough runs for the sliced-paint pump to yield
// BETWEEN runs and to exercise the intra-run cursor on a multi-run page.
freeink::book::Page makeMultiRunPage() {
  static const char kTexts[3][3] = {"AA", "BB", "CC"};
  static freeink::book::PageTextRun runs[3] = {};
  for (int r = 0; r < 3; ++r) {
    runs[r].text = kTexts[r];
    runs[r].charStart = 0;
    runs[r].len = 2;
    runs[r].x = static_cast<int16_t>(8 + r * 40);
    runs[r].baselineY = 24;
    runs[r].sizePx = 16;
    runs[r].charLen = 2;
    runs[r].styleFlags = freeink::book::StyleNone;
    runs[r].layoutFlags = 0;
  }
  static freeink::book::Page page{};
  page.runs = runs;
  page.runCount = 3;
  return page;
}

bool bitAt(const uint8_t* plane, const int x, const int y) {
  return (plane[y * GfxRenderer::kStride + x / 8] & (0x80 >> (x & 7))) != 0;
}

// The two glyphs render at x 8..15, rows 20..23 (baselineY 24, yoff -4).
bool inTestGlyph(const int x, const int y, int& sampleIdx) {
  if (x < 8 || x >= 8 + 2 * static_cast<int>(kW)) return false;
  const int gy = y - 20;
  if (gy < 0 || gy >= static_cast<int>(kH)) return false;
  sampleIdx = gy * kW + ((x - 8) % kW);
  return true;
}

}  // namespace

TEST(PagePaintQuantizer, UniformBaselineBoundaries) {
  using freeink::book::pagepaint::grayTone;
  EXPECT_EQ(grayTone(0), 0);
  EXPECT_EQ(grayTone(42), 0);
  EXPECT_EQ(grayTone(43), 1);
  EXPECT_EQ(grayTone(127), 1);
  EXPECT_EQ(grayTone(128), 2);
  EXPECT_EQ(grayTone(212), 2);
  EXPECT_EQ(grayTone(213), 3);
  EXPECT_EQ(grayTone(255), 3);
}

TEST(PagePaintQuantizer, ExhaustiveCoverageMapsToValidTones) {
  using freeink::book::pagepaint::grayTone;
  uint8_t prev = 0;
  for (unsigned coverage = 0; coverage <= 255; ++coverage) {
    const uint8_t tone = grayTone(static_cast<uint8_t>(coverage));
    ASSERT_LE(tone, 3);
    ASSERT_GE(tone, prev);  // monotonic: more coverage never gets lighter
    prev = tone;
  }
  // The baseline spans all four tones.
  EXPECT_EQ(prev, 3);
}

// Base and planes must derive from ONE quantizer: base plots tone >= 1,
// MSB marks tones 1-2, LSB marks tone 2, tone 3 is base-only, tone 0 is
// untouched everywhere.
TEST(PagePaintConsistency, BaseAndPlanesAgreeWithSharedQuantizer) {
  using freeink::book::pagepaint::grayTone;
  freeink::book::FontChain chain;
  FakeRenderFont face;
  chain.add(&face, freeink::book::StyleNone);

  GfxRenderer renderer;
  const freeink::book::Page page = makeTestPage();
  freeink::book::PagePaint::paintText(page, chain, renderer);

  uint8_t lsb[GfxRenderer::kPlaneBytes] = {};
  uint8_t msb[GfxRenderer::kPlaneBytes] = {};
  renderer.beginStripTarget(lsb, 0, GfxRenderer::kPanelH, msb);
  freeink::book::PagePaint::paintPlanes(page, chain, renderer);
  renderer.endStripTarget();

  for (int y = 0; y < GfxRenderer::kPanelH; ++y) {
    for (int x = 0; x < GfxRenderer::kPanelW; ++x) {
      int sampleIdx = -1;
      const uint8_t coverage = inTestGlyph(x, y, sampleIdx) ? kCoverage[sampleIdx] : 0;
      const uint8_t tone = grayTone(coverage);
      EXPECT_EQ(bitAt(renderer.base, x, y), tone >= 1) << x << "," << y;
      EXPECT_EQ(bitAt(msb, x, y), tone == 1 || tone == 2) << x << "," << y;
      EXPECT_EQ(bitAt(lsb, x, y), tone == 2) << x << "," << y;
    }
  }
}

// The two transports must produce identical plane bits: one full-frame DUAL
// target vs the aggregate of band-wise targets (the renderer-side rotate/
// clip is shared code, so the PagePaint walk + band culling is the part
// under test here).
TEST(PagePaintEquivalence, StripBandsMatchFullFrame) {
  freeink::book::FontChain chain;
  FakeRenderFont face;
  chain.add(&face, freeink::book::StyleNone);

  GfxRenderer renderer;
  const freeink::book::Page page = makeTestPage();

  uint8_t fullLsb[GfxRenderer::kPlaneBytes] = {};
  uint8_t fullMsb[GfxRenderer::kPlaneBytes] = {};
  renderer.beginStripTarget(fullLsb, 0, GfxRenderer::kPanelH, fullMsb);
  freeink::book::PagePaint::paintPlanes(page, chain, renderer);
  renderer.endStripTarget();

  constexpr int kBandRows = 16;
  uint8_t bandLsb[GfxRenderer::kPlaneBytes] = {};
  uint8_t bandMsb[GfxRenderer::kPlaneBytes] = {};
  for (int y0 = 0; y0 < GfxRenderer::kPanelH; y0 += kBandRows) {
    uint8_t lsb[kBandRows * GfxRenderer::kStride] = {};
    uint8_t msb[kBandRows * GfxRenderer::kStride] = {};
    renderer.beginStripTarget(lsb, y0, kBandRows, msb);
    freeink::book::PagePaint::paintPlanes(page, chain, renderer);
    renderer.endStripTarget();
    std::memcpy(bandLsb + y0 * GfxRenderer::kStride, lsb, kBandRows * GfxRenderer::kStride);
    std::memcpy(bandMsb + y0 * GfxRenderer::kStride, msb, kBandRows * GfxRenderer::kStride);
  }

  EXPECT_EQ(std::memcmp(fullLsb, bandLsb, GfxRenderer::kPlaneBytes), 0);
  EXPECT_EQ(std::memcmp(fullMsb, bandMsb, GfxRenderer::kPlaneBytes), 0);
}

// Absolute-gray plane passes (design 2026-11-08 §5.1): the pass-specific
// tone→plane mapping — tone 1 darkens LSB, tone 2 MSB, tone 3 both, tone 0
// neither. The stub's drawPixel(true) marks the calls; on device a true
// drawPixel clears the plane bit (the 0xFF seed is white).
TEST(PagePaintAbsolutePlanes, TonesDarkenExactlyTheirPlane) {
  using freeink::book::pagepaint::grayTone;
  freeink::book::FontChain chain;
  FakeRenderFont face;
  chain.add(&face, freeink::book::StyleNone);

  GfxRenderer renderer;
  const freeink::book::Page page = makeTestPage();

  uint8_t lsbCalls[GfxRenderer::kPlaneBytes] = {};
  uint8_t msbCalls[GfxRenderer::kPlaneBytes] = {};
  freeink::book::PagePaint::paintTextAbsolutePlanes(page, chain, renderer, /*msbPass=*/false);
  std::memcpy(lsbCalls, renderer.base, sizeof(lsbCalls));
  std::memset(renderer.base, 0, sizeof(renderer.base));
  freeink::book::PagePaint::paintTextAbsolutePlanes(page, chain, renderer, /*msbPass=*/true);
  std::memcpy(msbCalls, renderer.base, sizeof(msbCalls));

  for (int y = 0; y < GfxRenderer::kPanelH; ++y) {
    for (int x = 0; x < GfxRenderer::kPanelW; ++x) {
      int sampleIdx = -1;
      const uint8_t coverage = inTestGlyph(x, y, sampleIdx) ? kCoverage[sampleIdx] : 0;
      const uint8_t tone = grayTone(coverage);
      EXPECT_EQ(bitAt(lsbCalls, x, y), tone == 1 || tone == 3) << x << "," << y;
      EXPECT_EQ(bitAt(msbCalls, x, y), tone == 2 || tone == 3) << x << "," << y;
    }
  }
}

// Crisp mode (`binaryText`): coverage at/over the Mono1Sharp ink threshold
// (120) darkens BOTH planes (solid ink); anything below stays white — the
// plane-encoded text matches the crisp base instead of AA gray bands
// (review round 2).
TEST(PagePaintAbsolutePlanes, CrispBinaryTextDarkensBothPlanesAtInkOnly) {
  using freeink::book::pagepaint::grayTone;
  freeink::book::FontChain chain;
  FakeRenderFont face;
  chain.add(&face, freeink::book::StyleNone);

  GfxRenderer renderer;
  const freeink::book::Page page = makeTestPage();

  uint8_t lsbCalls[GfxRenderer::kPlaneBytes] = {};
  uint8_t msbCalls[GfxRenderer::kPlaneBytes] = {};
  freeink::book::PagePaint::paintTextAbsolutePlanes(page, chain, renderer, /*msbPass=*/false, /*binaryText=*/true);
  std::memcpy(lsbCalls, renderer.base, sizeof(lsbCalls));
  std::memset(renderer.base, 0, sizeof(renderer.base));
  freeink::book::PagePaint::paintTextAbsolutePlanes(page, chain, renderer, /*msbPass=*/true, /*binaryText=*/true);
  std::memcpy(msbCalls, renderer.base, sizeof(msbCalls));

  for (int y = 0; y < GfxRenderer::kPanelH; ++y) {
    for (int x = 0; x < GfxRenderer::kPanelW; ++x) {
      int sampleIdx = -1;
      const uint8_t coverage = inTestGlyph(x, y, sampleIdx) ? kCoverage[sampleIdx] : 0;
      EXPECT_EQ(bitAt(lsbCalls, x, y), coverage >= 120) << x << "," << y;
      EXPECT_EQ(bitAt(msbCalls, x, y), coverage >= 120) << x << "," << y;
    }
  }
}

// Tone 0 (white) must leave the 0xFF seed alone: the absolute planes carry
// the whole frame, so a white write would be redundant and a dark write on a
// white sample would speckle the panel.
TEST(PagePaintAbsolutePlanes, WhiteToneLeavesThePlaneSeed) {
  using freeink::book::pagepaint::grayTone;
  freeink::book::FontChain chain;
  FakeRenderFont face;
  chain.add(&face, freeink::book::StyleNone);

  GfxRenderer renderer;
  const freeink::book::Page page = makeTestPage();
  freeink::book::PagePaint::paintTextAbsolutePlanes(page, chain, renderer, /*msbPass=*/false);

  for (int y = 0; y < GfxRenderer::kPanelH; ++y) {
    for (int x = 0; x < GfxRenderer::kPanelW; ++x) {
      int sampleIdx = -1;
      const uint8_t coverage = inTestGlyph(x, y, sampleIdx) ? kCoverage[sampleIdx] : 0;
      const uint8_t tone = grayTone(coverage);
      if (tone == 0) EXPECT_FALSE(bitAt(renderer.base, x, y)) << x << "," << y;
    }
  }
}

// The base pass (paintText) is band-independent by construction (no filter);
// assert it plots the tone >= 1 rule for the same page.
TEST(PagePaintEquivalence, BasePlotsToneOneBoundary) {
  using freeink::book::pagepaint::grayTone;
  freeink::book::FontChain chain;
  FakeRenderFont face;
  chain.add(&face, freeink::book::StyleNone);

  GfxRenderer renderer;
  const freeink::book::Page page = makeTestPage();
  freeink::book::PagePaint::paintText(page, chain, renderer);

  // The base must carry every non-white sample of the two glyphs, exactly.
  int inkSamples = 0;
  for (const uint8_t coverage : kCoverage) {
    if (grayTone(coverage) >= 1) ++inkSamples;
  }
  int basePixels = 0;
  for (const uint8_t byte : renderer.base) {
    for (int bit = 0; bit < 8; ++bit) {
      if (byte & (0x80 >> bit)) ++basePixels;
    }
  }
  EXPECT_EQ(basePixels, 2 * inkSamples);  // two glyphs, same 4x4 bitmap
}

// The chunked-prerender pump (soak finding #5) paints the base in slices
// through paintTextSliced: each call resumes at the cursor. Slicing the
// page into per-run calls must produce EXACTLY the same framebuffer as one
// full paintText — same pixels, same coverage, cursor terminal == runCount.
TEST(PagePaintSliced, ResumedPaintMatchesFullPaint) {
  freeink::book::FontChain chain;
  FakeRenderFont face;
  chain.add(&face, freeink::book::StyleNone);

  const freeink::book::Page page = makeMultiRunPage();

  GfxRenderer full;
  freeink::book::PagePaint::paintText(page, chain, full);

  GfxRenderer sliced;
  uint16_t nextRun = 0;
  uint32_t nextChar = 0;
  uint8_t slices = 0;
  bool complete = false;
  while (!complete && slices < 16) {
    complete = freeink::book::PagePaint::paintTextSliced(page, chain, sliced, nextRun, nextChar, &nextRun, &nextChar,
                                                         /*budgetMs=*/0);
    ++slices;
  }
  ASSERT_TRUE(complete);
  EXPECT_EQ(nextRun, page.runCount);  // cursor terminal: every run painted
  ASSERT_EQ(slices, 1u);              // budget 0 never yields — one call finishes

  EXPECT_EQ(0, std::memcmp(full.base, sliced.base, sizeof(full.base)));
}

// A non-zero start index must SKIP the earlier runs, not repaint them from
// scratch (the pump relies on the cursor to advance, and repainting would
// double-plot ink on the real device).
TEST(PagePaintSliced, StartIndexSkipsEarlierRuns) {
  freeink::book::FontChain chain;
  FakeRenderFont face;
  chain.add(&face, freeink::book::StyleNone);

  const freeink::book::Page page = makeMultiRunPage();

  GfxRenderer renderer;
  // Drive the cursor directly: start at the LAST run and verify the
  // earlier runs' ink is unchanged after painting from that index alone.
  freeink::book::PagePaint::paintText(page, chain, renderer);
  const std::vector<uint8_t> afterFull(renderer.base, renderer.base + sizeof(renderer.base));

  GfxRenderer resumed;
  uint16_t nextRun = 0;
  uint32_t nextChar = 0;
  freeink::book::PagePaint::paintTextSliced(page, chain, resumed, static_cast<uint16_t>(page.runCount - 1), 0, &nextRun,
                                            &nextChar, /*budgetMs=*/0);
  // Resumed paints ONLY the last run (+ rubies): its base must be a SUBSET
  // of the full paint (every set bit in resumed is set in afterFull).
  for (size_t i = 0; i < sizeof(resumed.base); ++i) {
    ASSERT_EQ(resumed.base[i] & afterFull[i], resumed.base[i]) << "byte " << i;
  }
}

// The real yield path: a nonzero budget with a clock that advances per glyph
// must yield MID-RUN (intra-run cursor, review r5), resume without
// double-plotting, and converge to the same framebuffer as a full paint.
TEST(PagePaintSliced, BudgetYieldsMidRunAndResumes) {
  freeink::book::FontChain chain;
  FakeRenderFont face;
  chain.add(&face, freeink::book::StyleNone);

  const freeink::book::Page page = makeMultiRunPage();

  GfxRenderer full;
  g_fakeMillis = 0;
  freeink::book::PagePaint::paintText(page, chain, full);

  GfxRenderer sliced;
  g_fakeMillis = 0;
  uint16_t nextRun = 0;
  uint32_t nextChar = 0;
  bool complete = false;
  int slices = 0;
  int intraRunResumes = 0;
  while (!complete && slices < 128) {
    complete = freeink::book::PagePaint::paintTextSliced(page, chain, sliced, nextRun, nextChar, &nextRun, &nextChar,
                                                         /*budgetMs=*/120);
    ++slices;
    if (!complete && nextChar > 0) ++intraRunResumes;  // yielded INSIDE a run
  }
  ASSERT_TRUE(complete) << "pump did not converge (cursor run=" << nextRun << " char=" << nextChar << ")";
  EXPECT_GT(slices, 3);           // the budget really yielded, repeatedly
  EXPECT_GT(intraRunResumes, 0);  // at least one yield landed MID-RUN
  EXPECT_EQ(nextRun, page.runCount);
  EXPECT_EQ(nextChar, 0u);
  EXPECT_EQ(0, std::memcmp(full.base, sliced.base, sizeof(full.base)));
  g_fakeMillis = 0;
}
