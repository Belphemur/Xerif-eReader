#pragma once

// TtfImageCache — host-testable core of the TTF image engine: the tone-cache
// identity (cacheNameFor), validity check (hasValidCache), the streaming 2-bit
// .pxc writer (ToneCacheWriter) and the tone bake. Only depends on
// HalStorage/Logging, so the host suite exercises the exact code the device
// runs (design docs/design/2026-11-07-ttf-image-engine.md §5.4). The decode
// paths (SDK row render, converters, BMP row decoder) stay in
// TtfImageEngine.cpp — they pull the render stack.

#include <BitmapHelpers.h>  // the legacy AtkinsonDitherer (shared quantizer/diffusion)

#include <cstring>

#include "Epub/converters/DitherUtils.h"
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

// ToneBake4 (design 2026-11-08 §5.2; legacy-BMP parity amendment 9): the
// quality path's 4-level tone bake is the EXACT pixel transfer of the legacy
// engine's AtkinsonDitherer (lib/GfxRenderer/BitmapHelpers.h,
// originalThresholds=false) — the bake the owner approved on the X4 Pro for
// baseline EPUB illustrations (calm woven shading, clean paper highlights,
// crisp anti-aliased type):
//   - identity 0..255 luma — no histogram, no percentile stretch, no
//     gamma/contrast LUT (the legacy USE_BRIGHTNESS=false contract),
//   - cutoffs {30, 55, 150} and error targets {15, 35, 90, 210} (target 210
//     keeps highlight headroom, so paper tapers to clean white without a
//     harsh snap or highlight speckle),
//   - deterministic left-to-right 6/8 Atkinson diffusion — (adjusted −
//     target) >> 3 into right, right+1, below-left, below, below-right and
//     two rows down — over three int16 error rows of width+4, zero noise,
//     zero serpentine.
// The math is SHARED, not re-implemented (DRY): the bake embeds the legacy
// ditherer class itself, and the parity pin test bakes synthetic reference
// pages through both and asserts byte-identical 2-bit output. Single-pass by
// construction: no whole-image statistics precede row 0, so the decode
// streams destination rows straight through toneRow into the cache writer
// and the paint (amendment 9 — the .g SD temp and the percentile two-phase
// bake of the pre-parity iterations are gone). Error state still runs
// row-sequentially over IMAGE-LOCAL columns, so a completed .pxc encodes the
// image's tone alone and worker/foreground/placement parity holds by
// construction.
struct ToneBake4 : ToneBaker {
  explicit ToneBake4(const int w) : width(w), ditherer_(w) {}
  bool ok() const { return ditherer_.isValid(); }
  // Error-state reset for the re-decode paint fallbacks (storage/decode
  // failure doctrine): a fallback repaint must start from a clean plane —
  // rows may have been baked before the failure.
  void resetError() { ditherer_.reset(); }
  // x is image-local (0-based). Returns the quantized 4-level tone.
  uint8_t tone(const int x, const int gray) override { return ditherer_.processPixel(gray, x); }
  void nextRow() override { ditherer_.nextRow(); }
  // Row bake: visits the row left-to-right (the legacy scan — every row,
  // non-serpentine) and advances the error state itself.
  void toneRow(const uint8_t* gray, uint8_t* tones) override {
    if (!ok() || width <= 0) return;
    for (int x = 0; x < width; ++x) tones[x] = ditherer_.processPixel(gray[x], x);
    ditherer_.nextRow();
  }
  const int width;

 private:
  ::AtkinsonDitherer ditherer_;
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

// Header check shared by hasValidCache and the paint-side blit: the in-file
// identity matches, dims within ±1 of the placement, payload covers every row.
bool readValidCacheHeader(HalFile& cacheFile, int expectedWidth, int expectedHeight, const CacheIdentity& id,
                          uint16_t& cachedWidth, uint16_t& cachedHeight);

}  // namespace ttfimg

#endif  // CROSSPOINT_TTF_READER
