// Link stubs for the real Epub/Section/parsers translation units compiled into
// this test. Mirrors test/chapter_html_slim_parser/ParserLinkStubs.cpp, with
// two differences required by the section build path:
//   1. Page/PageLine/PageImage/PageHorizontalRule::serialize() actually write
//      a marker and return true — Section::commitBuildFile() rejects a build
//      whose page LUT contains a zero file offset, which is what the chapter
//      parser test's `return false` stubs would produce.
//   2. Hyphenator::setPreferredLanguage() (called from Section::startBuild).

#include <Epub/Page.h>
#include <Epub/TokenBoundary.h>
#include <Epub/blocks/ImageBlock.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/converters/ImageToFramebufferDecoder.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <ImageConverters.h>

const char* lookupHtmlEntity(const char*, size_t) { return nullptr; }

#include <BidiUtils.h>

bool isExplicitHyphen(uint32_t) { return false; }
bool isSoftHyphen(uint32_t) { return false; }

std::vector<Hyphenator::BreakInfo> Hyphenator::breakOffsets(const std::string&, bool) { return {}; }
void Hyphenator::setPreferredLanguage(const std::string&) {}

// Page/PageLine/PageImage/PageHorizontalRule, TextBlock, ImageBlock and the
// BidiUtils helpers link from the real lib sources (see CMakeLists.txt); only
// the units the section build never pulls in are stubbed here.

bool ImageDecoderFactory::isFormatSupported(const std::string&) { return false; }
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return nullptr; }
bool ImageToFramebufferDecoder::validateAndStoreDimensions(int64_t, int64_t, ImageDimensions&, const char*) {
  return false;
}

// Host stubs for the cover-image converters (declared in the JpegToBmpConverter.h /
// PngToBmpConverter.h stub headers); Epub.cpp's generateCoverBmp/generateThumbBmp
// are never called by this test.
bool JpegToBmpConverter::jpegFileToBmpStream(HalFile&, HalFile&, bool, bool) { return false; }
bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(HalFile&, HalFile&, int, int) { return false; }
bool JpegToBmpConverter::jpegMemToBmpStream(uint8_t*, size_t, Print&, bool, bool) { return false; }
bool JpegToBmpConverter::jpegMemTo1BitBmpStreamWithSize(uint8_t*, size_t, Print&, int, int) { return false; }
bool PngToBmpConverter::pngFileToBmpStream(HalFile&, HalFile&, bool, bool) { return false; }
bool PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(HalFile&, HalFile&, int, int) { return false; }
bool PngToBmpConverter::pngMemToBmpStream(uint8_t*, size_t, Print&, bool, bool) { return false; }
bool PngToBmpConverter::pngMemTo1BitBmpStreamWithSize(uint8_t*, size_t, Print&, int, int) { return false; }
bool JpegToBmpConverter::jpegFileToCoverSections(HalFile&, Print&, Print&, int, int, bool, uint16_t&, uint16_t&,
                                                 uint16_t&, uint16_t&) {
  return false;
}
bool JpegToBmpConverter::jpegMemToCoverSections(uint8_t*, size_t, Print&, Print&, int, int, bool, uint16_t&, uint16_t&,
                                                uint16_t&, uint16_t&) {
  return false;
}
bool PngToBmpConverter::pngFileToCoverSections(HalFile&, Print&, Print&, int, int, bool, uint16_t&, uint16_t&,
                                               uint16_t&, uint16_t&) {
  return false;
}
bool PngToBmpConverter::pngMemToCoverSections(uint8_t*, size_t, Print&, Print&, int, int, bool, uint16_t&, uint16_t&,
                                              uint16_t&, uint16_t&) {
  return false;
}
bool GifToBmpConverter::gifFileToBmpStream(HalFile&, Print&, bool, bool) { return false; }
bool GifToBmpConverter::gifFileTo1BitBmpStreamWithSize(HalFile&, Print&, int, int) { return false; }
