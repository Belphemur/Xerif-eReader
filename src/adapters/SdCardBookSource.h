#pragma once

#include <BookStorage.h>
#include <HalStorage.h>

namespace freeink {
namespace book {

// BookSource over one SD file through HalStorage (mutex-wrapped HalFile; SdFat
// is never touched directly). The size is cached at open — the book file does
// not change while the source is open. A failed open leaves the source
// invalid: readAt() returns -1 and size() 0 instead of crashing.
class SdCardBookSource : public BookSource {
 public:
  SdCardBookSource() = default;
  explicit SdCardBookSource(const char* path);
  ~SdCardBookSource() override = default;
  // Reopenable in place: defaulted move-assign (the user-declared destructor
  // suppresses the implicit one).
  SdCardBookSource& operator=(SdCardBookSource&&) = default;

  bool isValid() const { return file_.isOpen(); }

  int32_t readAt(uint64_t offset, void* dst, uint32_t len) override;
  uint64_t size() const override { return isValid() ? sizeBytes_ : 0; }

  // FAT modification time of the open file (0 when unavailable). Callers use
  // it as a staleness token beside size() — e.g. the image viewer's cache
  // generation (StandaloneImageCatalog.h viewerGeneration). The HAL call
  // takes the storage lock; do not call per-read.
  uint32_t modificationTime() { return isValid() ? file_.modificationTime() : 0; }

 private:
  HalFile file_;
  uint64_t sizeBytes_ = 0;
};

}  // namespace book
}  // namespace freeink
