// Host tests for the TTF image engine core (design
// docs/design/2026-11-07-ttf-image-engine.md): ImageDecodeGate claim/abort
// protocol, the tone-cache identity + validity, the streaming .pxc writer
// format, and the Floyd–Steinberg 4-level bake properties (determinism =
// worker/foreground parity by construction; image-local error state =
// placement independence).
//
// The decode stack itself (SDK row render, converters, BMP decoder) needs the
// device render surface and is exercised on-device; the host suite pins the
// deterministic core both decode paths share.

#include <Arduino.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "Epub/Epub/blocks/ImageDecodeGate.h"
#include "HalStorage.h"
#include "TtfImageCache.h"
#include "TtfImageEngine.h"

namespace {

constexpr const char* kDir = "/books/epub_abc/ficache";

ttfimg::ImageSpec makeSpec(const uint32_t generation, const int x, const int y, const int w, const int h) {
  ttfimg::ImageSpec spec{};
  snprintf(spec.href, sizeof(spec.href), "OEBPS/images/pattern.jpg");
  spec.generation = generation;
  spec.spine = 3;
  spec.page = 7;
  spec.imgIdx = 0;
  spec.x = static_cast<int16_t>(x);
  spec.y = static_cast<int16_t>(y);
  spec.w = static_cast<int16_t>(w);
  spec.h = static_cast<int16_t>(h);
  return spec;
}

// ── cache identity ──────────────────────────────────────────────────────────

TEST(CacheIdentity, DeterministicForSameSpec) {
  char a[256], b[256];
  const auto spec = makeSpec(0x1234, 10, 20, 100, 80);
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, kDir, true, false, a, sizeof(a)));
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, kDir, true, false, b, sizeof(b)));
  EXPECT_STREQ(a, b);
}

TEST(CacheIdentity, GenerationChangesTheName) {
  // Layout invalidation: a new generation must not reuse the old cache.
  char a[256], b[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(makeSpec(1, 10, 20, 100, 80), kDir, true, false, a, sizeof(a)));
  ASSERT_TRUE(ttfimg::cacheNameFor(makeSpec(2, 10, 20, 100, 80), kDir, true, false, b, sizeof(b)));
  EXPECT_STRNE(a, b);
}

TEST(CacheIdentity, PlacementChangesTheName) {
  // Dither/bake identity is placement-keyed: two placements of one image get
  // separate caches (the .fibp placement is part of the identity, §5.4).
  char a[256], b[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(makeSpec(1, 10, 20, 100, 80), kDir, true, false, a, sizeof(a)));
  ASSERT_TRUE(ttfimg::cacheNameFor(makeSpec(1, 12, 20, 100, 80), kDir, true, false, b, sizeof(b)));
  EXPECT_STRNE(a, b);
}

TEST(CacheIdentity, PolicyLetterSeparatesBakes) {
  // The quality revert switch must never mix tone bakes in one file.
  char a[256], b[256], c[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(makeSpec(1, 10, 20, 100, 80), kDir, true, false, a, sizeof(a)));
  ASSERT_TRUE(ttfimg::cacheNameFor(makeSpec(1, 10, 20, 100, 80), kDir, false, false, b, sizeof(b)));
  ASSERT_TRUE(ttfimg::cacheNameFor(makeSpec(1, 10, 20, 100, 80), kDir, true, true, c, sizeof(c)));
  EXPECT_STRNE(a, b);
  EXPECT_STRNE(a, c);
  // 'e' = Sharp, 'f' = DPID + CLAHE (Enhanced), 'l' = legacy: the
  // legacy-BMP-parity bake (amendment 9, 2026-11-11) invalidated every
  // prior bake — the exact AtkinsonDitherer(originalThresholds=false)
  // transfer (identity luma, cutoffs {30,55,150}, targets {15,35,90,210},
  // non-serpentine 6/8) and the single-pass stream.
  EXPECT_NE(strstr(a, "/ie"), nullptr);
  EXPECT_NE(strstr(b, "/il"), nullptr);
  EXPECT_NE(strstr(c, "/if"), nullptr);
}

TEST(CacheIdentity, RefusesOverlongDir) {
  char small[8];
  EXPECT_FALSE(ttfimg::cacheNameFor(makeSpec(1, 0, 0, 10, 10), kDir, true, false, small, sizeof(small)));
}

