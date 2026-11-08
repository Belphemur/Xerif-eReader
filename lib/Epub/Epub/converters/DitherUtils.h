#pragma once

#include <stdint.h>

// Tone baker interface (design 2026-11-08 §5.2): the quality decodes share
// ONE bake (the TTF image engine's ToneBake4) injected through
// RenderConfig::toneBake, so no converter carries bake math of its own. Not
// owned by the consumer; must outlive the decode call. Rows arrive in raster
// order over image-local columns.
class ToneBaker {
 public:
  virtual ~ToneBaker() = default;
  // Forward-kernel per-pixel bake (ascending-x callers); advances the error
  // state through nextRow(). Production quality paths use toneRow() instead
  // — the serpentine scan must visit whole rows in the bake's direction.
  virtual uint8_t tone(int x, int gray) = 0;
  // Advances the error state to the next image row.
  virtual void nextRow() = 0;
  // Row bake (serpentine, design 2026-11-08 §5.2): quantizes one full image
  // row — `gray` averaged luma in, `tones` quantized tones out, both exactly
  // `width` image-local columns — visiting rows in the bake's own scan
  // direction (the FS kernel mirrors with it) and advancing the error state
  // itself. One call per row replaces the per-pixel loop + nextRow().
  virtual void toneRow(const uint8_t* gray, uint8_t* tones) = 0;
};

// 4x4 Bayer matrix for ordered dithering
inline const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// Apply Bayer dithering and quantize to 4 levels (0-3)
// Stateless - works correctly with any pixel processing order
inline uint8_t applyBayerDither4Level(uint8_t gray, int x, int y) {
  int bayer = bayer4x4[y & 3][x & 3];
  int dither = (bayer - 8) * 5;  // Scale to +/-40 (half of quantization step 85)

  int adjusted = gray + dither;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  if (adjusted < 64) return 0;
  if (adjusted < 128) return 1;
  if (adjusted < 192) return 2;
  return 3;
}
