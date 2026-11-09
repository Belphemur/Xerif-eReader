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
// ToneBake4 (design 2026-11-08 §5.2 + §2 adaptive bake): the quality path's
// 4-level tone bake — per-image adaptive stretch, optional gamma stage,
// tuned Floyd–Steinberg diffusion (2048/2560 ≈ 0.8) and serpentine scanning.
// Two phases (design §2.1, amendment 2):
//   1. beginCollect(path) + collectRow(gray) per destination row, in raster
//      order — a 256-bin u32 histogram accumulates while the raw
//      8-bit gray rows append to the caller's .g temp; no tone is emitted.
//   2. beginRemap() derives the map from the histogram (percentile clip
//      1%/99% stretched to [0,255]; degenerate histograms fall back to the
//      fixed contrast 1.3; the gamma stage defaults OFF — identity), resets
//      the error state, and toneRow() bakes from the .g rows as before.
// The gamma 2.2 + contrast 1.3 constants stay as the fallback/A-B reference
// (the eink-image recipe is 1-bit-oriented: its brightening exists to push
// tones toward a 1-bit dither threshold and collapses midtones on a 4-level
// panel — the washed-out soak defect). The LUTs are built once (the doc's
// "no runtime float" is the per-pixel path: pure integer LUT lookups and
// fixed-point error diffusion); error state runs row-sequentially over
// IMAGE-LOCAL columns — the FloydSteinberg4 invariant — so placement
// independence and worker/foreground parity hold by construction.
// Quantization remains nearest-of-{0, 85, 170, 255} in the mapped space —
// thresholds move with the LUTs, none are hand-picked.
struct ToneBake4 : ToneBaker {
  static constexpr int kGammaNum = 22;  // γ = kGammaNum/kGammaDen (§7)
  static constexpr int kGammaDen = 10;
  static constexpr bool kGammaEnabled = false;  // §2.2: stretch owns the range; device tune flips this
  static constexpr int kContrastNum = 13;       // 1.3 (eink-image default), pivot 128
  static constexpr int kContrastDen = 10;
  static constexpr int kDiffusionNum = 2560;  // 1.0 in 8.8 fixed point (texture §2.3)
  static constexpr int kDiffusionDen = 2560;
  // Zero-mean pseudo-random threshold offset (texture §2.3): breaks the
  // row-as-mega-pixel mode-locking that drives period-2 banding on smooth
  // gradients. Deterministic pure function of (x, row) — worker/foreground
  // parity holds; never enters errVal, so the emitted tone stays true.
  static constexpr int kMaskAmp = 8;  // offset range −8..+7 (4-bit hash)
  // Flat-field preservation (texture §2.2/§3): a run of pixels whose mapped
  // tones are mutually within kFlatTol is locally flat; when such a run sits
  // within kSnap of one level AND the incoming error is small (no vertical
  // gradient flowing through), the run bakes as that single level and its
  // residual error is dropped instead of diffused into texture.
  static constexpr int kFlatTol = 2;  // mapped-tone variation tolerance
  static constexpr int kFlatErr = 8;  // max incoming error for a flat run
  static constexpr int kSnap = 21;    // half the 85/2 step
  // Thin-stroke anchoring (texture §2.4): a source pixel this dark bakes
  // level 0 regardless of accumulated error; its full error still diffuses,
  // so the local tone is preserved and no halo bias is added.
  static constexpr int kStrokeMap = 42;  // half the first shelf (85/2)
  static constexpr int kClipNum = 1;     // percentile clip: ~1% / ~99%
  static constexpr int kClipDen = 100;

