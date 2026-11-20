// Blue-noise cover bake suite (design
// docs/design/2026-11-20-cover-image-pipeline.md):
//  - ImageBakeQuantizer::quantizeGray2bit must be bit-exact with the reader's
//    ToneBake4 bake — both the per-pixel tone() path and the tiled toneRow()
//    fast path production decodes run;
//  - the 1-bit quantizer must keep the legacy tone curve (solid at 0/255,
//    ~50% white at gray 128, ordered-window bounds) with blue-noise shaping;
//  - BmpStreamWriter in BlueNoise mode must pack exactly what the quantizers
//    produce, so the BMP cover converters and the details paint emit the
//    reader bake's tones byte-for-byte.
#include <gtest/gtest.h>

#include <Print.h>

#include <cstdint>
#include <vector>

#include "BmpStreamWriter.h"
#include "ImageBakeQuantizer.h"
#include "TtfImageCache.h"  // ToneBake4, the reader's production bake

int adjustPixel(const int gray) { return gray; }  // USE_BRIGHTNESS=false today

namespace {

class VectorPrint final : public Print {
 public:
  size_t write(const uint8_t value) override {
    bytes.push_back(value);
    return 1;
  }

  std::vector<uint8_t> bytes;
};

// Synthetic gray ramp: deterministic, every value class represented, and a
// function of both coordinates so row/column patterns are exercised.
uint8_t rampGray(const int x, const int y) {
  return static_cast<uint8_t>((x * 7 + y * 13 + (x * y) / 5) % 256);
}

// Unpacks one 2-bit BMP row (MSB-first, 2 bits/pixel) written by
// BmpStreamWriter: row bytes start at data + 4-byte stride alignment.
uint8_t unpack2bit(const uint8_t* rowBytes, const int x) {
  const int bit = (x & 3) * 2;
  return static_cast<uint8_t>((rowBytes[(x * 2) / 8] >> (6 - (x * 2) % 8)) & 0x3);
}

uint8_t unpack1bit(const uint8_t* rowBytes, const int x) {
  return static_cast<uint8_t>((rowBytes[x / 8] >> (7 - x % 8)) & 0x1);
}

TEST(ImageBakeQuantizer, TwoBitPaletteAnchorsAreSolid) {
  // The generator's invariant: the palette anchors {0, 85, 170, 255} quantize
  // to the solid levels {0, 1, 2, 3} at every matrix position.
  for (int y = 0; y < 64; ++y) {
    for (int x = 0; x < 64; ++x) {
      EXPECT_EQ(imagebake::quantizeGray2bit(x, y, 0), 0);
      EXPECT_EQ(imagebake::quantizeGray2bit(x, y, 85), 1);
      EXPECT_EQ(imagebake::quantizeGray2bit(x, y, 170), 2);
      EXPECT_EQ(imagebake::quantizeGray2bit(x, y, 255), 3);
    }
  }
}

TEST(ImageBakeQuantizer, TwoBitMatchesReaderPerPixelTone) {
  // ToneBake4::tone() IS the reader's per-pixel bake (TtfImageCache.h): the
  // converter-side wrapper must agree on every position and gray, including
  // clamping and the tile seam (rows/cols 62..67 wrap the 64 boundary).
  // ToneBake4 owns its row counter: one instance, nextRow() per row.
  ttfimg::ToneBake4 bake(8);
  for (int y = 0; y < 70; ++y) {
    for (int x = 0; x < 70; ++x) {
      const uint8_t gray = rampGray(x, y);
      EXPECT_EQ(imagebake::quantizeGray2bit(x, y, gray), bake.tone(x, gray)) << "x=" << x << " y=" << y;
    }
    bake.nextRow();
  }
}

TEST(ImageBakeQuantizer, TwoBitMatchesReaderToneRowFastPath) {
  // Production reader decodes run toneRow() — the tiled fast path — not the
  // per-pixel loop. The converter's quantizer must match it row by row.
  constexpr int kWidth = 200;  // spans three 64-pixel tiles plus a tail
  std::vector<uint8_t> gray(kWidth);
  std::vector<uint8_t> tones(kWidth);
  ttfimg::ToneBake4 bake(kWidth);
  for (int y = 0; y < 70; ++y) {
    for (int x = 0; x < kWidth; ++x) gray[x] = rampGray(x, y);
    bake.toneRow(gray.data(), tones.data());
    for (int x = 0; x < kWidth; ++x) {
      EXPECT_EQ(imagebake::quantizeGray2bit(x, y, gray[x]), tones[x]) << "x=" << x << " y=" << y;
    }
  }
}

TEST(ImageBakeQuantizer, TwoBitIsPositionIndependent) {
  // Ordered dither: the same (x, y, gray) quantizes identically however the
  // pixels are visited (no row state, unlike the retired Atkinson pass).
  for (int y = 0; y < 64; ++y) {
    for (int x = 0; x < 64; ++x) {
      const uint8_t gray = rampGray(x, y);
      const uint8_t first = imagebake::quantizeGray2bit(x, y, gray);
      for (int pass = 0; pass < 3; ++pass) {
        EXPECT_EQ(imagebake::quantizeGray2bit(x, y, gray), first);
      }
    }
  }
}

TEST(ImageBakeQuantizer, OneBitExtremesAreSolid) {
  // The 64..192 ordered window: gray 255 >= every threshold, gray 0 < every
  // threshold — no salt/pepper dots at the extremes.
  for (int y = 0; y < 64; ++y) {
    for (int x = 0; x < 64; ++x) {
      EXPECT_EQ(imagebake::quantizeGray1bit(x, y, 0), 0);
      EXPECT_EQ(imagebake::quantizeGray1bit(x, y, 255), 1);
      // Below the window floor and above the ceiling the answer is constant.
      EXPECT_EQ(imagebake::quantizeGray1bit(x, y, 63), 0);
      EXPECT_EQ(imagebake::quantizeGray1bit(x, y, 192), 1);
    }
  }
}

TEST(ImageBakeQuantizer, OneBitMidpointSplitsAround50Percent) {
  // Gray 128 against the windowed matrix: ~half the tile turns white, and
  // both values must appear (an ordered texture, not a solid cut).
  int white = 0;
  for (int y = 0; y < 64; ++y) {
    for (int x = 0; x < 64; ++x) white += imagebake::quantizeGray1bit(x, y, 128);
  }
  EXPECT_GT(white, 0);
  EXPECT_LT(white, 4096);
  EXPECT_GE(white, 1900);
  EXPECT_LE(white, 2200);
}

TEST(ImageBakeQuantizer, OneBitIsMonotonicPerPosition) {
  // Raising gray can only turn a pixel white, never black: the ordered
  // threshold at a fixed position is a fixed cutoff.
  for (int y = 0; y < 64; ++y) {
    for (int x = 0; x < 64; ++x) {
      for (int g = 1; g < 256; ++g) {
        EXPECT_GE(imagebake::quantizeGray1bit(x, y, g), imagebake::quantizeGray1bit(x, y, g - 1));
      }
    }
  }
}

// Counts the packed 2-bit tones of a BlueNoise BmpStreamWriter run and
// compares them against ToneBake4::toneRow over the same rows.
TEST(ImageBakeBmpStream, TwoBitPackingMatchesReaderBake) {
  constexpr int kWidth = 200;
  constexpr int kHeight = 66;  // crosses the 64-row tile seam
  constexpr int kHeaderBytes = 54 + 16;  // 2-bit header + 4-entry palette

  VectorPrint output;
  BmpStreamWriter writer;
  ASSERT_TRUE(writer.begin(output, kWidth, kHeight, /*oneBit=*/false, /*originalThresholds=*/false,
                           BmpBake::BlueNoise));
  ASSERT_EQ(output.bytes.size(), kHeaderBytes);

  ttfimg::ToneBake4 bake(kWidth);
  std::vector<uint8_t> gray(kWidth);
  std::vector<uint8_t> tones(kWidth);
  const int bytesPerRow = (kWidth * 2 + 31) / 32 * 4;
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) gray[x] = rampGray(x, y);
    ASSERT_TRUE(writer.writeRow(gray.data(), y));
    bake.toneRow(gray.data(), tones.data());
    const uint8_t* rowBytes = output.bytes.data() + kHeaderBytes + static_cast<size_t>(y) * bytesPerRow;
    for (int x = 0; x < kWidth; ++x) {
      EXPECT_EQ(unpack2bit(rowBytes, x), tones[x]) << "x=" << x << " y=" << y;
    }
  }
  ASSERT_EQ(output.bytes.size(), static_cast<size_t>(kHeaderBytes + bytesPerRow * kHeight));
}

