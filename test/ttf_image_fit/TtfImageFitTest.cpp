// Host tests for the TTF image full-fit guarantee (design
// docs/design/2026-11-08-ttf-image-contrast-and-fit.md §1.B): the invariant
// **decoded + cached grid == placement box exactly**, pinned through the
// real engine paint path (DirectPixelWriter over a panel-shaped fake
// GfxRenderer) and the real cache blit, for every format at both ≥2x
// downscale and near-1x placements.
//
// Regression origin (owner soak, X4 Pro, 2026-11-08): the engine's paint
// bounds used the PHYSICAL panel accessors (getDisplayWidth/Height = 800x480)
// while the reader runs in Portrait — 480x800 LOGICAL coordinates. Every
// image row at logical y >= 480 was silently dropped, in both the fresh
// decode paint and blitFromCache, so large illustrations lost everything
// below the panel's physical height (the "cropped, blank space below" defect).
// The legacy bitmap reader is unaffected: ImageBlock::render clips with
// getScreenWidth()/getScreenHeight().

#include <FreeInkBook.h>
#include <HalMemory.h>
#include <ProgressiveJpeg.h>
#include <epub/ImageProbe.h>
#include <gtest/gtest.h>
#include <render/ImageRenderer.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "DpidScaler.h"
#include "Epub.h"
#include "Epub/converters/GifToFramebufferConverter.h"
#include "GfxRenderer.h"
#include "TtfImageCache.h"
#include "TtfImageEngine.h"

// decodeBmpRows is engine-internal (decodeAndCache's BMP branch); the suite
// drives it directly so BMP coverage needs no Epub. Rows stream single-pass
// through a BakeSinkCtx (TtfImageCache.h) — tone -> cache row -> paint.
namespace ttfimg {
DecodeResult decodeBmpRows(const ImageSpec& spec, const uint8_t* data, size_t size, BakeSinkCtx* sink, bool clahe,
                           bool* degradedOut, bool* ladderFailedOut = nullptr);
}  // namespace ttfimg

uint32_t g_fakeMillis = 0;

namespace {

constexpr uint16_t kPanelW = 800;
constexpr uint16_t kPanelH = 480;

// ── fake renderer + coverage assertions ─────────────────────────────────────

struct FakeRenderer : public GfxRenderer {
  FakeRenderer() {
    panelWidth = kPanelW;
    panelHeight = kPanelH;
    orientation = Portrait;
    mode = BW;
    fbStorage.assign(static_cast<size_t>(getDisplayWidthBytes()) * panelHeight, 0xFF);
    fb = reinterpret_cast<uint8_t*>(fbStorage.data());
  }
  std::vector<uint8_t> fbStorage;

  void fillWhite() { std::memset(fbStorage.data(), 0xFF, fbStorage.size()); }
};

// Physical bit for a logical portrait pixel — mirrors DirectPixelWriter's
// Portrait transform: phyX = y, phyY = (phyH-1) - x.
bool physicalBitIsBlack(const FakeRenderer& r, const int logicalX, const int logicalY) {
  const int phyX = logicalY;
  const int phyY = static_cast<int>(r.panelHeight) - 1 - logicalX;
  const int stride = r.getDisplayWidthBytes();
  return (r.fbStorage[static_cast<size_t>(phyY) * stride + (phyX >> 3)] & (0x80 >> (phyX & 7))) == 0;
}

// The placement box must be painted: Floyd–Steinberg legitimately leaves
// white speckles in dark fills, so the predicate is dither-tolerant — every
// row and column carries ink and the box is ≥80% inked (a dropped row paints
// 0%, which is the regression this suite exists for).
void expectBoxCoverage(const FakeRenderer& r, const ttfimg::ImageSpec& spec) {
  long dark = 0;
  for (int y = spec.y; y < spec.y + spec.h; ++y) {
    int rowDark = 0;
    for (int x = spec.x; x < spec.x + spec.w; ++x) {
      if (physicalBitIsBlack(r, x, y)) ++rowDark;
    }
    EXPECT_GT(rowDark, 0) << "placement row " << y << " has no ink";
    dark += rowDark;
  }
  for (int x = spec.x; x < spec.x + spec.w; ++x) {
    int colDark = 0;
    for (int y = spec.y; y < spec.y + spec.h; ++y) {
      if (physicalBitIsBlack(r, x, y)) ++colDark;
    }
    EXPECT_GT(colDark, 0) << "placement column " << x << " has no ink";
  }
  EXPECT_GE(dark * 5, static_cast<long>(spec.w) * spec.h * 4) << "placement under-filled";
  // Nothing outside the box, within the writable LOGICAL screen (the scan
  // must respect the same bounds the paint guards enforce — outside them the
  // physical transform has no target).
  int darkOutside = 0;
  for (int y = 0; y < r.getScreenHeight(); ++y) {
    for (int x = 0; x < r.getScreenWidth(); ++x) {
      const bool inside = x >= spec.x && x < spec.x + spec.w && y >= spec.y && y < spec.y + spec.h;
      if (!inside && physicalBitIsBlack(r, x, y)) ++darkOutside;
    }
  }
  EXPECT_EQ(darkOutside, 0) << "paint escaped the placement box";
}

// ── BMP fixture (24-bit BGR, bottom-up) ─────────────────────────────────────

std::vector<uint8_t> makeBmp(const int w, const int h, const uint8_t gray) {
  const int rowBytes = (w * 3 + 3) / 4 * 4;
  std::vector<uint8_t> data(14 + 40 + static_cast<size_t>(rowBytes) * h, 0);
  data[0] = 'B';
  data[1] = 'M';
  const uint32_t payload = 40 + static_cast<uint32_t>(rowBytes) * h;
  data[2] = static_cast<uint8_t>(payload);
  data[10] = 54;  // dataOffset
  data[14] = 40;  // BITMAPINFOHEADER
  auto put32 = [&data](const int at, const uint32_t v) {
    data[at] = static_cast<uint8_t>(v);
    data[at + 1] = static_cast<uint8_t>(v >> 8);
    data[at + 2] = static_cast<uint8_t>(v >> 16);
    data[at + 3] = static_cast<uint8_t>(v >> 24);
  };
  put32(18, w);
  put32(22, h);
  data[26] = 1;
  data[28] = 24;
  put32(34, payload);
  for (int y = 0; y < h; ++y) {
    uint8_t* row = data.data() + 54 + static_cast<size_t>(h - 1 - y) * rowBytes;
    for (int x = 0; x < w; ++x) {
      row[x * 3] = gray;
      row[x * 3 + 1] = gray;
      row[x * 3 + 2] = gray;
    }
  }
  return data;
}

ttfimg::ImageSpec makeSpec(const int x, const int y, const int w, const int h) {
  ttfimg::ImageSpec spec{};
  snprintf(spec.href, sizeof(spec.href), "OEBPS/images/pin.bmp");
  spec.generation = 7;
  spec.spine = 1;
  spec.page = 2;
  spec.imgIdx = 0;
  spec.x = static_cast<int16_t>(x);
  spec.y = static_cast<int16_t>(y);
  spec.w = static_cast<int16_t>(w);
  spec.h = static_cast<int16_t>(h);
  return spec;
}

}  // namespace

