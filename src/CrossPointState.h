#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

class CrossPointState : public PersistableStore<CrossPointState> {
  CrossPointState() = default;

  friend class PersistableStore<CrossPointState>;

 public:
  static constexpr uint8_t SLEEP_AVOID_REPEAT_LOOKBACK = 5;

  std::string openEpubPath;
  // Cover BMP shown on the auto power off shutdown screen; set when entering
  // deep sleep with the shutdown timer armed, cleared after rendering.
  std::string autoPowerOffCoverBmpPath;
  uint32_t sleepLcgCount = 0;
  uint32_t sleepLcgState = 0;
  uint32_t sleepOverlayLcgCount = 0;
  uint32_t sleepOverlayLcgState = 0;
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
  // True when the loaded state file predates the cache triple above (both
  // fields absent): the only state where the sleep screens' stats-record
  // fallback for time-left may run. Derived at load time, never serialized —
  // the first post-upgrade save writes the fields, so the flag clears itself;
  // the reader-entry reset sites also clear it once a session has begun.
  bool openBookCacheLegacy = false;

  static const char* getFilePath() { return "/.crosspoint/state.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  void getSleepLCG(uint32_t& seed, uint32_t& state, uint16_t& indexesLeft, uint16_t& indexesTotal) const;
  void getSleepOverlayLCG(uint32_t& seed, uint32_t& state, uint16_t& indexesLeft, uint16_t& indexesTotal) const;

  void setSleepLCG(uint32_t seed, uint32_t state, uint16_t indexesLeft, uint16_t indexesTotal);
  void setSleepOverlayLCG(uint32_t seed, uint32_t state, uint16_t indexesLeft, uint16_t indexesTotal);
};

#define APP_STATE CrossPointState::getInstance()
