#include "CrossPointState.h"

namespace {

void getLCG(uint32_t packedState, uint32_t packedCount, uint32_t& seed, uint32_t& state, uint16_t& indexesLeft,
            uint16_t& indexesTotal) {
  indexesLeft = packedCount >> 16;
  indexesTotal = packedCount & 0xffff;
  seed = packedState >> 16;
  state = packedState & 0xffff;
}

void setLCG(uint32_t& packedState, uint32_t& packedCount, uint32_t seedVal, uint32_t stateVal, uint16_t indexesLeftVal,
            uint16_t indexesTotalVal) {
  packedCount = (static_cast<uint32_t>(indexesLeftVal) << 16) | indexesTotalVal;
  packedState = (seedVal << 16) | (stateVal & 0xffff);
}

}  // namespace

void CrossPointState::getSleepLCG(uint32_t& seed, uint32_t& state, uint16_t& indexesLeft,
                                  uint16_t& indexesTotal) const {
  getLCG(sleepLcgState, sleepLcgCount, seed, state, indexesLeft, indexesTotal);
}

void CrossPointState::getSleepOverlayLCG(uint32_t& seed, uint32_t& state, uint16_t& indexesLeft,
                                         uint16_t& indexesTotal) const {
  getLCG(sleepOverlayLcgState, sleepOverlayLcgCount, seed, state, indexesLeft, indexesTotal);
}

void CrossPointState::setSleepLCG(uint32_t seed, uint32_t state, uint16_t indexesLeft, uint16_t indexesTotal) {
  setLCG(sleepLcgState, sleepLcgCount, seed, state, indexesLeft, indexesTotal);
}

void CrossPointState::setSleepOverlayLCG(uint32_t seed, uint32_t state, uint16_t indexesLeft, uint16_t indexesTotal) {
  setLCG(sleepOverlayLcgState, sleepOverlayLcgCount, seed, state, indexesLeft, indexesTotal);
}

void CrossPointState::toJson(JsonDocument& doc) const {
  doc["openEpubPath"] = openEpubPath;
  doc["autoPowerOffCoverBmpPath"] = autoPowerOffCoverBmpPath;
  doc["sleepLcgCount"] = sleepLcgCount;
  doc["sleepLcgState"] = sleepLcgState;
  doc["sleepOverlayLcgCount"] = sleepOverlayLcgCount;
  doc["sleepOverlayLcgState"] = sleepOverlayLcgState;
  doc["readerActivityLoadCount"] = readerActivityLoadCount;
  doc["lastSleepFromReader"] = lastSleepFromReader;
  doc["showBootScreen"] = showBootScreen;
  doc["openBookProgressPercent"] = openBookProgressPercent;
  doc["openBookTimeLeftSeconds"] = openBookTimeLeftSeconds;
  doc["openBookPagesLeft"] = openBookPagesLeft;
}

bool CrossPointState::fromJson(JsonVariantConst doc) {
  openEpubPath = doc["openEpubPath"] | "";
  autoPowerOffCoverBmpPath = doc["autoPowerOffCoverBmpPath"] | "";
  const int cachedProgress = doc["openBookProgressPercent"] | -1;
  openBookProgressPercent = (cachedProgress >= -1 && cachedProgress <= 100) ? static_cast<int8_t>(cachedProgress) : -1;
  openBookTimeLeftSeconds = doc["openBookTimeLeftSeconds"] | 0u;
  const int cachedPagesLeft = doc["openBookPagesLeft"] | -1;
  openBookPagesLeft =
      (cachedPagesLeft >= -1 && cachedPagesLeft <= INT16_MAX) ? static_cast<int16_t>(cachedPagesLeft) : -1;
  // Both cache fields shipped in one firmware change, so a state file missing
  // either predates the cache: the sleep screens may then consult the book's
  // persisted stats record for a time-left estimate.
  openBookCacheLegacy = doc["openBookTimeLeftSeconds"].isNull() && doc["openBookPagesLeft"].isNull();

  sleepLcgCount = doc["sleepLcgCount"] | static_cast<uint32_t>(0);
  sleepLcgState = doc["sleepLcgState"] | static_cast<uint32_t>(0);

  sleepOverlayLcgCount = doc["sleepOverlayLcgCount"] | static_cast<uint32_t>(0);
  sleepOverlayLcgState = doc["sleepOverlayLcgState"] | static_cast<uint32_t>(0);

  readerActivityLoadCount = doc["readerActivityLoadCount"] | static_cast<uint8_t>(0);
  lastSleepFromReader = doc["lastSleepFromReader"] | false;
  showBootScreen = doc["showBootScreen"] | true;
  return true;
}