// RAII fixture reader (kody rule): the raw-item bytes decodeAndCache extracts.
std::vector<uint8_t> readFixtureFile(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Runs the production single-pass BMP flow for a paint-only decode: rows
// stream through the bake's toneRow straight to the paint (no writer —
// no cache side effects).
void bmpDecodePaint(FakeRenderer& r, const ttfimg::ImageSpec& spec, const std::vector<uint8_t>& bmp) {
  ttfimg::ToneBake4 bake(spec.w);
  ASSERT_TRUE(bake.ok());
  std::vector<uint8_t> toneRow(static_cast<size_t>(spec.w), 0);
  ttfimg::BakeSinkCtx sink;
  sink.bake = &bake;
  sink.spec = &spec;
  sink.paint = &r;
  sink.toneRow = toneRow.data();
  const auto res = ttfimg::decodeBmpRows(spec, bmp.data(), bmp.size(), &sink, false, nullptr);
  ASSERT_EQ(res, ttfimg::DecodeResult::Ok);
}

// ── the regression: paint bounds in portrait ────────────────────────────────

TEST(ImageFitPaint, BmpCoversBoxBelowThePhysicalPanelHeight) {
  FakeRenderer r;
  // 320x480 source into a 3.2x-downscaled 100x150 placement that sits wholly
  // below the panel's PHYSICAL height (logical y 600..749 is valid portrait
  // space; getDisplayHeight() would clip it all).
  const auto bmp = makeBmp(320, 480, 10);
  const auto spec = makeSpec(100, 600, 100, 150);
  bmpDecodePaint(r, spec, bmp);
  expectBoxCoverage(r, spec);
}

TEST(ImageFitPaint, BmpCoversBoxStraddlingThePhysicalPanelHeight) {
  FakeRenderer r;
  // The box straddles logical y=480: pre-fix the decode painted only the
  // first 40 rows and the cache never noticed.
  const auto bmp = makeBmp(320, 480, 10);
  const auto spec = makeSpec(100, 440, 100, 150);
  bmpDecodePaint(r, spec, bmp);
  expectBoxCoverage(r, spec);
}

TEST(ImageFitPaint, BmpCoversNearOneToOneBox) {
  FakeRenderer r;
  // placeImage never upscales, so a source smaller than the content box is
  // painted 1:1 — still below the physical height.
  const auto bmp = makeBmp(100, 150, 10);
  const auto spec = makeSpec(50, 620, 100, 150);
  bmpDecodePaint(r, spec, bmp);
  expectBoxCoverage(r, spec);
}

TEST(ImageFitBlit, BlitCoversBoxBelowThePhysicalPanelHeight) {
  FakeRenderer r;
  auto& storage = HalStorage::getInstance();
  storage.resetFiles();
  const auto spec = makeSpec(100, 600, 100, 150);
  ttfimg::ToneCacheWriter writer;
  char path[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, "/books/epub_x/ficache", true, false, path, sizeof(path)));
  ASSERT_TRUE(writer.begin(path, spec.w, spec.h, ttfimg::cacheIdentity(spec, true, false)));
  std::vector<uint8_t> tones(static_cast<size_t>(spec.w), 0);  // all-black rows
  for (int i = 0; i < spec.h; ++i) ASSERT_TRUE(writer.writeRow(tones.data()));
  ASSERT_TRUE(writer.finalize());
  ASSERT_TRUE(ttfimg::blitFromCache(r, spec, "/books/epub_x/ficache", true, false));
  expectBoxCoverage(r, spec);
}

// ── SDK decode grid pins (design §Phase B item 4) ───────────────────────────
// The SDK row renderers must emit EXACTLY the placement grid — every row once,
// in order, at placement width — so the cache writer's writtenRows == height
// contract holds. This refutes the baseline-JPEG min() clamp and the
// progressive DC-path row-drop candidates as crop mechanisms: with a correct
// probe the decode always covers the box; the device defect was the paint
// bounds above. HostFileSource + the committed fixture EPUBs keep the pins
// independent of any image tooling.

namespace {

class HostFileSource : public freeink::book::BookSource {
 public:
  bool open(const char* path) {
    file_.reset(std::fopen(path, "rb"));
    if (file_ == nullptr) return false;
    std::fseek(file_.get(), 0, SEEK_END);
    size_ = static_cast<uint64_t>(std::ftell(file_.get()));
    return true;
  }
  int32_t readAt(uint64_t offset, void* dst, uint32_t len) override {
    if (std::fseek(file_.get(), static_cast<long>(offset), SEEK_SET) != 0) return -1;
    return static_cast<int32_t>(std::fread(dst, 1, len, file_.get()));
  }
  uint64_t size() const override { return size_; }

 private:
  // RAII handle (kody rule): the fclose deleter releases it on every path.
  std::unique_ptr<std::FILE, int (*)(std::FILE*)> file_{nullptr, &std::fclose};
  uint64_t size_ = 0;
};

struct RowCoverage {
  int rows = 0;
  int gaps = 0;
  int lastRow = -1;
  uint32_t maxWidth = 0;
  static bool onRow(void* user, uint16_t y, const uint8_t* gray, uint16_t width) {
    auto* self = static_cast<RowCoverage*>(user);
    if (y == self->lastRow + 1) {
      // in order
    } else if (y > self->lastRow) {
      self->gaps += static_cast<int>(y) - self->lastRow - 1;
    } else {
      self->gaps += 1000;  // duplicate or backward row
    }
    self->lastRow = y;
    self->rows++;
    if (width > self->maxWidth) self->maxWidth = width;
    (void)gray;
    return true;
  }
};

// X4 Pro portrait content box (480x800 panel minus reader margins).
constexpr int kContentW = 454;
constexpr int kContentH = 774;

// Renders `href` at (pw, ph) and asserts the full-grid invariant.
void expectSdkFullGrid(freeink::book::BookSource& source, freeink::book::Book& book, freeink::book::Arena& scratch,
                       const char* href, const int pw, const int ph, const char* label) {
  freeink::book::PageImage img{};
  img.href = href;
  img.width = static_cast<uint16_t>(pw);
  img.height = static_cast<uint16_t>(ph);
  RowCoverage cap;
  const size_t marked = scratch.mark();
  const auto st = freeink::book::ImageRenderer::render(source, book.zip(), img, scratch, RowCoverage::onRow, &cap);
  scratch.release(marked);
  EXPECT_EQ(st, freeink::book::BookStatus::Ok) << label;
  EXPECT_EQ(cap.rows, ph) << label;
  EXPECT_EQ(cap.gaps, 0) << label;
  EXPECT_EQ(cap.maxWidth, static_cast<uint32_t>(pw)) << label;
}

// Runs the three placement regimes for one fixture image.
void expectSdkPlacements(freeink::book::BookSource& source, freeink::book::Book& book, freeink::book::Arena& scratch,
                         const char* href, const char* label) {
  const freeink::book::ZipEntry* entry = book.zip().find(href);
  ASSERT_NE(entry, nullptr) << label;
  freeink::book::ImageInfo info;
  ASSERT_EQ(freeink::book::probeImage(source, *entry, scratch, &info), freeink::book::BookStatus::Ok) << label;
  ASSERT_GT(info.width, 0u) << label;
  ASSERT_GT(info.height, 0u) << label;
  const int w = info.width;
  const int h = info.height;
  char tag[160];

  // ≥2x downscale: the exact half (both axes).
  snprintf(tag, sizeof(tag), "%s half", label);
  expectSdkFullGrid(source, book, scratch, href, w / 2, h / 2, tag);

  // 1:1 / near-1x: sources that fit the content box unpainted-scale.
  if (w <= kContentW && h <= kContentH) {
    snprintf(tag, sizeof(tag), "%s one-to-one", label);
    expectSdkFullGrid(source, book, scratch, href, w, h, tag);
    if (w > 1 && h > 1) {
      snprintf(tag, sizeof(tag), "%s near-1x", label);
      expectSdkFullGrid(source, book, scratch, href, w - 1, h - 1, tag);
    }
  } else {
    // placeImage's integer fit — the deep-downscale regime a full-page
    // illustration actually takes.
    int fw = w, fh = h;
    if (fw > kContentW) {
      fh = fh * kContentW / fw;
      fw = kContentW;
    }
    if (fh > kContentH) {
      fw = fw * kContentH / fh;
      fh = kContentH;
    }
    snprintf(tag, sizeof(tag), "%s fitted", label);
    expectSdkFullGrid(source, book, scratch, href, fw, fh, tag);
  }
}

}  // namespace

