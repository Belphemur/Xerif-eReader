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

TEST(TextureBake, FlatNearLevelBakesToneCorrect) {
  // Flat 100 maps to 91 (fixed-contrast degenerate map) — nearest levels
  // are 48/112 under the panel-calibrated set: Atkinson dithers the pair at
  // the tone-correct duty (measured 72.5% level 2 vs the (91-48)/(112-48)
  // = 67% prediction — the 25% absorption shifts the measured duty a few
  // points; the dither IS the tone). No level 0/3 escapes the bracketing
  // pair.
  const std::vector<uint8_t> gray(96 * 48, 100);
  const auto tones = bake(96, 48, gray, "/t/texture_flat100.g");
  const int l1 = levelCount(tones, 1);
  const int l2 = levelCount(tones, 2);
  EXPECT_EQ(l1 + l2, 96 * 48);
  EXPECT_GT(l2 * 10, 6 * 96 * 48);  // dominant level 2 (measured 72.5%)
  EXPECT_LT(l1 * 10, 4 * 96 * 48);  // (tone-correct duty, not a wash)
  // Row-uniform dither (measured 4.94 mean row-count delta).
  EXPECT_LT(rowDelta(tones, 96, 48, 2), 10.0);
}

TEST(TextureBake, FlatDarkFieldStaysDominantlyBlack) {
  // Flat 40 maps to 13 — level 0 dominates. Under the smooth-region 8/8
  // mass-conserving kernel (amendment 7) the duty is the tone-correct one:
  // mapped 13 over the 0→48 span = 27% level-1 duty (measured 73.5% level
  // 0 / 26.5% level 1; the pure-Atkinson absorption used to bias this to
  // 82.4% level 0). No mid levels, no white, level-0 dominance with a
  // floor.
  const std::vector<uint8_t> gray(96 * 48, 40);
  const auto tones = bake(96, 48, gray, "/t/texture_flat40.g");
  const int l0 = levelCount(tones, 0);
  EXPECT_GE(l0, 65 * 96 * 48 / 100);  // ≥65% level 0 (measured 73.5%)
  EXPECT_EQ(levelCount(tones, 1), 96 * 48 - l0);
  EXPECT_EQ(levelCount(tones, 2), 0);
  EXPECT_EQ(levelCount(tones, 3), 0);
}

TEST(TextureBake, FlatMidtoneBetweenLevelsStaysToneCorrect) {
  // Flat 127 maps to 126 (fixed-contrast degenerate map) — between levels
  // 112 and 255 under the panel-calibrated set. Under the smooth-region
  // 8/8 mass-conserving kernel (amendment 7) the field dithers at its
  // tone-correct duty: predicted (126−112)/(255−112) = 9.8% level 3,
  // measured 8.5% — the pure-Atkinson dead zone (100% level 2) is gone by
  // design. The wide six-tap dispersion plus the smooth-region threshold
  // dither keep the low-duty dither worm-free: no black escape, the field
  // stays inside the bracketing pair, and it is row-uniform (no banding).
  const std::vector<uint8_t> gray(48 * 24, 127);
  const auto tones = bake(48, 24, gray, "/t/texture_flat127.g");
  EXPECT_EQ(levelCount(tones, 1), 0);
  EXPECT_EQ(levelCount(tones, 0), 0);
  const int l3 = levelCount(tones, 3);
  EXPECT_GE(l3, 48 * 24 / 20);  // ≥5% level 3 (tone-correct ~9.8%)
  EXPECT_LE(l3, 48 * 24 / 5);   // ≤20% (measured 8.5%)
  EXPECT_EQ(levelCount(tones, 2), 48 * 24 - l3);
  EXPECT_LT(rowDelta(tones, 48, 24, 2), 4.0);
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
  // The 25% absorption raises the noiseless ramp's row-delta floor vs the
  // mass-conserving quantizer (measured 6.06 / 8.54; the pre-fix mode-lock
  // defect measured 16.6 mean |delta|, and a mode-locked pattern under any
  // kernel produces far larger sustained swings than absorption noise).
  EXPECT_LT(rowDelta(tones, w, h, 1), 10.0);
  EXPECT_LT(rowDelta(tones, w, h, 2), 11.0);
}

TEST(TextureBake, GradientHasNoSustainedDensityOscillation) {
  // Same gradient: the pre-fix defect was a sustained period-2 swing
  // (transition rows alternating ~0/79 level-2 dots). Single plateau-edge
  // steps are the designed posterized-transition behavior; repeated
  // opposite-sign swings of shaded-shelf magnitude are the defect.
  // Under Atkinson the 25% absorption adds ISOLATED entry bursts at every
  // band boundary (a full row's worth of dots switching on then off — the
  // KCC look the owner ordered); mode-lock would alternate SUSTAINED
  // through the whole transition (~50+ counted swings vs measured 7).
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
        // Measured 7 isolated entry bursts; a mode-locked transition swings
        // at nearly every row (50+). The bound keeps that separation.
        EXPECT_LE(oscillations, 8) << "sustained oscillation at row " << y;
      }
      prevDelta = d;
    }
    prev = n;
  }
}

