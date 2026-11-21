#pragma once

// Host stub: cover-image conversion is only reached from generateCoverBmp()/
// generateThumbBmp(), which this test never calls. Signatures must line up
// with the call sites in Epub.cpp so it still links; definitions live in
// ParserLinkStubs.cpp.

#include <HalStorage.h>
#include <Memory.h>

class JpegToBmpConverter {
 public:
  static bool jpegFileToBmpStream(HalFile& jpegFile, HalFile& bmpFile, bool cropped, bool originalThresholds = false);
  static bool jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, HalFile& bmpFile, int targetWidth, int targetHeight);
  static bool jpegMemToBmpStream(uint8_t* jpegData, size_t jpegSize, Print& bmpOut, bool crop = true,
                                 bool originalThresholds = false);
  static bool jpegMemTo1BitBmpStreamWithSize(uint8_t* jpegData, size_t jpegSize, Print& bmpOut, int targetWidth,
                                             int targetHeight);
  // Dual-sink entries (design Amendment B.1): reached only from
  // generateCoverCache(), which these tests never call. The stub HalFile
  // derives Print, so the real Print& signatures line up directly.
  static bool jpegFileToCoverSections(HalFile& jpegFile, Print& coverOut, Print& thumbOut, int thumbTargetWidth,
                                      int thumbTargetHeight, bool cropped, uint16_t& coverW, uint16_t& coverH,
                                      uint16_t& thumbW, uint16_t& thumbH);
  static bool jpegMemToCoverSections(uint8_t* jpegData, size_t jpegSize, Print& coverOut, Print& thumbOut,
                                     int thumbTargetWidth, int thumbTargetHeight, bool cropped, uint16_t& coverW,
                                     uint16_t& coverH, uint16_t& thumbW, uint16_t& thumbH);
};

class PngToBmpConverter {
 public:
  static bool pngFileToBmpStream(HalFile& pngFile, HalFile& bmpFile, bool cropped, bool originalThresholds = false);
  static bool pngFileTo1BitBmpStreamWithSize(HalFile& pngFile, HalFile& bmpFile, int targetWidth, int targetHeight);
  static bool pngMemToBmpStream(uint8_t* pngData, size_t pngSize, Print& bmpOut, bool crop = true,
                                bool originalThresholds = false);
  static bool pngMemTo1BitBmpStreamWithSize(uint8_t* pngData, size_t pngSize, Print& bmpOut, int targetWidth,
                                            int targetHeight);
  // Dual-sink entries (design Amendment B.1) — see the JPEG stub's note.
  static bool pngFileToCoverSections(HalFile& pngFile, Print& coverOut, Print& thumbOut, int thumbTargetWidth,
                                     int thumbTargetHeight, bool cropped, uint16_t& coverW, uint16_t& coverH,
                                     uint16_t& thumbW, uint16_t& thumbH);
  static bool pngMemToCoverSections(uint8_t* pngData, size_t pngSize, Print& coverOut, Print& thumbOut,
                                    int thumbTargetWidth, int thumbTargetHeight, bool cropped, uint16_t& coverW,
                                    uint16_t& coverH, uint16_t& thumbW, uint16_t& thumbH);
};

class GifToBmpConverter {
 public:
  static bool gifFileToBmpStream(HalFile& gifFile, Print& bmpOut, bool crop = true, bool originalThresholds = false);
  static bool gifFileTo1BitBmpStreamWithSize(HalFile& gifFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
};