// Full-fidelity progressive pass-through (reset4): renderSourceRows decodes
// ALL scans through the SDK's ProgressiveJpeg and streams 800x1200 luma rows
// (was the DC-only 1/8-scale preview, 100-wide rows). Tonal pins reuse the
// fixture's known pattern: the gradient start (dark), the mid-left ramp, the
// dark top of the right half and the light bottom of the right half — but at
// FULL resolution (block averages exactly, quantizer 8).
TEST(ImageFitSdkProgressiveFull, RendersFullResLumaRowsThroughRenderSourceRows) {
  static uint8_t bookBuf[1 << 20];
  static uint8_t scratchBuf[4 << 20];
  HostFileSource source;
  ASSERT_TRUE(source.open(FIT_FIXTURES "/fit_fixtures.epub"));
  freeink::book::Arena bookArena(bookBuf, sizeof(bookBuf));
  freeink::book::Arena scratch(scratchBuf, sizeof(scratchBuf));
  freeink::book::Book book;
  ASSERT_EQ(book.open(source, bookArena, scratch), freeink::book::BookStatus::Ok);
  const freeink::book::ZipEntry* entry = book.zip().find("OEBPS/images/prog.jpg");
  ASSERT_NE(entry, nullptr);

  struct FullResCapture {
    int dimW = 0, dimH = 0;
    uint8_t pixels[800 * 1200] = {};
    bool seen[1200] = {};
    int rowW = 0;
    int pollCalls = 0;
    static bool onDims(void* user, uint16_t w, uint16_t h, bool /*scaleReduced*/) {
      auto* self = static_cast<FullResCapture*>(user);
      self->dimW = w;
      self->dimH = h;
      return true;
    }
    static bool onRow(void* user, uint16_t y, const uint8_t* gray, uint16_t width) {
      auto* self = static_cast<FullResCapture*>(user);
      self->rowW = width;
      if (y < 1200) self->seen[y] = true;
      if (static_cast<size_t>(y) * width + width <= sizeof(self->pixels)) {
        std::memcpy(self->pixels + static_cast<size_t>(y) * width, gray, width);
      }
      return true;
    }
    static bool onPoll(void* user) {
      auto* self = static_cast<FullResCapture*>(user);
      ++self->pollCalls;
      return false;
    }
  } cap;
  const freeink::book::BookStatus st = freeink::book::ImageRenderer::renderSourceRows(
      source, book.zip(), "OEBPS/images/prog.jpg", 454, 681, scratch, FullResCapture::onDims, &cap,
      FullResCapture::onRow, &cap, FullResCapture::onPoll, &cap);
  ASSERT_EQ(st, freeink::book::BookStatus::Ok);
  EXPECT_EQ(cap.dimW, 800);
  EXPECT_EQ(cap.dimH, 1200);
  EXPECT_EQ(cap.rowW, 800);
  for (int y = 0; y < 1200; ++y) ASSERT_TRUE(cap.seen[y]) << "full-res row " << y << " never streamed";
  EXPECT_GT(cap.pollCalls, 0) << "the poll hook never reached the decode";
  // Tonal pins at full resolution (the fixture's DC blocks carry exact block
  // averages; AC coefficients are absent so each 8x8 cell is constant).
  const auto at = [&cap](int x, int y) { return cap.pixels[static_cast<size_t>(y) * 800 + x]; };
  // Left-half gradient: x=2 sits in block col 0 (cols 0..7), ramp values 0..4.
  EXPECT_LT(at(2, 340), 40);
  // Mid-left: block col 25 (cols 200..207) averages ~127-135.
  EXPECT_GT(at(200, 340), 100);
  EXPECT_LT(at(200, 340), 180);
  // Right half: dark top block (y=96) and light bottom block (y=600).
  EXPECT_LT(at(400, 100), 60);
  EXPECT_GT(at(400, 600), 180);
}

// The abort poll must fire between scans: an immediately-true poll yields
// Ok with ZERO rows streamed (Stopped mapped by the renderer) but the dims
// hook already reported — the worker is out of the way, no rows.
TEST(ImageFitSdkProgressiveFull, PollHookAbortsBeforeAnyRow) {
  static uint8_t bookBuf[1 << 20];
  static uint8_t scratchBuf[4 << 20];
  HostFileSource source;
  ASSERT_TRUE(source.open(FIT_FIXTURES "/fit_fixtures.epub"));
  freeink::book::Arena bookArena(bookBuf, sizeof(bookBuf));
  freeink::book::Arena scratch(scratchBuf, sizeof(scratchBuf));
  freeink::book::Book book;
  ASSERT_EQ(book.open(source, bookArena, scratch), freeink::book::BookStatus::Ok);

  struct AbortCapture {
    int dimW = 0, dimH = 0;
    int rows = 0;
    static bool onDims(void* user, uint16_t w, uint16_t h, bool /*scaleReduced*/) {
      auto* self = static_cast<AbortCapture*>(user);
      self->dimW = w;
      self->dimH = h;
      return true;
    }
    static bool onRow(void* user, uint16_t y, const uint8_t* gray, uint16_t width) {
      static_cast<AbortCapture*>(user)->rows++;
      return true;
    }
    static bool onPoll(void* user) { return true; }
  } cap;
  const freeink::book::BookStatus st = freeink::book::ImageRenderer::renderSourceRows(
      source, book.zip(), "OEBPS/images/prog.jpg", 454, 681, scratch, AbortCapture::onDims, &cap, AbortCapture::onRow,
      &cap, AbortCapture::onPoll, &cap);
  ASSERT_EQ(st, freeink::book::BookStatus::Ok);  // Stopped maps to a clean stop
  // The dims report is deferred to the first emitted row (the single-report
  // fallback contract, 2026-11-10): an abort before any row reports NO
  // dims at all.
  EXPECT_EQ(cap.dimW, 0);
  EXPECT_EQ(cap.dimH, 0);
  EXPECT_EQ(cap.rows, 0) << "the abort poll fired AFTER rows streamed";
}

// Memory budget (owner directive 2026-11-09, binding): the full-fidelity
// store (~2 B/pixel) plus the app's decode scratch must stay inside the
// 5 MiB engine budget at the worst-case measured source.
TEST(ProgressiveBudget, StorePlusAppScratchFitsTheBudget) {
  const size_t store = freeink::ProgressiveJpeg::storeBytes(1500, 1000);
  EXPECT_GT(store, 0u);
  const ttfimg::DpidScaler::Plan appScratch{1500, 1000, 480, 800, true, true, false};
  const size_t scratch = ttfimg::DpidScaler::scratchBytes(appScratch);
  EXPECT_LT(store + scratch, ttfimg::kPsramBudgetBytes)
      << "full-fidelity store + decode scratch must fit the 5 MiB budget at 1500x1000";
}

// Review pin (2026-11-11): a baseline JPEG whose fidelity-preferred full-res
// decode exceeds the caller's decode arena steps down through the SDK's
// arena-retry, and the SDK now REPORTS that step-down through the dims hook.
// The engine must paint the reduced-scale decode but never publish it under
// the full-quality cache key — a cached reduced decode would permanently
// block the real page. The control leg pins the same image publishing
// normally once the arena holds the preferred pick.
TEST(ImageFitSdk, ArenaRetryDecodePaintsUncachedAndFullArenaPublishes) {
  static uint8_t bookBuf[1 << 20];
  static uint8_t openScratch[64 * 1024];
  static uint8_t smallScratch[18 * 1024];
  HostFileSource source;
  ASSERT_TRUE(source.open(FIT_FIXTURES "/fit_fixtures.epub"));
  freeink::book::Arena bookArena(bookBuf, sizeof(bookBuf));
  freeink::book::Book book;
  {
    freeink::book::Arena openScratchArena(openScratch, sizeof(openScratch));
    ASSERT_EQ(book.open(source, bookArena, openScratchArena), freeink::book::BookStatus::Ok);
  }
  Epub epub;
  auto& storage = HalStorage::getInstance();

  ttfimg::ImageSpec spec = makeSpec(0, 40, 480, 720);
  snprintf(spec.href, sizeof(spec.href), "OEBPS/images/baseline_stored.jpg");
  // STORED entry: a deflated one spends 32 KiB on the inflate window before
  // the JPEG decoder starts, which would mask the band-sized arena margin.
  ASSERT_EQ(book.zip().find(spec.href)->method, 0);

  // Arena sizing (SDK renderJpeg, this fixture): the JPEGDEC decoder itself
  // lives on the heap, so the caller arena holds only the decode band
  // (1200*16 = 19.2 KiB) + scaler state (dstW*7 ≈ 5 KiB) ≈ 24 KiB for the
  // preferred full-res pick; the retried scale-1 pick (600x750) ≈ 15 KiB.
  // 18 KiB forces the retry without failing the decode outright.
  {
    storage.resetFiles();
    spec.generation = 21;
    FakeRenderer r;
    char cachePath[256];
    ASSERT_TRUE(ttfimg::cacheNameFor(spec, "/books/epub_x/ficache", true, false, cachePath, sizeof(cachePath)));
    freeink::book::Arena small(smallScratch, sizeof(smallScratch));
    const auto res = ttfimg::decodeAndCache(spec, cachePath, source, book.zip(), small, &epub, &r, true, true, nullptr,
                                            nullptr, true);
    ASSERT_EQ(res, ttfimg::DecodeResult::Ok) << "the retry-scale decode must still paint";
    // Long ink in the box (a dropped/blanked paint is the failure mode this
    // suite guards; dither-tolerant like expectBoxCoverage).
    long dark = 0;
    for (int y = spec.y; y < spec.y + spec.h; y += 8)
      for (int x = spec.x; x < spec.x + spec.w; ++x)
        if (physicalBitIsBlack(r, x, y)) ++dark;
    EXPECT_GT(dark, 0) << "the placement never painted";
    // ...but nothing was published under the full-quality key.
    EXPECT_FALSE(ttfimg::hasValidCache(spec, "/books/epub_x/ficache", true, false));
    EXPECT_FALSE(storage.exists((std::string(cachePath) + ".t").c_str()));
    EXPECT_FALSE(storage.exists((std::string(cachePath) + ".g").c_str()));
  }

  // Control: a roomy arena holds the preferred full-res decode and publishes.
  {
    storage.resetFiles();
    spec.generation = 22;
    FakeRenderer r;
    static uint8_t bigScratch[4 << 20];
    char cachePath[256];
    ASSERT_TRUE(ttfimg::cacheNameFor(spec, "/books/epub_x/ficache", true, false, cachePath, sizeof(cachePath)));
    freeink::book::Arena big(bigScratch, sizeof(bigScratch));
    const auto res =
        ttfimg::decodeAndCache(spec, cachePath, source, book.zip(), big, &epub, &r, true, true, nullptr, nullptr, true);
    ASSERT_EQ(res, ttfimg::DecodeResult::Ok);
    EXPECT_TRUE(ttfimg::hasValidCache(spec, "/books/epub_x/ficache", true, false));
  }
}

