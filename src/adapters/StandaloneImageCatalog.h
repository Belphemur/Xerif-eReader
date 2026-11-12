#pragma once

#include <epub/ZipCatalog.h>

namespace ttfimg {

// One-entry ZipCatalog that presents a standalone SD image file as a
// HEADERLESS raw stored entry (ZipEntryReader::rawEntry — the source IS the
// entry data at offset 0), so ttfimg::decodeAndCache runs its production
// dispatch unmodified over a single file
// (design 2026-11-08-ttf-image-contrast-and-fit.md §7.1). Lookup ignores the
// requested path: the catalog answers for the one file it was opened with.
// find() returns a pointer into this object, which must outlive the decode —
// the viewer's use is one synchronous call, so the stack instance is safe.
class StandaloneImageCatalog : public freeink::book::ZipCatalog {
 public:
  // fileSize is the open image's byte size; the entry is only handed out when
  // it is nonzero and fits the 32-bit entry record.
  bool open(uint64_t fileSize);

  const freeink::book::ZipEntry* find(const char* path) const override;

 private:
  // find() is const and the engine calls it through a const ref; the entry is
  // fixed at open(), so mutable is record-keeping, not state mutation.
  mutable freeink::book::ZipEntry entry_{};
  bool ready_ = false;
};

// Cache-freshness token for a standalone viewer image (design §7.4): a FNV-1a
// fold of the FAT modification time AND the byte size, so a replacement that
// changes EITHER is a different cache NAME (a same-tick replacement with a
// different size no longer serves the stale bake — review 2026-11-11). Fills
// ImageSpec::generation, a NAME-level key. A rewrite that keeps both the tick
// and the size still reuses the bake (2 s FAT granularity is the floor); the
// 32-bit fold's collision odds for two different states are ~2^-32, far below
// the old same-tick certainty.
constexpr uint32_t viewerGeneration(const uint32_t modificationTime, const uint64_t sizeBytes) {
  uint64_t hash = 14695981039346656037ull;
  const uint64_t words[2] = {modificationTime, sizeBytes};
  for (const uint64_t w : words) {
    for (int i = 0; i < 8; ++i) {
      hash ^= static_cast<uint8_t>(w >> (8 * i));
      hash *= 1099511628211ull;
    }
  }
  return static_cast<uint32_t>(hash >> 32);
}

}  // namespace ttfimg
