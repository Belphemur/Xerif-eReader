#include "DevOtaPolicy.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

// kMaxVersions (the manifest's rolling CI window) bounds the picker's stack
// key tables; the constant lives beside the client that fetches the window.
#include "DevOtaClient.h"

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

// Display label for a dev asset name. Returns false when the name doesn't
// match the pattern; callers then show the raw asset name.
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

}  // anonymous namespace

// Single PR-asset grammar (struct/comment in the header): also consumed by
// the slot chooser's §20 cross-reference.
bool parsePrName(const char* name, PrName* out) {
  constexpr char kPrefix[] = "xerif-pr-";
  constexpr char kSuffix[] = "-x4pro.bin";
  const size_t prefixLen = sizeof(kPrefix) - 1;
  const size_t suffixLen = sizeof(kSuffix) - 1;
  if (!name || !out || strncmp(name, kPrefix, prefixLen) != 0) return false;
  const size_t nameLen = strlen(name);
  if (nameLen < prefixLen + 2 + suffixLen || strncmp(name + nameLen - suffixLen, kSuffix, suffixLen) != 0) {
    return false;
  }
  const char* body = name + prefixLen;
  const size_t bodyLen = nameLen - prefixLen - suffixLen;
  const char* dash = static_cast<const char*>(memchr(body, '-', bodyLen));
  if (!dash || dash == body) return false;
  out->num = body;
  out->numLen = static_cast<size_t>(dash - body);
  out->sha = dash + 1;
  out->shaLen = bodyLen - out->numLen - 1;
  bool numeric = true;
  long value = 0;
  for (size_t i = 0; i < out->numLen && numeric; ++i) {
    const char c = body[i];
    if (c < '0' || c > '9') {
      numeric = false;
    } else {
      value = value * 10 + (c - '0');
    }
  }
  out->value = numeric ? value : 0;
  return true;
}

// "pr <num> <sha>" badge — the picker's PR rows and the slot chooser's
// cross-reference both land here (§20: one grammar, one composer).
void prBadge(const PrName& pr, char* out, size_t cap) {
  snprintf(out, cap, "pr %.*s %.*s", static_cast<int>(pr.numLen), pr.num, static_cast<int>(pr.shaLen), pr.sha);
}

// --- §20 fork dev stamp "<base>-dev-<branch>-<sha>" ---------------------------
// git_branch.py derives <base>-dev-<branch>-<short sha>; patch_dev_version's
// fit_version fits the 31-byte field by dropping a whole overlength branch
// and, at worst, truncating the base on the right — the "-dev-<sha>" tail
// itself is never cut. So the text after the last dash is the short sha,
// 4..12 hex chars (git's own shortened form); longer, shorter, or non-hex
// tails are not a stamp this module claims to know.
bool parseDevStamp(const char* version, DevStamp* out) {
  constexpr char kDevMark[] = "-dev-";
  constexpr size_t kMinShaLen = 4;
  constexpr size_t kMaxShaLen = 12;
  if (!version) return false;
  // Bounded past the 31-byte field contract (real stamped fields are ≤31
  // bytes, NUL-padded by readSlotInfo); the host suite exercises longer
  // identity strings, so the bound must never cut a stamp mid-sha.
  const size_t len = strnlen(version, 64);
  constexpr size_t kMarkLen = sizeof(kDevMark) - 1;
  if (len < kMarkLen + kMinShaLen) return false;
  // The -dev- marker with at least one base char before, and the sha after.
  const char* mark = nullptr;
  for (size_t i = 1; i + kMarkLen < len; ++i) {
    if (strncmp(version + i, kDevMark, kMarkLen) == 0) {
      mark = version + i;
      break;
    }
  }
  if (!mark) return false;
  const char* rest = mark + kMarkLen;
  const size_t restLen = len - static_cast<size_t>(rest - version);
  // The sha is the segment after the FINAL dash of the tail (fit_version's
  // exact split: a branch kept whole keeps its own dashes; the
  // branch-dropped form has none left, so the whole tail is the sha).
  const char* sha = rest + restLen;
  while (sha > rest && sha[-1] != '-') --sha;
  const size_t shaLen = len - static_cast<size_t>(sha - version);
  if (shaLen < kMinShaLen || shaLen > kMaxShaLen) return false;
  for (size_t i = 0; i < shaLen; ++i) {
    const char c = sha[i];
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex) return false;
  }
  if (out) {
    out->base = version;
    out->baseLen = static_cast<size_t>(mark - version);
    if (out->baseLen == 0) return false;
    out->sha = sha;
    out->shaLen = shaLen;
    if (sha > rest) {
      out->branch = rest;
      out->branchLen = static_cast<size_t>(sha - rest - 1);  // minus the dash
    } else {
      out->branch = nullptr;
      out->branchLen = 0;
    }
  }
  return true;
}

