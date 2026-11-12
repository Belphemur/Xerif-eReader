#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
#include <HWCDC.h>
#endif

#ifdef ESP_PLATFORM
#include <freertos/task.h>
#endif

#include <string>

/*
Define ENABLE_SERIAL_LOG to enable logging
Can be set in platformio.ini build_flags or as a compile definition

Define LOG_LEVEL to control log verbosity:
0 = ERR only
1 = ERR + INF
2 = ERR + INF + DBG
If not defined, defaults to 0

If you have a legitimate need for raw Serial access (e.g., binary data,
special formatting), use the underlying logSerial object directly:
    logSerial.printf("Special case: %d\n", value);
    logSerial.write(binaryData, length);

The logSerial reference (defined below) points to the real Serial object and
won't trigger deprecation warnings.
*/

#ifndef LOG_LEVEL
#define LOG_LEVEL 0
#endif

#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
static HWCDC& logSerial = Serial;
#define LOG_SERIAL_HAS_TX_TIMEOUT 1
#else
static HardwareSerial& logSerial = Serial;
#define LOG_SERIAL_HAS_TX_TIMEOUT 0
#endif

void logPrintf(const char* level, const char* origin, const char* format, ...);

#ifdef ENABLE_SERIAL_LOG
#if LOG_LEVEL >= 0
#define LOG_ERR(origin, format, ...) logPrintf("ERR", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_ERR(origin, format, ...)
#endif

#if LOG_LEVEL >= 1
#define LOG_INF(origin, format, ...) logPrintf("INF", origin, format "\n", ##__VA_ARGS__)
#define LOG_WRN(origin, format, ...) logPrintf("WRN", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_INF(origin, format, ...)
#define LOG_WRN(origin, format, ...)
#endif

#if LOG_LEVEL >= 2
#define LOG_DBG(origin, format, ...) logPrintf("DBG", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_DBG(origin, format, ...)
#endif
#else
#define LOG_DBG(origin, format, ...)
#define LOG_ERR(origin, format, ...)
#define LOG_INF(origin, format, ...)
#define LOG_WRN(origin, format, ...)
#endif

std::string getLastLogs();
void clearLastLogs();
// Validates the RTC log state (magic word + logHead range). Returns true if
// corruption was detected (magic mismatch or logHead out of range), meaning
// logMessages is untrusted garbage. Callers should call clearLastLogs() when
// this returns true so getLastLogs() does not dump corrupt data into crash reports.
bool sanitizeLogHead();

class MySerialImpl : public Print {
 public:
  void begin(unsigned long baud) { logSerial.begin(baud); }

  // Support boolean conversion for compatibility with code like:
  //   if (Serial) or while (!Serial)
  operator bool() const { return logSerial; }

  __attribute__((deprecated("Use LOG_* macro instead"))) size_t printf(const char* format, ...);
  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buffer, size_t size) override;
  void flush() override;
  static MySerialImpl instance;
};

#ifdef Serial
#undef Serial
#endif
#define Serial MySerialImpl::instance

extern size_t g_psram_free_at_boot;

inline void logMemAt(const char* stage) {
  size_t heap_free = ESP.getFreeHeap();
  size_t psram_free = ESP.getFreePsram();
  LOG_INF("MEM", "Stage=%s HeapFree=%uKB PSRAMFree=%uKB PSRAMUsed=%uKB Core=%d", stage,
          static_cast<unsigned>(heap_free / 1024), static_cast<unsigned>(psram_free / 1024),
          static_cast<unsigned>((g_psram_free_at_boot - psram_free) / 1024), xPortGetCoreID());
}

#ifdef ESP_PLATFORM
// Log the loop task's stack high-water mark after heavy paths.
// Used for measurement-only builds to determine safe stack constants.
// Compiled out entirely unless this build actually emits LOG_DBG telemetry:
// the stack scan costs a whole-pass walk that telemetry-less builds should
// never pay (release and serial-disabled builds).
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
inline void logLoopTaskHwm(const char* stage) {
  const UBaseType_t hwm = uxTaskGetStackHighWaterMark(nullptr);
  LOG_DBG("STACK", "Stage=%s LoopHWM=%u bytes", stage, static_cast<unsigned>(hwm));
}
#else
inline void logLoopTaskHwm(const char*) {}
#endif
#else
inline void logLoopTaskHwm(const char*) {}
#endif
