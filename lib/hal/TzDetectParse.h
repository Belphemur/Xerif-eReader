#pragma once

#include <ArduinoJson.h>

#include <cctype>
#include <cstdio>
#include <string>

#include "HalTimeZone.h"

// Pure, host-testable parsers and validators for the IP-geolocation time-zone
// detection in HalTimeZone.cpp. All responses arrive over plain HTTP, so the
// parsers only see JSON text and touch no transport state.
namespace tzdetect {

// Tamper mitigation: accept only ids shaped like IANA zone names — 1 to 3
// segments of [A-Za-z0-9_+-] separated by '/' (e.g. "America/Toronto",
// "Etc/GMT+5", "UTC"), no empty segments, no leading/trailing slash, capped at
// sizeof(TimeZoneInfo::id)-1 characters. The id is only ever fed to AceTime's
// zone-name lookup and persisted in settings JSON, so a forged response can
// yield a wrong time zone, never a security breach.
constexpr size_t kMaxIanaIdLen = 39;  // TimeZoneInfo::id holds 40 with NUL.

inline bool isValidIanaZoneId(const char* id) {
  if (id == nullptr || id[0] == '\0') return false;
  size_t len = 0;
  size_t segments = 1;
  char prev = '\0';
  for (const char* p = id; *p != '\0'; ++p) {
    const char c = *p;
    if (c == '/') {
      if (prev == '\0' || prev == '/') return false;  // no leading/trailing '//'
      ++segments;
      if (segments > 3) return false;
    } else if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '+' || c == '-')) {
      return false;
    }
    prev = c;
    ++len;
    if (len > kMaxIanaIdLen) return false;
  }
  return prev != '/';  // no trailing slash
}

// ipwho.is with ?fields=success,timezone: the same nested shape the full
// response uses. Kept compatible with the full (no-fields) response, which
// also carries "success".
inline bool parseIpWhoIs(const std::string& body, freeink::TimeZoneInfo& out) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    return false;
  }
  if (!doc["success"].as<bool>()) {
    return false;
  }
  const char* id = doc["timezone"]["id"].as<const char*>();
  if (!id || !*id) {
    return false;
  }
  snprintf(out.id, sizeof(out.id), "%s", id);
  out.offsetMin = doc["timezone"]["offset"].as<int>() / 60;
  out.isDst = doc["timezone"]["is_dst"].as<bool>();
  out.valid = true;
  return true;
}

// ip-api.com with ?fields=timezone: a flat string only. The response carries
// no offset or DST flag — the AceTime zone resolution already computes the
// live offset from the id (clockEffectiveOffsetMin), so defaults here are
// correct and the cached settings values are only an offline fallback.
inline bool parseIpApi(const std::string& body, freeink::TimeZoneInfo& out) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    return false;
  }
  const char* id = doc["timezone"].as<const char*>();
  if (!id || !*id) {
    return false;
  }
  snprintf(out.id, sizeof(out.id), "%s", id);
  out.offsetMin = 0;
  out.isDst = false;
  out.valid = true;
  return true;
}

}  // namespace tzdetect