// ── cache validity + writer format ──────────────────────────────────────────

TEST(CacheValidity, AbsentCacheIsInvalid) {
  auto& storage = HalStorage::getInstance();
  storage.files.clear();
  EXPECT_FALSE(ttfimg::hasValidCache(makeSpec(1, 0, 0, 16, 8), kDir, true, false));
}

TEST(CacheValidity, WrittenCacheIsValid) {
  auto& storage = HalStorage::getInstance();
  storage.files.clear();
  const auto spec = makeSpec(1, 0, 0, 16, 8);
  ttfimg::ToneCacheWriter writer;
  char path[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, kDir, true, false, path, sizeof(path)));
  ASSERT_TRUE(writer.begin(path, 16, 8, ttfimg::cacheIdentity(spec, true, false)));
  uint8_t row[16] = {};
  for (int i = 0; i < 8; ++i) ASSERT_TRUE(writer.writeRow(row));
  ASSERT_TRUE(writer.finalize());

  EXPECT_TRUE(ttfimg::hasValidCache(spec, kDir, true, false));
}

TEST(CacheValidity, HeaderDimsToleranceAndPayloadGuard) {
  // The ±1 rule (the ImageBlock bounded-diff) lives in the header check: a
  // decode whose rounded dims differ by one pixel still loads; beyond that
  // the cache is stale. The payload must cover every cached row.
  auto& storage = HalStorage::getInstance();
  storage.files.clear();
  ttfimg::ToneCacheWriter writer;
  ASSERT_TRUE(writer.begin("/t/dims.pxc", 16, 8, ttfimg::CacheIdentity{'o', 0}));
  uint8_t row[16] = {};
  for (int i = 0; i < 8; ++i) ASSERT_TRUE(writer.writeRow(row));
  ASSERT_TRUE(writer.finalize());

  const ttfimg::CacheIdentity kId{'o', 0};
  const auto openFresh = [&storage](HalFile& f) {
    EXPECT_TRUE(storage.openFileForRead("T", "/t/dims.pxc", f));  // fresh cursor per check
  };
  uint16_t w = 0, h = 0;
  HalFile f;
  openFresh(f);
  EXPECT_TRUE(ttfimg::readValidCacheHeader(f, 15, 8, kId, w, h));  // -1 width
  EXPECT_EQ(w, 16);
  openFresh(f);
  EXPECT_TRUE(ttfimg::readValidCacheHeader(f, 16, 7, kId, w, h));  // -1 height
  openFresh(f);
  EXPECT_FALSE(ttfimg::readValidCacheHeader(f, 14, 8, kId, w, h));  // -2 width
  openFresh(f);
  EXPECT_FALSE(ttfimg::readValidCacheHeader(f, 16, 10, kId, w, h));  // +2 height
  // Truncate the payload: no longer valid for the cached dims.
  std::string& data = storage.files["/t/dims.pxc"];
  data.resize(ttfimg::kCacheHeaderBytes + 4 * 5);
  openFresh(f);
  EXPECT_FALSE(ttfimg::readValidCacheHeader(f, 16, 8, kId, w, h));
}

TEST(CacheValidity, TruncatedPayloadIsInvalid) {
  auto& storage = HalStorage::getInstance();
  storage.files.clear();
  const auto spec = makeSpec(1, 0, 0, 16, 8);
  ttfimg::ToneCacheWriter writer;
  char path[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, kDir, true, false, path, sizeof(path)));
  ASSERT_TRUE(writer.begin(path, 16, 8, ttfimg::cacheIdentity(spec, true, false)));
  uint8_t row[16] = {};
  for (int i = 0; i < 8; ++i) ASSERT_TRUE(writer.writeRow(row));
  ASSERT_TRUE(writer.finalize());
  // Simulate a torn file: cut the payload.
  std::string& data = storage.files[path];
  data.resize(ttfimg::kCacheHeaderBytes + ((16 + 3) / 4) * 5);  // 5 of 8 rows
  EXPECT_FALSE(ttfimg::hasValidCache(spec, kDir, true, false));
}

