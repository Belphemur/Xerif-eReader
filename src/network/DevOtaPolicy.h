#pragma once

#include <cstddef>
#include <cstdint>

#include "OtaJson.h"

// Dev-channel picker policies (brief 2026-11-09 dev-picker UX): pure
// decisions over the manifest entries, shared by the dev picker and its
// host tests — no device dependencies.

namespace dev_ota {

// §16 dance identity for the §5 dev-channel dance: the recorded pending
// payload names the exact manifest asset, and (recorded since the consent
// fix) its SHA-256 pins the bytes that were offered when the user picked it.
// Binding that record to a freshly fetched manifest must never default:
// a failed dance refuses (clears the record, keeps the current firmware),
// it never silently installs a different build than the user picked.
enum class ResumeBind {
  Exact,            // outIndex is the recorded entry; install exactly it
  EntryMissing,     // no manifest entry carries the recorded name
  DigestMismatch,   // same name, different image bytes than consented to
  ShaUnverifiable,  // same name, but the fetched entry carries no sha256
};

// Match the recorded pending payload against the fetched manifest entries.
// `entries`/`count` are the parsed manifest (may be empty), `pendingName`
// the recorded asset name, `digest` the SHA-256 recorded at dance-arm time
// (null for pre-digest records: the name match alone is then authoritative).
// On entry-missing classes `*outIndex` is -1; anything else than Exact means
// the caller MUST refuse (clear the record, keep the current firmware).
ResumeBind bindPendingEntry(const OtaDevVersionEntry* entries, int count, const char* pendingName,
                            const uint8_t digest[32], int* outIndex);

// Picker/progress-row label for a manifest entry, preferred order:
//  - dev builds: the stamped firmware version (build identity), then the
//    compact "dev <sha>" label, then the raw asset name;
//  - PR builds: a "pr <num> <sha>" label ALWAYS wins over the version — the
//    PR number is what distinguishes a PR entry from a develop build at a
//    glance (steer 2026-11-09); older manifests without a parseable name
//    fall back to the dev rules.
void entryLabel(const OtaDevVersionEntry& entry, char* out, size_t cap);

// Compact an ISO-8601 build date to "MM-DD HH:MM" so the timestamp fits
// beside the row label, converted to the DEVICE's wall clock: the stamp's
// own ISO offset ("+00:00", "Z", dev stamps carry the committer's zone) is
// subtracted to get the UTC instant, then `offsetMinutes` — the device
// clock's UTC offset, resolved by the caller with the clock's timezone id —
// is added. Pure so host tests can pin conversions without the settings
// singleton. False when the field is not the shape the manifest generator
// emits; digits-that-shape-but-not-a-date fall open to the verbatim UTC
// slice (the pre-conversion display) instead of hiding the row date.
bool compactDevDate(const char* iso, int offsetMinutes, char* out, size_t cap);

}  // namespace dev_ota
