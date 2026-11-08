// Host-test fake of GfxRenderer for the TTF image fit suite: a real panel-
// shaped 1-bit framebuffer (physical panel dims, byte stride) plus the
// orientation-aware logical-dimension accessors, so DirectPixelWriter's
// transform and the engine's paint bounds run exactly as on device.
#pragma once

#include <cstdint>
#include <cstring>

class GfxRenderer {
 public:
  enum Orientation {
    Portrait,  // 480x800 logical coordinates on the 800x480 panel
    LandscapeClockwise,
    PortraitInverted,
    LandscapeCounterClockwise  // native panel orientation
  };
  enum RenderMode { BW, GRAYSCALE_MSB, GRAYSCALE_LSB, GRAYSCALE_DUAL };

  // Fake panel: X4 Pro geometry.
  uint16_t panelWidth = 800;
  uint16_t panelHeight = 480;
  Orientation orientation = Portrait;
  RenderMode mode = BW;

  // Physical framebuffer: displayWidthBytes * panelHeight, caller-owned.
  uint8_t* fb = nullptr;

  uint16_t getDisplayWidth() const { return panelWidth; }
  uint16_t getDisplayHeight() const { return panelHeight; }

  // The orientation-aware LOGICAL screen dims — the accessors the image
  // paint bounds must use (ImageBlock::render precedent).
  int getScreenWidth() const {
    return (orientation == LandscapeClockwise || orientation == LandscapeCounterClockwise) ? panelWidth : panelHeight;
  }
  int getScreenHeight() const {
    return (orientation == LandscapeClockwise || orientation == LandscapeCounterClockwise) ? panelHeight : panelWidth;
  }

  Orientation getOrientation() const { return orientation; }
  void setOrientation(const Orientation o) { orientation = o; }

  RenderMode getRenderMode() const { return mode; }
  bool grayPlanesAreAbsolute() const { return false; }
  bool imagesDitheredInBw() const { return false; }

  uint8_t* getWriteTarget() { return fb; }
  uint8_t* getDualWriteTarget() { return nullptr; }
  int getWriteOriginY() const { return 0; }
  int getWriteRows() const { return panelHeight; }
  uint16_t getDisplayWidthBytes() const { return static_cast<uint16_t>((panelWidth + 7) / 8); }

  void fillWhite() {
    if (fb != nullptr) memset(fb, 0xFF, static_cast<size_t>(getDisplayWidthBytes()) * panelHeight);
  }
};
