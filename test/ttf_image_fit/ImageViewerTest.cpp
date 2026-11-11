// Host tests for the image-viewer engine path (design
// docs/design/2026-11-08-ttf-image-contrast-and-fit.md §7): the shared
// placement derivation's exactness against the legacy fitImage formula, the
// cache-freshness token, the standalone-file decode through the production
// dispatch (SdCardBookSource + the raw-entry catalog), the cache-hit blit,
// and the decode-failure contract (no plane pass — the placement stays
// empty).

#include <HalStorage.h>
#include <epub/ImageProbe.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "TtfImageCache.h"
#include "TtfImageEngine.h"
#include "adapters/SdCardBookSource.h"
#include "adapters/StandaloneImageCatalog.h"

namespace {

// The legacy viewer's fitImage formula verbatim (pre-amendment
// BmpViewerActivity.cpp) — the reference the helper must reproduce exactly.
struct LegacyRect {
  int x, y, w, h;
};

LegacyRect legacyFitImage(const int srcW, const int srcH, const int screenW, const int screenH) {
  const float scale = std::min({static_cast<float>(screenW) / srcW, static_cast<float>(screenH) / srcH, 1.0f});
  const int width = std::max(1, std::min(screenW, static_cast<int>(srcW * scale)));
  const int height = std::max(1, std::min(screenH, static_cast<int>(srcH * scale)));
  return {(screenW - width) / 2, (screenH - height) / 2, width, height};
}

TEST(Placement, MatchesLegacyFitImageFormula) {
  const int screens[][2] = {{480, 800}, {240, 320}, {176, 176}};
  const int sources[][2] = {{1, 1},      {100, 150}, {320, 480}, {934, 1400}, {965, 1400},
                            {1400, 900}, {657, 431}, {480, 800}, {2000, 50},  {50, 2000}};
  for (const auto& scr : screens) {
    for (const auto& src : sources) {
      const LegacyRect want = legacyFitImage(src[0], src[1], scr[0], scr[1]);
      const ttfimg::Placement got = ttfimg::fitPlacement(src[0], src[1], scr[0], scr[1]);
      EXPECT_EQ(want.x, got.x) << "src " << src[0] << "x" << src[1] << " screen " << scr[0] << "x" << scr[1];
      EXPECT_EQ(want.y, got.y) << "src " << src[0] << "x" << src[1] << " screen " << scr[0] << "x" << scr[1];
      EXPECT_EQ(want.w, got.w) << "src " << src[0] << "x" << src[1] << " screen " << scr[0] << "x" << scr[1];
      EXPECT_EQ(want.h, got.h) << "src " << src[0] << "x" << src[1] << " screen " << scr[0] << "x" << scr[1];
    }
  }
  // Never upscales: a small source into the big panel stays 1:1, centered.
  const ttfimg::Placement one = ttfimg::fitPlacement(48, 64, 480, 800);
  EXPECT_EQ(one.w, 48);
  EXPECT_EQ(one.h, 64);
  EXPECT_EQ(one.x, (480 - 48) / 2);
  EXPECT_EQ(one.y, (800 - 64) / 2);
}

TEST(ViewerGeneration, FoldsMtimeAndSize) {
  // Composite token: either component changing alone moves the cache name —
  // a same-tick replacement with a different size is NOT the old bake.
  EXPECT_EQ(ttfimg::viewerGeneration(0x20261111u, 12345), ttfimg::viewerGeneration(0x20261111u, 12345));
  EXPECT_NE(ttfimg::viewerGeneration(0x20261111u, 12345), ttfimg::viewerGeneration(0x20261112u, 12345));
  EXPECT_NE(ttfimg::viewerGeneration(0x20261111u, 12345), ttfimg::viewerGeneration(0x20261111u, 12346));
  EXPECT_NE(ttfimg::viewerGeneration(0, 12345), ttfimg::viewerGeneration(0, 12346));
}

TEST(ViewerGeneration, MtimeChangePicksAFreshCacheName) {
  // The generation keys the cache NAME: a replaced file (new mtime or new
  // size) never finds the previous bake.
  ttfimg::ImageSpec a{};
  snprintf(a.href, sizeof(a.href), "/img/anchor.png");
  a.w = 48;
  a.h = 64;
  a.x = 216;
  a.y = 368;
  ttfimg::ImageSpec b = a;
  a.generation = ttfimg::viewerGeneration(1000, 900);
  b.generation = ttfimg::viewerGeneration(1000, 901);  // same tick, different size
  char pa[256];
  char pb[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(a, "/v/ficache", true, false, pa, sizeof(pa)));
  ASSERT_TRUE(ttfimg::cacheNameFor(b, "/v/ficache", true, false, pb, sizeof(pb)));
  EXPECT_STRNE(pa, pb);
}

// Panel-shaped fake renderer, same shape as the fit suite's FakeRenderer:
// 800×480 panel in Portrait = 480×800 logical, the viewer's placement space.
struct ViewerRenderer : public GfxRenderer {
  std::vector<uint8_t> fbStorage;
  ViewerRenderer() {
    panelWidth = 800;
    panelHeight = 480;
    orientation = Portrait;
    mode = BW;
    fbStorage.assign(static_cast<size_t>(getDisplayWidthBytes()) * panelHeight, 0xFF);
    fb = reinterpret_cast<uint8_t*>(fbStorage.data());
  }
};

// Dither-tolerant coverage for the viewer's placement box: the 4-level bake
// legitimately leaves white speckles in dark fills, so the predicate mirrors
// the fit suite's — every row and column carries ink and the box is ≥80%
// inked (a dropped pass paints 0%, which is the regression class).
void expectViewerBoxInk(ViewerRenderer& r, const ttfimg::ImageSpec& spec) {
  long dark = 0;
  for (int y = spec.y; y < spec.y + spec.h; ++y) {
    int rowDark = 0;
    for (int x = spec.x; x < spec.x + spec.w; ++x) {
      const int phyX = y;
      const int phyY = static_cast<int>(r.panelHeight) - 1 - x;
      const int stride = r.getDisplayWidthBytes();
      if ((r.fbStorage[static_cast<size_t>(phyY) * stride + (phyX >> 3)] & (0x80 >> (phyX & 7))) == 0) ++rowDark;
    }
    EXPECT_GT(rowDark, 0) << "placement row " << y << " has no ink";
    dark += rowDark;
  }
  for (int x = spec.x; x < spec.x + spec.w; ++x) {
    int colDark = 0;
    for (int y = spec.y; y < spec.y + spec.h; ++y) {
      const int phyX = y;
      const int phyY = static_cast<int>(r.panelHeight) - 1 - x;
      const int stride = r.getDisplayWidthBytes();
      if ((r.fbStorage[static_cast<size_t>(phyY) * stride + (phyX >> 3)] & (0x80 >> (phyX & 7))) == 0) ++colDark;
    }
    EXPECT_GT(colDark, 0) << "placement column " << x << " has no ink";
  }
  EXPECT_GE(dark * 5, static_cast<long>(spec.w) * spec.h * 4) << "placement under-filled";
}

// RAII fixture reader (kody rule).
std::vector<uint8_t> readFixtureFile(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(ImageViewerPath, StandaloneDecodePublishesCacheAndBlits) {
  auto& storage = HalStorage::getInstance();
  storage.resetFiles();
  const auto bytes = readFixtureFile(FIT_FIXTURES "/dark.png");
  ASSERT_GT(bytes.size(), 32u);
  storage.files["/img/dark.png"] = std::string(bytes.begin(), bytes.end());

  static uint8_t scratchBuf[4 << 20];
  freeink::book::Arena scratch(scratchBuf, sizeof(scratchBuf));
  freeink::book::SdCardBookSource source("/img/dark.png");
  ASSERT_TRUE(source.isValid());
  ttfimg::StandaloneImageCatalog catalog;
  ASSERT_TRUE(catalog.open(source.size()));
  const freeink::book::ZipEntry* entry = catalog.find("/img/dark.png");
  ASSERT_NE(entry, nullptr);
  freeink::book::ImageInfo info;
  ASSERT_EQ(freeink::book::probeImage(source, *entry, scratch, &info), freeink::book::BookStatus::Ok);
  EXPECT_EQ(info.kind, freeink::book::ImageInfo::Kind::Png);

  ViewerRenderer r;
  const ttfimg::Placement place =
      ttfimg::fitPlacement(info.width, info.height, r.getScreenWidth(), r.getScreenHeight());
  // 800×1200 into 480×800: the min ratio (0.6) is width-constrained.
  EXPECT_EQ(place.w, 480);
  EXPECT_EQ(place.h, 720);
  EXPECT_EQ(place.x, 0);
  EXPECT_EQ(place.y, (800 - 720) / 2);

  ttfimg::ImageSpec spec{};
  snprintf(spec.href, sizeof(spec.href), "/img/dark.png");
  spec.generation = ttfimg::viewerGeneration(777, source.size());
  spec.x = place.x;
  spec.y = place.y;
  spec.w = place.w;
  spec.h = place.h;

  const char* dir = "/.crosspoint/viewer/ficache";
  char cachePath[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, dir, true, false, cachePath, sizeof(cachePath)));
  EXPECT_EQ(ttfimg::decodeAndCache(spec, cachePath, source, catalog, scratch, /*epub=*/nullptr, &r,
                                   /*qualityTone=*/true, /*caching=*/true, nullptr, nullptr,
                                   /*paintToTarget=*/true),
            ttfimg::DecodeResult::Ok);
  EXPECT_TRUE(ttfimg::hasValidCache(spec, dir, true, false));
  EXPECT_FALSE(storage.exists((std::string(cachePath) + ".t").c_str()));
  EXPECT_FALSE(storage.exists((std::string(cachePath) + ".g").c_str()));

  // Cache-hit path: the blit alone covers the placement box.
  r.fbStorage.assign(r.fbStorage.size(), 0xFF);
  EXPECT_TRUE(ttfimg::blitFromCache(r, spec, dir, true, false));
  expectViewerBoxInk(r, spec);
}

TEST(ImageViewerPath, CacheStreamFailureAfterPaintReportsOk) {
  // The paint-vs-publish contract (review 2026-11-11): a cache stream that
  // dies mid-remap after the rows were painted is a display success with no
  // cache — the viewer must not turn a painted image into a file error.
  auto& storage = HalStorage::getInstance();
  storage.resetFiles();
  const auto bytes = readFixtureFile(FIT_FIXTURES "/dark.png");
  ASSERT_GT(bytes.size(), 32u);
  storage.files["/img/fullcard.png"] = std::string(bytes.begin(), bytes.end());

  static uint8_t scratchBuf[4 << 20];
  freeink::book::Arena scratch(scratchBuf, sizeof(scratchBuf));
  freeink::book::SdCardBookSource source("/img/fullcard.png");
  ASSERT_TRUE(source.isValid());
  ttfimg::StandaloneImageCatalog catalog;
  ASSERT_TRUE(catalog.open(source.size()));
  const freeink::book::ZipEntry* entry = catalog.find("/img/fullcard.png");
  ASSERT_NE(entry, nullptr);
  freeink::book::ImageInfo info;
  ASSERT_EQ(freeink::book::probeImage(source, *entry, scratch, &info), freeink::book::BookStatus::Ok);

  ViewerRenderer r;
  const ttfimg::Placement place =
      ttfimg::fitPlacement(info.width, info.height, r.getScreenWidth(), r.getScreenHeight());
  ttfimg::ImageSpec spec{};
  snprintf(spec.href, sizeof(spec.href), "/img/fullcard.png");
  spec.generation = 3;
  spec.x = place.x;
  spec.y = place.y;
  spec.w = place.w;
  spec.h = place.h;

  // Write-call ordering (process-global stub counters): the gray collect
  // writes spec.h rows, then the cache stream writes the identity header +
  // one row per remap row. Failing the SECOND cache-row write arms a
  // MID-STREAM cache failure, after every gray row was collected.
  struct WriteFailGuard {
    const int grayRows;
    explicit WriteFailGuard(const int rows) : grayRows(rows) {
      HalFile::writeCallCount = 0;
      HalFile::failWriteAtCall = grayRows + 2;  // gray rows + header + 1st cache row
    }
    ~WriteFailGuard() { HalFile::failWriteAtCall = 0; }
  } guard{place.h};

  char cachePath[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, "/.crosspoint/viewer/ficache", true, false, cachePath, sizeof(cachePath)));
  const auto res = ttfimg::decodeAndCache(spec, cachePath, source, catalog, scratch, /*epub=*/nullptr, &r,
                                          /*qualityTone=*/true, /*caching=*/true, nullptr, nullptr,
                                          /*paintToTarget=*/true);
  EXPECT_EQ(res, ttfimg::DecodeResult::Ok);
  // The placement carries the painted image...
  expectViewerBoxInk(r, spec);
  // ...but no cache was published and no temp debris survives.
  EXPECT_FALSE(ttfimg::hasValidCache(spec, "/.crosspoint/viewer/ficache", true, false));
  EXPECT_FALSE(storage.exists((std::string(cachePath) + ".t").c_str()));
  EXPECT_FALSE(storage.exists((std::string(cachePath) + ".g").c_str()));
}

