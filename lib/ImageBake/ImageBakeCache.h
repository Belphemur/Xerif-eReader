#pragma once

// ImageBakeCache — the spec-independent pixel-cache core (design Amendment
// B.1): the identity-headered on-disk 2-bit format every pixel cache in the
// firmware shares. Extracted from TtfImageCache.h (ToneCacheWriter) and
// Epub/converters/PixelCache.h (the band-buffered writer the framebuffer
// converters stream through) so there is ONE writer/validator pair instead
// of two divergent formats.
//
// On-disk layout (kCacheHeaderBytes = 11):
//   magic 'C','P' | revision letter | u32 identity hash32 (LE)
//   u16 width     | u16 height       | 2-bit rows, 4 pixels per byte, MSB first
//
// The in-file identity (design 2026-11-10 fidelity campaign): a file found
// under a matching name whose content cannot match the key (stale payload
// from an older bake, a torn write that survived finalize, a collision
// across generations) is rejected at open and regenerated. The filename's
// revision channel (extension tokens like .pxc2) is retired: the letter
// inside the file is the only revision channel, so legacy headerless files
// simply fail the magic check and regenerate.
//
// Board-independent: only HalStorage/Logging/Memory dependencies, compiled
// on every board (const data and no-op when unused, per the C3 floor
// policy). Host suites exercise the exact code the device runs.

#include <HalStorage.h>
#include <Memory.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "ImageBakeSink.h"

namespace imagebake {

// The filename-independent identity baked into every cache header: the bake
// revision letter plus the top 32 bits of the caller's identity hash.
struct CacheIdentity {
  char letter = 0;      // bake revision letter
  uint32_t hash32 = 0;  // top 32 bits of the caller's identity hash
};

constexpr uint8_t kCacheMagic0 = 'C';
constexpr uint8_t kCacheMagic1 = 'P';
constexpr size_t kCacheHeaderBytes = 11;

// FNV-1a over a NUL-terminated string — the shared identity hash used when
// the cache path itself is the identity (framebuffer image-page caches: the
// staged image name encodes the source image).
inline uint32_t pathIdentityHash32(const char* path) {
  uint64_t hash = 14695981039346656037ull;
  for (const char* p = path; p != nullptr && *p != '\0'; ++p) {
    hash ^= static_cast<uint8_t>(*p);
    hash *= 1099511628211ull;
  }
  return static_cast<uint32_t>(hash >> 32);
}

inline CacheIdentity pathIdentity(const char* path, const char letter) {
  CacheIdentity id;
  id.letter = letter;
  id.hash32 = pathIdentityHash32(path);
  return id;
}

// Streaming .pxc writer for rows that arrive in raster order (the reader's
// bake sink): the 11-byte CacheIdentity header, then 2-bit rows, 4 pixels
// per byte, MSB first. abort() drops the partial file on any failure path.
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

// Band-buffered streaming writer for decode loops whose destination rows
// arrive per decode block rather than strictly one row at a time (the
// framebuffer converters): a PoolBytes band of (bandRows + 1) rows, advanceTo
// flushing every row below the new band top (raster order makes them final),
// finalize zero-filling rows the decode never covered (image clipped by the
// screen). Same identity header as ToneCacheWriter, same 2-bit rows.
struct BandCacheWriter {
  PoolBytes buffer;            // band buffer: (bandRows + 1) rows; last row kept zeroed
  uint8_t* zeroRow = nullptr;  // points at the spare zeroed row, for gap/clip fill
  int width = 0;
  int height = 0;
  int bytesPerRow = 0;
  int originX = 0;  // config.x - to convert screen coords to cache coords
  int originY = 0;  // config.y
  int bandRows = 0;
  int bandStart = 0;    // image-local row index of band buffer row 0
  int flushedRows = 0;  // image-local rows already written to file
  HalFile file;
  std::string cachePathStr;
  bool ok = false;

  BandCacheWriter() = default;
  BandCacheWriter(const BandCacheWriter&) = delete;
  BandCacheWriter& operator=(const BandCacheWriter&) = delete;
  // An open file at destruction means neither finalize() nor abort() ran:
  // drop the partial cache so no corrupt file survives the decode.
  ~BandCacheWriter();

  static constexpr int MIN_BAND_ROWS = 16;
  static constexpr size_t MAX_BAND_BYTES = 24 * 1024;  // band working-set ceiling

  // Open the cache file, write the identity header, and allocate a band
  // buffer big enough to hold the tallest single decode block
  // (maxBlockDstRows output rows).
  bool begin(const char* cachePath, int w, int h, int ox, int oy, int maxBlockDstRows, const CacheIdentity& id);
  // Flush every output row below newTopRow (final in raster order) and
  // reposition the band. false = stop caching for the rest of the decode.
  bool advanceTo(int newTopRow);
  // Flush the final band and zero-fill any rows never covered, then close.
  bool finalize();
  // Drop a partial/failed cache so a later decode re-creates it cleanly.
  void abort();
};

// Header check shared by every cache read: the in-file identity must match
// (magic + letter + hash), dims within ±1 of the placement (the same bounded
// tolerance the writers' callers use), payload covering every row.
bool readValidCacheHeader(HalFile& cacheFile, int expectedWidth, int expectedHeight, const CacheIdentity& id,
                          uint16_t& cachedWidth, uint16_t& cachedHeight);

}  // namespace imagebake
