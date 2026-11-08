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
#include <epub/ImageProbe.h>
#include <gtest/gtest.h>
#include <render/ImageRenderer.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Epub/converters/GifToFramebufferConverter.h"
#include "GfxRenderer.h"
#include "TtfImageCache.h"
#include "TtfImageEngine.h"

// decodeBmpRows is engine-internal (used by decodeAndCache's BMP branch);
// the suite drives it directly so the BMP coverage needs no Epub/ZIP.
namespace ttfimg {
DecodeResult decodeBmpRows(const ImageSpec& spec, const uint8_t* data, size_t size, GfxRenderer* paint,
                           const char* cachePath, bool caching, bool (*abortPoll)(void*), void* abortCtx,
                           bool* cacheStreamOk);
}

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

// ── the regression: paint bounds in portrait ────────────────────────────────

TEST(ImageFitPaint, BmpCoversBoxBelowThePhysicalPanelHeight) {
  FakeRenderer r;
  // 320x480 source into a 3.2x-downscaled 100x150 placement that sits wholly
  // below the panel's PHYSICAL height (logical y 600..749 is valid portrait
  // space; getDisplayHeight() would clip it all).
  const auto bmp = makeBmp(320, 480, 10);
  const auto spec = makeSpec(100, 600, 100, 150);
  const auto res =
      ttfimg::decodeBmpRows(spec, bmp.data(), bmp.size(), &r, nullptr, /*caching=*/false, nullptr, nullptr, nullptr);
  ASSERT_EQ(res, ttfimg::DecodeResult::Ok);
  expectBoxCoverage(r, spec);
}

TEST(ImageFitPaint, BmpCoversBoxStraddlingThePhysicalPanelHeight) {
  FakeRenderer r;
  // The box straddles logical y=480: pre-fix the decode painted only the
  // first 40 rows and the cache never noticed.
  const auto bmp = makeBmp(320, 480, 10);
  const auto spec = makeSpec(100, 440, 100, 150);
  const auto res =
      ttfimg::decodeBmpRows(spec, bmp.data(), bmp.size(), &r, nullptr, /*caching=*/false, nullptr, nullptr, nullptr);
  ASSERT_EQ(res, ttfimg::DecodeResult::Ok);
  expectBoxCoverage(r, spec);
}

TEST(ImageFitPaint, BmpCoversNearOneToOneBox) {
  FakeRenderer r;
  // placeImage never upscales, so a source smaller than the content box is
  // painted 1:1 — still below the physical height.
  const auto bmp = makeBmp(100, 150, 10);
  const auto spec = makeSpec(50, 620, 100, 150);
  const auto res =
      ttfimg::decodeBmpRows(spec, bmp.data(), bmp.size(), &r, nullptr, /*caching=*/false, nullptr, nullptr, nullptr);
  ASSERT_EQ(res, ttfimg::DecodeResult::Ok);
  expectBoxCoverage(r, spec);
}

TEST(ImageFitBlit, BlitCoversBoxBelowThePhysicalPanelHeight) {
  FakeRenderer r;
  auto& storage = HalStorage::getInstance();
  storage.files.clear();
  const auto spec = makeSpec(100, 600, 100, 150);
  ttfimg::ToneCacheWriter writer;
  char path[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, "/books/epub_x/ficache", true, path, sizeof(path)));
  ASSERT_TRUE(writer.begin(path, spec.w, spec.h));
  std::vector<uint8_t> tones(static_cast<size_t>(spec.w), 0);  // all-black rows
  for (int i = 0; i < spec.h; ++i) ASSERT_TRUE(writer.writeRow(tones.data()));
  ASSERT_TRUE(writer.finalize());
  ASSERT_TRUE(ttfimg::blitFromCache(r, spec, "/books/epub_x/ficache", true));
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
  ~HostFileSource() override {
    if (file_ != nullptr) std::fclose(file_);
  }
  bool open(const char* path) {
    file_ = std::fopen(path, "rb");
    if (file_ == nullptr) return false;
    std::fseek(file_, 0, SEEK_END);
    size_ = static_cast<uint64_t>(std::ftell(file_));
    return true;
  }
  int32_t readAt(uint64_t offset, void* dst, uint32_t len) override {
    if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0) return -1;
    return static_cast<int32_t>(std::fread(dst, 1, len, file_));
  }
  uint64_t size() const override { return size_; }

 private:
  std::FILE* file_ = nullptr;
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
  std::FILE* f = std::fopen(FIT_FIXTURES "/dark.gif", "rb");
  ASSERT_NE(f, nullptr);
  std::vector<uint8_t> gif;
  uint8_t buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) gif.insert(gif.end(), buf, buf + n);
  std::fclose(f);
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
