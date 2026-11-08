#pragma once

#include <esp_partition.h>

#include <cstddef>
#include <cstdint>

// Flash a firmware image from an SD-card path into the next OTA app
// partition, then switch otadata so the X3/X4 stock bootloader picks it up
// on next boot. Mirrors the web flasher: raw esp_partition_erase_range +
// esp_partition_write + ota_boot::switchTo (no Arduino Update class, no
// esp_image_verify — those reject our patched image on X4 silicon).
//
// Both the SD update activity and the OTA path land here. OTA first
// downloads the firmware to an SD-card cache file, then calls this.

namespace firmware_flash {

enum class Result {
  OK,
  OPEN_FAIL,
  TOO_SMALL,
  TOO_LARGE,
  BAD_MAGIC,
  BAD_SEGMENTS,  // segment table malformed or runs past EOF
  BAD_CHECKSUM,  // ESP image XOR checksum mismatch
  BAD_SHA,       // SHA256 trailer mismatch (hash_appended images)
  BAD_CHIP,      // image chip_id doesn't match the running MCU family
  WRONG_BOARD,   // image carries a board tag naming a different board
  BAD_SIZE,      // body+pad+sha length doesn't match file size
  NO_PARTITION,
  OOM,
  READ_FAIL,
  ERASE_FAIL,
  WRITE_FAIL,
  OTADATA_FAIL,
  DIGEST_MISMATCH,  // bytes read from SD during the flash differ from the caller's expected digest
};

// Progress callback: called after every chunk write. `written`/`total` are bytes.
using ProgressCb = void (*)(size_t written, size_t total, void* ctx);

// Open `sdPath`, validate it looks like an ESP32 image, then stream it into the
// next OTA app partition with interleaved 64 KiB erase + sector writes. On
// success switches otadata via ota_boot::switchTo. Caller is responsible for
// ESP.restart() afterwards.
//
// `alreadyValidated` lets callers that have just run `validateImageFile()`
// themselves (e.g. SdFirmwareUpdateActivity, which validates before showing
// the user the confirmation prompt) skip the redundant second pass. Defaults
// to false so callers without prior validation (any future entry point) keep
// the defense-in-depth check.
//
// `expectedDigest` (§16 dance identity): when non-null, the SHA-256 of the
// bytes actually read from SD during the write is compared against it BEFORE
// otadata is switched, so a file changed after the caller's consent digest
// check can never become the boot target. Returns DIGEST_MISMATCH and leaves
// the boot selection untouched on a mismatch.
Result flashFromSdPath(const char* sdPath, ProgressCb onProgress, void* ctx, bool alreadyValidated = false,
                       const uint8_t* expectedDigest = nullptr);

// Role-targeted variant (design §3): streams into `dest` explicitly instead of
// "whatever isn't running". The dev installer uses this to pin its target to
// the dev slot (app1) regardless of which slot is booted. `dest` must be a
// non-running OTA app partition; the running slot is never a legal target.
Result flashFromSdPathInto(const char* sdPath, const esp_partition_t* dest, ProgressCb onProgress, void* ctx,
                           bool alreadyValidated = false, const uint8_t* expectedDigest = nullptr);

// Full-image integrity check that mirrors the bootloader's verification:
// header magic, segment table walk, XOR checksum, and SHA256 trailer (when
// hash_appended == 1). Also scans for the embedded board tag (see
// FirmwareBoardTag.h) and rejects an image tagged for a different board.
// Run this before flashing a candidate firmware so a truncated/corrupted/
// wrong-board .bin never reaches otadata.
//
// `partitionSize` is the size of the destination OTA partition; pass 0 to
// skip the size-fits-partition check (e.g. when validating ahead of partition
// lookup). Streams the file in CHUNK-sized reads; the file is rewound on
// success so the caller can immediately reread it for flashing.
Result validateImageFile(const char* sdPath, size_t partitionSize);

const char* resultName(Result r);

// Returns the chip_id (esp_image_header_t offset 12) of the currently-running
// image, or 0xFFFF if it cannot be read. Because the running slot booted
// successfully, its chip_id is authoritative for the current CPU, so a
// candidate image must match it to be safe to flash.
uint16_t runningPartitionChipId();

}  // namespace firmware_flash
