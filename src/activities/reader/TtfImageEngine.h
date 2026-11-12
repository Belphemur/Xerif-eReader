#pragma once

// TtfImageEngine — tone-cache production and consumption for the native-TTF
// reader's image pass (design docs/design/2026-11-07-ttf-image-engine.md).
//
// The .fibp page blobs carry per-page image records {x, y, w, h, href}. This
// engine turns a record into:
//   1. a cache name — flat inside the book's ficache dir, keyed by generation
//      + href + placement (and the tone policy, so the revert switch picks a
//      clean cache instead of mixing bakes),
//   2. a decode that bakes 4-level tone rows into the .pxc (the shared 2-bit
//      format the BITMAP engine's ImageBlock uses) while blitting the same
//      rows through the live (or worker-scratch) GfxRenderer,
//   3. a cache blit — DirectPixelWriter over the cached tone rows, no decode.
//
// The worker (ImagePredecodeWorker) runs step 2 ahead of the paint; the
// foreground image step in QuickPageRelayout runs 1 + 3 with 2 as the
// fallback, so both paths produce byte-identical caches by construction
// (the same decode code runs on both sides).

#include <BookArena.h>  // freeink::book::Arena (book-side alias of freeink::font::Arena)
#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <cstring>

class Epub;
class GfxRenderer;

namespace freeink {
namespace book {
class BookSource;
class ZipCatalog;
}  // namespace book
}  // namespace freeink