TEST(TextureBake, GradientToneIsPreserved) {
  // The diffusion never leaks energy (Sierra-Lite in exact quarter-level
  // units), so the emitted average tone must track the mapped average
  // closely.
  const int w = 240;
  const int h = 240;
  std::vector<uint8_t> gray(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    const uint8_t g = static_cast<uint8_t>(30 + (y * 180) / (h - 1));
    std::fill(gray.begin() + static_cast<size_t>(y) * w, gray.begin() + static_cast<size_t>(y + 1) * w, g);
  }
  const auto tones = bake(w, h, gray, "/t/texture_gradient_tone.g");
  long long emitted = 0;
  for (const uint8_t t : tones) emitted += ttfimg::ToneBake4::kLevels[t];
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

// ── Amendment 7 (edge-aware hybrid diffusion) ───────────────────────────────

namespace {

// Bakes the given field rows plus a full-range 0..255 sweep that shapes the
// near-identity adaptive map (the AdaptiveBake full-range construction:
// lo=1, hi=254), so assertions can be written in mapped space. The field
// rows are collected FIRST so their bake starts from a clean error state;
// the sweep rows only feed the histogram/map and are ignored by the
// assertions.
struct FieldBake {
  std::vector<uint8_t> gray;
  std::vector<uint8_t> tones;
  ttfimg::ToneBake4 bake{0};
  int fieldRows = 0;

  FieldBake(const int w, const std::vector<uint8_t>& field, const int fieldRowCount, const char* tempPath) : bake{w} {
    gray = field;
    for (int v = 0; v < 256; ++v) gray.insert(gray.end(), static_cast<size_t>(w), static_cast<uint8_t>(v));
    fieldRows = fieldRowCount;
    if (bake.beginCollect(tempPath)) {
      for (size_t y = 0; y < gray.size() / w; ++y) bake.collectRow(&gray[y * w]);
    }
    EXPECT_TRUE(bake.grayDone());
    bake.beginRemap();
    tones.resize(gray.size());
    std::vector<uint8_t> row(w);
    for (size_t y = 0; y < gray.size() / w; ++y) {
      bake.toneRow(&gray[y * w], row.data());
      std::copy(row.begin(), row.end(), tones.begin() + static_cast<std::ptrdiff_t>(y * w));
    }
  }
};

}  // namespace

TEST(TextureBake, EdgeAwareHighlightRampHasNoDeadZoneShelf) {
  // The pure-Atkinson dead zone (amendment 7's defect): smooth highlight
  // shading inside the top span snapped flat to level 3 instead of
  // tapering. The hybrid's smooth regions diffuse 8/8 mass-conserving, so
  // the band keeps its gray stipple and the mean emitted tone tracks the
  // mapped input.
  const int w = 240;
  const int fieldLo = 175;
  const int fieldHi = 236;  // below the kWhiteSnap 240 clip
  std::vector<uint8_t> field;
  for (int v = fieldLo; v <= fieldHi; ++v) field.insert(field.end(), static_cast<size_t>(w), static_cast<uint8_t>(v));
  const int fieldRows = fieldHi - fieldLo + 1;
  const FieldBake baked(w, field, fieldRows, "/t/hybrid_ramp.g");

  // Non-zero gray stipple density in the 220..235 band (rows are constant).
  long stipple = 0;
  const int bandLo = 220;
  const int bandHi = 235;
  for (int v = bandLo; v <= bandHi; ++v) {
    const size_t y = static_cast<size_t>(v - fieldLo) * w;
    for (int x = 0; x < w; ++x) {
      if (baked.tones[y + x] != 3) ++stipple;
    }
  }
  EXPECT_GT(stipple, static_cast<long>((bandHi - bandLo + 1)) * w / 20)
      << "the highlight band parked flat on level 3 (dead-zone shelf)";

  // Mean emitted tone within 5 levels of the mapped input over the ramp.
  long long emitted = 0;
  long long mapped = 0;
  for (int r = 0; r < fieldRows; ++r) {
    mapped += static_cast<long long>(baked.bake.mapLut[fieldLo + r]) * w;
    for (int x = 0; x < w; ++x) {
      emitted += ttfimg::ToneBake4::kLevels[baked.tones[static_cast<size_t>(r) * w + x]];
    }
  }
  EXPECT_NEAR(static_cast<double>(emitted) / (fieldRows * w), static_cast<double>(mapped) / (fieldRows * w), 5.0);
}

TEST(TextureBake, EdgeAwareSharpStrokeBlocksCrossEdgeBleed) {
  // Midtone shading ramp → bright paper margin (mapped 232, below the 240
  // white snap) with a 2-px ink stroke inside it. The stroke-to-paper cliff
  // (mapped step ≥ kEdgeBarrier 80) must refuse the forward taps, so the
  // paper immediately after the stroke receives zero cross-cliff bleed and
  // stays clean level 3 while the stroke bakes dark.
  const int w = 96;
  std::vector<uint8_t> field(static_cast<size_t>(w), 232);
  for (int x = 0; x < 32; ++x) field[x] = 150;  // shading ramp
  field[47] = 12;                               // 2-px ink stroke
  field[48] = 12;
  const FieldBake baked(w, field, 1, "/t/hybrid_stroke.g");
  const uint8_t* t0 = baked.tones.data();
  for (int x = 47; x <= 48; ++x) {
    EXPECT_LE(t0[x], 1) << "stroke pixel " << x << " did not bake dark";
  }
  EXPECT_EQ(t0[49], 3) << "paper right after the stroke took cross-cliff bleed";
  EXPECT_EQ(t0[50], 3) << "paper after the stroke took cross-cliff bleed";
}
