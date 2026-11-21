#pragma once

// Position-independent blue-noise quantizers for the cover bake surfaces
// (design docs/design/2026-11-20-cover-image-pipeline.md section 2): the
// same bake tables the TTF reader's image engine uses (BlueNoise64.h),
// consumed by the BMP cover converters and the Details cover paint.
//
// Zero heap, zero float, zero row state: every call derives its threshold
// from the image-local pixel position, so any raster/scan order and any
// placement box quantize identically. Coordinates are masked unsigned so
// negatives wrap like the reader bake's (x & 63) indexing.

#include "BlueNoise64.h"
#include "Epub/converters/DitherUtils.h"
#include "ImageBakeSink.h"

namespace imagebake {

// 4-level tone (0..3) through the reader's exact linear-light quantizer
// (ttfimg::blueNoiseQuantize): kBlueNoiseLinearLut[gray] carries the base
// panel level (k<<9) plus the ordered threshold, nz = the toroidal matrix
// value at (x & 63, y & 63). Bit-exact with ToneBake4::tone() for the same
// gray and image-local coordinates (TtfImageCache.h).
inline uint8_t quantizeGray2bit(const int x, const int y, const int gray) noexcept {
  const int g = gray < 0 ? 0 : (gray > 255 ? 255 : gray);
  const uint8_t nz = ttfimg::kBlueNoise64[static_cast<unsigned>(y) & 63u][static_cast<unsigned>(x) & 63u];
  return ttfimg::blueNoiseQuantize(static_cast<uint8_t>(g), nz);
}

// 1-bit tone: 0 = black, 1 = white (the 1-bit BMP palette polarity).
//
// The reader bake has no 1-bit mode (ToneBake4 is 4-level only), so the
// brief's documented choice applies: the toroidal matrix is the ordered
// threshold field around the legacy 128 midpoint, with the same compressed
// 64..192 window the legacy hash-noise quantize1bit used
// (BitmapHelpers.cpp quantize1bit — matrix value instead of hash). Gray
// 128 lands on ~50% white ("50% anchors"); gray 255 is solid white and
// gray 0 solid black, so no salt/pepper dots at the extremes.
inline uint8_t quantizeGray1bit(const int x, const int y, const int gray) noexcept {
  const int nz = ttfimg::kBlueNoise64[static_cast<unsigned>(y) & 63u][static_cast<unsigned>(x) & 63u];
  const int threshold = 128 + ((nz - 128) / 2);
  return (gray >= threshold) ? 1u : 0u;
}

// Build-time sink dispatch (design Amendment B.2): ONE selection point for
// every framebuffer decode's 4-level quantization. PSRAM boards bake the
// quality blue-noise tone over image-local coordinates; no-PSRAM boards keep
// the legacy screen-space Bayer texture, byte-identical to the pre-quality
// baseline. Both bakes stay compiled on every board (KISS within the switch)
// and the selection carries no runtime cost — this replaces the five
// per-callsite grayscaleCapabilities() paint gates the earlier commits
// carried (one source of truth, PSRAM-only boundary, owner-settled).
inline uint8_t sinkQuantize2bit(const int dstX, const int dstY, const int outX, const int outY,
                                const int gray) noexcept {
  if constexpr (kQualitySink) {
    (void)outX;
    (void)outY;
    return quantizeGray2bit(dstX, dstY, gray);
  } else {
    (void)dstX;
    (void)dstY;
    const uint8_t g = gray < 0 ? 0 : (gray > 255 ? 255 : gray);
    return applyBayerDither4Level(g, outX, outY);
  }
}

}  // namespace imagebake
