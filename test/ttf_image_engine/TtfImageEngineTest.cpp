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
  // 'c' = Sharp, 'd' = DPID + CLAHE (Enhanced), 'l' = legacy: the
  // edge-aware hybrid bake (amendment 7, 2026-11-11) invalidated every
  // prior bake — gradient-gated 8/8 → 7/8 → 6/8 diffusion, the
  // kEdgeBarrier cross-cliff guard, smooth-region threshold dither,
  // prevRowMv context, static LUTs.
  EXPECT_NE(strstr(a, "/ic"), nullptr);
  EXPECT_NE(strstr(b, "/il"), nullptr);
  EXPECT_NE(strstr(c, "/id"), nullptr);
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

static int64_t planeMass(const ttfimg::ToneBake4& bake) {
  const int n = bake.width + 4;
  int64_t m = 0;
  for (int i = 0; i < n; ++i) m += bake.err[i] + bake.errNext[i] + bake.errNext2[i];
  return m;
}

TEST(ToneBake4, SmoothPixelsDiffuseTheFullError) {
  // Edge-aware hybrid (amendment 7): a smooth pixel (single-pixel tone()
  // = no row context, every gradient term 0) diffuses 8/8 — mass-conserving
  // — so subtle ramps reach the light levels instead of parking in the
  // pure-Atkinson dead zone. A fresh slot carries no residue (rem8 = 0).
  ttfimg::ToneBake4 bake(8);
  ASSERT_TRUE(bake.ok());
  const int64_t m0 = planeMass(bake);
  const int mv = bake.mapv(127);
  const int v = mv;  // fresh slot: no consumed error
  int t = 3;
  if (v < ttfimg::ToneBake4::kMids[0])
    t = 0;
  else if (v < ttfimg::ToneBake4::kMids[1])
    t = 1;
  else if (v < ttfimg::ToneBake4::kMids[2])
    t = 2;
  const int errVal = v - ttfimg::ToneBake4::kLevels[t];
  bake.tone(0, 127);
  EXPECT_EQ(planeMass(bake) - m0, 8 * errVal);
  // The smooth kernel (ascending visit: xi = x+2 = 2): 2/8 forward
  // (err[3]), 1/8 two-ahead (err[4]), 1/8 below-back (errNext[1]), 2/8
  // directly below (errNext[2]), 1/8 below-ahead (errNext[3]), 1/8 two
  // rows down below (errNext2[2]).
  EXPECT_EQ(bake.err[3], 2 * errVal);
  EXPECT_EQ(bake.err[4], errVal);
  EXPECT_EQ(bake.errNext[1], errVal);
  EXPECT_EQ(bake.errNext[2], 2 * errVal);
  EXPECT_EQ(bake.errNext[3], errVal);
  EXPECT_EQ(bake.errNext2[2], errVal);
}

TEST(ToneBake4, SharpPixelsKeepAtkinsonDamping) {
  // Edge-aware hybrid: a sharp pixel damps back to the 6/8 Atkinson spread
  // (six neighbors at 1/8, 2/8 absorbed). Simulate a stroke-adjacent pixel
  // by seeding prevRowMv far from the pixel's own value: a real sharp
  // edge, no row context needed for the horizontal terms.
  ttfimg::ToneBake4 bake(8);
  ASSERT_TRUE(bake.ok());
  bake.hasPrevRow = true;
  bake.prevRowMv[0] = 0;  // dyUp = mapv(127) - 0: sharp
  const int64_t m0 = planeMass(bake);
  const int mv = bake.mapv(127);
  const int v = mv;
  int t = 3;
  if (v < ttfimg::ToneBake4::kMids[0])
    t = 0;
  else if (v < ttfimg::ToneBake4::kMids[1])
    t = 1;
  else if (v < ttfimg::ToneBake4::kMids[2])
    t = 2;
  const int errVal = v - ttfimg::ToneBake4::kLevels[t];
  bake.tone(0, 127);
  EXPECT_EQ(planeMass(bake) - m0, 6 * errVal);
  EXPECT_EQ(bake.err[3], errVal);
  EXPECT_EQ(bake.err[4], errVal);
  EXPECT_EQ(bake.errNext[1], errVal);
  EXPECT_EQ(bake.errNext[2], errVal);
  EXPECT_EQ(bake.errNext[3], errVal);
  EXPECT_EQ(bake.errNext2[2], errVal);
}

