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
  // 't' = the adaptive two-phase bake with flat-field snap, threshold-noise
  // and stroke anchoring (design 2026-11-08-ttf-image-texture-quality.md §5
  // as-built); 's' was the pre-texture adaptive bake; 'l' = legacy.
  EXPECT_NE(strstr(a, "/it"), nullptr);
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
  EXPECT_TRUE(ttfimg::readValidCacheHeader(f, 15, 8, w, h));  // -1 width
  EXPECT_EQ(w, 16);
  openFresh(f);
  EXPECT_TRUE(ttfimg::readValidCacheHeader(f, 16, 7, w, h));  // -1 height
  openFresh(f);
  EXPECT_FALSE(ttfimg::readValidCacheHeader(f, 14, 8, w, h));  // -2 width
  openFresh(f);
  EXPECT_FALSE(ttfimg::readValidCacheHeader(f, 16, 10, w, h));  // +2 height
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

// ── ToneBake4 (design 2026-11-08 §5.2) ───────────────────────────────────

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

TEST(ToneBake4, DiffusionWeightScalesEveryKernelTerm) {
  // W = 2560/2560 = 1: term = errVal·k/16 for k = 7|3|5|1 (texture §5
  // as-built), C truncation toward zero.
  ttfimg::ToneBake4 bake(8);
  ASSERT_TRUE(bake.ok());
  EXPECT_EQ(bake.diffuse(20, 7), 8);     // 140/16
  EXPECT_EQ(bake.diffuse(20, 3), 3);     // 60/16
  EXPECT_EQ(bake.diffuse(20, 5), 6);     // 100/16
  EXPECT_EQ(bake.diffuse(20, 1), 1);     // 20/16
  EXPECT_EQ(bake.diffuse(-40, 7), -17);  // -280/16 truncates toward zero
  EXPECT_EQ(bake.diffuse(3, 7), 1);      // 21/16 truncated
}

TEST(ToneBake4, SerpentineMirrorsTheKernel) {
  // toneRow drives the scan direction itself: a mirror-symmetric row baked
  // ascending (row 0) equals the same row baked on a reverse-seeded bake —
  // the FS kernel mirrors with the visit direction, so the horizontal error
  // term always lands on the next-visited pixel.
  const int w = 16;
  uint8_t gray[w];
  for (int i = 0; i < w; ++i) gray[i] = static_cast<uint8_t>(60 + (i < w - 1 - i ? i : w - 1 - i) * 15);
  ttfimg::ToneBake4 forward(w);
  ttfimg::ToneBake4 backward(w);
  ASSERT_TRUE(forward.ok() && backward.ok());
  backward.reverse = true;  // row 0 visits right-to-left
  uint8_t fwd[w];
  uint8_t rev[w];
  forward.toneRow(gray, fwd);
  backward.toneRow(gray, rev);
  for (int x = 0; x < w; ++x) EXPECT_EQ(fwd[x], rev[w - 1 - x]) << "column " << x;
  // And the serpentine state flips per row: row 1 of the same bake (visited
  // descending) differs from row 0 — direction-aware error flow.
  uint8_t row2[w];
  forward.toneRow(gray, row2);
  EXPECT_NE(memcmp(fwd, row2, sizeof(fwd)), 0);
}

TEST(ToneBake4, PerPixelToneStaysForwardForAscendingCallers) {
  // The ToneBaker per-pixel kernel is ALWAYS ascending (the callers' raster
  // order): a reverse-flagged bake's tone() must not mirror the kernel —
  // the serpentine lives in toneRow's visit order, not in tone().
  const int w = 8;
  ttfimg::ToneBake4 a(w);
  ttfimg::ToneBake4 b(w);
  ASSERT_TRUE(a.ok() && b.ok());
  b.reverse = true;  // must not change tone()'s kernel
  for (int x = 0; x < w; ++x) EXPECT_EQ(a.tone(x, 128), b.tone(x, 128));
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

TEST(ToneBake4, FlatInputsPinEndpointsAndMidtones) {
  // Flat 0/255 have zero quantization error: no diffusion, one tone.
  {
    ttfimg::ToneBake4 bake(8);
    ASSERT_TRUE(bake.ok());
    for (int x = 0; x < 8; ++x) EXPECT_EQ(bake.tone(x, 0), 0);
    bake.nextRow();
    for (int x = 0; x < 8; ++x) EXPECT_EQ(bake.tone(x, 0), 0);
  }
  {
    ttfimg::ToneBake4 bake(8);
    ASSERT_TRUE(bake.ok());
    for (int x = 0; x < 8; ++x) EXPECT_EQ(bake.tone(x, 255), 3);
    bake.nextRow();
    for (int x = 0; x < 8; ++x) EXPECT_EQ(bake.tone(x, 255), 3);
  }
  {
    // Flat mid-gray: gamma+contrast map 128 into tone 2's band (the mapped
    // endpoints clamp, so the row stays inside {2,3} — no pepper into 0/1).
    ttfimg::ToneBake4 bake(32);
    ASSERT_TRUE(bake.ok());
    for (int r = 0; r < 4; ++r) {
      for (int x = 0; x < 32; ++x) {
        const uint8_t t = bake.tone(x, 128);
        EXPECT_GE(t, 2);
        EXPECT_LE(t, 3);
      }
      bake.nextRow();
    }
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

TEST(ToneBake4, StepInputChangesToneAtTheBoundary) {
  // The sRGB-space thresholds moved with the LUTs (§5.2): sRGB 60 maps into
  // the light band (was tone 1 under the plain quantizer), sRGB 30 into the
  // dark band — pinned with margin against LUT-build drift.
  ttfimg::ToneBake4 bake(8);
  ASSERT_TRUE(bake.ok());
  EXPECT_EQ(bake.tone(0, 30), 1);
  EXPECT_EQ(bake.tone(1, 60), 2);
  EXPECT_EQ(bake.tone(2, 100), 2);
  // Deep shadow: the brightening curve parks sRGB 10 below tone 0's mapped
  // boundary (gamma⁻¹(43) ≈ sRGB 5..10).
  EXPECT_EQ(bake.tone(3, 10), 0);
}

TEST(ToneBake4, LutStructure) {
  // Structural pins (libm-independent): endpoints fixed, non-decreasing,
  // contrast pivot preserved, midtone brightened per the reference curve.
  ttfimg::ToneBake4 bake(1);
  EXPECT_EQ(bake.gammaLut[0], 0);
  EXPECT_EQ(bake.gammaLut[255], 255);
  EXPECT_EQ(bake.contrastLut[0], 0);
  EXPECT_EQ(bake.contrastLut[255], 255);
  for (int i = 1; i < 256; ++i) {
    EXPECT_GE(bake.gammaLut[i], bake.gammaLut[i - 1]);
    EXPECT_GE(bake.contrastLut[i], bake.contrastLut[i - 1]);
  }
  EXPECT_NEAR(bake.contrastLut[128], 128, 2);  // pivot 128 stays
  EXPECT_NEAR(bake.gammaLut[128], 186, 4);     // 0.502^(1/2.2) brightens
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
