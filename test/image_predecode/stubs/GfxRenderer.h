#pragma once

#include <cstdint>
#include <cstring>

#include "HalDisplay.h"

enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

// Decode-surface mirror of lib/GfxRenderer (docs/design/2026-11-06-image-
// predecode.md §3.3): the converters and DirectPixelWriter read exactly this
// accessor set. Semantics mirror the real renderer: portrait-native panel,
// orientation-aware logical screen, strip-target write redirection.
class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB, GRAYSCALE_DUAL };
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };

  explicit GfxRenderer(HalDisplay&) : frameBuffer_(fbStorage_) {}

  // Test controls reproducing a foreground render pass's configuration.
  void setRenderMode(const RenderMode mode) { renderMode_ = mode; }
  void setOrientation(const Orientation orientation) { orientation_ = orientation; }
  void setDitherImagesInBw(const bool dither) { ditherBw_ = dither; }
  void beginStripTarget(uint8_t* buf, const int y0, const int rows, uint8_t* dual = nullptr) {
    stripBuf_ = buf;
    stripY0_ = y0;
    stripRows_ = rows;
    dualBuf_ = dual;
    stripActive_ = true;
  }
  void endStripTarget() { stripActive_ = false; }

  uint8_t* getWriteTarget() const { return stripActive_ ? stripBuf_ : frameBuffer_; }
  uint8_t* getDualWriteTarget() const { return stripActive_ ? dualBuf_ : nullptr; }
  int getWriteOriginY() const { return stripActive_ ? stripY0_ : 0; }
  int getWriteRows() const { return stripActive_ ? stripRows_ : static_cast<int>(panelHeight_); }
  RenderMode getRenderMode() const { return renderMode_; }
  bool grayPlanesAreAbsolute() const { return false; }
  bool imagesDitheredInBw() const { return ditherBw_; }
  uint16_t getDisplayWidth() const { return panelWidth_; }
  uint16_t getDisplayHeight() const { return panelHeight_; }
  uint16_t getDisplayWidthBytes() const { return panelWidthBytes_; }
  Orientation getOrientation() const { return orientation_; }

  int getScreenWidth() const {
    switch (orientation_) {
      case Portrait:
      case PortraitInverted:
        return panelHeight_;  // mirrored from GfxRenderer::getScreenWidth
      case LandscapeClockwise:
      case LandscapeCounterClockwise:
        return panelWidth_;
    }
    return panelHeight_;
  }
  int getScreenHeight() const {
    switch (orientation_) {
      case Portrait:
      case PortraitInverted:
        return panelWidth_;
      case LandscapeClockwise:
      case LandscapeCounterClockwise:
        return panelHeight_;
    }
    return panelWidth_;
  }

  // Portrait-native panel geometry, scaled down from the 480x800 X4 panel
  // (the mapping rules are what the decode path sees, not the size).
  static constexpr uint16_t panelWidth_ = 64;
  static constexpr uint16_t panelHeight_ = 40;
  static constexpr uint16_t panelWidthBytes_ = panelWidth_ / 8;

 private:
  uint8_t fbStorage_[static_cast<size_t>(panelHeight_) * panelWidthBytes_] = {};
  uint8_t* frameBuffer_ = nullptr;
  RenderMode renderMode_ = BW;
  Orientation orientation_ = Portrait;
  bool ditherBw_ = false;
  bool stripActive_ = false;
  uint8_t* stripBuf_ = nullptr;
  uint8_t* dualBuf_ = nullptr;
  int stripY0_ = 0;
  int stripRows_ = 0;
};