TEST(ToneBake4, ConsumedErrorFeedsTheQuantizerInput) {
  // The consumed slot error joins the mapped value BEFORE quantization:
  // seed e8 = 37 eighth-levels -> consumed e = 5 (round (37+4)>>3); the
  // rounding residual is absorbed with the plane's 2/8 (the consume zeroes
  // the slot); the quantizer sees mv + 5.
  ttfimg::ToneBake4 bake(8);
  ASSERT_TRUE(bake.ok());
  bake.err[2] = 37;
  const int64_t m0 = planeMass(bake);
  const int mv = bake.mapv(127);
  const int v = mv + 5;
  int t = 3;
  if (v < ttfimg::ToneBake4::kMids[0])
    t = 0;
  else if (v < ttfimg::ToneBake4::kMids[1])
    t = 1;
  else if (v < ttfimg::ToneBake4::kMids[2])
    t = 2;
  const int errVal = v - ttfimg::ToneBake4::kLevels[t];
  bake.tone(0, 127);
  EXPECT_EQ(bake.err[2], 0);  // fully consumed
  // Smooth kernel: the consume residue (rem8 = 37 − 40 = −3) rides the
  // 2/8 forward tap, rounding the plane back to exact mass conservation.
  EXPECT_EQ(bake.err[3], 2 * errVal - 3);
  EXPECT_EQ(bake.err[4], errVal);
  EXPECT_EQ(bake.errNext[1], errVal);
  EXPECT_EQ(bake.errNext[2], 2 * errVal);
  EXPECT_EQ(bake.errNext[3], errVal);
  EXPECT_EQ(bake.errNext2[2], errVal);
  // Mass: 8·errVal + rem8 distributed eighth-units minus the 37 consumed.
  EXPECT_EQ(planeMass(bake) - m0, 8 * errVal - 3 - 37);
}

TEST(ToneBake4, WhitePaperSnapsClean) {
  // Mapped values >= 240 (the KCC-standard clip, iteration-3 audit) emit
  // level 3 with the slot zeroed and no diffusion: the paper field stays
  // clean and carries no error — 8×8 DCT ringing (240..251) can no longer
  // accumulate into dark dots.
  ttfimg::ToneBake4 bake(8);
  ASSERT_TRUE(bake.ok());
  bake.err[2] = 37;  // pending error must be dropped by the snap
  const int64_t m0 = planeMass(bake);
  const uint8_t t = bake.tone(0, 255);
  EXPECT_EQ(t, 3);
  EXPECT_EQ(bake.err[2], 0);
  EXPECT_EQ(planeMass(bake) - m0, -37);  // slot zeroed, nothing diffused
}

