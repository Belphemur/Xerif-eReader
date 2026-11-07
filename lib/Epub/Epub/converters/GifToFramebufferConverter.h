#pragma once

#include "ImageToFramebufferDecoder.h"

class GifToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  static bool getDimensionsStatic(const std::string& imagePath, ImageDimensions& out);

  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) override;
  // PSRAM-buffer variant (TTF image engine): shared decode core, memory open.
  bool decodeToFramebuffer(uint8_t* data, size_t size, GfxRenderer& renderer, const RenderConfig& config) override;

  bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const override {
    return getDimensionsStatic(imagePath, dims);
  }

  static bool supportsFormat(const std::string& extension);
  const char* getFormatName() const override { return "GIF"; }

 private:
  // Shared decode core of both entry points; mem != nullptr selects the
  // PSRAM-buffer AnimatedGIF open (no IO callbacks needed for RAM sources).
  static bool decodeGifSource(const char* imagePath, uint8_t* mem, size_t memSize, GfxRenderer& renderer,
                              const RenderConfig& config);
};