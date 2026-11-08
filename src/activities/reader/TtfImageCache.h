#pragma once

// TtfImageCache — host-testable core of the TTF image engine: the tone-cache
// identity (cacheNameFor), validity check (hasValidCache), the streaming 2-bit
// .pxc writer (ToneCacheWriter) and the Floyd–Steinberg 4-level bake. Only
// depends on HalStorage/Logging, so the host suite exercises the exact code
// the device runs (design docs/design/2026-11-07-ttf-image-engine.md §5.4).
// The decode paths (SDK row render, converters, BMP row decoder) stay in
// TtfImageEngine.cpp — they pull the render stack.

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

#include "Epub/converters/DitherUtils.h"
#include "TtfImageEngine.h"

#if defined(CROSSPOINT_TTF_READER)

namespace ttfimg {

// Pure routing predicate (design 2026-11-08 §5.1): the image-page gray
// waveform decision table. ttfAbsoluteImageGray() (QuickPageRelayout.cpp)
// feeds it the runtime probes (X3 guard, UC8279 controller, capability); the
// revert switch and the page's image list decide the first two terms.
constexpr bool absoluteGrayImageRoute(const bool pageHasImages, const bool qualityTone, const bool deviceIsX3,
                                      const bool controllerUc8279, const bool absoluteSupported) {
  return pageHasImages && qualityTone && !deviceIsX3 && controllerUc8279 && absoluteSupported;
}

// Floyd–Steinberg error diffusion into the 4-level tone (§5.6). Error state
// runs row-sequentially over IMAGE-LOCAL columns — never absolute panel
// coordinates — so a completed .pxc encodes the image's tone alone and the
// placement lives only in the cache identity (parity across worker and
// foreground decodes, and across placements, follows from this).
// ToneBake4 (design 2026-11-08 §5.2): the quality path's 4-level tone bake —
// gamma 2.2 LUT, contrast 1.3 LUT, tuned Floyd–Steinberg diffusion (2048/2560
// ≈ 0.8) and serpentine scanning. The LUTs are built once at construction
// (the doc's "no runtime float" is the per-pixel path: pure integer LUT
// lookups and fixed-point error diffusion); error state runs row-sequentially
// over IMAGE-LOCAL columns — the FloydSteinberg4 invariant — so placement
// independence and worker/foreground parity hold by construction.
//
// The eink-image reference applies powf(1/gamma) — a BRIGHTENING curve —
// before dithering (its src/main.rs apply_gamma_correction), not a linear-
// light decode; the doc §7 constant γ = 22/10 is the exponent's denominator
// and the doc §5.2 wording is amended to match. Quantization remains
// nearest-of-{0, 85, 170, 255} in the mapped space — thresholds move with the
// LUTs, none are hand-picked.
struct ToneBake4 : ToneBaker {
  static constexpr int kGammaNum = 22;  // γ = kGammaNum/kGammaDen (§7)
  static constexpr int kGammaDen = 10;
  static constexpr int kContrastNum = 13;  // 1.3 (eink-image default), pivot 128
  static constexpr int kContrastDen = 10;
  static constexpr int kDiffusionNum = 2048;  // 0.8 in 8.8 fixed point (§7)
  static constexpr int kDiffusionDen = 2560;

  uint8_t gammaLut[256];
  uint8_t contrastLut[256];
  std::unique_ptr<int16_t[]> err;      // error carried into this pixel
  std::unique_ptr<int16_t[]> errNext;  // error passed to the next row
  const int width;
  bool reverse = false;  // serpentine: the current row scans right-to-left

  explicit ToneBake4(const int w) : width(w) {
    for (int i = 0; i < 256; ++i) {
      // eink-image's gamma pass: out = round(255 * (i/255)^(1/γ)).
      const float corrected = powf(static_cast<float>(i) / 255.0f, static_cast<float>(kGammaDen) / kGammaNum);
      int g = static_cast<int>(corrected * 255.0f + 0.5f);
      if (g < 0) g = 0;
      if (g > 255) g = 255;
      gammaLut[i] = static_cast<uint8_t>(g);
    }
    for (int i = 0; i < 256; ++i) {
      // eink-image's contrast pass as exact integers:
      // ((v/255 − 1/2)·13/10 + 1/2)·255 = (26·v − 765)/20 — pivot 128 fixed,
      // endpoints clamp to 0/255.
      int c = (26 * i - 765) / 20;
      if (c < 0) c = 0;
      if (c > 255) c = 255;
      contrastLut[i] = static_cast<uint8_t>(c);
    }
    if (w > 0) {
      err.reset(new (std::nothrow) int16_t[w + 2]);
      errNext.reset(new (std::nothrow) int16_t[w + 2]);
      if (err && errNext) {
        memset(err.get(), 0, (w + 2) * sizeof(int16_t));
        memset(errNext.get(), 0, (w + 2) * sizeof(int16_t));
      }
    }
  }
  bool ok() const { return err != nullptr && errNext != nullptr; }

