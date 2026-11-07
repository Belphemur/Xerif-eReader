#include "OtaJson.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

namespace {

// Bounds-safe copy that always null-terminates and never overflows.
void copyStr(const char* src, char* dst, size_t cap) {
  if (cap == 0) return;
  if (!src) src = "";
  const size_t len = strlen(src);
  const size_t n = len < cap ? len : cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Decode a 64-character hex digest into 32 raw bytes. Any other length or
// non-hex character means the manifest is malformed -> hasSha=false.
bool decodeSha256Hex(const char* hex, uint8_t out[32]) {
  if (!hex || strlen(hex) != 64) return false;
  for (int i = 0; i < 32; ++i) {
    const int hi = hexVal(hex[2 * i]);
    const int lo = hexVal(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

}  // namespace

void parseOtaRelease(const char* data, size_t len, const char* firmwareAssetName, const char* manifestAssetName,
                     OtaReleaseInfo& out) {
  out = OtaReleaseInfo{};
  if (!data || len == 0) return;

  // ArduinoJson 7: the document's pool is heap-backed (malloc, returns null on
  // OOM -> NoMemory error) and bounded by the already-capped input size.
  JsonDocument doc;
  if (deserializeJson(doc, data, len) != DeserializationError::Ok) return;

  copyStr(doc["tag_name"] | "", out.tagName, sizeof(out.tagName));
  out.hasTag = out.tagName[0] != '\0';

  for (JsonObjectConst asset : doc["assets"].as<JsonArrayConst>()) {
    const char* name = asset["name"] | "";
    if (firmwareAssetName && strcmp(name, firmwareAssetName) == 0) {
      out.hasFirmware = true;
      copyStr(asset["browser_download_url"] | "", out.firmwareUrl, sizeof(out.firmwareUrl));
      out.firmwareSize = asset["size"] | 0;
    } else if (manifestAssetName && strcmp(name, manifestAssetName) == 0) {
      out.hasManifest = true;
      copyStr(asset["browser_download_url"] | "", out.manifestUrl, sizeof(out.manifestUrl));
    }
  }
}

bool parseOtaManifest(const char* data, size_t len, ManifestBoardEntry* entries, int maxEntries, int* outCount,
                      char* version, size_t versionSize) {
  if (outCount) *outCount = 0;
  if (version && versionSize > 0) version[0] = '\0';
  if (!data || len == 0 || !entries || maxEntries <= 0 || !outCount) return false;

  JsonDocument doc;
  if (deserializeJson(doc, data, len) != DeserializationError::Ok) return false;

  // `version` is optional: only write when a non-null buffer with capacity
  // is provided.
  if (version && versionSize > 0) copyStr(doc["version"] | "", version, versionSize);

  int n = 0;
  for (JsonObjectConst board : doc["boards"].as<JsonArrayConst>()) {
    if (n >= maxEntries) break;
    ManifestBoardEntry& e = entries[n];
    memset(&e, 0, sizeof(e));
    copyStr(board["board"] | "", e.board, sizeof(e.board));
    copyStr(board["url"] | "", e.url, sizeof(e.url));
    e.size = board["size"] | 0;
    e.hasSha = decodeSha256Hex(board["sha256"] | "", e.sha256);
    ++n;
  }
  *outCount = n;
  return true;
}

bool parseOtaDevManifest(const char* data, size_t len, OtaDevVersionEntry* entries, int maxEntries, int* outCount) {
  if (outCount) *outCount = 0;
  if (!data || len == 0 || !entries || maxEntries <= 0 || !outCount) return false;

  JsonDocument doc;
  if (deserializeJson(doc, data, len) != DeserializationError::Ok) return false;

  int n = 0;
  for (JsonObjectConst version : doc["versions"].as<JsonArrayConst>()) {
    if (n >= maxEntries) break;
    OtaDevVersionEntry& e = entries[n];
    e = OtaDevVersionEntry{};
    copyStr(version["name"] | "", e.name, sizeof(e.name));
    copyStr(version["url"] | "", e.url, sizeof(e.url));
    copyStr(version["date"] | "", e.date, sizeof(e.date));
    e.hasSha = decodeSha256Hex(version["sha256"] | "", e.sha256);
    ++n;
  }
  *outCount = n;
  return true;
}

bool otaIsVersionNewer(const char* currentVersion, const char* latestTag) {
  if (!currentVersion || !latestTag) return false;

  // Release tags are GitHub "v"-prefixed ("v1.9.0"); the firmware version
  // carries a per-board/pre-release suffix ("1.9.0-x4pro", "1.9.0-rc1").
  // Compare only the leading N.N.N segments of both.
  if (*latestTag == 'v' || *latestTag == 'V') ++latestTag;

  char current[32];
  const char* dash = strchr(currentVersion, '-');
  const size_t currentLen = dash ? static_cast<size_t>(dash - currentVersion) : strlen(currentVersion);
  if (currentLen == 0 || currentLen >= sizeof(current)) return false;
  memcpy(current, currentVersion, currentLen);
  current[currentLen] = '\0';

  int currentMajor = 0, currentMinor = 0, currentPatch = 0;
  int latestMajor = 0, latestMinor = 0, latestPatch = 0;
  if (sscanf(current, "%d.%d.%d", &currentMajor, &currentMinor, &currentPatch) != 3) return false;
  if (sscanf(latestTag, "%d.%d.%d", &latestMajor, &latestMinor, &latestPatch) != 3) return false;

  if (latestMajor != currentMajor) return latestMajor > currentMajor;
  if (latestMinor != currentMinor) return latestMinor > currentMinor;
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // Equal semver triples: an RC build is pre-release, so the same-version
  // stable tag counts as newer.
  return strstr(currentVersion, "-rc") != nullptr;
}