TEST(ImageFitSdk, DegradedWorkerDecodeFailsForRetryWithoutCaching) {
  // Amendment 7: a degraded cache-only (worker) decode used to stream the
  // whole .g through the remap with caching forced off and still report Ok
  // — "Job done" with NO .pxc, stranding the image uncached for its whole
  // sighting window. The engine must return Failed so processJob defers
  // the job and a calmer pass re-decodes at full quality; no cache or temp
  // debris may survive.
  static uint8_t bookBuf[1 << 20];
  static uint8_t openScratch[64 * 1024];
  static uint8_t smallScratch[18 * 1024];
  HostFileSource source;
  ASSERT_TRUE(source.open(FIT_FIXTURES "/fit_fixtures.epub"));
  freeink::book::Arena bookArena(bookBuf, sizeof(bookBuf));
  freeink::book::Book book;
  {
    freeink::book::Arena openScratchArena(openScratch, sizeof(openScratch));
    ASSERT_EQ(book.open(source, bookArena, openScratchArena), freeink::book::BookStatus::Ok);
  }
  Epub epub;
  auto& storage = HalStorage::getInstance();

  ttfimg::ImageSpec spec = makeSpec(0, 40, 480, 720);
  snprintf(spec.href, sizeof(spec.href), "OEBPS/images/baseline_stored.jpg");
  spec.generation = 31;

  // 18 KiB forces the SDK's arena-retry step-down (same fixture/arena as
  // ArenaRetryDecodePaintsUncachedAndFullArenaPublishes) — the dims hook
  // reports scaleReduced and the decode is degraded.
  storage.resetFiles();
  FakeRenderer r;  // worker scratch renderer: non-null, never painted to
  char cachePath[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, "/books/epub_x/ficache", true, false, cachePath, sizeof(cachePath)));
  freeink::book::Arena small(smallScratch, sizeof(smallScratch));
  const auto res = ttfimg::decodeAndCache(spec, cachePath, source, book.zip(), small, &epub, &r, true,
                                          /*caching=*/true, nullptr, nullptr, /*paintToTarget=*/false, false);
  EXPECT_EQ(res, ttfimg::DecodeResult::Failed) << "a degraded worker job must defer, not report silent success";
  EXPECT_FALSE(ttfimg::hasValidCache(spec, "/books/epub_x/ficache", true, false));
  EXPECT_FALSE(storage.exists(cachePath));
  EXPECT_FALSE(storage.exists((std::string(cachePath) + ".t").c_str()));
  EXPECT_FALSE(storage.exists((std::string(cachePath) + ".g").c_str()));

  // Control: the same image under the foreground contract (paintToTarget)
  // still paints uncached and reports Ok — the user sees the image now.
  storage.resetFiles();
  spec.generation = 32;
  FakeRenderer r2;
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, "/books/epub_x/ficache", true, false, cachePath, sizeof(cachePath)));
  freeink::book::Arena small2(smallScratch, sizeof(smallScratch));
  const auto resFg = ttfimg::decodeAndCache(spec, cachePath, source, book.zip(), small2, &epub, &r2, true,
                                            /*caching=*/true, nullptr, nullptr, /*paintToTarget=*/true, false);
  EXPECT_EQ(resFg, ttfimg::DecodeResult::Ok) << "the foreground degraded decode must still paint";
  long dark = 0;
  for (int y = spec.y; y < spec.y + spec.h; y += 8)
    for (int x = spec.x; x < spec.x + spec.w; ++x)
      if (physicalBitIsBlack(r2, x, y)) ++dark;
  EXPECT_GT(dark, 0) << "the placement never painted";
  EXPECT_FALSE(ttfimg::hasValidCache(spec, "/books/epub_x/ficache", true, false))
      << "a degraded rung must never publish under the requested quality's key";
}

TEST(ImageFitSdk, DcFallbackWorkerJobSkipsInsteadOfDeferring) {
  // review 2026-11-11: a progressive stream whose full decode fell back to
  // the DC-only first scan degrades deterministically (dcfallback_prog.jpg
  // carries a corrupt marker between the scans, so the full decode refuses
  // every time and the fallback reports 1/8-scale dims). The decode is a
  // bilinear thumbnail that can never publish under the requested quality's
  // key, so a worker job is DROPPED (Skip — processJob logs and drops) not
  // deferred: five identical full decodes holding the decode bus buy
  // nothing. The foreground keeps the paint-uncached contract.
  static uint8_t bookBuf[1 << 20];
  static uint8_t openScratch[64 * 1024];
  static uint8_t scratchBuf[4 << 20];
  HostFileSource source;
  ASSERT_TRUE(source.open(FIT_FIXTURES "/fit_fixtures.epub"));
  freeink::book::Arena bookArena(bookBuf, sizeof(bookBuf));
  freeink::book::Book book;
  {
    freeink::book::Arena openScratchArena(openScratch, sizeof(openScratch));
    ASSERT_EQ(book.open(source, bookArena, openScratchArena), freeink::book::BookStatus::Ok);
  }
  Epub epub;
  auto& storage = HalStorage::getInstance();

  ttfimg::ImageSpec spec = makeSpec(0, 40, 480, 720);
  snprintf(spec.href, sizeof(spec.href), "OEBPS/images/dcfallback_prog.jpg");
  ASSERT_TRUE(book.zip().find(spec.href) != nullptr);

  // Worker leg: roomy arena (no scaleReduced confound) — dcFallback only.
  storage.resetFiles();
  spec.generation = 41;
  FakeRenderer r;  // worker scratch renderer: non-null, never painted to
  char cachePath[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, "/books/epub_x/ficache", true, false, cachePath, sizeof(cachePath)));
  freeink::book::Arena scratch(scratchBuf, sizeof(scratchBuf));
  const auto res = ttfimg::decodeAndCache(spec, cachePath, source, book.zip(), scratch, &epub, &r, true,
                                          /*caching=*/true, nullptr, nullptr, /*paintToTarget=*/false, false);
  EXPECT_EQ(res, ttfimg::DecodeResult::Skip)
      << "a deterministic DC fallback must drop the job, not burn the deferral budget";
  EXPECT_FALSE(ttfimg::hasValidCache(spec, "/books/epub_x/ficache", true, false));
  EXPECT_FALSE(storage.exists(cachePath));
  EXPECT_FALSE(storage.exists((std::string(cachePath) + ".t").c_str()));
  EXPECT_FALSE(storage.exists((std::string(cachePath) + ".g").c_str()));

  // Control: the foreground still paints the DC thumbnail and reports Ok,
  // and nothing is published under the full-quality key.
  storage.resetFiles();
  spec.generation = 42;
  FakeRenderer r2;
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, "/books/epub_x/ficache", true, false, cachePath, sizeof(cachePath)));
  freeink::book::Arena scratch2(scratchBuf, sizeof(scratchBuf));
  const auto resFg = ttfimg::decodeAndCache(spec, cachePath, source, book.zip(), scratch2, &epub, &r2, true,
                                            /*caching=*/true, nullptr, nullptr, /*paintToTarget=*/true, false);
  EXPECT_EQ(resFg, ttfimg::DecodeResult::Ok) << "the foreground DC fallback must still paint";
  long dark = 0;
  for (int y = spec.y; y < spec.y + spec.h; y += 8)
    for (int x = spec.x; x < spec.x + spec.w; ++x)
      if (physicalBitIsBlack(r2, x, y)) ++dark;
  EXPECT_GT(dark, 0) << "the placement never painted";
  EXPECT_FALSE(ttfimg::hasValidCache(spec, "/books/epub_x/ficache", true, false))
      << "a DC fallback decode must never publish under the requested quality's key";
}

