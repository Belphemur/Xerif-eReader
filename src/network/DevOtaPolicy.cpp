#include "DevOtaPolicy.h"

#include <cstdint>
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

namespace {

// Days from 1970-01-01 for a proleptic-Gregorian (y, m, d) — Howard
// Hinnant's integer algorithms; no library dependency, exact for the ranges
// a manifest stamp can reach.
constexpr int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civilFromDays(int64_t z, int* y, unsigned* m, unsigned* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t year = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp + (mp < 10 ? 3 : -9);
  *y = static_cast<int>(year + (*m <= 2));
}

constexpr int64_t kNoStampMinutes = INT64_MIN;

const auto digit = [](char c) { return c >= '0' && c <= '9'; };

// Structurally the shape the generator emits (YYYY-MM-DDTHH:MM, digits at
// the masked positions)? Everything past this gate falls open verbatim;
// only the unrecognizable shape hides the date entirely (as before).
bool stampShapeOk(const char* iso) {
  if (!iso || strlen(iso) < 16) return false;
  if (iso[4] != '-' || iso[7] != '-' || iso[10] != 'T' || iso[13] != ':') return false;
  for (const int i : {5, 6, 8, 9, 11, 12, 14, 15}) {
    if (!digit(iso[i])) return false;
  }
  return true;
}

// Raw "MM-DD HH:MM" slice of an offset-bearing stamp — the pre-conversion
// display, kept intact when the stamp carries digits but not a real date.
void stampVerbatim(const char* iso, char* out, size_t cap) {
  snprintf(out, cap, "%.2s-%.2s %.2s:%.2s", iso + 5, iso + 8, iso + 11, iso + 14);
}

// Parse the stamp to its UTC EPOCH MINUTES. The written stamp carries its
// own ISO offset — dev-build.yml takes the committer zone (%cI), PR/carried
// entries are GitHub created_at ("Z") — so the offset is subtracted, not
// assumed to be zero. False when the stamp is not a real instant; callers
// then treat the entry as undated (policy) or show the raw slice (display).
bool stampUtcMinutes(const char* iso, int64_t* outMinutes) {
  const size_t len = strlen(iso);  // >= 16, gate checked by the caller
  const int year = (iso[0] - '0') * 1000 + (iso[1] - '0') * 100 + (iso[2] - '0') * 10 + (iso[3] - '0');
  const int month = (iso[5] - '0') * 10 + (iso[6] - '0');
  const int day = (iso[8] - '0') * 10 + (iso[9] - '0');
  const int hour = (iso[11] - '0') * 10 + (iso[12] - '0');
  const int minute = (iso[14] - '0') * 10 + (iso[15] - '0');
  if (iso[0] < '0' || iso[0] > '9' || iso[1] < '0' || iso[1] > '9' || iso[2] < '0' || iso[2] > '9' ||
      iso[3] < '0' || iso[3] > '9' || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
      minute > 59) {
    return false;
  }

  int64_t utcMinutes = daysFromCivil(year, month, day) * 1440 + hour * 60 + minute;

  // Offset suffix after the optional seconds / fraction: Z, ±HH:MM, ±HHMM —
  // anything else along the tail is a stamp we refuse to guess about.
  size_t i = 16;
  if (i < len && iso[i] == ':') ++i;  // seconds colon
  if (i + 2 <= len && digit(iso[i]) && digit(iso[i + 1])) i += 2;  // seconds
  if (i < len && iso[i] == '.') {
    while (i < len && digit(iso[i])) ++i;
  }
  if (i < len) {
    if (iso[i] == 'Z' || iso[i] == 'z') {
      // utc
    } else if (iso[i] == '+' || iso[i] == '-') {
      const int sign = iso[i] == '-' ? -1 : 1;
      if (i + 5 > len || !digit(iso[i + 1]) || !digit(iso[i + 2])) return false;
      const int oh = (iso[i + 1] - '0') * 10 + (iso[i + 2] - '0');
      int om = -1;
      if (i + 6 <= len && iso[i + 3] == ':' && digit(iso[i + 4]) && digit(iso[i + 5])) {
        om = (iso[i + 4] - '0') * 10 + (iso[i + 5] - '0');
      } else if (i + 5 <= len && digit(iso[i + 3]) && digit(iso[i + 4])) {
        om = (iso[i + 3] - '0') * 10 + (iso[i + 4] - '0');
      }
      if (om < 0 || oh > 23 || om > 59) return false;
      utcMinutes -= sign * (oh * 60 + om);
    } else {
      return false;
    }
  }
  *outMinutes = utcMinutes;
  return true;
}

}  // namespace

bool compactDevDate(const char* iso, int offsetMinutes, char* out, size_t cap) {
  if (!stampShapeOk(iso)) return false;
  int64_t stampMinutes = 0;
  if (!stampUtcMinutes(iso, &stampMinutes)) {
    stampVerbatim(iso, out, cap);  // digits, not a date: fail open
    return true;
  }
  const int64_t local = stampMinutes + offsetMinutes;
  int64_t days = local / 1440;  // floor, not trunc: negatives must roll back
  int64_t rem = local % 1440;
  if (rem < 0) {
    rem += 1440;
    --days;
  }
  int year = 0;
  unsigned month = 0, day = 0;
  civilFromDays(days, &year, &month, &day);
  if (year < 0 || year > 9999) {
    stampVerbatim(iso, out, cap);  // outside the display range: fail open
    return true;
  }
  snprintf(out, cap, "%02u-%02u %02u:%02u", month, day, static_cast<unsigned>(rem / 60),
           static_cast<unsigned>(rem % 60));
  return true;
}

}  // namespace dev_ota
