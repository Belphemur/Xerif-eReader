// Host-test stub of Epub.h: TtfImageEngine.cpp needs only the PSRAM
// extraction member for the GIF/BMP branch (the GIF coverage tests drive the
// converter directly; decodeAndCache is not exercised against these stubs,
// so the body is a refusal). The real header drags the whole parser stack.
#pragma once

#include <cstddef>
#include <cstdint>

class Epub {
 public:
  uint8_t* extractItemToPsram(const char* href, size_t* outSize) {
    (void)href;
    if (outSize != nullptr) *outSize = 0;
    return nullptr;
  }
};