// POD per-image record handed to the worker; declared unguarded so the inert
// worker stub (PSRAM-less builds) keeps the same enqueue signature.
namespace ttfimg {

struct ImageSpec {
  static constexpr size_t kHrefCap = 112;  // zip item href, NUL included
  char href[kHrefCap] = {};
  // Not owned; PAINT-SIDE ONLY (the cache name and the decode probe hash/
  // read it while the Page lives — the worker queue stores specs by value,
  // so a pointer there would dangle; sightings skip overlong hrefs). A Page
  // href past kHrefCap cannot fit `href`, but the cache identity is a hash
  // of the string and the decode stack takes the href by pointer with no
  // length cap — so an overlong href decodes and caches like any other
  // (review round 2: instead of an uncached special case).
  const char* hrefExt = nullptr;
  uint32_t generation = 0;  // layout generation the placement came from
  uint16_t spine = 0;
  uint16_t page = 0;
  uint16_t imgIdx = 0;
  int16_t x = 0;  // .fibp placement (screen coordinates)
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
  uint8_t orientation = 0;  // GfxRenderer::Orientation at capture (§5.6)
  uint8_t deferrals = 0;    // heap/PSRAM gate retries before the job drops
};

// Centered aspect-preserving fit of a source image into a screen, the ONE
// integer placement derivation (design 2026-11-08-ttf-image-contrast-and-fit.md
// §7.2): the reader's page placements come from layout, the image viewer's
// come from here. The expression sequence replicates the legacy viewer's
// fitImage verbatim — float min of the two fit ratios and 1.0 (never
// upscales), truncating scale, 1-clamps, screen clamps (belt-and-braces
// against float rounding at the ratio boundary), centered origin.
// srcW/srcH must be positive; non-positive input returns a 1x1 top-left
// placement instead of the float-UB the raw formula would hit.
struct Placement {
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
};

inline Placement fitPlacement(const int srcW, const int srcH, const int screenW, const int screenH) {
  if (srcW <= 0 || srcH <= 0 || screenW <= 0 || screenH <= 0) {
    Placement degenerate;
    degenerate.w = 1;
    degenerate.h = 1;
    return degenerate;
  }
  const float scaleW = static_cast<float>(screenW) / static_cast<float>(srcW);
  const float scaleH = static_cast<float>(screenH) / static_cast<float>(srcH);
  float scale = scaleW < scaleH ? scaleW : scaleH;
  if (scale > 1.0f) scale = 1.0f;
  int w = static_cast<int>(static_cast<float>(srcW) * scale);
  int h = static_cast<int>(static_cast<float>(srcH) * scale);
  if (w < 1) w = 1;
  if (w > screenW) w = screenW;
  if (h < 1) h = 1;
  if (h > screenH) h = screenH;
  Placement p;
  p.x = static_cast<int16_t>((screenW - w) / 2);
  p.y = static_cast<int16_t>((screenH - h) / 2);
  p.w = static_cast<int16_t>(w);
  p.h = static_cast<int16_t>(h);
  return p;
}

#if defined(CROSSPOINT_TTF_READER)

// Flat tone-cache name (inside the book's ficache dir, one SdCardCacheStorage
// validName blob). Keyed by the generation, the placement and the tone
// policy: a layout change or a policy flip picks a fresh file; stale ones
// live until the book cache sweep (epub/<hash> clearCache) removes it.
bool cacheNameFor(const ImageSpec& spec, const char* dir, bool qualityTone, bool clahe, char* out, size_t cap);

// Cache lookup: header dims must match the placement (±1, the same bounded
// tolerance ImageBlock uses) and the file must cover the full payload.
bool hasValidCache(const ImageSpec& spec, const char* dir, bool qualityTone, bool clahe);

// Tone-row blit through `renderer` from the cache (no decode). DirectPixel
// writes are band-clipped; rows are drawn at (x, y). The blit runs per
// DirectPixelWriter pass through the strip/jointed gray planes (row-by-row
// reads like ImageBlock's streaming path), in the orientation the renderer
// reached the tone pointer at the read.
bool blitFromCache(GfxRenderer& renderer, const ImageSpec& spec, const char* dir, bool qualityTone, bool clahe);

enum class DecodeResult : uint8_t {
  Ok = 0,      // cache written (when caching) + rows painted
  Skip = 1,    // nothing applicable (unknown format / missing entry / policy off)
  Busy = 2,    // abort fired at a row boundary (foreground took over)
  Failed = 3,  // transient memory/storage failure — retry next pass
};

// Tone-cache production. `paint` is the decode's render target; with
// `paintToTarget` false (worker cache-only jobs) the JPEG/PNG and BMP paths
// skip the placement writes and only produce the cache — the GIF converter
// structurally requires a renderer, so `paint` must still be a live one
// there. `qualityTone` picks the bake; abortPoll returns true at row
// boundaries. When `caching` is true the decode streams to
// "<cachePath>.t" and publishes it onto `cachePath` with
// Storage::replaceFile on success — an interrupted decode never leaves a
// torn final cache.
DecodeResult decodeAndCache(const ImageSpec& spec, const char* cachePath, freeink::book::BookSource& source,
                            const freeink::book::ZipCatalog& zip, freeink::book::Arena& scratch, Epub* epub,
                            GfxRenderer* paint, bool qualityTone, bool caching, bool (*abortPoll)(void* ctx),
                            void* abortCtx, bool paintToTarget = true, bool clahe = false);
// The probe/extract run against spec.hrefExt when set (overlong href — see
// ImageSpec), else spec.href.

// Full decode gates for a worker job (the #292 heap-gate precedents).
bool decodeGatesOk();

// Decode-arena floor shared by the worker and the foreground cold paint
// (owner-audit iteration 3, 2026-11-11): a deflated full-page JPEG needs
// ~74–81 KiB inside the decode arena (inflate window + tinfl ~46 KiB,
// skipBuf + TJpgDec ~9 KiB, srcW×16 band 15.5–22 KiB, RowScaler ~3.5 KiB);
// 128 KiB covers it with margin so no decode steps down through the SDK's
// arena retry. PSRAM-backed on device; the 5 MiB budget pins hold (the
// budget measures the store + scaler scratch, this is a separate reserve).
constexpr size_t kDecodeArenaBytes = 128 * 1024;

// PSRAM budget for one image decode (owner directive 2026-11-09): the TTF
// reader boards are all 8 MB PSRAM S3s (platformio.ini gates
// CROSSPOINT_TTF_READER to -DBOARD_HAS_PSRAM boards) and the reader uses
// ~1 MB of it, so the decode ladder may claim up to 5 MB. The old 256 KiB
// gate floor let full-page decodes start, fail the ~432 KiB rung-1 scratch
// allocation and silently degrade to the ladder's bottom rung.
constexpr size_t kPsramBudgetBytes = 5u * 1024u * 1024u;

// Pre-budget floor: boards (or host runs) whose total PSRAM cannot back the
// budget keep the old behavior instead of never decoding.
constexpr size_t kPsramSmallPsramFloorBytes = 256u * 1024u;

// Pure gate decision: a decode may start only when the budget floor is met.
// Boards whose total PSRAM is below the budget fall back to the small floor;
// PSRAM-less builds (0 total bytes) defer like any gate-starved job, and the
// ladder inside the decode still degrades every rung it cannot fit. The
// comparison is STRICT (>): a board whose total PSRAM exactly equals the
// budget cannot keep all of it free, so demanding the budget as free would
// defer that board forever instead of engaging the small floor.
constexpr bool psramBudgetOk(const size_t psramTotal, const size_t psramFree) {
  const size_t floor = psramTotal > kPsramBudgetBytes ? kPsramBudgetBytes : kPsramSmallPsramFloorBytes;
  return psramFree >= floor;
}

#endif  // CROSSPOINT_TTF_READER

}  // namespace ttfimg
// (ImageSpec above is unguarded so the inert worker stub on non-TTF builds
// keeps the same enqueue signature; the namespace closes after the guard.)
