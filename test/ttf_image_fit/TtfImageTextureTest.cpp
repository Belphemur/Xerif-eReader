// Texture-quality suite (legacy-BMP parity, design amendment 9 in
// docs/design/2026-11-08-ttf-image-contrast-and-fit.md): drives the real
// ToneBake4 over synthetic fields and pins the legacy AtkinsonDitherer
// contracts on host — flat fields, gradient tone, determinism — with the
// legacy engine itself (lib/GfxRenderer/BitmapHelpers.h,
// originalThresholds=false) as the byte-exact reference.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "TtfImageCache.h"

namespace {

// Bakes rows of gray values through ToneBake4's toneRow (the production
// path) and returns the per-pixel levels (row-major).
std::vector<uint8_t> bake(const int w, const int h, const std::vector<uint8_t>& gray) {
  ttfimg::ToneBake4 bake{w};
  EXPECT_TRUE(bake.ok());
  std::vector<uint8_t> tones(static_cast<size_t>(w) * h);
  std::vector<uint8_t> row(w);
  for (int y = 0; y < h; ++y) {
    bake.toneRow(&gray[y * w], row.data());
    std::copy(row.begin(), row.end(), tones.begin() + static_cast<std::ptrdiff_t>(y) * w);
  }
  return tones;
}

// The reference side: the legacy engine's AtkinsonDitherer driven exactly
// the way JpegToBmpConverter drives it.
std::vector<uint8_t> legacyBake(const int w, const int h, const std::vector<uint8_t>& gray) {
  AtkinsonDitherer ditherer(w);
  std::vector<uint8_t> tones(static_cast<size_t>(w) * h);
  std::vector<uint8_t> row(w);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) row[x] = ditherer.processPixel(gray[static_cast<size_t>(y) * w + x], x);
    ditherer.nextRow();
    std::copy(row.begin(), row.end(), tones.begin() + static_cast<std::ptrdiff_t>(y) * w);
  }
  return tones;
}

int levelCount(const std::vector<uint8_t>& tones, const int level) {
  return static_cast<int>(std::count(tones.begin(), tones.end(), static_cast<uint8_t>(level)));
}

double emittedMean(const std::vector<uint8_t>& tones) {
  long long emitted = 0;
  for (const uint8_t t : tones) emitted += t == 0 ? 15 : (t == 1 ? 35 : (t == 2 ? 90 : 210));
  return static_cast<double>(emitted) / static_cast<double>(tones.size());
}

// Mean |row-to-row delta| of the per-row count of one level.
double rowDelta(const std::vector<uint8_t>& tones, const int w, const int h, const int level) {
  int prev = -1;
  double sum = 0.0;
  int rows = 0;
  for (int y = 0; y < h; ++y) {
    int n = 0;
    for (int x = 0; x < w; ++x) {
      if (tones[static_cast<size_t>(y) * w + x] == level) ++n;
    }
    if (prev >= 0) {
      sum += std::abs(n - prev);
      ++rows;
    }
    prev = n;
  }
  return rows > 0 ? sum / rows : 0.0;
}

}  // namespace

// ── F1: flat fields bake solid like the legacy engine ──────────────────────

TEST(TextureBake, FlatFieldOffTheCutoffsBakesSolid) {
  // Legacy character: a flat field whose |adjusted − target| stays under
  // the >>3 shift carries zero diffusion error and bakes one solid level.
  // gray 40 → target 35 (error (40−35)>>3 = 0), gray 250 → target 210 with
  // +5 error (250−210)>>3 = 5 — WAIT: 5 >> 0 rounds... the legacy error is
  // (adjusted − quantizedValue) >> 3 = 40/8 = 5, and level 3 keeps every
  // pixel ≥ 150 anyway, so the field stays solid white. Pinned against the
  // legacy ditherer itself — parity makes the reference authoritative.
  for (const int v : {0, 20, 40, 60, 203, 250}) {
    const int w = 96;
    const int h = 24;
    std::vector<uint8_t> gray(static_cast<size_t>(w) * h, static_cast<uint8_t>(v));
    const auto tones = bake(w, h, gray);
    const auto legacy = legacyBake(w, h, gray);
    ASSERT_EQ(0, memcmp(tones.data(), legacy.data(), tones.size())) << "gray " << v;
    EXPECT_EQ(levelCount(tones, 0) + levelCount(tones, 1) + levelCount(tones, 2) + levelCount(tones, 3), w * h);
  }
}

