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
// exact Floyd–Steinberg diffusion and serpentine scanning.
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
// Quantization is nearest-of-{0, 80, 155, 255} via the midpoints
// {40, 117, 205} (the 2026-11-10 fidelity campaign: level 2 moved off the
// eink-image 170 — photo paper whites dithered 170/255 at ~50% duty where
// the panel's own white would do). The diffusion is Sierra-Lite in exact
// 1/4-level units: forward 2/4, down-behind 1/4, down 1/4 — the plane holds
// quarter-levels, so every distributed term is an integer and per-pixel
// mass is conserved bit-exactly (4·errVal quarter-units out per 4·e in,
// with the consume-rounding residue rem4 carried into the forward slot).
// Near-white paper (mapped ≥ 252) snaps to level 3 with no diffusion: the
// white field carries no error and stays clean (no salt churn).
struct ToneBake4 : ToneBaker {
  static constexpr int kGammaNum = 22;  // γ = kGammaNum/kGammaDen (§7)
  static constexpr int kGammaDen = 10;
  static constexpr bool kGammaEnabled = false;  // §2.2: stretch owns the range; device tune flips this
  static constexpr int kContrastNum = 13;       // 1.3 (eink-image default), pivot 128
  static constexpr int kContrastDen = 10;
  // Sierra-Lite plane (fidelity campaign 2026-11-10): the plane holds error
  // in exact 1/4-level units, so every distributed term is an integer and
  // per-pixel mass is conserved bit-exactly (forward 2/4, down-behind 1/4,
  // down 1/4 — 4·errVal quarter-units out per 4·e consumed, with the
  // consume-rounding residue rem4 carried into the forward slot).
  // Percentile clip (blockiness design 2026-11-09): 0.5% / 99.5%. The old
  // 1%/99% clip sat at gray ~62 on soft-shadow art, crushing every shadow
  // below it to level 0 (the "blacks too deep" soak defect) while
  // stretching the rest; 0.5% keeps midtone structure and still reaches
  // true black/white where the art contains them.
  static constexpr int kClipNum = 5;     // percentile clip out of kClipDen
  static constexpr int kClipDen = 1000;  // => 0.5% / 99.5%
  // Sierra-Lite quantizer.
  static constexpr uint8_t kLevels[4] = {0, 80, 155, 255};
  static constexpr int kMids[3] = {40, 117, 205};  // nearest-level midpoints
  static constexpr int kWhiteSnap = 252;           // mapped value that snaps to paper white

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
  bool grayFailed = false;         // sticky write failure (grayDone refuses)
  std::unique_ptr<int32_t[]> err;  // error plane, quarter-level units
  std::unique_ptr<int32_t[]> errNext;
  const int width;
  bool reverse = false;  // serpentine: the current row scans right-to-left
  int row = 0;           // baked row index

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
      err.reset(new (std::nothrow) int32_t[w + 2]);
      errNext.reset(new (std::nothrow) int32_t[w + 2]);
      if (err && errNext) {
        memset(err.get(), 0, (w + 2) * sizeof(int32_t));
        memset(errNext.get(), 0, (w + 2) * sizeof(int32_t));
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

  // x is image-local (0-based). `reverseScan` selects the mirrored kernel:
  // a serpentine row is VISITED in its scan direction, so the horizontal
  // term always lands on the next-visited pixel (2/4 to x+1 ascending,
  // to x−1 descending). Returns the quantized 4-level tone.
  // Mapped tone of one source pixel (the LUT chain the bake quantizes).
  int mapv(const uint8_t gray) const { return adaptive ? mapLut[gray] : contrastLut[gammaLut[gray]]; }

  uint8_t toneDir(const int x, const int gray, const bool reverseScan) {
    const int mv = mapv(static_cast<uint8_t>(gray));
    // White-paper snap: near-pure paper emits level 3, zeroes the slot and
    // diffuses nothing — the white field carries no error and stays clean
    // (no salt churn over the page).
    if (mv >= kWhiteSnap) {
      err[x + 1] = 0;
      return 3;
    }
    // Consume the slot exactly: e4 is quarter-levels; e the rounded level,
    // rem4 the exact rounding residue (carried into the forward slot below
    // — nothing of the plane is ever dropped).
    const int e4 = err[x + 1];
    const int e = e4 >= 0 ? (e4 + 2) >> 2 : -((2 - e4) >> 2);
    err[x + 1] = 0;
    const int rem4 = e4 - (e << 2);
    const int v = mv + e;
    int t = 3;
    if (v < kMids[0])
      t = 0;
    else if (v < kMids[1])
      t = 1;
    else if (v < kMids[2])
      t = 2;
    const int errVal = v - kLevels[t];
    // Serpentine Sierra-Lite, quarter-level units: forward 2/4 (+ the
    // consume residue), down-behind 1/4, down 1/4.
    if (!reverseScan) {
      if (x + 1 < width) err[x + 2] += (errVal << 1) + rem4;
      errNext[x] += errVal;  // down-behind
      errNext[x + 1] += errVal;
    } else {
      if (x > 0) err[x] += (errVal << 1) + rem4;
      errNext[x + 2] += errVal;  // down-behind (to the right)
      errNext[x + 1] += errVal;
    }
    return static_cast<uint8_t>(t);
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
      tones[x] = toneDir(x, gray[x], rev);
    }
    nextRow();
  }
  void nextRow() override {
    if (err == nullptr || errNext == nullptr) return;
    memcpy(err.get(), errNext.get(), (width + 2) * sizeof(int32_t));
    memset(errNext.get(), 0, (width + 2) * sizeof(int32_t));
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

// Cache identity (2026-11-10 fidelity campaign): the bake-revision letter
// AND the identity-hash top 32 bits live INSIDE the file, not only in the
// filename — a file found under a matching name whose content cannot match
// the key (stale payload from an older bake, a torn write that survived
// finalize, a 64-bit hash collision across generations) is rejected at open
// and regenerated. Layout: magic 'C','P', revision letter, u32 hash32 (LE),
// u16 width, u16 height, then the 2-bit rows.
struct CacheIdentity {
  char letter = 0;      // bake revision letter (the filename's i<letter>)
  uint32_t hash32 = 0;  // top 32 bits of the FNV identity hash
};
constexpr uint8_t kCacheMagic0 = 'C';
constexpr uint8_t kCacheMagic1 = 'P';
constexpr size_t kCacheHeaderBytes = 11;

// The filename's identity components, shared with the in-file header.
uint64_t cacheHash64(const ImageSpec& spec);
char cacheRevisionLetter(bool qualityTone, bool clahe);
CacheIdentity cacheIdentity(const ImageSpec& spec, bool qualityTone, bool clahe);

// Streaming .pxc writer (on-disk format: the 11-byte CacheIdentity header
// above, then 2-bit rows, 4 pixels per byte, MSB first). Rows arrive in
// raster order; abort() drops the partial file on any failure path.
struct ToneCacheWriter {
  HalFile file;
  char path[160] = {};
  int width = 0;
  int height = 0;
  int bytesPerRow = 0;
  int writtenRows = 0;
  bool ok = false;

  bool begin(const char* p, int w, int h, const CacheIdentity& id);
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

// Cache lookup: the in-file identity header (magic + revision letter + hash)
// must match, the header dims must match the placement (±1, the same bounded
// tolerance ImageBlock uses) and the file must cover the full payload. A file
// that EXISTS but fails the identity/dims check is removed (regenerated by
// the next decode).
bool hasValidCache(const ImageSpec& spec, const char* dir, bool qualityTone, bool clahe);

// Halo-free clamped dark-stroke sharpen core (the owner's
// sharpenRowHaloFree, fidelity campaign 2026-11-10): one row triple in, one
// row out, pure integer. A 3×3 tent blur (separable (1,2,1)/4 ⊗ (1,2,1)/4,
// >>4) gives the high-pass detail; a ±3 coring deadzone leaves smooth
// gradients untouched; the detail is restored asymmetrically (dark strokes
// at strength 1.0, bright side at 0.375) and the output is strictly clamped
// to the original 3×3 [min,max] — zero overshoot, so no halos. Border rows
// replicate: pass the same pointer for prev/next at the frame edges.
constexpr int kSharpenDarkQ4 = 16;  // dark-stroke gain, Q4 (16/16 = 1.0)
constexpr int kSharpenLightQ4 = 6;  // bright-side gain, Q4 (6/16 = 0.375)
constexpr int kSharpenCoring = 3;   // |detail| deadzone
inline void sharpenRowHaloFree(const uint8_t* prev, const uint8_t* self, const uint8_t* next, const int w,
                               uint8_t* out) {
  for (int x = 0; x < w; ++x) {
    const int xl = x > 0 ? x - 1 : 0;
    const int xr = x < w - 1 ? x + 1 : w - 1;
    const int orig = self[x];
    int outv = orig;
    // 3×3 tent, (1,2,1) ⊗ (1,2,1) / 16 — exact integer weights.
    const int h0 = prev[xl] + 2 * prev[x] + prev[xr];
    const int h1 = self[xl] + 2 * self[x] + self[xr];
    const int h2 = next[xl] + 2 * next[x] + next[xr];
    const int detail = orig - ((h0 + 2 * h1 + h2) >> 4);
    if (detail < -kSharpenCoring || detail > kSharpenCoring) {
      const int kQ4 = detail < 0 ? kSharpenDarkQ4 : kSharpenLightQ4;
      outv = orig + ((detail * kQ4) >> 4);
      // Strict local clamp against the ORIGINAL 3×3 neighborhood.
      int mn = 255;
      int mx = 0;
      for (const uint8_t* r : {prev, self, next}) {
        const int a = r[xl];
        const int b = r[x];
        const int c = r[xr];
        if (a < mn) mn = a;
        if (a > mx) mx = a;
        if (b < mn) mn = b;
        if (b > mx) mx = b;
        if (c < mn) mn = c;
        if (c > mx) mx = c;
      }
      if (outv < mn) outv = mn;
      if (outv > mx) outv = mx;
    }
    out[x] = static_cast<uint8_t>(outv);
  }
}

// Gate: genuine downscales only — at 1:1 and upscale the tent bleeds ink
// into flat margins (the tentPreblurOk finding). Same shape as the old
// tentPreblurOk, either axis: a fit on one axis with a >1.25x squeeze on
// the other is still a downscale decode.
constexpr bool sharpenOk(const int srcW, const int srcH, const int dstW, const int dstH) {
  return dstW > 0 && dstH > 0 && (srcW * 4 > dstW * 5 || srcH * 4 > dstH * 5);
}

// Header check shared by hasValidCache and the paint-side blit: the in-file
// identity matches, dims within ±1 of the placement, payload covers every row.
bool readValidCacheHeader(HalFile& cacheFile, int expectedWidth, int expectedHeight, const CacheIdentity& id,
                          uint16_t& cachedWidth, uint16_t& cachedHeight);

}  // namespace ttfimg

#endif  // CROSSPOINT_TTF_READER