namespace {

// One recognized shape of a stamped dev version (§20): the branch kept or
// dropped, and the shown-sha/base budgets the shrink loop steps through.
// Every shape keeps the "-dev-<sha>" tail — the build identity; the base
// and the shown sha are the only shrinking surfaces.
void composeStampShape(const DevStamp& stamp, const size_t baseLen, const size_t shaLen, const bool withBranch,
                       char* out, const size_t cap) {
  if (withBranch && stamp.branchLen > 0) {
    snprintf(out, cap, "%.*s-dev-%.*s-%.*s", static_cast<int>(baseLen), stamp.base, static_cast<int>(stamp.branchLen),
             stamp.branch, static_cast<int>(shaLen), stamp.sha);
  } else {
    snprintf(out, cap, "%.*s-dev-%.*s", static_cast<int>(baseLen), stamp.base, static_cast<int>(shaLen), stamp.sha);
  }
}

void composeStampShape(const DevStamp& stamp, const size_t baseLen, const size_t shaLen, char* out, const size_t cap) {
  composeStampShape(stamp, baseLen, shaLen, true, out, cap);  // widest shape has the branch
}

}  // anonymous namespace

void slotRowValue(const char* version, const OtaDevVersionEntry* manifest, const int count, TextMeasure measure,
                  const void* measureCtx, const int valueSlotPx, const char* suffix, int& prNumOut, char* out,
                  const size_t cap) {
  prNumOut = -1;
  if (!out || cap == 0) return;
  if (!version) {
    out[0] = '\0';
    return;
  }
  if (!measure) measure = [](const void*, const char*) { return 0; };
  int slotPx = valueSlotPx;
  if (suffix) slotPx -= measure(measureCtx, suffix);

  // Manifest cross-reference first (§20): a PR asset whose name sha shares
  // a prefix with the stamp's trailing sha (the shorter of the two decides
  // — a 7-char field and a 12-char manifest sha still name one commit).
  // First manifest match wins.
  DevStamp stamp;
  const bool stamped = parseDevStamp(version, &stamp);
  if (manifest && count > 0 && stamped) {
    for (int i = 0; i < count; ++i) {
      PrName pr;
      if (!parsePrName(manifest[i].name, &pr)) continue;
      const size_t cmpLen = pr.shaLen < stamp.shaLen ? pr.shaLen : stamp.shaLen;
      if (memcmp(stamp.sha, pr.sha, cmpLen) != 0) continue;
      prBadge(pr, out, cap);
      prNumOut = static_cast<int>(pr.value);
      if (measure(measureCtx, out) <= slotPx) return;
      // A badge wider than the slot (pathological narrow row) keeps the
      // PR number — the identity — and drops the shown sha to the minimum.
      snprintf(out, cap, "pr %.*s %.4s", static_cast<int>(pr.numLen), pr.num, pr.sha);
      if (measure(measureCtx, out) <= slotPx) return;
      break;  // badge never fits: fall through to the stamp shapes
    }
  }
  if (!stamped) {
    // Not a fork stamp (stable release, the prebuilt core's constant,
    // garbage): fail-safe verbatim, never shortened to a placeholder.
    snprintf(out, cap, "%s", version);
    return;
  }

  // Recognized shapes, widest identity first: the full stamp (branch
  // kept), the fit_version's branch-dropped form, then a shorter shown sha,
  // then a shorter base. Each keeps the "-dev-<sha>" tail.
  char shape[48];
  composeStampShape(stamp, stamp.baseLen, stamp.shaLen, shape, sizeof(shape));
  if (measure(measureCtx, shape) <= slotPx) {
    snprintf(out, cap, "%s", shape);
    return;
  }
  if (stamp.branchLen > 0) {
    composeStampShape(stamp, stamp.baseLen, stamp.shaLen, false, shape, sizeof(shape));
    if (measure(measureCtx, shape) <= slotPx) {
      snprintf(out, cap, "%s", shape);
      return;
    }
  }
  // Shorter sha (identity holds: the manifest cross-reference already
  // matched on a shared prefix, whatever the shown length), then a shorter
  // base on the right — both on the fit_version's branch-dropped form.
  size_t shaLen = stamp.shaLen;
  while (shaLen > 4) {
    --shaLen;
    composeStampShape(stamp, stamp.baseLen, shaLen, false, shape, sizeof(shape));
    if (measure(measureCtx, shape) <= slotPx) {
      snprintf(out, cap, "%s", shape);
      return;
    }
  }
  size_t baseLen = stamp.baseLen;
  while (baseLen > 1) {
    --baseLen;
    composeStampShape(stamp, baseLen, shaLen, false, shape, sizeof(shape));
    if (measure(measureCtx, shape) <= slotPx) {
      snprintf(out, cap, "%s", shape);
      return;
    }
  }
  // Nothing fits: the verbatim stamp (fail safe — never a placeholder).
  composeStampShape(stamp, stamp.baseLen, stamp.shaLen, shape, sizeof(shape));
  snprintf(out, cap, "%s", shape);
}