TEST(ToneWriter, PacksTwoBitPixelsMsbFirst) {
  auto& storage = HalStorage::getInstance();
  storage.files.clear();
  ttfimg::ToneCacheWriter writer;
  ASSERT_TRUE(writer.begin("/t/test.pxc", 8, 1, ttfimg::CacheIdentity{'o', 0}));
  const uint8_t tones[8] = {0, 1, 2, 3, 0, 1, 2, 3};
  ASSERT_TRUE(writer.writeRow(tones));
  ASSERT_TRUE(writer.finalize());
  const std::string& data = storage.files["/t/test.pxc"];
  ASSERT_EQ(data.size(), ttfimg::kCacheHeaderBytes + 2u);  // identity header + one row of 8 px
  // In-file identity header: magic, bake letter, hash32 (0), dims LE16 8, 1.
  EXPECT_EQ(static_cast<uint8_t>(data[0]), ttfimg::kCacheMagic0);
  EXPECT_EQ(static_cast<uint8_t>(data[1]), ttfimg::kCacheMagic1);
  EXPECT_EQ(static_cast<char>(data[2]), 'o');
  EXPECT_EQ(static_cast<uint8_t>(data[3]), 0);  // hash32 low ...
  EXPECT_EQ(static_cast<uint8_t>(data[6]), 0);  // ... high
  EXPECT_EQ(static_cast<uint8_t>(data[7]), 8);  // width LE16
  EXPECT_EQ(static_cast<uint8_t>(data[8]), 0);
  EXPECT_EQ(static_cast<uint8_t>(data[9]), 1);  // height LE16
  EXPECT_EQ(static_cast<uint8_t>(data[10]), 0);
  // MSB-first pairs: 0,1,2,3 -> 00 01 10 11.
  EXPECT_EQ(static_cast<uint8_t>(data[11]), 0b00011011);
  EXPECT_EQ(static_cast<uint8_t>(data[12]), 0b00011011);
}

TEST(ToneWriter, IncompleteFinalizeDropsTheFile) {
  auto& storage = HalStorage::getInstance();
  storage.files.clear();
  ttfimg::ToneCacheWriter writer;
  ASSERT_TRUE(writer.begin("/t/partial.pxc", 16, 8, ttfimg::CacheIdentity{'o', 0}));
  uint8_t row[16] = {};
  ASSERT_TRUE(writer.writeRow(row));
  EXPECT_FALSE(writer.finalize());  // 1 of 8 rows
  EXPECT_EQ(storage.files.count("/t/partial.pxc"), 0u);
}

TEST(ToneWriter, AbortRemovesThePartialFile) {
  auto& storage = HalStorage::getInstance();
  storage.files.clear();
  ttfimg::ToneCacheWriter writer;
  ASSERT_TRUE(writer.begin("/t/abort.pxc", 16, 4, ttfimg::CacheIdentity{'o', 0}));
  uint8_t row[16] = {};
  ASSERT_TRUE(writer.writeRow(row));
  writer.abort();
  EXPECT_EQ(storage.files.count("/t/abort.pxc"), 0u);
  EXPECT_FALSE(writer.writeRow(row));  // writer closed
}

// ── Floyd–Steinberg bake ────────────────────────────────────────────────────

TEST(ToneBake, IsDeterministicAcrossRuns) {
  // G3 parity foundation: worker and foreground run the same bake over the
  // same gray rows — identical inputs must give identical tone bytes.
  const int w = 37;
  uint8_t gray[37];
  for (int i = 0; i < w; ++i) gray[i] = static_cast<uint8_t>(i * 7);
  uint8_t run1[3][37];
  uint8_t run2[3][37];
  for (int pass = 0; pass < 2; ++pass) {
    ttfimg::FloydSteinberg4 bake(w);
    ASSERT_TRUE(bake.ok());
    for (int r = 0; r < 3; ++r) {
      for (int x = 0; x < w; ++x) (pass == 0 ? run1 : run2)[r][x] = bake.tone(x, gray[x]);
      bake.nextRow();
    }
  }
  EXPECT_EQ(memcmp(run1, run2, sizeof(run1)), 0);
}

TEST(ToneBake, IsPlacementIndependent) {
  // The corrected §5.6 contract: the error chain runs over image-local
  // columns, so baking the same image "at" different placements (which only
  // live in the cache identity) yields the same tone bytes — pinned by
  // baking two fresh bakers over identical input (the placement never enters
  // the bake).
  const int w = 24;
  uint8_t gray[24];
  for (int i = 0; i < w; ++i) gray[i] = static_cast<uint8_t>(255 - i * 9);
  ttfimg::FloydSteinberg4 a(w);
  ttfimg::FloydSteinberg4 b(w);
  ASSERT_TRUE(a.ok() && b.ok());
  for (int r = 0; r < 4; ++r) {
    for (int x = 0; x < w; ++x) {
      EXPECT_EQ(a.tone(x, gray[x]), b.tone(x, gray[x]));
    }
    a.nextRow();
    b.nextRow();
  }
}