  uint8_t gammaLut[256];
  uint8_t contrastLut[256];
  uint8_t mapLut[256];    // adaptive phase-2 map (stretch ⊙ gamma)
  bool adaptive = false;  // toneRow uses mapLut once beginRemap() ran
  // u32 bins: percentile targets are n·1%/n·99% of the FULL pixel count, so a
  // bin must hold the whole image (a flat full-page fill would saturate u16
  // and leave hi at 255 — the range expansion this bake exists to fix).
  uint32_t hist[256];
  uint32_t histCount = 0;
  HalFile grayFile;  // phase-1 gray temp (see beginCollect)
  char grayPathBuf[280] = {};
  size_t grayPathLen = 0;
  bool grayOpen = false;
  bool grayFailed = false;             // sticky write failure (grayDone refuses)
  std::unique_ptr<int16_t[]> err;      // error carried into this pixel
  std::unique_ptr<int16_t[]> errNext;  // error passed to the next row
  const int width;
  bool reverse = false;  // serpentine: the current row scans right-to-left
  int row = 0;           // baked row index (threshold-noise key; texture §2.3)

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
    memset(hist, 0, sizeof(hist));
    memset(mapLut, 0, sizeof(mapLut));  // valid only after beginRemap()
  }
  bool ok() const { return err != nullptr && errNext != nullptr; }

  // ── adaptive two-phase API (§2.1) ───────────────────────────────────────
  // The gray temp is a raw row-major 8-bit stream (no header: the caller
  // knows w×h from the spec). The bake owns the file handle between
  // beginCollect and grayDone so every decoder routes through one writer.
  bool beginCollect(const char* grayPath);
  void collectRow(const uint8_t* gray) override;
  // Closes the temp. false on a write/close failure — the caller drops the
  // file and fails the decode; nothing valid can be produced from it.
  bool grayDone();
  void grayDiscard();  // close + remove (abort/failure path)
  // Sticky phase-1 failure probe: true once a gray temp write failed. The
  // row collectors stop feeding rows and the engine goes straight to the
  // legacy paint pass instead of finishing a decode whose temp is dead.
  bool grayWriteFailed() const override { return grayFailed; }
  const char* grayPath() const { return grayPathBuf; }
  // Percentiles → mapLut, error state reset. Pure integer; deterministic.
  void beginRemap();
  // Storage-failure fallback: reset the histogram, then beginRemap() — the
  // deterministic fixed-contrast map for a paint-only legacy pass.
  void beginLegacyFallback();

  // Every Floyd–Steinberg term scales by kDiffusionNum/kDiffusionDen:
  // (errVal·k·2048)/(16·2560) = errVal·k/20 for k = 7|3|5|1 (§7).
  int16_t diffuse(const int errVal, const int k) const {
    return static_cast<int16_t>(errVal * k * kDiffusionNum / (16 * kDiffusionDen));
  }

  // x is image-local (0-based). `reverseScan` selects the mirrored kernel:
  // a serpentine row is VISITED in its scan direction, so the horizontal
  // term always lands on the next-visited pixel (7/20 to x+1 ascending,
  // to x−1 descending). Returns the quantized 4-level tone.
  // Mapped tone of one source pixel (the LUT chain the bake quantizes).
  int mapv(const uint8_t gray) const { return adaptive ? mapLut[gray] : contrastLut[gammaLut[gray]]; }

  uint8_t toneDir(const int x, const int gray, const bool reverseScan, const uint8_t* rowGray = nullptr) {
    const int mv = mapv(static_cast<uint8_t>(gray));
    const int v = mv + err[x + 1];
    // Zero-mean pseudo-random threshold offset (texture §2.3): breaks the
    // row-as-mega-pixel mode-locking behind period-2 gradient banding.
    // Integer bit-mix of (x, row); never enters errVal — tone stays true.
    uint32_t h = (static_cast<uint32_t>(x) + 1) * 374761393u + (static_cast<uint32_t>(row) + 1) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    const int veff = v + (static_cast<int>((h >> 17) & 15) - 8);
    int t = (veff + 42) / 85;  // nearest of {0, 85, 170, 255} in the mapped space
    if (t < 0) t = 0;
    if (t > 3) t = 3;
    const int errVal = v - t * 85;
    // Flat-field preservation (texture §2.2/§3): the SOURCE neighborhood is
    // flat (both horizontal neighbours map within kFlatTol of this pixel;
    // out-of-image counts as continuing the flat), no vertical gradient is
    // flowing through (small incoming error), and the mapped tone sits within
    // kSnap of one level: bake that level uniformly and drop the residual
    // error instead of diffusing texture the source does not have.
    if (rowGray != nullptr && errVal >= -kSnap && errVal <= kSnap && err[x + 1] >= -kFlatErr &&
        err[x + 1] <= kFlatErr) {
      bool flat = true;
      if (x > 0 && (mapv(rowGray[x - 1]) - mv > kFlatTol || mv - mapv(rowGray[x - 1]) > kFlatTol)) flat = false;
      if (flat && x + 1 < width && (mapv(rowGray[x + 1]) - mv > kFlatTol || mv - mapv(rowGray[x + 1]) > kFlatTol))
        flat = false;
      if (flat) return static_cast<uint8_t>(t);
    }
    // Thin-stroke anchoring (texture §2.4): a source pixel this dark bakes
    // black even when accumulated neighbour error would push it up a level;
    // the full error still diffuses, so the local tone is preserved.
    if (mv <= kStrokeMap && t != 0) return emitDir(x, v, reverseScan, 0);
    return emitDir(x, errVal, reverseScan, t);
  }
  // Diffuse errVal through the serpentine kernel and return the level.
  uint8_t emitDir(const int x, const int errVal, const bool reverseScan, const uint8_t t) {
    if (!reverseScan) {
      // Ascending visit: 7 right, then 3 below-left, 5 below, 1 below-right.
      if (x + 1 < width) err[x + 2] += diffuse(errVal, 7);
      errNext[x] += diffuse(errVal, 3);
      errNext[x + 1] += diffuse(errVal, 5);
      errNext[x + 2] += diffuse(errVal, 1);
    } else {
      // Descending visit: 7 left, then 3 below-right, 5 below, 1 below-left.
      if (x > 0) err[x] += diffuse(errVal, 7);
      errNext[x + 2] += diffuse(errVal, 3);
      errNext[x + 1] += diffuse(errVal, 5);
      errNext[x] += diffuse(errVal, 1);
    }
    return t;
  }
  uint8_t diffuseDir(const int x, const int errVal, const bool reverseScan) {
    return emitDir(x, errVal, reverseScan, 0);
  }
  // ToneBaker's forward kernel — kept for ascending-x callers and the tests;
  // the quality decodes use toneRow().
  uint8_t tone(const int x, const int gray) override { return toneDir(x, gray, false); }

  // Row bake (the serpentine contract): visits even rows ascending and odd
  // rows descending so the mirrored kernel's horizontal term always lands
  // on the next-visited pixel, and advances the error state itself.
  void toneRow(const uint8_t* gray, uint8_t* tones) override {
    if (!ok() || width <= 0) return;
    const bool rev = reverse;
    for (int i = 0; i < width; ++i) {
      const int x = rev ? (width - 1 - i) : i;
      tones[x] = toneDir(x, gray[x], rev, gray);
    }
    nextRow();
  }
  void nextRow() override {
    if (err == nullptr || errNext == nullptr) return;
    memcpy(err.get(), errNext.get(), (width + 2) * sizeof(int16_t));
    memset(errNext.get(), 0, (width + 2) * sizeof(int16_t));
    reverse = !reverse;
    ++row;
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
bool cacheNameFor(const ImageSpec& spec, const char* dir, bool qualityTone, bool clahe, char* out, size_t cap);

// Cache lookup: header dims must match the placement (±1, the same bounded
// tolerance ImageBlock uses) and the file must cover the full payload.
bool hasValidCache(const ImageSpec& spec, const char* dir, bool qualityTone, bool clahe);

// Header check shared by hasValidCache and the paint-side blit: dims within
// ±1 of the placement and a payload that covers every row.
bool readValidCacheHeader(HalFile& cacheFile, int expectedWidth, int expectedHeight, uint16_t& cachedWidth,
                          uint16_t& cachedHeight);

}  // namespace ttfimg

#endif  // CROSSPOINT_TTF_READER
