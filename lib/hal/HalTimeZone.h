#pragma once

#include <cstddef>
#include <cstdint>

namespace freeink {

struct TimeZoneInfo {
  bool valid = false;
  char id[40] = "";
  int offsetMin = 0;
  bool isDst = false;
};

// Best-effort time zone detection via IP geolocation over plain HTTP
// (ipwho.is primary, ip-api.com fallback).
// Blocking (~2-10s over WiFi); returns TimeZoneInfo{valid=false} when detection fails.
TimeZoneInfo detectTimeZoneFromIp();

// Resolve the *live* UTC offset (minutes, signed) for an IANA zone id at a given
// UTC epoch, folding DST automatically from the bundled AceTime zone database.
// This is what keeps the displayed clock correct across DST transitions without
// any network call or a stored static offset.
//
// Returns true and writes `offsetMin` on success. Returns false when the id is
// unknown to the database or `ianaId` is empty/null (caller should fall back to
// its last-detected cached offset in that case).
bool resolveUtcOffsetMinutes(const char* ianaId, int64_t utcEpochSeconds, int& offsetMin);

// Resolve the zone abbreviation in effect at a given UTC epoch — "EST", "EDT",
// "PDT", "UTC", … — for an IANA zone id. Writes a NUL-terminated string of at
// most kZoneAbbrevSize bytes (including the terminator) and returns true.
// Returns false when the id is unknown/empty or the epoch is outside the
// database's coverage; `out` is left untouched in that case, so callers must
// seed it with their own fallback first.
//
// The buffer is small because AceTime's own abbreviation storage is: the TZ
// database caps abbreviations at 6 characters, so anything longer would be
// truncated anyway.
static constexpr size_t kZoneAbbrevSize = 8;
bool resolveZoneAbbreviation(const char* ianaId, int64_t utcEpochSeconds, char* out, size_t outSize);

}  // namespace freeink
