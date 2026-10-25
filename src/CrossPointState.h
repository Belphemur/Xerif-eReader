#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

class CrossPointState : public PersistableStore<CrossPointState> {
  CrossPointState() = default;

  friend class PersistableStore<CrossPointState>;

 public:
  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;

  std::string openEpubPath;
  // Cover BMP shown on the auto power off shutdown screen; set when entering
  // deep sleep with the shutdown timer armed, cleared after rendering.
  std::string autoPowerOffCoverBmpPath;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};
  uint8_t recentSleepPos = 0;
  uint8_t recentSleepFill = 0;
  uint16_t recentOverlaySleepImages[SLEEP_RECENT_COUNT] = {};
  uint8_t recentOverlaySleepPos = 0;
  uint8_t recentOverlaySleepFill = 0;
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  bool showBootScreen = true;
  // Reading progress (0-100, -1 unknown) of the book in openEpubPath, seeded by
  // the reader at exit so the cover sleep/shutdown screens skip the SD
  // metadata reload during those transitions.
  int8_t openBookProgressPercent = -1;
  // Estimated reading time left (seconds; 0 = unknown) and whole-book pages
  // left (-1 = unknown) for the book in openEpubPath — exit-time snapshots that
  // ride the same cache as openBookProgressPercent so the sleep/shutdown
  // progress line renders without touching SD. Never regenerated during
  // reading; refreshed only when the reader exits and saves stats.
  uint32_t openBookTimeLeftSeconds = 0;
  int16_t openBookPagesLeft = -1;

  static const char* getFilePath() { return "/.crosspoint/state.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;
  bool isRecentOverlaySleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);
  void pushRecentOverlaySleep(uint16_t idx);
};

#define APP_STATE CrossPointState::getInstance()