TEST(ImageFitSdk, BaselineJpegCoversEveryPlacement) {
  static uint8_t bookBuf[1 << 20];
  static uint8_t scratchBuf[4 << 20];
  HostFileSource source;
  ASSERT_TRUE(source.open(TESTDATA_DIR "/epubs/test_jpeg_images.epub"));
  freeink::book::Arena bookArena(bookBuf, sizeof(bookBuf));
  freeink::book::Arena scratch(scratchBuf, sizeof(scratchBuf));
  freeink::book::Book book;
  ASSERT_EQ(book.open(source, bookArena, scratch), freeink::book::BookStatus::Ok);
  expectSdkPlacements(source, book, scratch, "OEBPS/images/scaling_test.jpg", "jpeg-1200x1500");
  expectSdkPlacements(source, book, scratch, "OEBPS/images/wide_scaling_test.jpg", "jpeg-1807x736");
  expectSdkPlacements(source, book, scratch, "OEBPS/images/jpeg_format.jpg", "jpeg-350x250");
}

TEST(ImageFitSdk, PngCoversEveryPlacement) {
  static uint8_t bookBuf[1 << 20];
  static uint8_t scratchBuf[4 << 20];
  HostFileSource source;
  ASSERT_TRUE(source.open(TESTDATA_DIR "/epubs/test_mixed_images.epub"));
  freeink::book::Arena bookArena(bookBuf, sizeof(bookBuf));
  freeink::book::Arena scratch(scratchBuf, sizeof(scratchBuf));
  freeink::book::Book book;
  ASSERT_EQ(book.open(source, bookArena, scratch), freeink::book::BookStatus::Ok);
  expectSdkPlacements(source, book, scratch, "OEBPS/images/png_format.png", "png");
}

TEST(ImageFitSdk, ProgressiveJpegDcPathCoversEveryPlacement) {
  static uint8_t bookBuf[1 << 20];
  static uint8_t scratchBuf[4 << 20];
  HostFileSource source;
  ASSERT_TRUE(source.open(FIT_FIXTURES "/fit_fixtures.epub"));
  freeink::book::Arena bookArena(bookBuf, sizeof(bookBuf));
  freeink::book::Arena scratch(scratchBuf, sizeof(scratchBuf));
  freeink::book::Book book;
  ASSERT_EQ(book.open(source, bookArena, scratch), freeink::book::BookStatus::Ok);
  const freeink::book::ZipEntry* entry = book.zip().find("OEBPS/images/prog.jpg");
  ASSERT_NE(entry, nullptr);
  freeink::book::ImageInfo info;
  ASSERT_EQ(freeink::book::probeImage(source, *entry, scratch, &info), freeink::book::BookStatus::Ok);
  EXPECT_TRUE(info.progressive);
  // Tonal pin at the DC path's exact arithmetic (quantizer 8: gray == block
  // average): the gradient start and the dark/light right half survive.
  struct ValueCapture {
    uint8_t at(int x, int y) { return pixels[static_cast<size_t>(y) * 454 + x]; }
    uint8_t pixels[454 * 800] = {};
    int rowW = 0;
    // Row-coverage ledger: a renderer that returns Ok while dropping rows
    // must fail here (review 2026-11-10) — count and width per row.
    bool seen[800] = {};
    static bool onRow(void* user, uint16_t y, const uint8_t* gray, uint16_t width) {
      auto* self = static_cast<ValueCapture*>(user);
      self->rowW = width;
      if (y < 800) self->seen[y] = true;
      if (static_cast<size_t>(y) * width + width <= sizeof(self->pixels)) {
        std::memcpy(self->pixels + static_cast<size_t>(y) * width, gray, width);
      }
      return true;
    }
  } cap;
  freeink::book::PageImage img{};
  img.href = "OEBPS/images/prog.jpg";
  img.width = 454;
  img.height = 681;  // placeImage fit of 800x1200 into 454x774
  const size_t marked = scratch.mark();
  ASSERT_EQ(freeink::book::ImageRenderer::render(source, book.zip(), img, scratch, ValueCapture::onRow, &cap),
            freeink::book::BookStatus::Ok);
  scratch.release(marked);
  EXPECT_EQ(cap.rowW, 454);
  // Every row of the placement arrived, in order, at full width: the DC
  // renderer returning Ok with a dropped row band (601-680 or any other)
  // cannot pass (review 2026-11-10).
  for (int y = 0; y < 681; ++y) {
    ASSERT_TRUE(cap.seen[y]) << "progressive DC row " << y << " never rendered";
  }
  // Gradient: left edge dark, mid-left midtone (integer DC rounding ±8).
  EXPECT_LT(cap.at(2, 340), 60);
  EXPECT_GT(cap.at(200, 340), 40);
  // Right half: dark top, light bottom.
  EXPECT_LT(cap.at(400, 100), 90);
  EXPECT_GT(cap.at(400, 600), 180);
}

// ── GIF converter pin ───────────────────────────────────────────────────────
// The converter paints with the LOGICAL screen dims (getScreenWidth/Height —
// the same contract ImageBlock::render sets); this pin holds it there and
// proves the exact-dimension mode fills the placement box at a 2x downscale
// below the panel's physical height.

TEST(ImageFitGif, ConverterCoversExactDimBoxBelowThePhysicalPanelHeight) {
  FakeRenderer r;
  // Load the configure-time generated all-dark 96x144 GIF (palette index 0).
  std::vector<uint8_t> gif = readFixtureFile(FIT_FIXTURES "/dark.gif");
  ASSERT_GT(gif.size(), 32u);

  ttfimg::ToneBake4 bake(48);
  ASSERT_TRUE(bake.ok());

  RenderConfig config;
  config.x = 20;
  config.y = 600;  // wholly below the physical 480 — valid portrait space
  config.maxWidth = 48;
  config.maxHeight = 72;  // 2x downscale of 96x144
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;
  config.qualityTone = true;
  config.toneBake = &bake;

  GifToFramebufferConverter converter;
  ASSERT_TRUE(converter.decodeToFramebuffer(gif.data(), gif.size(), r, config));
  const auto spec = makeSpec(20, 600, 48, 72);
  expectBoxCoverage(r, spec);
}

// ── single-pass cache-stream pins (legacy parity, amendment 9) ──────────────

TEST(SinglePassBake, CacheStreamIsDeterministicAcrossRuns) {
  // Worker/foreground parity: two full single-pass decodes of the same input
  // produce byte-identical 2bpp caches (the same legacy Atkinson sequence).
  // Separate names so neither run overwrites the other's bytes.
  auto& storage = HalStorage::getInstance();
  storage.resetFiles();
  const auto bmp = makeBmp(64, 48, 137);
  const auto spec = makeSpec(0, 0, 32, 24);
  for (int run = 0; run < 2; ++run) {
    char cache[64];
    snprintf(cache, sizeof(cache), "/t/parity%d.pxc", run);
    ttfimg::ToneBake4 bake(spec.w);
    ASSERT_TRUE(bake.ok());
    ttfimg::ToneCacheWriter writer;
    ASSERT_TRUE(writer.begin(cache, spec.w, spec.h, ttfimg::cacheIdentity(spec, true, false)));
    std::vector<uint8_t> toneRow(static_cast<size_t>(spec.w), 0);
    ttfimg::BakeSinkCtx sink;
    sink.bake = &bake;
    sink.spec = &spec;
    sink.writer = &writer;
    sink.toneRow = toneRow.data();
    ASSERT_EQ(ttfimg::decodeBmpRows(spec, bmp.data(), bmp.size(), &sink, false, nullptr), ttfimg::DecodeResult::Ok);
    ASSERT_TRUE(writer.finalize());
  }
  EXPECT_EQ(storage.files["/t/parity0.pxc"], storage.files["/t/parity1.pxc"]);
  EXPECT_FALSE(storage.files["/t/parity0.pxc"].empty());
}

