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

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include <Arduino.h>

#include "HalStorage.h"

#include "Epub/Epub/blocks/ImageDecodeGate.h"
#include "TtfImageEngine.h"
#include "TtfImageCache.h"

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
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, kDir, true, a, sizeof(a)));
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, kDir, true, b, sizeof(b)));
  EXPECT_STREQ(a, b);
}

TEST(CacheIdentity, GenerationChangesTheName) {
  // Layout invalidation: a new generation must not reuse the old cache.
  char a[256], b[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(makeSpec(1, 10, 20, 100, 80), kDir, true, a, sizeof(a)));
  ASSERT_TRUE(ttfimg::cacheNameFor(makeSpec(2, 10, 20, 100, 80), kDir, true, b, sizeof(b)));
  EXPECT_STRNE(a, b);
}

TEST(CacheIdentity, PlacementChangesTheName) {
  // Dither/bake identity is placement-keyed: two placements of one image get
  // separate caches (the .fibp placement is part of the identity, §5.4).
  char a[256], b[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(makeSpec(1, 10, 20, 100, 80), kDir, true, a, sizeof(a)));
  ASSERT_TRUE(ttfimg::cacheNameFor(makeSpec(1, 12, 20, 100, 80), kDir, true, b, sizeof(b)));
  EXPECT_STRNE(a, b);
}

TEST(CacheIdentity, PolicyLetterSeparatesBakes) {
  // The quality revert switch must never mix tone bakes in one file.
  char a[256], b[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(makeSpec(1, 10, 20, 100, 80), kDir, true, a, sizeof(a)));
  ASSERT_TRUE(ttfimg::cacheNameFor(makeSpec(1, 10, 20, 100, 80), kDir, false, b, sizeof(b)));
  EXPECT_STRNE(a, b);
  EXPECT_NE(strstr(a, "/iq"), nullptr);
  EXPECT_NE(strstr(b, "/il"), nullptr);
}

TEST(CacheIdentity, RefusesOverlongDir) {
  char small[8];
  EXPECT_FALSE(ttfimg::cacheNameFor(makeSpec(1, 0, 0, 10, 10), kDir, true, small, sizeof(small)));
}

// ── cache validity + writer format ──────────────────────────────────────────

TEST(CacheValidity, AbsentCacheIsInvalid) {
  auto& storage = HalStorage::getInstance();
  storage.files.clear();
  EXPECT_FALSE(ttfimg::hasValidCache(makeSpec(1, 0, 0, 16, 8), kDir, true));
}

TEST(CacheValidity, WrittenCacheIsValid) {
  auto& storage = HalStorage::getInstance();
  storage.files.clear();
  const auto spec = makeSpec(1, 0, 0, 16, 8);
  ttfimg::ToneCacheWriter writer;
  char path[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, kDir, true, path, sizeof(path)));
  ASSERT_TRUE(writer.begin(path, 16, 8));
  uint8_t row[16] = {};
  for (int i = 0; i < 8; ++i) ASSERT_TRUE(writer.writeRow(row));
  ASSERT_TRUE(writer.finalize());

  EXPECT_TRUE(ttfimg::hasValidCache(spec, kDir, true));
}

