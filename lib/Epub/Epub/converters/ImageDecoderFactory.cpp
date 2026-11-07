#include "ImageDecoderFactory.h"

#include <Logging.h>
#include <Memory.h>

#include <memory>
#include <mutex>
#include <string>

#include "GifToFramebufferConverter.h"
#include "JpegToFramebufferConverter.h"
#include "PngToFramebufferConverter.h"

std::unique_ptr<JpegToFramebufferConverter> ImageDecoderFactory::jpegDecoder = nullptr;
std::unique_ptr<PngToFramebufferConverter> ImageDecoderFactory::pngDecoder = nullptr;
std::unique_ptr<GifToFramebufferConverter> ImageDecoderFactory::gifDecoder = nullptr;

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string& imagePath) {
  // Lazy singleton init must be thread-safe: the image predecode worker
  // (core 0) and the loop task (core 1, page renders) can reach this
  // concurrently. The converters' decode state is call-local, so once
  // constructed the shared instances need no further locking.
  static std::mutex initMux;
  std::lock_guard<std::mutex> initLock(initMux);

  std::string ext = imagePath;
  size_t dotPos = ext.rfind('.');
  if (dotPos != std::string::npos) {
    ext = ext.substr(dotPos);
    for (auto& c : ext) {
      c = tolower(c);
    }
  } else {
    ext = "";
  }

  if (JpegToFramebufferConverter::supportsFormat(ext)) {
    if (!jpegDecoder) {
      jpegDecoder.reset(new JpegToFramebufferConverter());
    }
    return jpegDecoder.get();
  } else if (PngToFramebufferConverter::supportsFormat(ext)) {
    if (!pngDecoder) {
      pngDecoder.reset(new PngToFramebufferConverter());
    }
    return pngDecoder.get();
  } else if (GifToFramebufferConverter::supportsFormat(ext)) {
    if (!gifDecoder) {
      gifDecoder = makeUniqueNoThrow<GifToFramebufferConverter>();
      if (!gifDecoder) {
        LOG_ERR("DEC", "OOM: GIF decoder");
        return nullptr;
      }
    }
    return gifDecoder.get();
  }

  LOG_ERR("DEC", "No decoder found for image: %s", imagePath.c_str());
  return nullptr;
}

bool ImageDecoderFactory::isFormatSupported(const std::string& imagePath) { return getDecoder(imagePath) != nullptr; }