TEST(SinglePassBake, CacheStreamRowsArriveInRasterOrder) {
  // The bake's error state advances per row, so the stream contract (each
  // destination row exactly once, in raster order) is observable through
  // the cache payload: a vertically graded BMP (dark top → bright bottom)
  // bakes into a cache whose FIRST row carries the dark tones and whose
  // LAST row carries the bright tones. A row fed out of order would leak
  // the previous rows' error across the whole plane and flip this.
  auto& storage = HalStorage::getInstance();
  storage.resetFiles();
  const int w = 64, h = 32;
  std::vector<uint8_t> data = makeBmp(w, h, 0);
  const int rowBytes = (w * 3 + 3) / 4 * 4;
  for (int y = 0; y < h; ++y) {
    uint8_t* row = data.data() + 54 + static_cast<size_t>(h - 1 - y) * rowBytes;
    const uint8_t g = static_cast<uint8_t>(20 + (y * 180) / (h - 1));
    for (int x = 0; x < w; ++x) {
      row[x * 3] = g;
      row[x * 3 + 1] = g;
      row[x * 3 + 2] = g;
    }
  }
  const auto spec = makeSpec(0, 0, 32, 16);
  ttfimg::ToneBake4 bake(spec.w);
  ASSERT_TRUE(bake.ok());
  ttfimg::ToneCacheWriter writer;
  const char* cache = "/t/gradient.pxc";
  ASSERT_TRUE(writer.begin(cache, spec.w, spec.h, ttfimg::cacheIdentity(spec, true, false)));
  std::vector<uint8_t> toneRow(static_cast<size_t>(spec.w), 0);
  ttfimg::BakeSinkCtx sink;
  sink.bake = &bake;
  sink.spec = &spec;
  sink.writer = &writer;
  sink.toneRow = toneRow.data();
  ASSERT_EQ(ttfimg::decodeBmpRows(spec, data.data(), data.size(), &sink, false, nullptr), ttfimg::DecodeResult::Ok);
  ASSERT_TRUE(writer.finalize());
  // The published file parses: header + 16 rows of 8 bytes, letter 'g'
  // (the linear-light blue-noise quality bake).
  const std::string& bytes = storage.files[cache];
  ASSERT_EQ(bytes.size(), ttfimg::kCacheHeaderBytes + (32 + 3) / 4 * 16u);
  EXPECT_EQ(static_cast<char>(bytes[2]), 'g');
  // Row tone histogram straight from the 2-bit payload (MSB-first, 4 px/B).
  const auto rowLevelCounts = [&bytes, &spec](const int y) {
    int counts[4] = {0, 0, 0, 0};
    const uint8_t* row = reinterpret_cast<const uint8_t*>(bytes.data()) + ttfimg::kCacheHeaderBytes + y * 8;
    for (int x = 0; x < spec.w; ++x) {
      const int tone = (row[x >> 2] >> (6 - (x & 3) * 2)) & 0x03;
      ++counts[tone];
    }
    return std::array<int, 4>{counts[0], counts[1], counts[2], counts[3]};
  };
  const auto first = rowLevelCounts(0);
  const auto last = rowLevelCounts(spec.h - 1);
  // The source gradient runs 20..200: the top dest band sits at gray ~20-26
  // (level 0/1 territory), the bottom at ~188-200 (level 2, weaving 2/3).
  EXPECT_EQ(first[0] + first[1], spec.w) << "the first cache row lost the dark top band";
  EXPECT_EQ(first[2] + first[3], 0);
  EXPECT_GT(last[2] + last[3], spec.w / 2) << "the last cache row lost the bright bottom band";
  EXPECT_EQ(last[0], 0);
}

// ── end-to-end decodeAndCache pins (the production flow) ────────────────────
// GIF and BMP through the REAL decodeAndCache: PSRAM extraction (stub Epub
// with injectable items), collect-only converter/decode, shared remap pass,
// atomic publish, then a valid cache that blits with full coverage in
// portrait below the physical height.

// Runs the production decodeAndCache for a GIF/BMP fixture and asserts the
// published cache is valid and blits with full box coverage in portrait.
// The Epub stub serves the raw item bytes (read from the fixture dir); the
// ZIP entry itself only feeds probeImage.
void expectDecodeCacheBlit(freeink::book::BookSource& source, freeink::book::Book& book, freeink::book::Arena& scratch,
                           Epub& epub, const char* href, const char* file, const char* kind, const int specX,
                           const int specY, const int specW, const int specH) {
  std::vector<uint8_t> bytes = readFixtureFile(file);
  ASSERT_GT(bytes.size(), 32u) << kind;
  epub.items[href] = bytes;

  FakeRenderer r;
  auto& storage = HalStorage::getInstance();
  storage.resetFiles();

  ttfimg::ImageSpec spec = makeSpec(specX, specY, specW, specH);
  snprintf(spec.href, sizeof(spec.href), "%s", href);
  spec.generation = 9;

  char cachePath[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, "/books/epub_x/ficache", true, false, cachePath, sizeof(cachePath)));
  const auto res = ttfimg::decodeAndCache(spec, cachePath, source, book.zip(), scratch, &epub, &r,
                                          /*qualityTone=*/true, /*caching=*/true, nullptr, nullptr,
                                          /*paintToTarget=*/true);
  ASSERT_EQ(res, ttfimg::DecodeResult::Ok) << kind;
  // The published cache is valid and complete...
  EXPECT_TRUE(ttfimg::hasValidCache(spec, "/books/epub_x/ficache", true, false)) << kind;
  // ...no temp debris survives the publish...
  EXPECT_FALSE(storage.exists((std::string(cachePath) + ".t").c_str())) << kind;
  EXPECT_FALSE(storage.exists((std::string(cachePath) + ".g").c_str())) << kind;
  // ...and the blit covers the placement box (portrait, below physical height).
  r.fillWhite();
  EXPECT_TRUE(ttfimg::blitFromCache(r, spec, "/books/epub_x/ficache", true, false)) << kind;
  expectBoxCoverage(r, spec);
}

TEST(ImageFitEndToEnd, BmpDecodePublishesCacheAndBlits) {
  static uint8_t bookBuf[1 << 20];
  static uint8_t scratchBuf[4 << 20];
  freeink::book::Arena bookArena(bookBuf, sizeof(bookBuf));
  freeink::book::Arena scratch(scratchBuf, sizeof(scratchBuf));
  HostFileSource source;
  ASSERT_TRUE(source.open(FIT_FIXTURES "/fit_fixtures.epub"));
  freeink::book::Book book;
  ASSERT_EQ(book.open(source, bookArena, scratch), freeink::book::BookStatus::Ok);
  Epub epub;
  // 64x48 BMP into a 2x-downscaled 32x24 box below the physical height.
  expectDecodeCacheBlit(source, book, scratch, epub, "OEBPS/images/small.bmp", FIT_FIXTURES "/small.bmp", "bmp", 20,
                        600, 32, 24);
}

TEST(ImageFitEndToEnd, GifDecodePublishesCacheAndBlits) {
  static uint8_t bookBuf[1 << 20];
  static uint8_t scratchBuf[4 << 20];
  freeink::book::Arena bookArena(bookBuf, sizeof(bookBuf));
  freeink::book::Arena scratch(scratchBuf, sizeof(scratchBuf));
  HostFileSource source;
  ASSERT_TRUE(source.open(FIT_FIXTURES "/fit_fixtures.epub"));
  freeink::book::Book book;
  ASSERT_EQ(book.open(source, bookArena, scratch), freeink::book::BookStatus::Ok);
  Epub epub;
  // 96x144 GIF into a 3x/6x-downscaled 32x24 box below the physical height.
  expectDecodeCacheBlit(source, book, scratch, epub, "OEBPS/images/dark.gif", FIT_FIXTURES "/dark.gif", "gif", 20, 600,
                        32, 24);
}

TEST(ImageFitEndToEnd, JpegDecodePublishesCacheAndBlits) {
  static uint8_t bookBuf[1 << 20];
  static uint8_t scratchBuf[4 << 20];
  freeink::book::Arena bookArena(bookBuf, sizeof(bookBuf));
  freeink::book::Arena scratch(scratchBuf, sizeof(scratchBuf));
  HostFileSource source;
  ASSERT_TRUE(source.open(FIT_FIXTURES "/fit_fixtures.epub"));
  freeink::book::Book book;
  ASSERT_EQ(book.open(source, bookArena, scratch), freeink::book::BookStatus::Ok);
  Epub epub;
  // All-dark progressive fixture: exercises the SDK DC path end to end
  // (white image regions would legitimately paint no ink).
  expectDecodeCacheBlit(source, book, scratch, epub, "OEBPS/images/dark_prog.jpg", FIT_FIXTURES "/dark_prog.jpg",
                        "jpeg", 20, 440, 227, 340);
}

TEST(ImageFitEndToEnd, PngDecodePublishesCacheAndBlits) {
  static uint8_t bookBuf[1 << 20];
  static uint8_t scratchBuf[4 << 20];
  freeink::book::Arena bookArena(bookBuf, sizeof(bookBuf));
  freeink::book::Arena scratch(scratchBuf, sizeof(scratchBuf));
  HostFileSource source;
  ASSERT_TRUE(source.open(FIT_FIXTURES "/fit_fixtures.epub"));
  freeink::book::Book book;
  ASSERT_EQ(book.open(source, bookArena, scratch), freeink::book::BookStatus::Ok);
  Epub epub;
  // All-dark 800x1200 PNG into a 2x-downscaled 400x600 box below the
  // physical height (white regions would legitimately paint no ink).
  expectDecodeCacheBlit(source, book, scratch, epub, "OEBPS/images/dark.png", FIT_FIXTURES "/dark.png", "png", 20, 180,
                        400, 600);
}

// The storage-failure fallbacks (review round 1): cache-stream unavailability
// must degrade to uncached painting, never to a blank placement.