TEST(ImageViewerPath, DecodeFailureLeavesPlacementEmpty) {
  auto& storage = HalStorage::getInstance();
  storage.resetFiles();
  const auto bytes = readFixtureFile(FIT_FIXTURES "/dark.png");
  ASSERT_GT(bytes.size(), 32u);
  storage.files["/img/broken.png"] = std::string(bytes.begin(), bytes.end());

  static uint8_t scratchBuf[4 << 20];
  freeink::book::Arena scratch(scratchBuf, sizeof(scratchBuf));
  freeink::book::SdCardBookSource source("/img/broken.png");
  ASSERT_TRUE(source.isValid());
  ttfimg::StandaloneImageCatalog catalog;
  ASSERT_TRUE(catalog.open(source.size()));
  const freeink::book::ZipEntry* entry = catalog.find("/img/broken.png");
  ASSERT_NE(entry, nullptr);
  freeink::book::ImageInfo info;
  ASSERT_EQ(freeink::book::probeImage(source, *entry, scratch, &info), freeink::book::BookStatus::Ok);

  ViewerRenderer r;
  const ttfimg::Placement place =
      ttfimg::fitPlacement(info.width, info.height, r.getScreenWidth(), r.getScreenHeight());
  ttfimg::ImageSpec spec{};
  snprintf(spec.href, sizeof(spec.href), "/img/broken.png");
  spec.generation = 5;
  spec.x = place.x;
  spec.y = place.y;
  spec.w = place.w;
  spec.h = place.h;

  // The viewer's paint-step contract: a failed decode returns false and
  // paints NOTHING — the caller keeps the base (design §7.3). The first
  // HalFile read fails (the probe's header scan), so no pass may paint.
  struct ReadFailGuard {
    // The write/read call counters are process-global (static in the stub);
    // an earlier test in the same binary can leave them nonzero, so the
    // arming must be independent of run order (review 2026-11-11).
    ReadFailGuard() {
      HalFile::readCallCount = 0;
      HalFile::failReadAtCall = 1;
    }
    ~ReadFailGuard() { HalFile::failReadAtCall = 0; }
  } guard;

  char cachePath[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, "/.crosspoint/viewer/ficache", true, false, cachePath, sizeof(cachePath)));
  const auto res = ttfimg::decodeAndCache(spec, cachePath, source, catalog, scratch, /*epub=*/nullptr, &r,
                                          /*qualityTone=*/true, /*caching=*/true, nullptr, nullptr,
                                          /*paintToTarget=*/true);
  EXPECT_NE(res, ttfimg::DecodeResult::Ok);
  EXPECT_FALSE(ttfimg::hasValidCache(spec, "/.crosspoint/viewer/ficache", true, false));

  // The placement box carries no ink at all.
  int dark = 0;
  for (int y = spec.y; y < spec.y + spec.h; ++y) {
    for (int x = spec.x; x < spec.x + spec.w; ++x) {
      const int phyX = y;
      const int phyY = static_cast<int>(r.panelHeight) - 1 - x;
      const int stride = r.getDisplayWidthBytes();
      if ((r.fbStorage[static_cast<size_t>(phyY) * stride + (phyX >> 3)] & (0x80 >> (phyX & 7))) == 0) ++dark;
    }
  }
  EXPECT_EQ(dark, 0);
}
