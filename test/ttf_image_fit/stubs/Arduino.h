// Host-test stub of Arduino.h — a fake millis clock that delay() advances,
// so ImageDecodeGate's bounded foreground wait is testable without real time.
#pragma once

#include <cstdint>

extern uint32_t g_fakeMillis;

inline uint32_t millis() { return g_fakeMillis; }
inline void delay(uint32_t ms) { g_fakeMillis += ms; }

// FreeRTOS yield shim for ImageToFramebufferDecoder::yieldDuringDecode.
inline void vTaskDelay(uint32_t) {}
