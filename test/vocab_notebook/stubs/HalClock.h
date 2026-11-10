#pragma once

struct HalClock {
  // The notebook's append path seeds a timestamp from the RTC; a disabled
  // clock is the documented fallback (timestamp 0). The read/delete sides
  // never touch the clock.
  bool getDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) const {
    year = 0;
    month = day = hour = minute = 0;
    return false;
  }
};
inline HalClock halClock;