TEST(CacheValidity, HeaderDimsToleranceAndPayloadGuard) {
  // The ±1 rule (the ImageBlock bounded-diff) lives in the header check: a
  // decode whose rounded dims differ by one pixel still loads; beyond that
  // the cache is stale. The payload must cover every cached row.
  auto& storage = HalStorage::getInstance();
  storage.files.clear();
  ttfimg::ToneCacheWriter writer;
  ASSERT_TRUE(writer.begin("/t/dims.pxc", 16, 8));
  uint8_t row[16] = {};
  for (int i = 0; i < 8; ++i) ASSERT_TRUE(writer.writeRow(row));
  ASSERT_TRUE(writer.finalize());

  const auto openFresh = [&storage](HalFile& f) {
    EXPECT_TRUE(storage.openFileForRead("T", "/t/dims.pxc", f));  // fresh cursor per check
  };
  uint16_t w = 0, h = 0;
  HalFile f;
  openFresh(f);
  EXPECT_TRUE(ttfimg::readValidCacheHeader(f, 15, 8, w, h));   // -1 width
  EXPECT_EQ(w, 16);
  openFresh(f);
  EXPECT_TRUE(ttfimg::readValidCacheHeader(f, 16, 7, w, h));   // -1 height
  openFresh(f);
  EXPECT_FALSE(ttfimg::readValidCacheHeader(f, 14, 8, w, h));  // -2 width
  openFresh(f);
  EXPECT_FALSE(ttfimg::readValidCacheHeader(f, 16, 10, w, h)); // +2 height
  // Truncate the payload: no longer valid for the cached dims.
  std::string& data = storage.files["/t/dims.pxc"];
  data.resize(4 + 4 * 5);
  openFresh(f);
  EXPECT_FALSE(ttfimg::readValidCacheHeader(f, 16, 8, w, h));
}

TEST(CacheValidity, TruncatedPayloadIsInvalid) {
  auto& storage = HalStorage::getInstance();
  storage.files.clear();
  const auto spec = makeSpec(1, 0, 0, 16, 8);
  ttfimg::ToneCacheWriter writer;
  char path[256];
  ASSERT_TRUE(ttfimg::cacheNameFor(spec, kDir, true, path, sizeof(path)));
  ASSERT_TRUE(writer.begin(path, 16, 8));
  uint8_t row[16] = {};
  for (int i = 0; i < 8; ++i) ASSERT_TRUE(writer.writeRow(row));
  ASSERT_TRUE(writer.finalize());
  // Simulate a torn file: cut the payload.
  std::string& data = storage.files[path];
  data.resize(4 + ((16 + 3) / 4) * 5);  // 5 of 8 rows
  EXPECT_FALSE(ttfimg::hasValidCache(spec, kDir, true));
}

TEST(ToneWriter, PacksTwoBitPixelsMsbFirst) {
  auto& storage = HalStorage::getInstance();
  storage.files.clear();
  ttfimg::ToneCacheWriter writer;
  ASSERT_TRUE(writer.begin("/t/test.pxc", 8, 1));
  const uint8_t tones[8] = {0, 1, 2, 3, 0, 1, 2, 3};
  ASSERT_TRUE(writer.writeRow(tones));
  ASSERT_TRUE(writer.finalize());
  const std::string& data = storage.files["/t/test.pxc"];
  ASSERT_EQ(data.size(), 4u + 2u);  // header + one row of 8 px
  // Header LE16 8, 1.
  EXPECT_EQ(static_cast<uint8_t>(data[0]), 8);
  EXPECT_EQ(static_cast<uint8_t>(data[1]), 0);
  EXPECT_EQ(static_cast<uint8_t>(data[2]), 1);
  EXPECT_EQ(static_cast<uint8_t>(data[3]), 0);
  // MSB-first pairs: 0,1,2,3 -> 00 01 10 11.
  EXPECT_EQ(static_cast<uint8_t>(data[4]), 0b00011011);
  EXPECT_EQ(static_cast<uint8_t>(data[5]), 0b00011011);
}

TEST(ToneWriter, IncompleteFinalizeDropsTheFile) {
  auto& storage = HalStorage::getInstance();
  storage.files.clear();
  ttfimg::ToneCacheWriter writer;
  ASSERT_TRUE(writer.begin("/t/partial.pxc", 16, 8));
  uint8_t row[16] = {};
  ASSERT_TRUE(writer.writeRow(row));
  EXPECT_FALSE(writer.finalize());  // 1 of 8 rows
  EXPECT_EQ(storage.files.count("/t/partial.pxc"), 0u);
}

TEST(ToneWriter, AbortRemovesThePartialFile) {
  auto& storage = HalStorage::getInstance();
  storage.files.clear();
  ttfimg::ToneCacheWriter writer;
  ASSERT_TRUE(writer.begin("/t/abort.pxc", 16, 4));
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

}  // namespace

uint32_t g_fakeMillis = 0;