TEST(ImageFitFallback, CacheOpenFailureStillPaintsTheImage) {
  static uint8_t bookBuf[1 << 20];
  static uint8_t scratchBuf[4 << 20];
  freeink::book::Arena bookArena(bookBuf, sizeof(bookBuf));
  freeink::book::Arena scratch(scratchBuf, sizeof(scratchBuf));
  HostFileSource source;
  ASSERT_TRUE(source.open(FIT_FIXTURES "/fit_fixtures.epub"));
  freeink::book::Book book;
  ASSERT_EQ(book.open(source, bookArena, scratch), freeink::book::BookStatus::Ok);
  Epub epub;
  epub.items["OEBPS/images/small.bmp"] = readFixtureFile(FIT_FIXTURES "/small.bmp");

  FakeRenderer r;
  auto& storage = HalStorage::getInstance();
  storage.resetFiles();
  storage.failOpenForWriteAtCall = 1;  // the .t cache-stream open fails
  storage.openForWriteCallCount = 0;

  ttfimg::ImageSpec spec = makeSpec(20, 600, 32, 24);
  snprintf(spec.href, sizeof(spec.href), "OEBPS/images/small.bmp");
  spec.generation = 11;
  char cachePath[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, "/books/epub_x/ficache", true, false, cachePath, sizeof(cachePath)));
  const auto res = ttfimg::decodeAndCache(spec, cachePath, source, book.zip(), scratch, &epub, &r, true, true, nullptr,
                                          nullptr, true);
  storage.failOpenForWriteAtCall = 0;
  EXPECT_EQ(res, ttfimg::DecodeResult::Ok);
  // The image painted single-pass and no cache debris exists.
  expectBoxCoverage(r, spec);
  EXPECT_FALSE(ttfimg::hasValidCache(spec, "/books/epub_x/ficache", true, false));
}

TEST(ImageFitFallback, CacheWriteFailureStopsRetryingWrites) {
  // kody round: the cache-write failure is sticky — after the first failed
  // row write the sink detaches the writer and the remaining rows must NOT
  // retry the write (one log line total, no write storm); the placement
  // still paints single-pass.
  static uint8_t bookBuf[1 << 20];
  static uint8_t scratchBuf[4 << 20];
  freeink::book::Arena bookArena(bookBuf, sizeof(bookBuf));
  freeink::book::Arena scratch(scratchBuf, sizeof(scratchBuf));
  HostFileSource source;
  ASSERT_TRUE(source.open(FIT_FIXTURES "/fit_fixtures.epub"));
  freeink::book::Book book;
  ASSERT_EQ(book.open(source, bookArena, scratch), freeink::book::BookStatus::Ok);
  Epub epub;
  epub.items["OEBPS/images/small.bmp"] = readFixtureFile(FIT_FIXTURES "/small.bmp");

  FakeRenderer r;
  auto& storage = HalStorage::getInstance();
  storage.resetFiles();
  // Armed after the last ASSERT: a failing ASSERT must not leak the injection.
  HalFile::failWriteAtCall = 3;  // the third cache-stream write fails
  HalFile::writeCallCount = 0;

  ttfimg::ImageSpec spec = makeSpec(20, 600, 32, 24);
  snprintf(spec.href, sizeof(spec.href), "OEBPS/images/small.bmp");
  spec.generation = 14;
  char cachePath[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, "/books/epub_x/ficache", true, false, cachePath, sizeof(cachePath)));
  const auto res = ttfimg::decodeAndCache(spec, cachePath, source, book.zip(), scratch, &epub, &r, true, true, nullptr,
                                          nullptr, true);
  HalFile::failWriteAtCall = 0;
  EXPECT_EQ(res, ttfimg::DecodeResult::Ok);
  expectBoxCoverage(r, spec);
  EXPECT_FALSE(ttfimg::hasValidCache(spec, "/books/epub_x/ficache", true, false));
  // Exactly three write attempts (header + two rows): the sticky gate
  // stopped the retry storm after the first failure — no per-row retries.
  EXPECT_EQ(HalFile::writeCallCount, 3);
  // The dead .t stream is discarded even though the failure short-circuited
  // (the header + two rows were the only writes; no retries after).
  EXPECT_FALSE(storage.exists((std::string(cachePath) + ".t").c_str()));
}

TEST(ImageFitFallback, CacheMidStreamWriteFailureStillPaintsTheImage) {
  static uint8_t bookBuf[1 << 20];
  static uint8_t scratchBuf[4 << 20];
  freeink::book::Arena bookArena(bookBuf, sizeof(bookBuf));
  freeink::book::Arena scratch(scratchBuf, sizeof(scratchBuf));
  HostFileSource source;
  ASSERT_TRUE(source.open(FIT_FIXTURES "/fit_fixtures.epub"));
  freeink::book::Book book;
  ASSERT_EQ(book.open(source, bookArena, scratch), freeink::book::BookStatus::Ok);
  Epub epub;
  epub.items["OEBPS/images/small.bmp"] = readFixtureFile(FIT_FIXTURES "/small.bmp");

  FakeRenderer r;
  auto& storage = HalStorage::getInstance();
  storage.resetFiles();
  HalFile::failWriteAtCall = 3;  // the third cache-stream write fails mid-stream
  HalFile::writeCallCount = 0;

  ttfimg::ImageSpec spec = makeSpec(20, 600, 32, 24);
  snprintf(spec.href, sizeof(spec.href), "OEBPS/images/small.bmp");
  spec.generation = 12;
  char cachePath[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, "/books/epub_x/ficache", true, false, cachePath, sizeof(cachePath)));
  const auto res = ttfimg::decodeAndCache(spec, cachePath, source, book.zip(), scratch, &epub, &r, true, true, nullptr,
                                          nullptr, true);
  HalFile::failWriteAtCall = 0;
  EXPECT_EQ(res, ttfimg::DecodeResult::Ok);
  expectBoxCoverage(r, spec);
  EXPECT_FALSE(ttfimg::hasValidCache(spec, "/books/epub_x/ficache", true, false));
}

TEST(ImageBudgetGate, RequiresTheFiveMebibyteBudgetOnBudgetClassBoards) {
  // PSRAM budget 2026-11-09: on an 8 MB board the gate defers below 5 MB free
  // so a full-page decode can no longer start, fail the ~437 KiB rung-1
  // scratch allocation and silently degrade to the lambda-0 box.
  struct Restore {
    HalMemory::HeapStats saved = HalMemory::getPsramHeap();
    ~Restore() { HalMemory::setPsramHeapStats(saved); }
  } restore;

  HalMemory::setPsramHeapStats({4u * 1024 * 1024, 8u * 1024 * 1024, 0, 6u * 1024 * 1024});
  EXPECT_FALSE(ttfimg::decodeGatesOk());
  HalMemory::setPsramHeapStats({ttfimg::kPsramBudgetBytes, 8u * 1024 * 1024, 0, 6u * 1024 * 1024});
  EXPECT_TRUE(ttfimg::decodeGatesOk());
  HalMemory::setPsramHeapStats({ttfimg::kPsramBudgetBytes + 512 * 1024, 8u * 1024 * 1024, 0, 6u * 1024 * 1024});
  EXPECT_TRUE(ttfimg::decodeGatesOk());
}

TEST(ImageBudgetGate, SmallPsramBoardsKeepTheOldFloor) {
  struct Restore {
    HalMemory::HeapStats saved = HalMemory::getPsramHeap();
    ~Restore() { HalMemory::setPsramHeapStats(saved); }
  } restore;

  // Total PSRAM under the budget (a hypothetical sub-5-MB TTF board): the
  // small floor applies — the board still decodes instead of never decoding,
  // and the ladder inside the decode degrades rungs that cannot fit.
  HalMemory::setPsramHeapStats({300u * 1024, 2u * 1024 * 1024, 0, 300u * 1024});
  EXPECT_TRUE(ttfimg::decodeGatesOk());
  HalMemory::setPsramHeapStats({200u * 1024, 2u * 1024 * 1024, 0, 300u * 1024});
  EXPECT_FALSE(ttfimg::decodeGatesOk());
  // No PSRAM at all: defer like any gate-starved job.
  HalMemory::setPsramHeapStats({0, 0, 0, 0});
  EXPECT_FALSE(ttfimg::decodeGatesOk());
}