void entryLabel(const OtaDevVersionEntry& entry, char* out, size_t cap) {
  // PR identity first: the PR number distinguishes it from develop builds,
  // and PR stamps rotate out of the manifest anyway.
  PrName pr;
  if (parsePrName(entry.name, &pr)) {
    prBadge(pr, out, cap);
    return;
  }
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

// NotFound sentinel for pickerOrder's date key: an unusable stamp is the
// oldest build.
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
  if (iso[0] < '0' || iso[0] > '9' || iso[1] < '0' || iso[1] > '9' || iso[2] < '0' || iso[2] > '9' || iso[3] < '0' ||
      iso[3] > '9' || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59) {
    return false;
  }

  int64_t utcMinutes = daysFromCivil(year, month, day) * 1440 + hour * 60 + minute;

  // Offset suffix after the optional seconds / fraction: Z, ±HH:MM, ±HHMM —
  // anything else along the tail is a stamp we refuse to guess about.
  size_t i = 16;
  if (i < len && iso[i] == ':') ++i;                               // seconds colon
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
      } else if (digit(iso[i + 3]) && digit(iso[i + 4])) {
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

EntryGroup entryGroup(const OtaDevVersionEntry& entry) {
  PrName pr;
  return parsePrName(entry.name, &pr) ? EntryGroup::Pr : EntryGroup::Dev;
}

namespace {

// Build instant of an entry's stamp for the DESC comparisons: an unusable
// stamp is the oldest build, so undated entries drop below dated ones and
// ties resolve to manifest order (stable sort).
int64_t buildMinutes(const OtaDevVersionEntry& entry) {
  int64_t minutes = 0;
  return stampShapeOk(entry.date) && stampUtcMinutes(entry.date, &minutes) ? minutes : kNoStampMinutes;
}

struct PickerKey {
  bool isPr = false;
  long prNum = 0;  // PR entries: parsed id (0 when the slice is not numeric)
  int64_t minutes = kNoStampMinutes;
};

// Strictly-"newer" by the §19 order: PR before dev; PR id descending and
// newest first within one PR; dev newest first. Equal keys keep the
// manifest order via the stable insertion sort below.
bool keyBefore(const PickerKey& a, const PickerKey& b) {
  if (a.isPr != b.isPr) return a.isPr;
  if (a.isPr && a.prNum != b.prNum) return a.prNum > b.prNum;
  return a.minutes > b.minutes;
}

}  // namespace

void pickerOrder(const OtaDevVersionEntry* entries, int count, int* order) {
  if (!entries || !order || count <= 0) return;
  if (count > kMaxVersions) count = kMaxVersions;  // callers cap too; defensive

  PickerKey keys[kMaxVersions];
  for (int i = 0; i < count; ++i) {
    PickerKey& key = keys[i];
    key.minutes = buildMinutes(entries[i]);
    PrName pr;
    key.isPr = parsePrName(entries[i].name, &pr);
    key.prNum = key.isPr ? pr.value : 0;
    order[i] = i;
  }
  // Insertion sort: deterministic (no library sort), stable, and n ≤
  // kMaxVersions so the O(n²) swings are irrelevant against one SD read.
  for (int i = 1; i < count; ++i) {
    const int cur = order[i];
    int j = i - 1;
    while (j >= 0 && keyBefore(keys[cur], keys[order[j]])) {
      order[j + 1] = order[j];
      --j;
    }
    order[j + 1] = cur;
  }
}

int pickerRows(const OtaDevVersionEntry* entries, int count, PickerRow* rows) {
  if (!entries || count <= 0) return 0;
  if (count > kMaxVersions) count = kMaxVersions;
  int order[kMaxVersions];
  pickerOrder(entries, count, order);
  int prCount = 0;
  for (int i = 0; i < count; ++i) {
    if (entryGroup(entries[order[i]]) == EntryGroup::Pr) ++prCount;
  }
  int written = 0;
  if (prCount > 0) {
    rows[written].entry = -1;
    rows[written].group = static_cast<uint8_t>(EntryGroup::Pr);
    ++written;
  }
  for (int i = 0; i < count; ++i) {
    // prCount < count holds wherever i reaches prCount (i stops before it
    // otherwise), so the dev header is written exactly once when both
    // groups are populated.
    if (i == prCount) {
      rows[written].entry = -1;
      rows[written].group = static_cast<uint8_t>(EntryGroup::Dev);
      ++written;
    }
    rows[written].entry = static_cast<int16_t>(order[i]);
    rows[written].group = 0;
    ++written;
  }
  return written;
}

}  // namespace dev_ota
