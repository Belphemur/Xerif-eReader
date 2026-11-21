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
  // Dual-sink (Amendment B.1): ONE decode emits the sleep-cover 2-bit BMP
  // (coverOut, display-fit, `cropped`) and the home-thumb 1-bit BMP (thumbOut,
  // crop into thumbTargetWidth x thumbTargetHeight) — the combined cover
  // cache's two sections. Used by Epub::generateCoverCache on PSRAM builds.
  static bool pngMemToCoverSections(uint8_t* pngData, size_t pngSize, Print& coverOut, Print& thumbOut,
                                    int thumbTargetWidth, int thumbTargetHeight, bool cropped, uint16_t& coverW,
                                    uint16_t& coverH, uint16_t& thumbW, uint16_t& thumbH);
  // SD-file variant of the dual-sink decode (companion cover art on SD).
  static bool pngFileToCoverSections(HalFile& pngFile, Print& coverOut, Print& thumbOut, int thumbTargetWidth,
                                     int thumbTargetHeight, bool cropped, uint16_t& coverW, uint16_t& coverH,
                                     uint16_t& thumbW, uint16_t& thumbH);
};
