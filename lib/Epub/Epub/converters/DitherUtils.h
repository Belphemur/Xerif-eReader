#pragma once

#include <stdint.h>

// Tone baker interface (design 2026-11-08 §5.2): the quality decodes share
// ONE bake (the TTF image engine's ToneBake4) injected through
// RenderConfig::toneBake, so no converter carries bake math of its own. Not
// owned by the consumer; must outlive the decode call. Row calls come in
// raster order over image-local columns.
class ToneBaker {
 public:
  virtual ~ToneBaker() = default;
  // x is image-local (0-based); gray is the source luma 0..255. Returns the
  // quantized 4-level tone.
  virtual uint8_t tone(int x, int gray) = 0;
  // Advances the error state to the next image row.
  virtual void nextRow() = 0;
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
