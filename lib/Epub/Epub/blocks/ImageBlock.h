#pragma once
#include <HalStorage.h>
#include <Memory.h>

#include <cstddef>
#include <memory>
#include <string>

#include "Block.h"

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  const std::string& getSrcPath() const { return srcPath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  bool hasValidCache() const;
  bool needsDecode() const;
  // Shared single-writer helpers (ImageDecodeGate consumers): the .pxc path
  // derived from an image path, and a cache-validity check for a block's
  // dimensions without an ImageBlock instance (the predecode worker holds
  // only the sighting's paths and sizes).
  static std::string cachePathFor(const std::string& imagePath);
  static bool hasValidCacheFor(const std::string& imagePath, int16_t width, int16_t height);
  void renderPlaceholder(GfxRenderer& renderer, int x, int y) const;
  static void clearRenderFailures();

  // A page render draws its image up to ~13 times (BW double-refresh plus every
  // grayscale band pass), and each draw streams the whole .pxc off SD. The
  // first draw caches the pixel payload in RAM (chunked, heap-gated, falls back
  // to streaming when it doesn't fit); the reader calls this when the page
  // render completes so nothing stays resident between pages.
  static void releaseRenderCache();

  // Lazy extraction hook: the section build only header-probes images for their
  // dimensions; the file at imagePath is extracted out of the book on first
  // render, via this callback (function pointer + context, not std::function —
  // this is render-loop code). Registered by the reader activity that owns the
  // Epub, cleared on its exit.
  using ExtractFn = bool (*)(void* ctx, const char* srcPath, const char* destPath);
  static void setExtractor(void* ctx, ExtractFn fn);

  // PSRAM staging hook: inflates the compressed image straight into a pool
  // (PSRAM) buffer so the decode never stages compressed bytes on SD. Same
  // callback discipline as ExtractFn. Registered by the reader activity.
  using PsramExtractFn = PoolBytes (*)(void* ctx, const char* srcPath, size_t& outSize);
  static void setPsramExtractor(void* ctx, PsramExtractFn fn);

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y);
  bool serialize(HalFile& file);
  static std::unique_ptr<ImageBlock> deserialize(HalFile& file);

 private:
  std::string imagePath;
  std::string srcPath;  // book-internal source href; empty once known-extracted
  int16_t width;
  int16_t height;

  static void* extractCtx;
  static ExtractFn extractFn;
  static void* psramExtractCtx;
  static PsramExtractFn psramExtractFn;

  // PSRAM decode path: extract into a pool buffer and decode from memory.
  // Returns false when anything fails so the caller falls back to the SD path.
  bool renderFromPsram(GfxRenderer& renderer, int x, int y, const std::string& cachePath);
};
