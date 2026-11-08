// Host-test stub of ImageDecoderFactory: the factory's real constructor pulls
// every converter (JPEG/PNG device paths); the GIF end-to-end test drives
// GifToFramebufferConverter directly through a local instance, so the factory
// hands out that converter for GIF and refuses everything else.
#include "Epub/converters/ImageDecoderFactory.h"

#include "Epub/converters/GifToFramebufferConverter.h"

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string& imagePath) {
  static GifToFramebufferConverter gif;
  if (gif.supportsFormat(imagePath)) return &gif;
  return nullptr;
}
