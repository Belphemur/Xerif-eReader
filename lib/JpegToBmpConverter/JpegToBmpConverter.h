#pragma once

#include <HalStorage.h>
#include <Memory.h>

class JPEGDEC;
class Print;
class ZipFile;

class JpegToBmpConverter {
  static bool jpegFileToBmpStreamInternal(HalFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit, bool crop = true, bool originalThresholds = false);
  // Shared decode core: jpeg is open, source-agnostic. The dual-sink tail
  // (thumbOut + its target box + the emitted-dims out-params) serves the
  // combined cover cache (design Amendment B.1); null thumbOut keeps the
  // single-sink behavior every other caller uses.
  static bool jpegToBmpStreamOpen(JPEGDEC& jpeg, Print& bmpOut, int targetWidth, int targetHeight, bool oneBit,
                                  bool crop, bool originalThresholds, Print* thumbOut = nullptr,
                                  int thumbTargetWidth = 0, int thumbTargetHeight = 0, uint16_t* outCoverW = nullptr,
                                  uint16_t* outCoverH = nullptr, uint16_t* outThumbW = nullptr,
                                  uint16_t* outThumbH = nullptr);
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
  // Dual-sink (Amendment B.1): ONE decode emits the sleep-cover 2-bit BMP
  // (coverOut, display-fit, `cropped`) and the home-thumb 1-bit BMP (thumbOut,
  // crop into thumbTargetWidth x thumbTargetHeight) — the combined cover
  // cache's two sections. The out-params receive the emitted BMP dims for the
  // section directory. Used by Epub::generateCoverCache on PSRAM builds.
  static bool jpegMemToCoverSections(uint8_t* jpegData, size_t jpegSize, Print& coverOut, Print& thumbOut,
                                     int thumbTargetWidth, int thumbTargetHeight, bool cropped, uint16_t& coverW,
                                     uint16_t& coverH, uint16_t& thumbW, uint16_t& thumbH);
  // SD-file variant of the dual-sink decode (companion cover art on SD).
  static bool jpegFileToCoverSections(HalFile& jpegFile, Print& coverOut, Print& thumbOut, int thumbTargetWidth,
                                      int thumbTargetHeight, bool cropped, uint16_t& coverW, uint16_t& coverH,
                                      uint16_t& thumbW, uint16_t& thumbH);
};