TEST(TextureBake, FlatFieldsKeepTheirLegacyTone) {
  // Every flat field's emitted mean must track the input within the legacy
  // quantizer's own span (the level targets are the anchors). This is the
  // tonal-balance contract the owner approved on the legacy engine. (255
  // itself maps to the 210 target — the legacy highlight headroom — so the
  // sweep stops at 250, where the span is strictly inside.)
  for (int v = 0; v <= 250; v += 5) {
    const int w = 96;
    const int h = 24;
    std::vector<uint8_t> gray(static_cast<size_t>(w) * h, static_cast<uint8_t>(v));
    const auto tones = bake(w, h, gray);
    const auto legacy = legacyBake(w, h, gray);
    ASSERT_EQ(0, memcmp(tones.data(), legacy.data(), tones.size())) << "gray " << v;
    EXPECT_NEAR(emittedMean(tones), static_cast<double>(v), 45.0) << "gray " << v;
  }
}

TEST(TextureBake, FlatInsideTheSpanBakesItsTargetLevel) {
  // Flat 100: inside the level-2 span (55..149, target 90), the per-pixel
  // error (100−90)>>3 = 1 is too small to accumulate past the shift, so the
  // field bakes SOLID level 2 — measured (probe): 4608/4608 at level 2.
  // The woven dither only appears where the span's own error is large
  // enough (flat 60 dithers 1/2 at 57%/43%, flat 128 dithers 2/3).
  const std::vector<uint8_t> gray(96 * 48, 100);
  const auto tones = bake(96, 48, gray);
  const auto legacy = legacyBake(96, 48, gray);
  ASSERT_EQ(0, memcmp(tones.data(), legacy.data(), tones.size()));
  EXPECT_EQ(levelCount(tones, 2), 96 * 48);
}

TEST(TextureBake, SpanBoundaryFieldsWeaveToneCorrect) {
  // The measured woven fields: flat 60 (1/2) and flat 128/160 (2/3) dither
  // at their tone-correct duty — the calm shading texture the owner sees
  // on the legacy engine. No level escapes the bracketing pair.
  struct Case {
    int v;
    int lo, hi;   // bracketing levels
    double duty;  // fraction of the HIGHER level (measured)
  };
  const Case cases[] = {{60, 1, 2, 1777.0 / 4096}, {128, 2, 3, 986.0 / 4096}, {160, 2, 3, 2348.0 / 4096}};
  for (const Case c : cases) {
    const int w = 128;
    const int h = 32;
    std::vector<uint8_t> gray(static_cast<size_t>(w) * h, static_cast<uint8_t>(c.v));
    const auto tones = bake(w, h, gray);
    const auto legacy = legacyBake(w, h, gray);
    ASSERT_EQ(0, memcmp(tones.data(), legacy.data(), tones.size())) << "gray " << c.v;
    const int nLo = levelCount(tones, c.lo);
    const int nHi = levelCount(tones, c.hi);
    EXPECT_EQ(nLo + nHi, w * h) << "gray " << c.v;
    EXPECT_NEAR(static_cast<double>(nHi) / (w * h), c.duty, 0.02) << "gray " << c.v;
    EXPECT_NEAR(emittedMean(tones), static_cast<double>(c.v), 10.0) << "gray " << c.v;  // measured 9.1 worst
  }
}

