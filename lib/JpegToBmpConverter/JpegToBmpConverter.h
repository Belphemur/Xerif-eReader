#pragma once

#include <HalStorage.h>
#include <Memory.h>

class JPEGDEC;
class Print;
class ZipFile;

class JpegToBmpConverter {
  static bool jpegFileToBmpStreamInternal(HalFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit, bool crop = true, bool originalThresholds = false);
  // Shared decode core: jpeg is open, source-agnostic.
  static bool jpegToBmpStreamOpen(JPEGDEC& jpeg, Print& bmpOut, int targetWidth, int targetHeight, bool oneBit,
                                  bool crop, bool originalThresholds);
  // Memory-source open + decode; both memory wrappers delegate here.
  static bool jpegMemToBmpStreamInternal(uint8_t* jpegData, size_t jpegSize, Print& bmpOut, int targetWidth,
                                         int targetHeight, bool oneBit, bool crop, bool originalThresholds);

 public:
  static bool jpegFileToBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop = true, bool originalThresholds = false);
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
  static bool jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                              int targetMaxHeight);
  // Memory-backed variant for PSRAM staging: decode from a pool buffer.
  static bool jpegMemToBmpStream(uint8_t* jpegData, size_t jpegSize, Print& bmpOut, bool crop = true,
                                 bool originalThresholds = false);
  // Memory-backed 1-bit variant with explicit target size (thumbnails).
  static bool jpegMemTo1BitBmpStreamWithSize(uint8_t* jpegData, size_t jpegSize, Print& bmpOut, int targetMaxWidth,
                                             int targetMaxHeight);
};
