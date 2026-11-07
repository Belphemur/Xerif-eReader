#pragma once

#include <esp_partition.h>

#include <cstddef>
#include <cstdint>

// Slot roles are fixed by partition position (design §3): app0 = stable,
// app1 = dev. Every writer picks its target from the role, never from
// "whatever isn't running", so the running slot can never become a write
// target by accident.
//
// Version strings are read straight from each image's esp_app_desc_t header —
// no NVS shadow copy that could diverge from the image bytes.

namespace ota_slots {

// app0 / ota_0.
const esp_partition_t* stableSlot();
// app1 / ota_1. Null when the partition table has no second OTA slot (Picco).
const esp_partition_t* devSlot();

// True when both pointers refer to the same partition (by address, not
// pointer identity: partition lookups may return distinct objects).
bool sameSlot(const esp_partition_t* a, const esp_partition_t* b);

// Fixed-size, copy-by-value descriptor of one slot (design §9 lifetime rule).
struct SlotInfo {
  bool present = false;  // partition exists and carries a valid esp_app_desc_t
  char version[33] = {0};
};
static_assert(sizeof(SlotInfo) == 34, "SlotInfo must stay a POD of 34 bytes");

// Read `app`'s esp_app_desc_t (first DROM segment, offset 0x20) and copy the
// version string. Returns false (and zeroes `out`) when the partition is
// null, smaller than the descriptor, the magic word is absent, or the version
// field is empty — the caller renders that slot as "empty".
bool readSlotInfo(const esp_partition_t* app, SlotInfo& out);

// Flash-scannable marker identifying images that carry the bootFinalize()
// resume hook (main.cpp). The esp_app_desc_t.version field cannot serve this:
// with the prebuilt Arduino core it is a constant string baked into
// libesp_app_format.a (e.g. "6671d0b") that no version comparison can parse,
// so it cannot distinguish hook-bearing images from older ones. main.cpp
// embeds the marker next to the hook; the dev install scans the target slot
// for it before rebooting into that slot.
constexpr char kResumeHookMarker[] = "XERIF-DEVOTA-RESUME-HOOK-V1";

// True when `app`'s image bytes contain kResumeHookMarker — the image carries
// the resume hook, so the running-dev dance may reboot into it. Walks the
// image's segment chain to bound the scan to the real image extent (erased
// flash beyond it can never match). False when the partition is null, the
// image header is malformed, or the marker is absent.
bool imageHasResumeMarker(const esp_partition_t* app);

// The slot currently booted (by address match against the running partition),
// or nullptr when the running image is not in an OTA slot.
const esp_partition_t* runningSlot();

}  // namespace ota_slots
