// Host-test stub of HalDisplay.h — DirectPixelWriter.h includes it for the
// panel-geometry constants; the suite runs at X4 Pro panel geometry.
#pragma once

#include <cstdint>

class HalDisplay {
 public:
  static constexpr uint16_t DISPLAY_WIDTH = 800;
  static constexpr uint16_t DISPLAY_HEIGHT = 480;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;
};
