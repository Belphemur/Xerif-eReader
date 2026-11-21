#pragma once

// TtfImageCache — host-testable core of the TTF image engine: the tone bake
// (ToneBake4) and the spec-dependent cache naming (cacheNameFor/cacheIdentity
// over ImageSpec). The identity-headered writers and the header validator are
// the shared spec-independent core (lib/ImageBake/ImageBakeCache.h, design
// Amendment B.1) re-exported below. Only depends on HalStorage/Logging, so
// the host suite exercises the exact code the device runs (design
// docs/design/2026-11-07-ttf-image-engine.md §5.4). The decode paths (SDK
// row render, converters, BMP row decoder) stay in TtfImageEngine.cpp — they
// pull the render stack.

#include <cstring>

#include "BlueNoise64.h"
#include "Epub/converters/DitherUtils.h"
#include "ImageBakeCache.h"
#include "TtfImageEngine.h"

#if defined(CROSSPOINT_TTF_READER)

class GfxRenderer;  // paint target (pointer only — the render stack stays in TtfImageEngine.cpp)

namespace ttfimg {

// Pure routing predicate (design 2026-11-08 §5.1): the image-page gray
// waveform decision table. ttfAbsoluteImageGray() (QuickPageRelayout.cpp)
// feeds it the runtime probes (X3 guard, UC8279 controller, capability); the
// revert switch and the page's image list decide the first two terms.
constexpr bool absoluteGrayImageRoute(const bool pageHasImages, const bool qualityTone, const bool deviceIsX3,
                                      const bool controllerUc8279, const bool absoluteSupported) {
  return pageHasImages && qualityTone && !deviceIsX3 && controllerUc8279 && absoluteSupported;
}

// ToneBake4 (design 2026-11-08 §5.2; linear-light blue-noise amendment 10):
// the quality path's 4-level tone bake is 64x64 toroidal Void-and-Cluster
// blue-noise ORDERED dithering in linear light — the bake that won the
// owner's 293-round blind A/B tournament over the corpus (11-0 vs the
// Atkinson bake, 8-0 vs the legacy engine):
//   - linear-light threshold: source gray g → v = 255·(g/255)^2.2 against
//     the panel's 4 nominal reflectances {0, 85, 170, 255} — exactly the
//     levels the panel actually renders, so the bake's mean reflectance is
//     the image's mean reflectance (no tone-mapping cutoffs, no error
//     targets, no diffusion error state at all),
//   - the ordered threshold is kBlueNoise64[y & 63][x & 63]: stateless per
//     pixel, fully deterministic, seamless at 64-pixel boundaries,
//   - quantization through kBlueNoiseLinearLut: zero float, zero divide,
//     zero branch on the hot path.
// Zero-heap and sizeof-bounded (the vtable pointer + width + row counter);
// the whole bake lives on the caller's stack frame. Single-pass by
// construction: no whole-image statistics precede row 0, so the decode
// streams destination rows straight through toneRow into the cache writer
// and the paint. Rows arrive in raster order over image-local columns, so
// worker/foreground/placement parity holds by construction — and
// resetError() is a no-op kept for the re-decode paint fallback contract.
struct ToneBake4 : ToneBaker {
  explicit ToneBake4(const int w) noexcept : width(w) {}
  bool ok() const noexcept { return width > 0; }
  // Stateless bake: nothing accumulates, so the re-decode paint fallback's
  // clean-plane requirement holds without touching state.
  void resetError() noexcept { row_ = 0; }
  // x is image-local (0-based; masked unsigned so x < 0 is well-defined).
  // Returns the quantized 4-level tone.
  uint8_t tone(const int x, const int gray) override {
    const uint8_t g = static_cast<uint8_t>(gray < 0 ? 0 : (gray > 255 ? 255 : gray));
    const uint8_t nz = kBlueNoise64[static_cast<unsigned>(row_) & 63u][static_cast<unsigned>(x) & 63u];
    return blueNoiseQuantize(g, nz);
  }
  void nextRow() override { ++row_; }
  // Row bake: the ESP32-S3 fast path — hoist the 64-byte noise row slice
  // (two 32B L1 cache lines) and step in 64-pixel tiles so the inner loop
  // has no (x & 63) masking and no loop-carried dependencies. Advances the
  // row itself (one call per row replaces the per-pixel loop + nextRow()).
  void toneRow(const uint8_t* gray, uint8_t* tones) override {
    if (width <= 0) return;
    const uint8_t* nz = kBlueNoise64[static_cast<unsigned>(row_) & 63u];
    int x = 0;
    for (; x + 64 <= width; x += 64) {
      const uint8_t* gTile = gray + x;
      uint8_t* tTile = tones + x;
      for (int i = 0; i < 64; ++i) {
        const uint16_t entry = kBlueNoiseLinearLut[gTile[i]];
        tTile[i] = static_cast<uint8_t>((entry >> 9) + (static_cast<uint16_t>(nz[i]) < (entry & 0x1FFu)));
      }
    }
    for (int i = 0; x < width; ++x, ++i) {
      const uint16_t entry = kBlueNoiseLinearLut[gray[x]];
      tones[x] = static_cast<uint8_t>((entry >> 9) + (static_cast<uint16_t>(nz[i]) < (entry & 0x1FFu)));
    }
    ++row_;
  }
  const int width;