TEST(ToneBake, QuantizesToFourLevels) {
  ttfimg::FloydSteinberg4 bake(4);
  ASSERT_TRUE(bake.ok());
  EXPECT_EQ(bake.tone(0, 0), 0);
  bake.nextRow();
  ttfimg::FloydSteinberg4 bake2(4);
  EXPECT_EQ(bake2.tone(0, 255), 3);
  bake2.nextRow();
  ttfimg::FloydSteinberg4 bake3(4);
  const uint8_t mid = bake3.tone(0, 127);
  EXPECT_LE(mid, 3);
  EXPECT_GE(mid, 1);
}

// ── ToneBake4: legacy-BMP parity (amendment 9) ───────────────────────────

namespace {

// Drives the legacy AtkinsonDitherer exactly the way JpegToBmpConverter
// does (left-to-right, nextRow() per row) and returns the per-row 2-bit
// tones — the reference side of the parity pins.
std::vector<uint8_t> legacyBakeRows(const int w, const int h, const std::vector<uint8_t>& gray) {
  AtkinsonDitherer ditherer(w);
  std::vector<uint8_t> tones(gray.size(), 0);
  std::vector<uint8_t> row(static_cast<size_t>(w));
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) row[x] = ditherer.processPixel(gray[static_cast<size_t>(y) * w + x], x);
    ditherer.nextRow();
    std::copy(row.begin(), row.end(), tones.begin() + static_cast<std::ptrdiff_t>(y) * w);
  }
  return tones;
}

// Bakes the same rows through ToneBake4's toneRow (the production path).
std::vector<uint8_t> ttfBakeRows(const int w, const int h, const std::vector<uint8_t>& gray) {
  ttfimg::ToneBake4 bake(w);
  EXPECT_TRUE(bake.ok());
  std::vector<uint8_t> tones(gray.size(), 0);
  std::vector<uint8_t> row(static_cast<size_t>(w));
  for (int y = 0; y < h; ++y) {
    bake.toneRow(&gray[static_cast<size_t>(y) * w], row.data());
    std::copy(row.begin(), row.end(), tones.begin() + static_cast<std::ptrdiff_t>(y) * w);
  }
  return tones;
}

}  // namespace

TEST(LegacyAtkinsonParityOnSampleImage, SmoothWallFadeIsByteIdentical) {
  // A 00005-shaped page: a smooth 90..250 wall fade over 120 rows (the
  // tonal balance the owner approved on the legacy engine). Both engines
  // must agree byte-for-byte on every 2-bit pixel.
  const int w = 96;
  const int h = 120;
  std::vector<uint8_t> gray(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    const uint8_t g = static_cast<uint8_t>(90 + (y * 160) / (h - 1));
    std::fill(gray.begin() + static_cast<size_t>(y) * w, gray.begin() + static_cast<size_t>(y + 1) * w, g);
  }
  const auto legacy = legacyBakeRows(w, h, gray);
  const auto ttf = ttfBakeRows(w, h, gray);
  EXPECT_EQ(0, memcmp(legacy.data(), ttf.data(), legacy.size()));
}

