#include "StandaloneImageCatalog.h"

#if defined(CROSSPOINT_TTF_READER)

namespace ttfimg {

bool StandaloneImageCatalog::open(const uint64_t fileSize) {
  entry_ = {};
  ready_ = false;
  if (fileSize == 0 || fileSize > 0xFFFFFFFFull) return false;
  // rawEntry(): headerless stored entry — ZipEntryReader reads the raw bytes
  // from source offset 0, no ZIP local header expected.
  entry_ = freeink::book::ZipEntryReader::rawEntry(static_cast<uint32_t>(fileSize));
  ready_ = true;
  return true;
}

const freeink::book::ZipEntry* StandaloneImageCatalog::find(const char* path) const {
  (void)path;  // single-entry catalog: every lookup is the one file
  return ready_ ? &entry_ : nullptr;
}

}  // namespace ttfimg

#endif  // CROSSPOINT_TTF_READER