 private:
  int row_ = 0;  // count of toneRow()/nextRow() completions; masked & 63
};
static_assert(sizeof(ToneBake4) <= 32, "the bake must stay a stack-sized value (zero heap)");

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

// Cache identity + the streaming .pxc writers + the header validator live in
// the shared spec-independent core (lib/ImageBake/ImageBakeCache.h, design
// Amendment B.1) — one on-disk format for the reader stream, the framebuffer
// converters' band stream and the cover cache. Re-exported under ttfimg for
// the reader's existing call sites; the spec-dependent naming (cacheNameFor /
// cacheIdentity / cacheRevisionLetter over ImageSpec) stays here.
using imagebake::CacheIdentity;
using imagebake::kCacheHeaderBytes;
using imagebake::kCacheMagic0;
using imagebake::kCacheMagic1;
using imagebake::readValidCacheHeader;
using imagebake::ToneCacheWriter;

// The filename's identity components, shared with the in-file header.
uint64_t cacheHash64(const ImageSpec& spec);
char cacheRevisionLetter(bool qualityTone, bool clahe);
CacheIdentity cacheIdentity(const ImageSpec& spec, bool qualityTone, bool clahe);

// The single-pass bake sink the engine feeds every decoder's destination
// rows through (legacy-BMP parity amendment 9): toneRow → cache row →
// paint, in the scaler's raster-order emission (each row exactly once —
// bufferless Sharp streaming at band completion, CLAHE after finish()'s
// emit walk). The writer detaches mid-decode on a degraded rung (never
// publish) or a failed row write (keep painting); `paint` is null for
// worker cache-only jobs. Shared by the JPEG/PNG SDK decode, the GIF
// converter pipeline, the BMP row decode (decodeBmpRows) and the re-decode
// paint fallback — worker/foreground cache parity is this one sink.
struct BakeSinkCtx {
  ToneBake4* bake = nullptr;
  const ImageSpec* spec = nullptr;
  ToneCacheWriter* writer = nullptr;  // active cache stream; nullptr = cacheless
  GfxRenderer* paint = nullptr;       // nullptr = worker leg (paintToTarget false)
  uint8_t* toneRow = nullptr;         // [spec->w]
  bool (*abortPoll)(void* ctx) = nullptr;
  void* abortCtx = nullptr;
  bool aborted = false;

  // Detach the cache stream on a degraded decode: the partial .t (the
  // identity header and any rows already streamed) is removed, nothing
  // further is written or published.
  void detachWriter() {
    if (writer == nullptr) return;
    writer->abort();
    writer = nullptr;
  }
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

}  // namespace ttfimg

#endif  // CROSSPOINT_TTF_READER