  // Every Floyd–Steinberg term scales by kDiffusionNum/kDiffusionDen:
  // (errVal·k·2048)/(16·2560) = errVal·k/20 for k = 7|3|5|1 (§7).
  int16_t diffuse(const int errVal, const int k) const {
    return static_cast<int16_t>(errVal * k * kDiffusionNum / (16 * kDiffusionDen));
  }

  // x is image-local (0-based). Returns the quantized 4-level tone.
  uint8_t tone(const int x, const int gray) override {
    const int v = contrastLut[gammaLut[static_cast<uint8_t>(gray)]] + err[x + 1];
    int t = (v + 42) / 85;  // nearest of {0, 85, 170, 255} in the mapped space
    if (t < 0) t = 0;
    if (t > 3) t = 3;
    const int errVal = v - t * 85;
    if (!reverse) {
      // Forward row: 7 right, then 3 below-left, 5 below, 1 below-right.
      if (x + 1 < width) err[x + 2] += diffuse(errVal, 7);
      errNext[x] += diffuse(errVal, 3);
      errNext[x + 1] += diffuse(errVal, 5);
      errNext[x + 2] += diffuse(errVal, 1);
    } else {
      // Mirrored serpentine row: 7 left, then 3 below-right, 5 below,
      // 1 below-left.
      if (x > 0) err[x] += diffuse(errVal, 7);
      errNext[x + 2] += diffuse(errVal, 3);
      errNext[x + 1] += diffuse(errVal, 5);
      errNext[x] += diffuse(errVal, 1);
    }
    return static_cast<uint8_t>(t);
  }
  void nextRow() override {
    if (err == nullptr || errNext == nullptr) return;
    memcpy(err.get(), errNext.get(), (width + 2) * sizeof(int16_t));
    memset(errNext.get(), 0, (width + 2) * sizeof(int16_t));
    reverse = !reverse;
  }
};

// Retained verbatim for A/B on device and as the revert switch's reference
// bake (design 2026-11-08 §5.2); the quality decodes use ToneBake4.
struct FloydSteinberg4 {
  std::unique_ptr<int16_t[]> err;      // error carried into this pixel
  std::unique_ptr<int16_t[]> errNext;  // error passed to the next row
  const int width;

  explicit FloydSteinberg4(int w) : width(w) {
    err.reset(new (std::nothrow) int16_t[w + 2]);
    errNext.reset(new (std::nothrow) int16_t[w + 2]);
    if (err && errNext) {
      memset(err.get(), 0, (w + 2) * sizeof(int16_t));
      memset(errNext.get(), 0, (w + 2) * sizeof(int16_t));
    }
  }
  bool ok() const { return err != nullptr && errNext != nullptr; }

  // x is image-local (0-based). Returns the quantized 4-level tone.
  uint8_t tone(int x, int gray) {
    const int v = gray + err[x + 1];
    int t = (v + 42) / 85;  // nearest of {0, 85, 170, 255}
    if (t < 0) t = 0;
    if (t > 3) t = 3;
    const int errVal = v - t * 85;
    if (x + 1 < width) err[x + 2] += static_cast<int16_t>(errVal * 7 / 16);
    errNext[x] += static_cast<int16_t>(errVal * 3 / 16);
    errNext[x + 1] += static_cast<int16_t>(errVal * 5 / 16);
    errNext[x + 2] += static_cast<int16_t>(errVal * 1 / 16);
    return static_cast<uint8_t>(t);
  }
  void nextRow() {
    if (err == nullptr || errNext == nullptr) return;
    memcpy(err.get(), errNext.get(), (width + 2) * sizeof(int16_t));
    memset(errNext.get(), 0, (width + 2) * sizeof(int16_t));
  }
};

// Streaming .pxc writer (same on-disk format as the shared PixelCache: u16
// width, u16 height, then 2-bit rows, 4 pixels per byte, MSB first). Rows
// arrive in raster order; abort() drops the partial file on any failure path.
struct ToneCacheWriter {
  HalFile file;
  char path[160] = {};
  int width = 0;
  int height = 0;
  int bytesPerRow = 0;
  int writtenRows = 0;
  bool ok = false;

  bool begin(const char* p, int w, int h);
  // toneValues carries width quantized tones (0..3), image-local columns.
  bool writeRow(const uint8_t* toneValues);
  bool finalize();
  void abort();
};

// Flat tone-cache name (inside the book's ficache dir, one SdCardCacheStorage
// validName blob). Keyed by the generation, the placement and the tone
// policy: a layout change or a policy flip picks a fresh file; stale ones
// live until the book cache sweep (epub/<hash> clearCache) removes it.
bool cacheNameFor(const ImageSpec& spec, const char* dir, bool qualityTone, char* out, size_t cap);

// Cache lookup: header dims must match the placement (±1, the same bounded
// tolerance ImageBlock uses) and the file must cover the full payload.
bool hasValidCache(const ImageSpec& spec, const char* dir, bool qualityTone);

// Header check shared by hasValidCache and the paint-side blit: dims within
// ±1 of the placement and a payload that covers every row.
bool readValidCacheHeader(HalFile& cacheFile, int expectedWidth, int expectedHeight, uint16_t& cachedWidth,
                          uint16_t& cachedHeight);

}  // namespace ttfimg

#endif  // CROSSPOINT_TTF_READER