TEST(LegacyAtkinsonParityOnSampleImage, AntiAliasedTextPageIsByteIdentical) {
  // A 00006-shaped page: white paper (255) with anti-aliased black text
  // strokes sweeping 0..160 across the row, plus light midtone shading.
  const int w = 128;
  const int h = 64;
  std::vector<uint8_t> gray(static_cast<size_t>(w) * h, 255);
  for (int y = 8; y < 56; ++y) {
    for (int x = 4; x < 124; ++x) {
      // A sweep of stroke-edge values: ink, anti-aliased shoulders, paper.
      const int t = (x - 4) * 160 / 119;
      gray[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(t < 128 ? 255 - t : 255 - (255 - t) / 2);
    }
  }
  const auto legacy = legacyBakeRows(w, h, gray);
  const auto ttf = ttfBakeRows(w, h, gray);
  EXPECT_EQ(0, memcmp(legacy.data(), ttf.data(), legacy.size()));
}

TEST(LegacyAtkinsonParityOnSampleImage, MidtoneShadingIsByteIdentical) {
  // Flat midtone fields bracketing each legacy cutoff (29/30/54/55/149/
  // 150) plus a woven-shading ramp — the exact-value pins against
  // AtkinsonDitherer.
  const int w = 64;
  const int h = 8;
  std::vector<uint8_t> gray;
  for (const int v : {0, 20, 29, 30, 40, 54, 55, 60, 100, 128, 149, 150, 151, 160, 202, 203, 210, 250, 255}) {
    for (int r = 0; r < h; ++r) gray.insert(gray.end(), static_cast<size_t>(w), static_cast<uint8_t>(v));
  }
  const auto legacy = legacyBakeRows(w, h, gray);
  const auto ttf = ttfBakeRows(w, h, gray);
  EXPECT_EQ(0, memcmp(legacy.data(), ttf.data(), legacy.size()));
}

TEST(LegacyAtkinsonParityOnSampleImage, QuantizerBoundariesMatchTheLegacyThresholds) {
  // Fresh-bake per-pixel picks: a fresh bake carries no error, so the
  // adjusted value IS the source gray — the legacy cutoffs {30, 55, 150}
  // and targets {15, 35, 90, 210} must show through exactly.
  const auto pick = [](const uint8_t g) {
    ttfimg::ToneBake4 bake(8);
    return bake.tone(0, g);
  };
  EXPECT_EQ(pick(0), 0);
  EXPECT_EQ(pick(29), 0);  // < 30
  EXPECT_EQ(pick(30), 1);  // 30..54 → target 35
  EXPECT_EQ(pick(54), 1);
  EXPECT_EQ(pick(55), 2);  // 55..149 → target 90
  EXPECT_EQ(pick(100), 2);
  EXPECT_EQ(pick(149), 2);
  EXPECT_EQ(pick(150), 3);  // >= 150 → target 210 (highlight headroom)
  EXPECT_EQ(pick(203), 3);  // clean paper: no harsh snap below 203
  EXPECT_EQ(pick(255), 3);
}

TEST(ToneBake4, IsDeterministicAcrossRuns) {
  // Worker/foreground parity foundation: the same bake over the same rows.
  const int w = 37;
  uint8_t gray[37];
  for (int i = 0; i < w; ++i) gray[i] = static_cast<uint8_t>(i * 7);
  uint8_t run1[3][37];
  uint8_t run2[3][37];
  for (int pass = 0; pass < 2; ++pass) {
    ttfimg::ToneBake4 bake(w);
    ASSERT_TRUE(bake.ok());
    for (int r = 0; r < 3; ++r) {
      for (int x = 0; x < w; ++x) (pass == 0 ? run1 : run2)[r][x] = bake.tone(x, gray[x]);
      bake.nextRow();
    }
  }
  EXPECT_EQ(memcmp(run1, run2, sizeof(run1)), 0);
}

TEST(ToneBake4, IsPlacementIndependent) {
  // Error state runs over image-local columns; the placement never enters
  // the bake (§5.4 of the 2026-11-07 design).
  const int w = 24;
  uint8_t gray[24];
  for (int i = 0; i < w; ++i) gray[i] = static_cast<uint8_t>(255 - i * 9);
  ttfimg::ToneBake4 a(w);
  ttfimg::ToneBake4 b(w);
  ASSERT_TRUE(a.ok() && b.ok());
  for (int r = 0; r < 4; ++r) {
    for (int x = 0; x < w; ++x) {
      EXPECT_EQ(a.tone(x, gray[x]), b.tone(x, gray[x]));
    }
    a.nextRow();
    b.nextRow();
  }
}

TEST(ToneBake4, ToneRowDeterministicAcrossRuns) {
  // Worker/foreground parity through the row API (the production path).
  const int w = 37;
  uint8_t gray[37];
  for (int i = 0; i < w; ++i) gray[i] = static_cast<uint8_t>(i * 7);
  uint8_t run1[3][37];
  uint8_t run2[3][37];
  for (int pass = 0; pass < 2; ++pass) {
    ttfimg::ToneBake4 bake(w);
    ASSERT_TRUE(bake.ok());
    for (int r = 0; r < 3; ++r) bake.toneRow(gray, (pass == 0 ? run1 : run2)[r]);
  }
  EXPECT_EQ(memcmp(run1, run2, sizeof(run1)), 0);
}

TEST(ToneBake4, ErrorStateResetsForTheRepaintFallback) {
  // resetError() gives the re-decode paint fallbacks a clean plane: after
  // baking rows, the next bake must equal a fresh one (the same rows).
  const int w = 16;
  uint8_t gray[16];
  for (int i = 0; i < w; ++i) gray[i] = static_cast<uint8_t>(30 + i * 14);
  ttfimg::ToneBake4 dirty(w);
  ASSERT_TRUE(dirty.ok());
  uint8_t row[16];
  dirty.toneRow(gray, row);
  dirty.resetError();
  ttfimg::ToneBake4 fresh(w);
  ASSERT_TRUE(fresh.ok());
  uint8_t row2[16];
  dirty.toneRow(gray, row);
  fresh.toneRow(gray, row2);
  EXPECT_EQ(0, memcmp(row, row2, sizeof(row)));
}

TEST(ToneBake4, FlatFieldsBakeTheirLegacyLevel) {
  // Legacy character: a flat field far from every cutoff carries (almost)
  // no diffusion error and bakes SOLID; the woven dither only appears where
  // |adjusted − target| accumulates past the >>3 shift. Pinned against the
  // legacy ditherer itself — parity makes the reference authoritative.
  for (const int v : {0, 20, 40, 60, 100, 128, 160, 203, 250}) {
    const int w = 96;
    const int h = 24;
    std::vector<uint8_t> gray(static_cast<size_t>(w) * h, static_cast<uint8_t>(v));
    const auto legacy = legacyBakeRows(w, h, gray);
    const auto ttf = ttfBakeRows(w, h, gray);
    ASSERT_EQ(0, memcmp(legacy.data(), ttf.data(), legacy.size())) << "gray " << v;
    // Tone-correctness: the emitted mean tracks the input within the
    // legacy quantizer's own span (level target ± accumulated diffusion).
    long long emitted = 0;
    for (const uint8_t t : legacy) emitted += t == 0 ? 15 : (t == 1 ? 35 : (t == 2 ? 90 : 210));
    const double mean = static_cast<double>(emitted) / legacy.size();
    EXPECT_NEAR(mean, static_cast<double>(v), 45.0) << "gray " << v << " mean emitted " << mean;
  }
}

TEST(ToneBake4, GradientUsesAllFourTones) {
  ttfimg::ToneBake4 bake(64);
  ASSERT_TRUE(bake.ok());
  bool seen[4] = {false, false, false, false};
  for (int r = 0; r < 8; ++r) {
    for (int x = 0; x < 64; ++x) {
      const int gray = (r * 64 + x) * 255 / 512;
      seen[bake.tone(x, gray)] = true;
    }
    bake.nextRow();
  }
  for (int t = 0; t < 4; ++t) EXPECT_TRUE(seen[t]) << "tone " << t << " never emitted";
}

// ── Display routing predicate (design 2026-11-08 §5.1) ───────────────────

TEST(ImageGrayRoute, PredicateMatrix) {
  using ttfimg::absoluteGrayImageRoute;
  // Image page, quality path, capable panel: the absolute-gray route.
  EXPECT_TRUE(absoluteGrayImageRoute(true, true, false, true, true));
  // Text-only page: the overlay parity route, never the absolute image arm.
  EXPECT_FALSE(absoluteGrayImageRoute(false, true, false, true, true));
  // Revert switch (imageDecodeQuality == 0): today's behavior verbatim.
  EXPECT_FALSE(absoluteGrayImageRoute(true, false, false, true, true));
  // X3 guard + non-UC8279 controller + no absolute capability.
  EXPECT_FALSE(absoluteGrayImageRoute(true, true, true, true, true));
  EXPECT_FALSE(absoluteGrayImageRoute(true, true, false, false, true));
  EXPECT_FALSE(absoluteGrayImageRoute(true, true, false, true, false));
}

// ── ImageDecodeGate protocol ────────────────────────────────────────────────

class GateProtocol : public ::testing::Test {
 protected:
  void SetUp() override {
    g_fakeMillis = 0;
    ImageDecodeGate::resetForTest();
  }
};

TEST_F(GateProtocol, WorkerClaimIsExclusive) {
  const uint64_t h = ImageDecodeGate::pathHash("/c/iq00000001-00000002.pxc");
  EXPECT_TRUE(ImageDecodeGate::tryAcquireWorker(h));
  EXPECT_FALSE(ImageDecodeGate::tryAcquireWorker(h));  // second claim refused
  ImageDecodeGate::releaseWorker(h);
  EXPECT_TRUE(ImageDecodeGate::tryAcquireWorker(h));  // free again
  ImageDecodeGate::releaseWorker(h);
}

TEST_F(GateProtocol, ForegroundAbortsWorkerClaimAndTakesOver) {
  const uint64_t h = ImageDecodeGate::pathHash("/c/a.pxc");
  ASSERT_TRUE(ImageDecodeGate::tryAcquireWorker(h));
  // Foreground acquire flags the worker abort immediately; with the worker
  // still holding the claim the bounded wait expires (false) — the paint
  // leaves the space blank this pass.
  EXPECT_FALSE(ImageDecodeGate::acquireForeground(h));
  EXPECT_TRUE(ImageDecodeGate::workerAbortRequested(h));
  // The worker honors the abort at its next band boundary and releases; the
  // foreground then takes the claim on its next pass.
  ImageDecodeGate::releaseWorker(h);
  EXPECT_TRUE(ImageDecodeGate::acquireForeground(h));
  ImageDecodeGate::releaseForeground(h);
  EXPECT_FALSE(ImageDecodeGate::workerAbortRequested(h));
}

TEST_F(GateProtocol, ForegroundTimesOutOnWedgedWorkerClaim) {
  const uint64_t h = ImageDecodeGate::pathHash("/c/b.pxc");
  ASSERT_TRUE(ImageDecodeGate::tryAcquireWorker(h));
  // The bounded wait polls with delay(); the fake clock advances per poll,
  // so the wait ends (false) instead of hanging.
  EXPECT_FALSE(ImageDecodeGate::acquireForeground(h));
  EXPECT_TRUE(ImageDecodeGate::workerAbortRequested(h));  // abort was still flagged
  ImageDecodeGate::releaseWorker(h);
}

TEST_F(GateProtocol, AbortAllWorkerClaimsSparesForeground) {
  const uint64_t w = ImageDecodeGate::pathHash("/c/w.pxc");
  const uint64_t f = ImageDecodeGate::pathHash("/c/f.pxc");
  ASSERT_TRUE(ImageDecodeGate::tryAcquireWorker(w));
  ASSERT_TRUE(ImageDecodeGate::acquireForeground(f));
  ImageDecodeGate::abortAllWorkerClaims();
  EXPECT_TRUE(ImageDecodeGate::workerAbortRequested(w));
  // Foreground claims are untouched (re-acquire is idempotent).
  EXPECT_TRUE(ImageDecodeGate::acquireForeground(f));
  ImageDecodeGate::releaseWorker(w);
  ImageDecodeGate::releaseForeground(f);
}

// Decode bus (rule 4): the foreground signals pending while it holds the
// bus, the flag clears with the release, and a wedged holder only costs the
// bounded wait (the fake clock advances per poll).
TEST_F(GateProtocol, ForegroundBusSignalsWhileHeldAndClearsOnRelease) {
  EXPECT_TRUE(ImageDecodeGate::acquireForegroundBus(4));
  EXPECT_TRUE(ImageDecodeGate::foregroundBusPending());
  ImageDecodeGate::releaseForegroundBus();
  EXPECT_FALSE(ImageDecodeGate::foregroundBusPending());
  // The bus is free again for the worker's poll.
  EXPECT_TRUE(ImageDecodeGate::tryAcquireDecodeBus());
  ImageDecodeGate::releaseDecodeBus();
}

TEST_F(GateProtocol, ForegroundBusTimesOutOnWedgedHolder) {
  ASSERT_TRUE(ImageDecodeGate::tryAcquireDecodeBus());
  EXPECT_FALSE(ImageDecodeGate::acquireForegroundBus(4));
  EXPECT_FALSE(ImageDecodeGate::foregroundBusPending());  // cleared on timeout
  ImageDecodeGate::releaseDecodeBus();
  EXPECT_TRUE(ImageDecodeGate::tryAcquireDecodeBus());
  ImageDecodeGate::releaseDecodeBus();
}

}  // namespace

uint32_t g_fakeMillis = 0;
