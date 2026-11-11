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

// Cache-freshness token for a standalone viewer image (design §7.4): the FAT
// modification time (2 s granularity), falling back to the byte size when the
// timestamp is unreadable. Fills ImageSpec::generation — a NAME-level key, so
// a replaced file gets a fresh bake; a replacement that keeps both the tick
// and the size serves the stale bake until the next change (documented
// limitation; a mtime+size composite does not fit the uint32 field).
constexpr uint32_t viewerGeneration(const uint32_t modificationTime, const uint64_t sizeBytes) {
  return modificationTime != 0 ? modificationTime : static_cast<uint32_t>(sizeBytes);
}

}  // namespace ttfimg
