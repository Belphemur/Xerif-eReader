#pragma once

#include <EpdFontFamily.h>
#include <FontCacheManager.h>
#include <GfxRenderer/FontVariantProvider.h>

#include <deque>
#include <string>

namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}

enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

class GfxRenderer {
 public:
  enum class TextMeasureMode { Layout, Rendered };
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB, GRAYSCALE_DUAL };
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  // The fixture has no framebuffer to lend; image probes run as without a loan.
  class FrameBufferLoan {
   public:
    explicit FrameBufferLoan(GfxRenderer&) {}
    void end() {}
  };
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  // Stub font cache: Section::startBuild null-checks then releases caches;
  // there is nothing to release in the host test.
  FontCacheManager* getFontCacheManager() const { return nullptr; }
  bool isFontCacheScanning() const { return false; }
  int getLineHeight(int, float = 1.0f) const { return 16; }
  // DirectPixelWriter queries the BW-dither image mode; the host fixture keeps it off.
  bool imagesDitheredInBw() const { return false; }
  int getFontAscenderSize(int) const { return 12; }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 4; }
  // Fixture metrics: every glyph is 8 px wide, a space is 4 px, kerning is zero.
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style, int8_t tracking = 0,
                      BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO,
                      TextMeasureMode = TextMeasureMode::Layout) const {
    int width = 0;
    while (*text++) width += 8;
    return width;
  }
  int getKerning(int, uint32_t, uint32_t, EpdFontFamily::Style, int8_t tracking = 0) const { return 0; }
  int getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 4; }
  bool isSdCardFont(int) const { return false; }
  void ensureSdCardFontReady(int, const char*, uint8_t) const {}
  void ensureSdCardFontReady(int, const char* const*, const size_t*, size_t, bool, bool, uint8_t,
                             bool includeSmallCaps = false) const {}
  // Drop-cap fixture: scaled glyph advance is 8 px * scale256/256.
  int drawScaledCodepoint(int, uint32_t, EpdFontFamily::Style, int, int, int scale256) const {
    return 8 * scale256 / 256;
  }
  // Drop-cap metrics fixture: 8 px advance (12.4 fixed point), 10 px top bearing.
  bool getCodepointMetrics(int, uint32_t, EpdFontFamily::Style, int32_t& advanceFP, int& top) const {
    advanceFP = 8 << 4;
    top = 10;
    return true;
  }
  void drawLine(int, int, int, int, bool = true) const {}
  void drawLine(int, int, int, int, int, bool) const {}
  void fillRect(int, int, int, int, bool = true) const {}
  void fillRectDither(int, int, int, int, Color) const {}
  void drawText(int, int, int, const char*, bool, EpdFontFamily::Style,
                BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO, int8_t = 0) const {}
  void setFontVariantProvider(const FontVariantProvider& provider) { fontVariantProvider_ = provider; }
  // Variant ids resolve through the provider; without one every scale keeps the base font.
  int resolveFontVariant(int fontId, float scale) const { return fontVariantProvider_.resolveFont(fontId, scale); }
  bool ensureFontLoaded(int fontId) const {
    return fontVariantProvider_.load && fontVariantProvider_.load(fontVariantProvider_.ctx, fontId);
  }
  // DirectPixelWriter-facing surface: ImageBlock compiles it, but the fixture never
  // renders an image, so every accessor returns a static no-framebuffer answer.
  uint8_t* getWriteTarget() const { return nullptr; }
  uint8_t* getDualWriteTarget() const { return nullptr; }
  int getWriteOriginY() const { return 0; }
  int getWriteRows() const { return 0; }
  RenderMode getRenderMode() const { return BW; }
  bool grayPlanesAreAbsolute() const { return true; }
  // Develop's #4160 image-grayscale toggle (upstream port 5d1142c1) added the
  // member to lib/GfxRenderer without updating this fixture; DirectPixelWriter
  // unconditionally reads it in init(), so the host build broke on CI.
  bool imagesDitheredInBw() const { return false; }
  uint16_t getDisplayWidthBytes() const { return 0; }
  uint16_t getDisplayWidth() const { return 480; }
  uint16_t getDisplayHeight() const { return 800; }
  Orientation getOrientation() const { return Portrait; }
  // No strip is ever active in the fixture: every rect "intersects".
  bool glyphIntersectsStrip(int, int, int, int) const { return true; }
  void preserveImagePolarity(int, int, int, int) const {}

 private:
  FontVariantProvider fontVariantProvider_;
};
