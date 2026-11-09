#include "DevOtaPolicy.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace dev_ota {

ResumeBind bindPendingEntry(const OtaDevVersionEntry* entries, int count, const char* pendingName,
                            const uint8_t digest[32], int* outIndex) {
  if (outIndex) *outIndex = -1;
  if (!entries || count <= 0 || !pendingName || pendingName[0] == '\0') {
    return ResumeBind::EntryMissing;
  }
  for (int i = 0; i < count; ++i) {
    if (strcmp(entries[i].name, pendingName) != 0) continue;
    if (outIndex) *outIndex = i;
    // No digest recorded (legacy pending record, pre-consent firmware):
    // the exact name is the whole recorded consent.
    if (!digest) return ResumeBind::Exact;
    // A recorded digest can only bind against a digest-bearing entry — the
    // installer refuses sha-less entries anyway (fail closed).
    if (!entries[i].hasSha) return ResumeBind::ShaUnverifiable;
    if (memcmp(entries[i].sha256, digest, 32) != 0) return ResumeBind::DigestMismatch;
    return ResumeBind::Exact;
  }
  return ResumeBind::EntryMissing;
}

namespace {

// Display label for a dev asset name "xerif-dev-<sha>-x4pro.bin" → "dev <sha12>".
// Returns false when the name doesn't match the pattern; callers then show
// the raw asset name.
bool devNameLabel(const char* name, char* out, size_t cap) {
  constexpr char kPrefix[] = "xerif-dev-";
  const size_t prefixLen = sizeof(kPrefix) - 1;
  if (!name || strncmp(name, kPrefix, prefixLen) != 0) return false;
  const char* sha = name + prefixLen;
  const char* dash = strchr(sha, '-');
  if (!dash || dash == sha) return false;
  const size_t shaLen = static_cast<size_t>(dash - sha);
  const size_t shown = shaLen < 12 ? shaLen : 12;
  snprintf(out, cap, "dev %.*s", static_cast<int>(shown), sha);
  return true;
}

// Display label for a PR asset name "xerif-pr-<num>-<sha>-x4pro.bin" →
// "pr <num> <sha>". The PR number is what tells a PR entry apart from a
// develop build in the picker; the sha tells which build of the PR. Returns
// false when the name doesn't match the pattern.
bool prNameLabel(const char* name, char* out, size_t cap) {
  constexpr char kPrefix[] = "xerif-pr-";
  constexpr char kSuffix[] = "-x4pro.bin";
  const size_t prefixLen = sizeof(kPrefix) - 1;
  const size_t suffixLen = sizeof(kSuffix) - 1;
  if (!name || strncmp(name, kPrefix, prefixLen) != 0) return false;
  const size_t nameLen = strlen(name);
  if (nameLen < prefixLen + 2 + suffixLen || strncmp(name + nameLen - suffixLen, kSuffix, suffixLen) != 0) {
    return false;
  }
  char body[64];
  const size_t bodyLen = nameLen - prefixLen - suffixLen;
  if (bodyLen >= sizeof(body)) return false;
  memcpy(body, name + prefixLen, bodyLen);
  body[bodyLen] = '\0';
  const char* dash = strchr(body, '-');
  if (!dash || dash == body) return false;
  const size_t numLen = static_cast<size_t>(dash - body);
  snprintf(out, cap, "pr %.*s %s", static_cast<int>(numLen), body, dash + 1);
  return true;
}

}  // namespace

void entryLabel(const OtaDevVersionEntry& entry, char* out, size_t cap) {
  // PR identity first: the PR number distinguishes it from develop builds,
  // and PR stamps rotate out of the manifest anyway.
  if (prNameLabel(entry.name, out, cap)) return;
  if (entry.version[0]) {
    snprintf(out, cap, "%s", entry.version);
  } else if (!devNameLabel(entry.name, out, cap)) {
    snprintf(out, cap, "%s", entry.name);
  }
}

bool compactDevDate(const char* iso, char* out, size_t cap) {
  if (!iso || strlen(iso) < 16) return false;
  const auto digit = [](char c) { return c >= '0' && c <= '9'; };
  if (iso[4] != '-' || iso[7] != '-' || iso[10] != 'T' || iso[13] != ':') return false;
  for (const int i : {5, 6, 8, 9, 11, 12, 14, 15}) {
    if (!digit(iso[i])) return false;
  }
  snprintf(out, cap, "%.2s-%.2s %.2s:%.2s", iso + 5, iso + 8, iso + 11, iso + 14);
  return true;
}

}  // namespace dev_ota