// Same contract for the 1-bit thumb bake.
TEST(ImageBakeBmpStream, OneBitPackingMatchesQuantizer) {
  constexpr int kWidth = 200;
  constexpr int kHeight = 3;
  constexpr int kHeaderBytes = 54 + 8;  // 1-bit header + 2-entry palette

  VectorPrint output;
  BmpStreamWriter writer;
  ASSERT_TRUE(writer.begin(output, kWidth, kHeight, /*oneBit=*/true, /*originalThresholds=*/false,
                           BmpBake::BlueNoise));
  ASSERT_EQ(output.bytes.size(), kHeaderBytes);

  std::vector<uint8_t> gray(kWidth);
  const int bytesPerRow = (kWidth + 31) / 32 * 4;
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) gray[x] = rampGray(x, y);
    ASSERT_TRUE(writer.writeRow(gray.data(), y));
    const uint8_t* rowBytes = output.bytes.data() + kHeaderBytes + static_cast<size_t>(y) * bytesPerRow;
    for (int x = 0; x < kWidth; ++x) {
      EXPECT_EQ(unpack1bit(rowBytes, x), imagebake::quantizeGray1bit(x, y, gray[x])) << "x=" << x << " y=" << y;
    }
  }
  ASSERT_EQ(output.bytes.size(), static_cast<size_t>(kHeaderBytes + bytesPerRow * kHeight));
}

// The legacy bake stays reachable: begin() without the bake argument keeps
// the Atkinson ditherers (GIF covers, and the kUseBlueNoiseBake revert).
TEST(ImageBakeBmpStream, LegacyModeStillAllocatesAndWrites) {
  VectorPrint output;
  BmpStreamWriter writer;
  ASSERT_TRUE(writer.begin(output, 4, 1, /*oneBit=*/false));
  const uint8_t row[] = {10, 60, 120, 200};
  ASSERT_TRUE(writer.writeRow(row, 0));
  EXPECT_EQ(output.bytes.size(), 54 + 16 + 4);
}

}  // namespace
