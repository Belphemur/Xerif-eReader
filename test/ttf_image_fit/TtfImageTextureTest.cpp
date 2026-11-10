// Texture-quality suite (design docs/design/2026-11-08-ttf-image-texture-quality.md
// §2.2–§2.4): drives the real ToneBake4 over synthetic fields and pins the
// flat-field, banding and stroke-retention contracts on host. Promoted from the
// verification campaign's scratch probes (/tmp/texture_probe*.cpp).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "TtfImageCache.h"

namespace {

// Bake rows of gray values through the full two-phase adaptive bake and
// return the per-pixel levels (row-major).
std::vector<uint8_t> bake(const int w, const int h, const std::vector<uint8_t>& gray, const char* tempPath) {
  ttfimg::ToneBake4 bake{w};
  EXPECT_TRUE(bake.ok());
  if (bake.beginCollect(tempPath)) {
    for (int y = 0; y < h; ++y) bake.collectRow(&gray[y * w]);
  }
  EXPECT_TRUE(bake.grayDone());
  bake.beginRemap();
  std::vector<uint8_t> tones(static_cast<size_t>(w) * h);
  std::vector<uint8_t> row(w);
  for (int y = 0; y < h; ++y) {
    bake.toneRow(&gray[y * w], row.data());
    std::copy(row.begin(), row.end(), tones.begin() + static_cast<size_t>(y) * w);
  }
  return tones;
}

int levelCount(const std::vector<uint8_t>& tones, const int level) {
  return static_cast<int>(std::count(tones.begin(), tones.end(), static_cast<uint8_t>(level)));
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

// ── F1: flat-field preservation (texture §2.2) ──────────────────────────────

TEST(TextureBake, FlatNearLevelBakesOneUniformLevel) {
  // Flat 100 maps to 91 (fixed-contrast degenerate map) — within ±kSnap of
  // level 85: the whole field must bake as that single level, no dither.
  const std::vector<uint8_t> gray(96 * 48, 100);
  const auto tones = bake(96, 48, gray, "/t/texture_flat100.g");
  EXPECT_EQ(levelCount(tones, 1), 96 * 48);
  EXPECT_EQ(levelCount(tones, 0), 0);
  EXPECT_EQ(levelCount(tones, 2), 0);
  EXPECT_EQ(levelCount(tones, 3), 0);
}

TEST(TextureBake, FlatDarkFieldStaysDominantlyBlack) {
  // Flat 40 maps to 13 — level 0 dominates. The reset4 exact-residue plane
  // conserves mass, so the flat-field FS equilibrium is the TONE-CORRECT
  // dropout fraction (mapped 13 → 13/85 ≈ 10% level-1 specks; measured
  // 4146/4608 = 90.02% level 0) instead of the old 0.8-truncation regime's
  // 8%, which under-diffused and quietly darkened the field. The pins that
  // still matter: no mid levels, no white, and the field keeps level-0
  // dominance with a Floor, not an exact-8% banding contract.
  const std::vector<uint8_t> gray(96 * 48, 40);
  const auto tones = bake(96, 48, gray, "/t/texture_flat40.g");
  const int l0 = levelCount(tones, 0);
  EXPECT_GE(l0, 85 * 96 * 48 / 100);  // ≥85% level 0 (tone-correct ≈90%)
  EXPECT_EQ(levelCount(tones, 1), 96 * 48 - l0);
  EXPECT_EQ(levelCount(tones, 2), 0);
  EXPECT_EQ(levelCount(tones, 3), 0);
}

TEST(TextureBake, FlatMidtoneBetweenLevelsStaysToneCorrect) {
  // Flat 127 sits mid-way between levels 85 and 170 — no single level is
  // within ±kSnap, so the field dithers (the documented 4-level physical
  // limit). The contract that still holds: the mix stays tone-correct and
  // row-uniform (no banding).
  const std::vector<uint8_t> gray(48 * 24, 127);
  const auto tones = bake(48, 24, gray, "/t/texture_flat127.g");
  const int l1 = levelCount(tones, 1);
  const int l2 = levelCount(tones, 2);
  EXPECT_EQ(l1 + l2, 48 * 24);      // nothing escapes the bracketing pair
  EXPECT_GE(l1, 4 * 48 * 24 / 10);  // each side ≥ 40%: tone-correct mix
  EXPECT_GE(l2, 4 * 48 * 24 / 10);
  EXPECT_LT(rowDelta(tones, 48, 24, 1), 4.0);
}

// ── F2: band-free gradients (texture §2.3) ──────────────────────────────────

TEST(TextureBake, GradientHasNoRowPeriodicBanding) {
  // Full vertical gradient 30→210 at 240×240. The pre-fix bake mode-locked
  // into period-2 rows (mean |row-to-row L1 delta| 16.6 vs ideal ~2.0); the
  // fixed bake must stay near the ideal gradient step.
  const int w = 240;
  const int h = 240;
  std::vector<uint8_t> gray(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    const uint8_t g = static_cast<uint8_t>(30 + (y * 180) / (h - 1));
    std::fill(gray.begin() + static_cast<size_t>(y) * w, gray.begin() + static_cast<size_t>(y + 1) * w, g);
  }
  const auto tones = bake(w, h, gray, "/t/texture_gradient.g");
  EXPECT_LT(rowDelta(tones, w, h, 1), 6.0);
  EXPECT_LT(rowDelta(tones, w, h, 2), 6.0);
}

TEST(TextureBake, GradientHasNoSustainedDensityOscillation) {
  // Same gradient: the pre-fix defect was a sustained period-2 swing
  // (transition rows alternating ~0/79 level-2 dots). Single plateau-edge
  // steps are the designed posterized-transition behavior; repeated
  // opposite-sign swings of shaded-shelf magnitude are the defect.
  const int w = 240;
  const int h = 240;
  std::vector<uint8_t> gray(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    const uint8_t g = static_cast<uint8_t>(30 + (y * 180) / (h - 1));
    std::fill(gray.begin() + static_cast<size_t>(y) * w, gray.begin() + static_cast<size_t>(y + 1) * w, g);
  }
  const auto tones = bake(w, h, gray, "/t/texture_gradient_mono.g");
  int prev = -1;
  int prevDelta = 0;
  int oscillations = 0;
  for (int y = 0; y < h; ++y) {
    int n = 0;
    for (int x = 0; x < w; ++x) {
      if (tones[static_cast<size_t>(y) * w + x] == 1) ++n;
    }
    if (prev >= 0) {
      const int d = n - prev;
      if (std::abs(d) >= 32 && std::abs(prevDelta) >= 32 && (d > 0) != (prevDelta > 0)) {
        ++oscillations;
        EXPECT_LE(oscillations, 2) << "sustained oscillation at row " << y;
      }
      prevDelta = d;
    }
    prev = n;
  }
}

TEST(TextureBake, GradientToneIsPreservedByTheThresholdNoise) {
  // The pseudo-random threshold offset shifts decisions but never enters the
  // error bookkeeping, so the emitted average tone must track the mapped
  // average closely.
  const int w = 240;
  const int h = 240;
  std::vector<uint8_t> gray(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    const uint8_t g = static_cast<uint8_t>(30 + (y * 180) / (h - 1));
    std::fill(gray.begin() + static_cast<size_t>(y) * w, gray.begin() + static_cast<size_t>(y + 1) * w, g);
  }
  const auto tones = bake(w, h, gray, "/t/texture_gradient_tone.g");
  long long emitted = 0;
  for (const uint8_t t : tones) emitted += t * 85;
  const double avgEmitted = static_cast<double>(emitted) / tones.size();
  // Average mapped value of the gradient (30..210 stretched): ~mid 120s.
  EXPECT_NEAR(avgEmitted, 127.5, 6.0);
}

TEST(TextureBake, BakeIsDeterministicAcrossRuns) {
  // The (x, row) threshold noise is a pure function — two bakes of the same
  // field must agree byte-for-byte (worker/foreground parity).
  const int w = 96;
  const int h = 64;
  std::vector<uint8_t> gray(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    const uint8_t g = static_cast<uint8_t>(40 + (y * 150) / (h - 1));
    std::fill(gray.begin() + static_cast<size_t>(y) * w, gray.begin() + static_cast<size_t>(y + 1) * w, g);
  }
  const auto a = bake(w, h, gray, "/t/texture_det_a.g");
  const auto b = bake(w, h, gray, "/t/texture_det_b.g");
  EXPECT_EQ(0, memcmp(a.data(), b.data(), a.size()));
}

// ── F3: stroke retention (texture §2.4) ─────────────────────────────────────

TEST(TextureBake, ThinStrokesSurviveTheBake) {
  // Light field (220) with a 2px vertical stroke (x 20–21) and a 1px vertical
  // stroke (x 40), rows 30–39: every stroke pixel must bake level 0, and the
  // background between the strokes must stay bright (no anchor halo).
  const int w = 64;
  const int h = 64;
  std::vector<uint8_t> gray(static_cast<size_t>(w) * h, 220);
  for (int y = 30; y < 40; ++y) {
    gray[static_cast<size_t>(y) * w + 20] = 0;
    gray[static_cast<size_t>(y) * w + 21] = 0;
    gray[static_cast<size_t>(y) * w + 40] = 0;
  }
  const auto tones = bake(w, h, gray, "/t/texture_strokes.g");
  for (int y = 30; y < 40; ++y) {
    EXPECT_EQ(tones[static_cast<size_t>(y) * w + 20], 0) << "2px stroke left col, row " << y;
    EXPECT_EQ(tones[static_cast<size_t>(y) * w + 21], 0) << "2px stroke right col, row " << y;
    EXPECT_EQ(tones[static_cast<size_t>(y) * w + 40], 0) << "1px stroke, row " << y;
  }
  // No anchor halo: the columns beside the strokes stay bright levels.
  for (int y = 30; y < 40; ++y) {
    EXPECT_NE(tones[static_cast<size_t>(y) * w + 19], 0) << "halo left of 2px stroke, row " << y;
    EXPECT_NE(tones[static_cast<size_t>(y) * w + 22], 0) << "halo right of 2px stroke, row " << y;
    EXPECT_NE(tones[static_cast<size_t>(y) * w + 39], 0) << "halo left of 1px stroke, row " << y;
    EXPECT_NE(tones[static_cast<size_t>(y) * w + 41], 0) << "halo right of 1px stroke, row " << y;
  }
}
