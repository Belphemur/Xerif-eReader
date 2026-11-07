#pragma once

#include <cstddef>
#include <cstdint>

#include "OtaJson.h"

// Dev OTA channel client (design §4/§5). Fetches the rolling `dev` prerelease
// from the OTA repo, parses its unsigned manifest (SHA-256 pinned only), and
// installs a dev build into the dev slot (app1) through the shared
// FirmwareFlasher raw-write path.
//
// Compiled into EVERY build: the running-dev dance's boot hook (design §5)
// runs in whatever slot boots after the dance reboot, and the stable slot is
// normally a release build with the dev-channel UI compiled out. Only the
// dev-channel UI entry is gated by CROSSPOINT_DEV_OTA; this machinery is
// dormant in release builds (the pending-install flag is only ever written by
// a dev build).
namespace dev_ota {

enum class Error {
  OK = 0,
  HTTP_ERROR,        // release JSON / manifest / firmware fetch failed
  JSON_PARSE_ERROR,  // release JSON or manifest malformed
  NO_MANIFEST,       // dev release carries no manifest.json asset
  NO_VERSIONS,       // manifest parsed but lists no version
  SHA_MISMATCH,      // downloaded bytes differ from the manifest digest
  NO_DEV_SLOT,       // partition table has no app1
  FLASH_FAILED,      // validation or write failed (see FirmwareFlasher logs)
};

// CI publishes the newest 3 dev builds (design §4).
constexpr int kMaxVersions = 3;

// Fixed-capacity manifest buffer: one activity-held copy of the dev versions
// (POD array, no heap, no owning pointers).
struct EntryBuffer {
  OtaDevVersionEntry items[kMaxVersions];
  int count = 0;
};

using ProgressCallback = void (*)(void* ctx, int percent, bool writing);

// Fetch the dev manifest into `entries` (at most `maxEntries`, newest first).
Error fetchDevManifest(OtaDevVersionEntry* entries, int maxEntries, int* outCount);

// Download `entry`'s image, verify its SHA-256 against the manifest (mismatch
// drops the download — fail closed), run the full image validation, then
// raw-write the dev slot (app1) and point otadata at it. The caller reboots
// on OK. Reuses HttpDownloader + FirmwareFlasher; no second flash path.
Error installFromManifestEntry(const OtaDevVersionEntry& entry, ProgressCallback onProgress, void* ctx);

// --- running-dev dance (design §5) -----------------------------------------

// True when an install was requested from the dev slot and this boot must
// complete it. `versionOut` receives the requested version (may be empty when
// the stored flag predates version stamping).
bool pendingInstallGet(char* versionOut, size_t cap);

// Record the pending install before rebooting into stable.
bool pendingInstallSet(const char* version);

void pendingInstallClear();

}  // namespace dev_ota
