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

// CI's rolling window: 3 dev builds + up to 6 PR entries (design §4; the
// manifest prune keeps the newest of each). The device must list the whole
// window — a cap below it hides installable versions from the picker.
constexpr int kMaxVersions = 9;

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

// --- pending-install dance (design §5/§16) ----------------------------------

// The dance is the ONE pending-install mechanism (design §16): the running
// slot stages the asset on SD (/.crosspoint, the swap space), records a
// pending install, restarts into the OTHER slot (the swap worker), and that
// boot hook completes the write of the TARGET slot. The §5 dev dance (target
// = dev slot, worker = stable) is the same mechanism with a different target.
// The kind names the dance arm: it fixes both the completion pass the boot
// hook launches and the target slot that pass writes.
enum class PendingKind : uint8_t {
  DevChannel = 0,    // §5 dev dance: the completion pass (stable slot) re-fetches
                     // the dev manifest and writes app1; payload = version string.
  SdDev = 1,         // §16 SD dance, target dev: the completion pass (stable slot)
                     // writes app1 from the user's SD file; payload = SD path.
  StableStaged = 2,  // §16 stable→stable dance, gh_release staged on SD: the
                     // completion pass (dev slot) writes app0 from the staging
                     // file; payload = staging path; the staging file is removed
                     // after the flash.
  StablePicked = 3,  // §16 stable→stable dance, user-picked SD file: the
                     // completion pass (dev slot) writes app0 from the user's
                     // file; payload = SD path; the file is never removed.
};

// The pending-install payload capacity (version string or SD path). The boot
// hook and both completion activities size their buffers from this constant.
constexpr size_t kPendingPayloadCap = 64;

// True when an install was requested from the other slot and this boot must
// complete it. `payloadOut` receives the recorded payload (the version string
// or the SD path; may be empty when the stored flag predates payload
// stamping).
bool pendingInstallGet(char* payloadOut, size_t cap);

// Record the pending install before rebooting into the swap worker.
bool pendingInstallSet(const char* payload, PendingKind kind);

// The §16 kind of the stored pending install (DevChannel when the stored flag
// predates kind stamping).
PendingKind pendingInstallKind();

// §16 dance identity (design §16): the SHA-256 of the exact SD image that was
// validated and consented to at dance-arm time. The unattended completion
// pass re-hashes the file and refuses to flash when it differs — an SD card
// swapped between consent and the completion boot must never reach flash.
// Stored under its own NVS key; a DevChannel dance records none (its
// completion pass re-fetches the unsigned manifest and checks the downloaded
// image against the manifest's SHA-256 instead).
bool pendingInstallSetDigest(const uint8_t digest[32]);
bool pendingInstallGetDigest(uint8_t digestOut[32]);

// §16 write-phase latch: the completion pass sets it BEFORE the first erase,
// so a later boot can tell the TARGET slot may already be partially written.
// A resume (or in-session failure) that reads the latch true must keep the
// swap worker selected — unwinding otadata into a half-written slot would
// boot-loop. Arming a new dance resets the latch.
bool pendingInstallMarkWriteStarted();
// False when the latch was never set for this record. TRUE on a read failure
// (fail-safe: an unreadable latch means the slot may be modified).
bool pendingInstallWriteStarted();

// Streams `path` once and returns its SHA-256 (the §16 dance identity check:
// arm-time consent record and completion-time re-verification). False when the
// file cannot be opened or a read fails mid-stream.
bool sha256OfFile(const char* path, uint8_t out[32]);

// Erases every pending-install key (payload, kind, digest, write latch) and
// verifies the record is gone. False when the record may still be armed: a
// dance reported as disarmed MUST NOT leave a request resumable.
bool pendingInstallClear();

}  // namespace dev_ota