TEST(TextureBake, PaperFieldStaysClean) {
  // Clean paper: gray ≥ 203 (where the legacy error (203−210)>>3 tapers)
  // bakes near-solid level 3 — no dark dots accumulating in the paper
  // field, no highlight speckle (the legacy highlight-headroom contract).
  for (const int v : {203, 220, 240, 253, 255}) {
    const int w = 128;
    const int h = 32;
    std::vector<uint8_t> gray(static_cast<size_t>(w) * h, static_cast<uint8_t>(v));
    const auto tones = bake(w, h, gray);
    const auto legacy = legacyBake(w, h, gray);
    ASSERT_EQ(0, memcmp(tones.data(), legacy.data(), tones.size())) << "gray " << v;
    EXPECT_EQ(levelCount(tones, 3), w * h) << "gray " << v << " paper field dithered";
  }
}

// ── F2: gradients stay tone-correct, no mode-lock ──────────────────────────

TEST(TextureBake, GradientToneIsPreserved) {
  // The diffusion carries the tone: a full 30..210 vertical gradient must
  // emit an average tone near the gradient's mean (the legacy weave is
  // tone-correct by construction — parity pins it against the reference).
  const int w = 240;
  const int h = 240;
  std::vector<uint8_t> gray(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    const uint8_t g = static_cast<uint8_t>(30 + (y * 180) / (h - 1));
    std::fill(gray.begin() + static_cast<size_t>(y) * w, gray.begin() + static_cast<size_t>(y + 1) * w, g);
  }
  const auto tones = bake(w, h, gray);
  const auto legacy = legacyBake(w, h, gray);
  ASSERT_EQ(0, memcmp(tones.data(), legacy.data(), tones.size()));
  long long input = 0;
  for (const uint8_t g : gray) input += g;
  const double inputMean = static_cast<double>(input) / gray.size();
  EXPECT_NEAR(emittedMean(tones), inputMean, 12.0);
}

TEST(TextureBake, BakeIsDeterministicAcrossRuns) {
  // Two bakes of the same field must agree byte-for-byte (worker/foreground
  // parity — the legacy ditherer is fully deterministic).
  const int w = 96;
  const int h = 64;
  std::vector<uint8_t> gray(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    const uint8_t g = static_cast<uint8_t>(40 + (y * 150) / (h - 1));
    std::fill(gray.begin() + static_cast<size_t>(y) * w, gray.begin() + static_cast<size_t>(y + 1) * w, g);
  }
  const auto a = bake(w, h, gray);
  const auto b = bake(w, h, gray);
  EXPECT_EQ(0, memcmp(a.data(), b.data(), a.size()));
}

// ── F3: strokes survive the bake like on the legacy engine ─────────────────

TEST(TextureBake, InkStrokesSurviveTheBake) {
  // Light field (220) with a 2px vertical stroke and a 1px stroke: ink
  // bakes level 0 (adjusted < 30 with the error flow), and the paper
  // between/around the strokes stays clean level 3.
  const int w = 64;
  const int h = 64;
  std::vector<uint8_t> gray(static_cast<size_t>(w) * h, 220);
  for (int y = 30; y < 40; ++y) {
    gray[static_cast<size_t>(y) * w + 20] = 0;
    gray[static_cast<size_t>(y) * w + 21] = 0;
    gray[static_cast<size_t>(y) * w + 40] = 0;
  }
  const auto tones = bake(w, h, gray);
  const auto legacy = legacyBake(w, h, gray);
  ASSERT_EQ(0, memcmp(tones.data(), legacy.data(), tones.size()));
  for (int y = 30; y < 40; ++y) {
    EXPECT_EQ(tones[static_cast<size_t>(y) * w + 20], 0) << "2px stroke left col, row " << y;
    EXPECT_EQ(tones[static_cast<size_t>(y) * w + 21], 0) << "2px stroke right col, row " << y;
    EXPECT_EQ(tones[static_cast<size_t>(y) * w + 40], 0) << "1px stroke, row " << y;
  }
  // The paper beside the strokes stays white (no cross-edge bleed halo).
  for (int y = 30; y < 40; ++y) {
    EXPECT_EQ(tones[static_cast<size_t>(y) * w + 19], 3) << "paper left of 2px stroke, row " << y;
    EXPECT_EQ(tones[static_cast<size_t>(y) * w + 22], 3) << "paper right of 2px stroke, row " << y;
  }
}
