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

#if defined(CROSSPOINT_TTF_READER)

// Flat tone-cache name (inside the book's ficache dir, one SdCardCacheStorage
// validName blob). Keyed by the generation, the placement and the tone
// policy: a layout change or a policy flip picks a fresh file; stale ones
// live until the book cache sweep (epub/<hash> clearCache) removes it.
bool cacheNameFor(const ImageSpec& spec, const char* dir, bool qualityTone, char* out, size_t cap);

// Cache lookup: header dims must match the placement (±1, the same bounded
// tolerance ImageBlock uses) and the file must cover the full payload.
bool hasValidCache(const ImageSpec& spec, const char* dir, bool qualityTone);

// Tone-row blit through `renderer` from the cache (no decode). DirectPixel
// writes are band-clipped; rows are drawn at (x, y). The blit runs per
// DirectPixelWriter pass through the strip/jointed gray planes (row-by-row
// reads like ImageBlock's streaming path), in the orientation the renderer
// reached the tone pointer at the read.
bool blitFromCache(GfxRenderer& renderer, const ImageSpec& spec, const char* dir, bool qualityTone);

enum class DecodeResult : uint8_t {
  Ok = 0,      // cache written (when caching) + rows painted
  Skip = 1,    // nothing applicable (unknown format / missing entry / policy off)
  Busy = 2,    // abort fired at a row boundary (foreground took over)
  Failed = 3,  // transient memory/storage failure — retry next pass
};

// Tone-cache production. `paint` may be nullptr: worker JPEG/PNG decodes
// write rows cache-only (no scratch render); GIF/BMP need the renderer.
// `qualityTone` picks the bake; abortPoll returns true at row boundaries.
DecodeResult decodeAndCache(const ImageSpec& spec, const char* cachePath, freeink::book::BookSource& source,
                            const freeink::book::ZipCatalog& zip, freeink::book::Arena& scratch, Epub* epub,
                            GfxRenderer* paint, bool qualityTone, bool caching, bool (*abortPoll)(void* ctx),
                            void* abortCtx);

// Full decode gates for a worker job (the #292 heap-gate precedents).
bool decodeGatesOk();

#endif  // CROSSPOINT_TTF_READER

}  // namespace ttfimg
// (ImageSpec above is unguarded so the inert worker stub on non-TTF builds
// keeps the same enqueue signature; the namespace closes after the guard.)
