#pragma once

#include <cstddef>
#include <cstdint>

#include "OtaJsonManifest.h"

// ArduinoJson-based parsers for the OTA update path. Both take the whole
// response body (already bounded by the fetch-size caps in OtaUpdater) and
// produce fixed-size, overflow-truncated outputs.

// Result of parsing a GitHub "latest release" JSON body.
struct OtaReleaseInfo {
  bool hasTag = false;
  char tagName[32] = {0};
  bool hasFirmware = false;
  char firmwareUrl[512] = {0};
  size_t firmwareSize = 0;
  bool hasManifest = false;
  char manifestUrl[512] = {0};
};

// Parse a GitHub release JSON body. `firmwareAssetName` and
// `manifestAssetName` are matched exactly against each asset's `name` field;
// their `browser_download_url`/`size` are captured. `out` is reset first, and
// left zeroed when `data` is not valid JSON.
void parseOtaRelease(const char* data, size_t len, const char* firmwareAssetName, const char* manifestAssetName,
                     OtaReleaseInfo& out);

// Parse a signed manifest.json body into `entries` (at most `maxEntries`).
// On success returns true and `*outCount` holds the number of boards parsed.
// `version` is optional: if `version` is non-null and `versionSize > 0`, the
// parsed manifest version is written; otherwise skipped. Returns false on
// malformed JSON or null required arguments.
bool parseOtaManifest(const char* data, size_t len, ManifestBoardEntry* entries, int maxEntries, int* outCount,
                      char* version, size_t versionSize);

// One dev-channel build entry (design §4). POD, copied by value; fixed-size
// overflow-truncated strings like the rest of this module.
struct OtaDevVersionEntry {
  bool hasSha = false;
  char name[64] = {0};   // asset file name, e.g. "xerif-dev-<sha>-x4pro.bin"
  char url[192] = {0};   // browser_download_url
  char date[32] = {0};   // build date (ISO 8601), display only
  uint8_t sha256[32] = {0};
};

// Parse the dev channel's manifest.json ({"versions":[{name,url,sha256,date},...]
// in newest-first order). Returns false on malformed JSON or null required
// arguments; entries beyond `maxEntries` are dropped.
bool parseOtaDevManifest(const char* data, size_t len, OtaDevVersionEntry* entries, int maxEntries, int* outCount);

// True when `latestTag` (GitHub release tag, e.g. "v1.9.0") is newer than
// `currentVersion` (firmware version string, e.g. "1.8.0-x4pro"). Compares
// only the leading N.N.N segments; a "-rc" pre-release is older than a
// same-version stable tag.
bool otaIsVersionNewer(const char* currentVersion, const char* latestTag);