TEST(ToneBake4, SerpentineMirrorsTheKernel) {
  // toneRow drives the scan direction itself: a mirror-symmetric row baked
  // ascending (row 0) equals the same row baked on a reverse-seeded bake —
  // the FS kernel mirrors with the visit direction, so the horizontal error
  // term always lands on the next-visited pixel. Row 0 only (hasPrevRow
  // false ⇒ dyUp = 0) and noiseAmp = 0: the threshold dither keys on the
  // absolute column, which is not mirror-symmetric — with it the two runs
  // legitimately differ.
  const int w = 16;
  uint8_t gray[w];
  for (int i = 0; i < w; ++i) gray[i] = static_cast<uint8_t>(60 + (i < w - 1 - i ? i : w - 1 - i) * 15);
  // Symmetric sharp cliffs (mapped |105→12| ≥ kEdgeBarrier): the mirrored
  // kernel AND the cross-cliff barrier must mirror together.
  gray[3] = gray[12] = 12;
  gray[4] = gray[11] = 12;
  ttfimg::ToneBake4 forward(w);
  ttfimg::ToneBake4 backward(w);
  ASSERT_TRUE(forward.ok() && backward.ok());
  forward.noiseAmp = 0;
  backward.noiseAmp = 0;
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

TEST(ToneBake4, SmallRangeHistUsesFixedContrastFallback) {
  // A two-level near-flat histogram (99% level 236, speckle at 234) would
  // stretch range 2 to the full scale, baking the flat field near-black —
  // the small-range guard routes it to the fixed-contrast map (review
  // 2026-11-10). compare against the degenerate (contrast-only) map.
  ttfimg::ToneBake4 bake(16);
  ASSERT_TRUE(bake.ok());
  bake.hist[234] = 10;
  bake.hist[236] = 990;
  bake.histCount = 1000;
  bake.beginRemap();
  ttfimg::ToneBake4 ref(16);
  ASSERT_TRUE(ref.ok());
  ref.beginLegacyFallback();  // fixed contrast 1.3, gamma off
  for (int i = 0; i < 256; ++i) ASSERT_EQ(bake.mapLut[i], ref.mapLut[i]) << "level " << i;
}

TEST(ToneBake4, PaperGuardKeepsTheDarkAnchorOnWhiteHeavyPages) {
  // Owner directive (fidelity campaign 2026-11-10), implemented verbatim:
  // when non-paper pixels are >= 5% of the histogram, the paper bins
  // (250..255) are ignored when locating the dark anchor.
  //
  // MEASURED SCOPE (verified against the loop order, kept as documentation):
  // the ascending lo loop stops at the FIRST bin where cum >= 0.5%, so with
  // >= 5% non-paper pixels the unguarded anchor already lands on content
  // (0..249) — the guard's outcome is identical there. The anchor only
  // lands inside the paper bins when content < 0.5%, where the guard does
  // NOT fire (5% not met) — that case keeps the pre-campaign behavior
  // (lo in the paper bins, the stretch inverts paper) and is REPORTED as
  // residual risk; the white-page cleanliness this campaign ships rides on
  // the white-paper snap (kWhiteSnap) instead.
  //
  // Pin 1 — guard fires (content 6%), anchor unchanged vs unguarded:
  auto build = [](uint32_t dark, uint32_t paper) {
    ttfimg::ToneBake4 b(16);
    b.hist[10] = dark;
    b.hist[253] = paper;
    b.hist[255] = 1000 - dark - paper;
    b.histCount = 1000;
    b.beginRemap();
    return b;
  };
  // Pin 1 — guard fires (content 6% >= 5%): the anchor sits on the dark
  // content; paper maps back to paper.
  const auto guarded = build(60, 800);  // 6% content: guard fires
  EXPECT_EQ(guarded.mapLut[10], 0);     // the dark point stretches to black
  EXPECT_GT(guarded.mapLut[253], 200);  // paper stays paper

  // Pin 2 — content 0.4% (guard inert by the directive's own threshold):
  // pre-campaign behavior stands (anchor in the paper bins; the stretch
  // runs because the paper's own bin spread gives range 5). Documented
  // residual risk, unchanged by this campaign.
  ttfimg::ToneBake4 sparse(16);
  sparse.hist[10] = 4;
  sparse.hist[250] = 166;
  sparse.hist[251] = 166;
  sparse.hist[252] = 166;
  sparse.hist[253] = 166;
  sparse.hist[254] = 166;
  sparse.hist[255] = 166;
  sparse.histCount = 1000;
  sparse.beginRemap();
  EXPECT_EQ(sparse.mapLut[253], 153);  // (253−250)·255/5 — paper mid-stretch
  EXPECT_EQ(sparse.mapLut[10], 0);
}

TEST(ToneBake4, StretchStillAppliesToRealHistograms) {
  // The guard must not eat genuine range: a wide two-level histogram still
  // stretches to full scale (lo -> 0, hi -> 255).
  ttfimg::ToneBake4 bake(16);
  ASSERT_TRUE(bake.ok());
  bake.hist[40] = 512;
  bake.hist[255] = 512;
  bake.histCount = 1024;
  bake.beginRemap();
  EXPECT_EQ(bake.mapLut[40], 0);
  EXPECT_EQ(bake.mapLut[255], 255);
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
  // The sRGB-space thresholds moved with the LUTs (§5.2) and now with the
  // panel-calibrated midpoints {28, 78, 182}: sRGB 10 maps into the level-1
  // band (mapped 38, between 28 and 78), sRGB 30/60/100 into level 2 —
  // pinned with margin against LUT-build drift. Fresh bake per pick:
  // tone() diffuses, and a prior pick's error must not move a boundary pin.
  const auto pick = [](const uint8_t g) {
    ttfimg::ToneBake4 bake(8);
    return bake.tone(0, g);
  };
  EXPECT_EQ(pick(6), 0);   // mapped 21 < 28: level 0
  EXPECT_EQ(pick(10), 1);  // mapped 38: level 1
  EXPECT_EQ(pick(30), 2);  // mapped 86: level 2
  EXPECT_EQ(pick(60), 2);
  EXPECT_EQ(pick(100), 2);  // mapped 178, four under the 182 midpoint
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
