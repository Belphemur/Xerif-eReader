#pragma once

#include <HalStorage.h>

class Print;

class PngToBmpConverter {
  static bool pngFileToBmpStreamInternal(HalFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight,
                                         bool oneBit, bool crop = true, bool originalThresholds = false);
  // Memory-source attach + decode; both memory wrappers delegate here.
  static bool pngMemToBmpStreamInternal(uint8_t* pngData, size_t pngSize, Print& bmpOut, int targetWidth,
                                        int targetHeight, bool oneBit, bool crop, bool originalThresholds);

 public:
  static bool pngFileToBmpStream(HalFile& pngFile, Print& bmpOut, bool crop = true, bool originalThresholds = false);
  static bool pngFileTo1BitBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  // Memory-backed variant for PSRAM staging: the view satisfies the read/
  // seek surface the chunk walker consumes.
  static bool pngMemToBmpStream(uint8_t* pngData, size_t pngSize, Print& bmpOut, bool crop = true,
                                bool originalThresholds = false);
  // Memory-backed 1-bit variant with explicit target size (thumbnails).
  static bool pngMemTo1BitBmpStreamWithSize(uint8_t* pngData, size_t pngSize, Print& bmpOut, int targetMaxWidth,
                                            int targetMaxHeight);
};
