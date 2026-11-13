// Texture-quality suite (linear-light blue-noise bake, design amendment 10
// in docs/design/2026-11-08-ttf-image-contrast-and-fit.md): drives the real
// ToneBake4 over synthetic fields and pins the ordered-dither contracts on
// host — solid palette anchors, exact linear-light duties, tone-correct
// gradients, determinism. The blue-noise bake is stateless: duties come from
// the LUT itself (thresh(g)/256 per full 64x64 tile), and the LUT's
// bit-exactness against the float reference is pinned in the engine suite
// (LinearLutBitExactAgainstFloatReference).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "BlueNoise64.h"
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

// The panel's 4 nominal levels in linear light (255·(enc/255)^2.2).
double linearLevel(const int level) {
  const double enc[4] = {0.0, 85.0, 170.0, 255.0};
  return 255.0 * std::pow(enc[level] / 255.0, 2.2);
}

int levelCount(const std::vector<uint8_t>& tones, const int level) {
  return static_cast<int>(std::count(tones.begin(), tones.end(), static_cast<uint8_t>(level)));
}

double emittedLinearMean(const std::vector<uint8_t>& tones) {
  double sum = 0.0;
  for (const uint8_t t : tones) sum += linearLevel(t);
  return sum / static_cast<double>(tones.size());
}

}  // namespace

// ── F1: flat fields — solid anchors, exact duties ──────────────────────────

TEST(TextureBake, PaletteAnchorFieldsBakeSolid) {
  // Ordered-dither character: the four palette anchors {0, 85, 170, 255}
  // (the levels the panel actually renders) bake one solid level with zero
  // speckle; every other flat field dithers its bracketing pair.
  for (const int v : {0, 85, 170, 255}) {
    const int w = 96;
    const int h = 24;
    std::vector<uint8_t> gray(static_cast<size_t>(w) * h, static_cast<uint8_t>(v));
    const auto tones = bake(w, h, gray);
    EXPECT_EQ(levelCount(tones, v / 85), w * h) << "anchor " << v << " dithered";
  }
}

TEST(TextureBake, FlatFieldsKeepTheirLinearReflectance) {
  // Tone-correctness in linear light: every flat field's emitted mean
  // reflectance tracks (g/255)^2.2 within a threshold-quantization step
  // (duty is exact per full tile; the residual is the ceil() of the
  // fractional position).
  for (int v = 0; v <= 255; v += 5) {
    const int w = 128;
    const int h = 64;
    std::vector<uint8_t> gray(static_cast<size_t>(w) * h, static_cast<uint8_t>(v));
    const auto tones = bake(w, h, gray);
    const double mean = emittedLinearMean(tones);
    const double target = 255.0 * std::pow(v / 255.0, 2.2);
    EXPECT_NEAR(mean, target, 1.1) << "gray " << v << " mean " << mean;
  }
}

TEST(TextureBake, FlatFieldDithersItsExactDuty) {
  // The ordered duty is thresh(g)/256 per aligned full 64x64 tile — pin the
  // exact level counts for a representative mid-span field (128: 41% of
  // level 3 over 2 aligned tiles) and a dark one (60: 46.9% of level 1).
  for (const int v : {60, 100, 128, 160, 203}) {
    const uint16_t entry = ttfimg::kBlueNoiseLinearLut[v];
    const int level = (entry >> 9) + 1;  // the higher level of the pair
    const int thresh = entry & 0x1FF;
    const int w = 128;
    const int h = 64;  // 2 aligned tiles
    std::vector<uint8_t> gray(static_cast<size_t>(w) * h, static_cast<uint8_t>(v));
    const auto tones = bake(w, h, gray);
    const int nHigh = levelCount(tones, level);
    EXPECT_EQ(nHigh, thresh * 32) << "gray " << v << " duty count (thresh " << thresh << ")";
  }
}

// ── F2: gradients stay tone-correct, no mode-lock ──────────────────────────

TEST(TextureBake, GradientToneIsPreserved) {
  // The ordered threshold carries the tone in linear light: a full
  // 30..210 vertical gradient must emit an average linear reflectance near
  // the gradient's own mean — and all four levels must appear (no level
  // mode-locks the page).
  const int w = 240;
  const int h = 240;
  std::vector<uint8_t> gray(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    const uint8_t g = static_cast<uint8_t>(30 + (y * 180) / (h - 1));
    std::fill(gray.begin() + static_cast<size_t>(y) * w, gray.begin() + static_cast<size_t>(y + 1) * w, g);
  }
  const auto tones = bake(w, h, gray);
  double inputLinear = 0.0;
  for (const uint8_t g : gray) inputLinear += 255.0 * std::pow(g / 255.0, 2.2);
  const double inputMean = inputLinear / gray.size();
  EXPECT_NEAR(emittedLinearMean(tones), inputMean, 3.0);
  for (int t = 0; t < 4; ++t) EXPECT_GT(levelCount(tones, t), 0) << "tone " << t << " never emitted";
}

TEST(TextureBake, BakeIsDeterministicAcrossRuns) {
  // Two bakes of the same field must agree byte-for-byte (worker/foreground
  // parity — the ordered bake is stateless and fully deterministic).
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

// ── F3: strokes survive the bake like the legacy engine's do ───────────────

TEST(TextureBake, InkStrokesSurviveTheBake) {
  // Ordered dithering has no diffusion to bleed: ink (0, a palette anchor)
  // bakes solid level 0, and no halo forms around the strokes — the paper
  // (220) dithers its own linear-light duty exactly as it would without
  // the strokes nearby.
  const int w = 64;
  const int h = 64;
  std::vector<uint8_t> gray(static_cast<size_t>(w) * h, 220);
  for (int y = 30; y < 40; ++y) {
    gray[static_cast<size_t>(y) * w + 20] = 0;
    gray[static_cast<size_t>(y) * w + 21] = 0;
    gray[static_cast<size_t>(y) * w + 40] = 0;
  }
  const auto tones = bake(w, h, gray);
  for (int y = 30; y < 40; ++y) {
    EXPECT_EQ(tones[static_cast<size_t>(y) * w + 20], 0) << "2px stroke left col, row " << y;
    EXPECT_EQ(tones[static_cast<size_t>(y) * w + 21], 0) << "2px stroke right col, row " << y;
    EXPECT_EQ(tones[static_cast<size_t>(y) * w + 40], 0) << "1px stroke, row " << y;
  }
  // Reference: the same field WITHOUT the strokes (stateless bake — the
  // paper pixels must be bit-identical with and around the strokes).
  std::vector<uint8_t> plain(static_cast<size_t>(w) * h, 220);
  const auto plainTones = bake(w, h, plain);
  for (int y = 30; y < 40; ++y) {
    EXPECT_EQ(tones[static_cast<size_t>(y) * w + 19], plainTones[static_cast<size_t>(y) * w + 19])
        << "paper pixel changed near a stroke, row " << y;
    EXPECT_EQ(tones[static_cast<size_t>(y) * w + 22], plainTones[static_cast<size_t>(y) * w + 22])
        << "paper pixel changed near a stroke, row " << y;
  }
}
