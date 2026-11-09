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

// Which picker group a manifest entry renders in. PR builds are identified
// by the same name parse entryLabel builds its "pr <num> <sha>" badge from —
// a badge and its group can never disagree.
enum class EntryGroup : uint8_t { Pr = 0, Dev = 1 };
EntryGroup entryGroup(const OtaDevVersionEntry& entry);

// Picker display ORDER (brief: the flat newest-first mix made PR builds hard
// to tell apart from develop builds): fills `order` (room for `count`, at
// most kMaxVersions) with entry indexes — PR builds first, by PR id
// DESCENDING and newest first within one PR (build instant DESC, manifest
// order as tie-break); then dev builds, newest first (build instant DESC,
// manifest order as tie-break). Stable, comparison-only, no heap.
void pickerOrder(const OtaDevVersionEntry* entries, int count, int* order);

// One picker display row: an entry row (entry >= 0: the manifest index it
// renders) or a plain group header (entry < 0 — never selectable, never a
// touch target; `group` names the header's text).
struct PickerRow {
  int16_t entry = -1;
  uint8_t group = 0;  // EntryGroup value; header rows only
};

// Display rows for the picker (§19 grouped order): a header, then that
// group's entries in picker order, then the next header. Groups with no
// entries contribute no header. `rows` must have room for count + 2.
// Returns the row count (0 for an empty manifest).
int pickerRows(const OtaDevVersionEntry* entries, int count, PickerRow* rows);

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

// --- §20 slot-row identity (brief 2026-11-09 slots screen) -------------------
// One grammar knowledge for both consumers of the build identity: the PR
// asset name (one parse, public record) and the fork's stamped dev version.

// One parse of a PR asset name "xerif-pr-<num>-<sha>-x4pro.bin": the "pr
// <num> <sha>" badge, the picker group and the slot chooser's
// cross-reference all come from here, so they can never disagree on
// whether an entry is a PR build. num/sha point into `name`, bounded by
// the leng fields. `value` is 0 (still a PR) when <num> is not purely
// numeric — the badge keeps the raw slice.
struct PrName {
  const char* num = nullptr;
  size_t numLen = 0;
  long value = 0;
  const char* sha = nullptr;
  size_t shaLen = 0;
};
bool parsePrName(const char* name, PrName* out);

// One parse of the fork's stamped dev version "<base>-dev-<branch>-<sha>"
// (scripts/git_branch.py derivation): the "-dev-" marker makes an image
// self-identifying, and patch_dev_version's fit_version guarantees the
// 32-byte esp_app_desc field holds the "-dev-<sha>" tail uncut, so the
// segment after the LAST dash is the build's short sha (4..12 hex chars —
// the manifest records the same sha in its asset names). Pointers point
// into `version`; `branch` carries the branch component only when the fit
// could keep it whole (fit_version drops a branch that would overflow).
// False when the text is not a fork dev
// stamp — stable releases ("2.16.1"), the prebuilt core's constant
// version, empty/erased fields — callers print it verbatim. Pass null
// `out` to just probe the shape.
struct DevStamp {
  const char* base = nullptr;
  size_t baseLen = 0;
  const char* branch = nullptr;
  size_t branchLen = 0;  // valid only when branch != null (branch dropped otherwise)
  const char* sha = nullptr;
  size_t shaLen = 0;
};
bool parseDevStamp(const char* version, DevStamp* out);

// "pr <num> <sha>" badge text — the exact grammar the dev picker's PR rows
// show (entryLabel) and the slot chooser re-produces on a manifest match.
void prBadge(const PrName& pr, char* out, size_t cap);

// Pixel width of `text` in the slots rows' value font (§20 fit). Device:
// the uiTheme smallText font (uiScaleSpec().smallFontId, what the resolved
// list props give every value); host tests inject a stub.
using TextMeasure = int (*)(const void* ctx, const char* text);

// The slot chooser's row value (§20, one composition for both rows):
//  - the "pr <num> <sha>" badge (prBadge above) when the slot's stamped dev
//    version cross-references `manifest` (the CURRENT dev manifest, caller
//    fetched): a PR asset whose name sha shares a prefix with the stamp's
//    trailing sha (the shorter of the two decides the prefix — a 7-char
//    field sha and a 12-char manifest sha still name one commit); first
//    manifest match wins;
//  - otherwise the stamped dev version itself (the real build identity;
//    never a placeholder, matching output such as "2.16.1-dev-develop-<sha>");
//  - an unrecognizable text (not a fork stamp) is printed VERBATIM — bad
//    input fails safe, never "unknown".
// The composed value is then shrunk by recognized shapes only — full stamp →
// "<base>-dev-<sha>" → shorter sha → shorter base — until it plus `suffix`
// (the caller's " · running" marker, may be null) fits `valueSlotPx` as
// measured by `measure` in the row's value font. `prNumOut` receives the
// matched PR number (PrName::value) or -1. Needs cap ≥ 48 (31-byte stamp +
// suffix + NUL).
void slotRowValue(const char* version, const OtaDevVersionEntry* manifest, int count, TextMeasure measure,
                  const void* measureCtx, int valueSlotPx, const char* suffix, int& prNumOut, char* out, size_t cap);

}  // namespace dev_ota