TEST(ImageBudgetGate, BudgetDecisionIsAConstantExpression) {
  static_assert(ttfimg::psramBudgetOk(8u * 1024 * 1024, ttfimg::kPsramBudgetBytes),
                "exactly the budget free is allowed");
  // Strict total comparison: a board whose TOTAL equals the budget can never
  // keep all of it free — it must fall to the small floor, not defer forever.
  static_assert(ttfimg::psramBudgetOk(ttfimg::kPsramBudgetBytes, ttfimg::kPsramSmallPsramFloorBytes),
                "total == budget decodes on the small floor");
  static_assert(!ttfimg::psramBudgetOk(ttfimg::kPsramBudgetBytes, ttfimg::kPsramSmallPsramFloorBytes - 1),
                "small floor still owns the short side at equality");
  static_assert(!ttfimg::psramBudgetOk(8u * 1024 * 1024, ttfimg::kPsramBudgetBytes - 1), "one byte short defers");
  static_assert(ttfimg::psramBudgetOk(2u * 1024 * 1024, ttfimg::kPsramSmallPsramFloorBytes), "small-board floor");
  static_assert(!ttfimg::psramBudgetOk(2u * 1024 * 1024, ttfimg::kPsramSmallPsramFloorBytes - 1),
                "small-board floor short");
  static_assert(!ttfimg::psramBudgetOk(0, 0), "no PSRAM defers");
  SUCCEED();
}

// ── DPID ladder exhaustion (review 2026-11-13) ──────────────────────────────
// attachScaler refusing every rung is a MEMORY failure: a cache-only worker
// job must defer (Failed), not report the silent no-op Ok. The seam
// (setAttachLadderFailForTest) makes the ladder reachable without a real
// allocation failure.

struct AttachLadderFailGuard {
  AttachLadderFailGuard() { ttfimg::setAttachLadderFailForTest(true); }
  ~AttachLadderFailGuard() { ttfimg::setAttachLadderFailForTest(false); }
};

TEST(ImageBmpLadder, ExhaustedLadderReportsTheFailure) {
  const auto bmp = makeBmp(64, 48, 137);
  const auto spec = makeSpec(0, 0, 32, 24);
  ttfimg::ToneBake4 bake(spec.w);
  ASSERT_TRUE(bake.ok());
  std::vector<uint8_t> toneRow(static_cast<size_t>(spec.w), 0);
  ttfimg::BakeSinkCtx sink;
  sink.bake = &bake;
  sink.spec = &spec;
  sink.toneRow = toneRow.data();

  bool ladderFailed = false;
  {
    AttachLadderFailGuard guard;
    EXPECT_EQ(ttfimg::decodeBmpRows(spec, bmp.data(), bmp.size(), &sink, false, nullptr, &ladderFailed),
              ttfimg::DecodeResult::Failed);
  }
  EXPECT_TRUE(ladderFailed);

  // Control (seam disarmed): the same decode succeeds and the out-param is
  // not written — the report is the ladder's, not the decode's.
  ladderFailed = false;
  EXPECT_EQ(ttfimg::decodeBmpRows(spec, bmp.data(), bmp.size(), &sink, false, nullptr, &ladderFailed),
            ttfimg::DecodeResult::Ok);
  EXPECT_FALSE(ladderFailed);
}

TEST(ImageFitFallback, BmpLadderExhaustionDefersTheCacheOnlyJob) {
  static uint8_t bookBuf[1 << 20];
  static uint8_t scratchBuf[4 << 20];
  freeink::book::Arena bookArena(bookBuf, sizeof(bookBuf));
  freeink::book::Arena scratch(scratchBuf, sizeof(scratchBuf));
  HostFileSource source;
  ASSERT_TRUE(source.open(FIT_FIXTURES "/fit_fixtures.epub"));
  freeink::book::Book book;
  ASSERT_EQ(book.open(source, bookArena, scratch), freeink::book::BookStatus::Ok);
  Epub epub;
  epub.items["OEBPS/images/small.bmp"] = readFixtureFile(FIT_FIXTURES "/small.bmp");

  FakeRenderer r;
  auto& storage = HalStorage::getInstance();
  storage.resetFiles();

  ttfimg::ImageSpec spec = makeSpec(20, 600, 32, 24);
  snprintf(spec.href, sizeof(spec.href), "OEBPS/images/small.bmp");
  spec.generation = 11;
  char cachePath[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, "/books/epub_x/ficache", true, false, cachePath, sizeof(cachePath)));
  AttachLadderFailGuard guard;
  const auto res = ttfimg::decodeAndCache(spec, cachePath, source, book.zip(), scratch, &epub, &r, true, true, nullptr,
                                          nullptr, /*paintToTarget=*/false);
  EXPECT_EQ(res, ttfimg::DecodeResult::Failed) << "worker job must defer, not report the silent no-op Ok";
  EXPECT_FALSE(ttfimg::hasValidCache(spec, "/books/epub_x/ficache", true, false));
}

// ── in-file cache identity header (fidelity campaign 2026-11-10) ────────────

TEST(CacheIdentityHeader, WrongInFileLetterIsRejectedAndDropped) {
  // A .pxc whose IN-FILE revision letter does not match the requested bake
  // must not paint and must be dropped for regeneration — the filename
  // alone is not trusted (defense-in-depth, 0b).
  auto& storage = HalStorage::getInstance();
  storage.files.clear();
  const auto spec = makeSpec(3, 0, 16, 8);
  ttfimg::ToneCacheWriter writer;
  char path[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, "/books/epub_x/ficache", true, false, path, sizeof(path)));
  ASSERT_TRUE(writer.begin(path, 16, 8, ttfimg::cacheIdentity(spec, true, false)));
  uint8_t row[16] = {};
  for (int i = 0; i < 8; ++i) ASSERT_TRUE(writer.writeRow(row));
  ASSERT_TRUE(writer.finalize());
  ASSERT_TRUE(ttfimg::hasValidCache(spec, "/books/epub_x/ficache", true, false));  // sanity: matching

  // Corrupt the letter byte (offset 2): a stale-bake payload under a live name.
  std::string& data = storage.files[path];
  data[2] = 'm';  // an older bake's letter
  EXPECT_FALSE(ttfimg::hasValidCache(spec, "/books/epub_x/ficache", true, false));
  EXPECT_EQ(storage.files.count(path), 0u) << "the stale payload was kept";
}

TEST(CacheIdentityHeader, WrongInFileHashIsRejected) {
  auto& storage = HalStorage::getInstance();
  storage.files.clear();
  const auto spec = makeSpec(4, 0, 16, 8);
  ttfimg::ToneCacheWriter writer;
  char path[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, "/books/epub_x/ficache", true, false, path, sizeof(path)));
  ASSERT_TRUE(writer.begin(path, 16, 8, ttfimg::cacheIdentity(spec, true, false)));
  uint8_t row[16] = {};
  for (int i = 0; i < 8; ++i) ASSERT_TRUE(writer.writeRow(row));
  ASSERT_TRUE(writer.finalize());

  std::string& data = storage.files[path];
  data[3] ^= 0xFF;  // flip a hash32 byte: different identity, same name
  EXPECT_FALSE(ttfimg::hasValidCache(spec, "/books/epub_x/ficache", true, false));
}

TEST(CacheIdentityHeader, TruncatedHeaderIsRejected) {
  auto& storage = HalStorage::getInstance();
  storage.files.clear();
  ttfimg::ToneCacheWriter writer;
  ASSERT_TRUE(writer.begin("/t/short.pxc", 16, 8, ttfimg::CacheIdentity{'o', 0}));
  uint8_t row[16] = {};
  for (int i = 0; i < 8; ++i) ASSERT_TRUE(writer.writeRow(row));
  ASSERT_TRUE(writer.finalize());
  std::string& data = storage.files["/t/short.pxc"];
  data.resize(6);  // shorter than the 11-byte identity header
  HalFile f;
  ASSERT_TRUE(storage.openFileForRead("T", "/t/short.pxc", f));
  uint16_t w = 0, h = 0;
  EXPECT_FALSE(ttfimg::readValidCacheHeader(f, 16, 8, ttfimg::CacheIdentity{'o', 0}, w, h));
}

TEST(CacheIdentityHeader, PolicyLetterMismatchBetweenFileAndKey) {
  // A cache written for Sharp ('o') must not load under the Enhanced ('p')
  // key even if the file landed under the right name.
  auto& storage = HalStorage::getInstance();
  storage.files.clear();
  const auto spec = makeSpec(5, 0, 16, 8);
  ttfimg::ToneCacheWriter writer;
  char path[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, "/books/epub_x/ficache", true, false, path, sizeof(path)));
  ASSERT_TRUE(writer.begin(path, 16, 8, ttfimg::cacheIdentity(spec, true, false)));
  uint8_t row[16] = {};
  for (int i = 0; i < 8; ++i) ASSERT_TRUE(writer.writeRow(row));
  ASSERT_TRUE(writer.finalize());
  EXPECT_FALSE(ttfimg::hasValidCache(spec, "/books/epub_x/ficache", true, true));  // Enhanced key
}
