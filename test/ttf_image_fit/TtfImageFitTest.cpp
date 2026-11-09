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
#include <epub/ImageProbe.h>
#include <gtest/gtest.h>
#include <render/ImageRenderer.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/converters/GifToFramebufferConverter.h"
#include "GfxRenderer.h"
#include "TtfImageCache.h"
#include "TtfImageEngine.h"

// decodeBmpRows and remapGrayToCache are engine-internal (decodeAndCache's
// BMP branch); the suite drives them directly so BMP coverage needs no Epub.
namespace ttfimg {
DecodeResult decodeBmpRows(const ImageSpec& spec, const uint8_t* data, size_t size, ToneBake4* bake,
                           GfxRenderer* legacyPaint, bool (*abortPoll)(void*), void* abortCtx, bool clahe = false);
DecodeResult remapGrayToCache(const ImageSpec& spec, ToneBake4& bake, const char* writePath, bool caching,
                              GfxRenderer* paint, bool paintToTarget, bool (*abortPoll)(void*), void* abortCtx,
                              bool* cacheStreamOk);
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

// Runs the production two-phase BMP flow for a paint-only decode: phase 1
// collects into a gray temp (in-memory storage), phase 2 remaps and paints
// (caching=false — no cache side effects).
void bmpDecodeRemapPaint(FakeRenderer& r, const ttfimg::ImageSpec& spec, const std::vector<uint8_t>& bmp) {
  ttfimg::ToneBake4 bake(spec.w);
  ASSERT_TRUE(bake.ok());
  ASSERT_TRUE(bake.beginCollect("/t/paint_case.g"));
  const auto res =
      ttfimg::decodeBmpRows(spec, bmp.data(), bmp.size(), &bake, /*legacyPaint=*/nullptr, nullptr, nullptr);
  ASSERT_EQ(res, ttfimg::DecodeResult::Ok);
  ASSERT_TRUE(bake.grayDone());
  ASSERT_EQ(ttfimg::remapGrayToCache(spec, bake, "/t/paint_case.pxc", /*caching=*/false, &r,
                                     /*paintToTarget=*/true, nullptr, nullptr, nullptr),
            ttfimg::DecodeResult::Ok);
}

// ── the regression: paint bounds in portrait ────────────────────────────────

TEST(ImageFitPaint, BmpCoversBoxBelowThePhysicalPanelHeight) {
  FakeRenderer r;
  // 320x480 source into a 3.2x-downscaled 100x150 placement that sits wholly
  // below the panel's PHYSICAL height (logical y 600..749 is valid portrait
  // space; getDisplayHeight() would clip it all).
  const auto bmp = makeBmp(320, 480, 10);
  const auto spec = makeSpec(100, 600, 100, 150);
  bmpDecodeRemapPaint(r, spec, bmp);
  expectBoxCoverage(r, spec);
}

TEST(ImageFitPaint, BmpCoversBoxStraddlingThePhysicalPanelHeight) {
  FakeRenderer r;
  // The box straddles logical y=480: pre-fix the decode painted only the
  // first 40 rows and the cache never noticed.
  const auto bmp = makeBmp(320, 480, 10);
  const auto spec = makeSpec(100, 440, 100, 150);
  bmpDecodeRemapPaint(r, spec, bmp);
  expectBoxCoverage(r, spec);
}

TEST(ImageFitPaint, BmpCoversNearOneToOneBox) {
  FakeRenderer r;
  // placeImage never upscales, so a source smaller than the content box is
  // painted 1:1 — still below the physical height.
  const auto bmp = makeBmp(100, 150, 10);
  const auto spec = makeSpec(50, 620, 100, 150);
  bmpDecodeRemapPaint(r, spec, bmp);
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
  ASSERT_TRUE(writer.begin(path, spec.w, spec.h));
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
    static bool onRow(void* user, uint16_t y, const uint8_t* gray, uint16_t width) {
      auto* self = static_cast<ValueCapture*>(user);
      self->rowW = width;
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

// ── adaptive bake pins (design 2026-11-08 §2, amendment 2) ──────────────────
// Byte-exact, deterministic, pure-integer: the histogram → percentile →
// stretch map, the degenerate fallback, and the phase-1 gray temp stream.

TEST(AdaptiveBake, MidtoneStaysMidtoneWhenRangeIsFull) {
  // The defect-A signature pin: the old fixed map sent source 128 → 176
  // (gamma 2.2 brightening) → 190 (contrast) before quantization, collapsing
  // midtones into the panel's two lightest levels. A near-uniform histogram
  // (4 px per bin, N=1024) clips at lo=1 / hi=253 and the stretch keeps the
  // midtone at 128 — no brightening anywhere in the map.
  ttfimg::ToneBake4 bake(4);
  ASSERT_TRUE(bake.ok());
  ASSERT_TRUE(bake.beginCollect("/t/bake_full.g"));
  for (int v = 0; v < 256; ++v) {
    const uint8_t row[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v), static_cast<uint8_t>(v),
                            static_cast<uint8_t>(v)};
    bake.collectRow(row);
  }
  ASSERT_TRUE(bake.grayDone());
  bake.beginRemap();
  // 0.5%/99.5% clip (blockiness design 2026-11-09): lo: cum ≥ 1024·5/1000 = 5
  // first at v=1 (8). hi: cum ≥ 1024·995/1000 = 1018 first at v=254 (1020).
  // Stretch [1,254] → [0,255].
  EXPECT_EQ(bake.mapLut[1], 0);
  EXPECT_EQ(bake.mapLut[254], 255);
  EXPECT_EQ(bake.mapLut[128], static_cast<uint8_t>((128 - 1) * 255 / 253));  // == 128
  EXPECT_EQ(bake.mapLut[128], 128);
  EXPECT_EQ(bake.mapLut[2], 1);      // first in-range step above black
  EXPECT_EQ(bake.mapLut[0], 0);      // below lo clamps to black
  EXPECT_EQ(bake.mapLut[253], 253);  // one below hi: just under the top step
}

TEST(AdaptiveBake, PaleBandStretchesToTheFullRange) {
  // The "Insert" case: a low-contrast scan concentrated in a bright band
  // [50,128], with dust outliers at 0 and 255 that the percentile clip must
  // ignore. Rows of 100 pixels, one row per band value (50..127), then an
  // outlier row and one more all-128 row.
  ttfimg::ToneBake4 bake(100);
  ASSERT_TRUE(bake.ok());
  ASSERT_TRUE(bake.beginCollect("/t/bake_pale.g"));
  std::vector<uint8_t> row(100);
  for (int v = 50; v <= 127; ++v) {
    std::fill(row.begin(), row.end(), static_cast<uint8_t>(v));
    bake.collectRow(row.data());
  }
  std::fill(row.begin(), row.end(), static_cast<uint8_t>(128));
  bake.collectRow(row.data());
  row[0] = 0;
  row[1] = 255;
  bake.collectRow(row.data());
  ASSERT_TRUE(bake.grayDone());
  bake.beginRemap();
  // N=8000: lo target = 80 → cum(0)=1, cum(50)=101 ≥ 80 → lo=50 (the lone 0
  // is clipped). hi target = 7920 → cum(127)=7801, cum(128)=8001 ≥ 7920 →
  // hi=128 (the lone 255 is clipped). Stretch [50,128] → [0,255].
  EXPECT_EQ(bake.mapLut[50], 0);
  EXPECT_EQ(bake.mapLut[128], 255);
  EXPECT_EQ(bake.mapLut[89], static_cast<uint8_t>((89 - 50) * 255 / 78));
  EXPECT_EQ(bake.mapLut[0], 0);      // clipped outlier → black
  EXPECT_EQ(bake.mapLut[255], 255);  // clipped outlier → white
}

TEST(AdaptiveBake, DegenerateHistogramFallsBackToFixedContrast) {
  // A flat scan (single tone) has no range to stretch: the §2.1 fallback is
  // the fixed contrast 1.3 map — without gamma.
  ttfimg::ToneBake4 bake(8);
  ASSERT_TRUE(bake.ok());
  ASSERT_TRUE(bake.beginCollect("/t/bake_flat.g"));
  const uint8_t row[8] = {128, 128, 128, 128, 128, 128, 128, 128};
  for (int i = 0; i < 4; ++i) bake.collectRow(row);
  ASSERT_TRUE(bake.grayDone());
  bake.beginRemap();
  for (int i = 0; i < 256; ++i) {
    int c = (26 * i - 765) / 20;
    if (c < 0) c = 0;
    if (c > 255) c = 255;
    ASSERT_EQ(bake.mapLut[i], c) << "i=" << i;
  }
}

TEST(AdaptiveBake, GrayTempStreamsEveryRowByteExact) {
  auto& storage = HalStorage::getInstance();
  storage.resetFiles();
  ttfimg::ToneBake4 bake(6);
  ASSERT_TRUE(bake.ok());
  ASSERT_TRUE(bake.beginCollect("/t/bake_stream.g"));
  std::vector<std::vector<uint8_t>> rows;
  for (int r = 0; r < 5; ++r) {
    std::vector<uint8_t> row(6);
    for (int x = 0; x < 6; ++x) row[x] = static_cast<uint8_t>(r * 40 + x);
    rows.push_back(row);
    bake.collectRow(row.data());
  }
  ASSERT_TRUE(bake.grayDone());
  const auto& data = storage.files["/t/bake_stream.g"];
  ASSERT_EQ(data.size(), 30u);
  for (int r = 0; r < 5; ++r) {
    EXPECT_EQ(0, memcmp(data.data() + r * 6, rows[r].data(), 6)) << "row " << r;
  }
  // The temp is consumed by the remap pass: no leftover .g after remap.
  const ttfimg::ImageSpec spec = makeSpec(0, 0, 6, 5);
  ttfimg::remapGrayToCache(spec, bake, "/t/bake_stream.pxc", /*caching=*/false, nullptr, /*paintToTarget=*/false,
                           nullptr, nullptr, nullptr);
  EXPECT_FALSE(storage.exists("/t/bake_stream.g"));
}

TEST(AdaptiveBake, TwoPhaseCacheIsDeterministicAcrossRuns) {
  // Worker/foreground parity: two full two-phase decodes of the same input
  // produce byte-identical 2bpp caches (same histogram → same LUT → same
  // serpentine FS sequence). Separate names so neither run overwrites the
  // other's bytes.
  auto& storage = HalStorage::getInstance();
  storage.resetFiles();
  const auto bmp = makeBmp(64, 48, 137);
  const auto spec = makeSpec(0, 0, 32, 24);
  for (int run = 0; run < 2; ++run) {
    char gray[64];
    char cache[64];
    snprintf(gray, sizeof(gray), "/t/parity%d.g", run);
    snprintf(cache, sizeof(cache), "/t/parity%d.pxc", run);
    ttfimg::ToneBake4 bake(spec.w);
    ASSERT_TRUE(bake.ok());
    ASSERT_TRUE(bake.beginCollect(gray));
    ASSERT_EQ(ttfimg::decodeBmpRows(spec, bmp.data(), bmp.size(), &bake, /*legacyPaint=*/nullptr, nullptr, nullptr),
              ttfimg::DecodeResult::Ok);
    ASSERT_TRUE(bake.grayDone());
    ASSERT_EQ(ttfimg::remapGrayToCache(spec, bake, cache, /*caching=*/true, nullptr, /*paintToTarget=*/false, nullptr,
                                       nullptr, nullptr),
              ttfimg::DecodeResult::Ok);
  }
  EXPECT_EQ(storage.files["/t/parity0.pxc"], storage.files["/t/parity1.pxc"]);
  EXPECT_FALSE(storage.files["/t/parity0.pxc"].empty());
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

// ── review round 1 pins ─────────────────────────────────────────────────────

TEST(AdaptiveBake, FullPageFlatFillPercentileNotSaturatedByU16Bins) {
  // The qodo/kody finding: u16 bins saturate at 65535, so a flat full-page
  // fill (90% at one tone) left hi at 255 and the stretch collapsed back to
  // near-identity. u32 bins keep the percentile targets comparable to the
  // real counts: the used range [100,200] must stretch to [0,255].
  ttfimg::ToneBake4 bake(454);
  ASSERT_TRUE(bake.ok());
  ASSERT_TRUE(bake.beginCollect("/t/bake_flatfill.g"));
  std::vector<uint8_t> row(454, 100);
  for (int r = 0; r < 774; ++r) {
    if (r % 10 == 9) std::fill(row.begin(), row.end(), static_cast<uint8_t>(200));  // 10% at 200
    bake.collectRow(row.data());
    if (r % 10 == 9) std::fill(row.begin(), row.end(), static_cast<uint8_t>(100));
  }
  ASSERT_TRUE(bake.grayDone());
  bake.beginRemap();
  // N = 351,396: lo target 3513 → first bin reaching it is 100; hi target
  // 347,882 → first bin reaching it is 200 (the u16 bug left hi at 255).
  EXPECT_EQ(bake.mapLut[100], 0);
  EXPECT_EQ(bake.mapLut[200], 255);
  EXPECT_EQ(bake.mapLut[150], static_cast<uint8_t>((150 - 100) * 255 / 100));
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

// The storage-failure fallbacks (review round 1): gray-temp unavailability
// must degrade to uncached painting, never to a blank placement.

TEST(ImageFitFallback, GrayOpenFailureStillPaintsTheImage) {
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
  storage.failOpenForWriteAtCall = 1;  // the .g temp open fails
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
  // The image painted (legacy single pass) and no cache/temp debris exists.
  expectBoxCoverage(r, spec);
  EXPECT_FALSE(ttfimg::hasValidCache(spec, "/books/epub_x/ficache", true, false));
}

TEST(ImageFitFallback, GrayWriteFailureStopsRetryingWrites) {
  // kody round: the gray-write failure is sticky — after the first failed
  // row write the collectors stop feeding rows and the remaining rows must
  // NOT retry the write (one log line total, no write storm), then the
  // legacy pass paints the placement.
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
  HalFile::failWriteAtCall = 3;  // the third gray-row write fails
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
  // Exactly three write attempts: the sticky gate stopped the retry storm
  // after the first failure (the 24-row collect used to attempt every row).
  EXPECT_EQ(HalFile::writeCallCount, 3);
  // The dead temp is discarded even though the failure short-circuited.
  EXPECT_FALSE(storage.exists((std::string(cachePath) + ".g").c_str()));
}

TEST(ImageFitFallback, GrayMidStreamWriteFailureStillPaintsTheImage) {
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
  HalFile::failWriteAtCall = 3;  // the third gray-row write fails mid-stream
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

// Phase-2 read-failure fallbacks (review round 3): a failed gray re-open or a
// failed mid-stream gray read leaves nothing painted — the decode must fall
// back to the legacy paint (Ok, full coverage, no cache), not a blank box.

namespace {
// Shared body of the two phase-2 read-failure cases.
void expectReadFailureStillPaints(const int failReadAtCall, const int failOpenForReadAtCall) {
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
  spec.generation = 13;
  char cachePath[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, "/books/epub_x/ficache", true, false, cachePath, sizeof(cachePath)));
  // Arm the injections only after the last ASSERT: a failing ASSERT returns
  // early, and leaked injections would fail every later ImageFit* test.
  HalFile::failReadAtCall = failReadAtCall;
  HalFile::readCallCount = 0;
  storage.failOpenForReadAtCall = failOpenForReadAtCall;
  storage.openForReadCallCount = 0;
  const auto res = ttfimg::decodeAndCache(spec, cachePath, source, book.zip(), scratch, &epub, &r, true, true, nullptr,
                                          nullptr, true);
  HalFile::failReadAtCall = 0;
  storage.failOpenForReadAtCall = 0;
  EXPECT_EQ(res, ttfimg::DecodeResult::Ok);
  expectBoxCoverage(r, spec);
  EXPECT_FALSE(ttfimg::hasValidCache(spec, "/books/epub_x/ficache", true, false));
}
}  // namespace

TEST(ImageFitFallback, GrayReadOpenFailureStillPaintsTheImage) {
  // The remap's gray re-open fails (the .t writer's open is write-side, so
  // the first READ open in the flow is the gray temp).
  expectReadFailureStillPaints(0, 1);
}

TEST(ImageFitFallback, GrayMidReadFailureStillPaintsTheImage) {
  // The third gray-row read fails mid-stream.
  expectReadFailureStillPaints(3, 0);
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
  static_assert(!ttfimg::psramBudgetOk(8u * 1024 * 1024, ttfimg::kPsramBudgetBytes - 1), "one byte short defers");
  static_assert(ttfimg::psramBudgetOk(2u * 1024 * 1024, ttfimg::kPsramSmallPsramFloorBytes), "small-board floor");
  static_assert(!ttfimg::psramBudgetOk(2u * 1024 * 1024, ttfimg::kPsramSmallPsramFloorBytes - 1),
                "small-board floor short");
  static_assert(!ttfimg::psramBudgetOk(0, 0), "no PSRAM defers");
  SUCCEED();
}
