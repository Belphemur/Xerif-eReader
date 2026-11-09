// §16 dance-identity binding (brief 2026-11-09 dev-picker UX): the pending
// install record must bind to the EXACT manifest entry the user picked, by
// name and (since the consent fix) arm-time SHA-256. A missing or changed
// entry is a refusal — the newest entry must never silently wear the
// selection as a fallback (a fallback once flashed an unrelated develop
// build over the user's PR-build pick).

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "network/DevOtaPolicy.h"

// OtaDevVersionEntry lives beside the policy; kMaxVersions bounds the
// manifest window. Both headers are POD-only (no device deps) by design.
#include "network/DevOtaClient.h"
#include "network/OtaJson.h"  // OtaDevVersionEntry

namespace {

constexpr dev_ota::ResumeBind bind(const char* pendingName, const uint8_t* digest, OtaDevVersionEntry* entries,
                                   int count, int* outIndex) {
  return dev_ota::bindPendingEntry(entries, count, pendingName, digest, outIndex);
}

void makeEntry(OtaDevVersionEntry& e, const char* name, const uint8_t sha[32], bool hasSha) {
  strncpy(e.name, name, sizeof(e.name) - 1);
  if (sha && hasSha) {
    memcpy(e.sha256, sha, 32);
    e.hasSha = true;
  }
}

}  // namespace

TEST(DevOtaPolicyBind, ExactBindsRecordedPrEntryOverDevFallback) {
  // The owner-reported failure shape: manifest newest-first (develop at
  // index 0, PR entries behind) and a pending record naming the PR asset —
  // the bind must land on the PR row, never default to index 0.
  OtaDevVersionEntry entries[2]{};
  uint8_t devSha[32] = {0xAA};
  uint8_t prSha[32] = {0x77};
  makeEntry(entries[0], "xerif-dev-af2169b62e08d8e0c-x4pro.bin", devSha, true);
  makeEntry(entries[1], "xerif-pr-303-772d2c5-x4pro.bin", prSha, true);
  int index = -1;
  const auto verdict = bind("xerif-pr-303-772d2c5-x4pro.bin", prSha, entries, 2, &index);
  EXPECT_EQ(verdict, dev_ota::ResumeBind::Exact);
  EXPECT_EQ(index, 1);
}

TEST(DevOtaPolicyBind, EntryMissingRefusesInsteadOfDefaulting) {
  // The rolling manifest pruned or superseded the recorded asset: a refusal,
  // never index 0.
  OtaDevVersionEntry entries[1]{};
  uint8_t devSha[32] = {0xAA};
  makeEntry(entries[0], "xerif-dev-af2169b62e08d8e0c-x4pro.bin", devSha, true);
  uint8_t prSha[32] = {0x77};
  int index = 0;
  const auto verdict = bind("xerif-pr-303-772d2c5-x4pro.bin", prSha, entries, 1, &index);
  EXPECT_EQ(verdict, dev_ota::ResumeBind::EntryMissing);
  EXPECT_EQ(index, -1);
}

TEST(DevOtaPolicyBind, ShaUnverifiableOnRepublishedShalessEntry) {
  // Same asset name re-published with the sha field dropped: the recorded
  // digest cannot be compared, so fail closed.
  OtaDevVersionEntry entries[2]{};
  uint8_t devSha[32] = {0xAA};
  makeEntry(entries[0], "xerif-dev-af2169b62e08d8e0c-x4pro.bin", devSha, true);
  makeEntry(entries[1], "xerif-pr-303-772d2c5-x4pro.bin", nullptr, false);
  uint8_t prShaOld[32] = {0x77};
  int index = -1;
  const auto verdict = bind("xerif-pr-303-772d2c5-x4pro.bin", prShaOld, entries, 2, &index);
  EXPECT_EQ(verdict, dev_ota::ResumeBind::ShaUnverifiable);
}

TEST(DevOtaPolicyBind, DigestMismatchOnKnownSha) {
  OtaDevVersionEntry entries[1]{};
  uint8_t prShaNew[32] = {0x99};
  makeEntry(entries[0], "xerif-pr-303-772d2c5-x4pro.bin", prShaNew, true);
  uint8_t prShaOld[32] = {0x77};
  int index = -1;
  const auto verdict = bind("xerif-pr-303-772d2c5-x4pro.bin", prShaOld, entries, 1, &index);
  EXPECT_EQ(verdict, dev_ota::ResumeBind::DigestMismatch);
}

TEST(DevOtaPolicyBind, LegacyRecordWithoutDigestBindsByName) {
  // Pre-consent records carry no arm-time digest: the exact name is the
  // whole recorded consent.
  OtaDevVersionEntry entries[1]{};
  uint8_t devSha[32] = {0xAA};
  makeEntry(entries[0], "xerif-dev-af2169b62e08d8e0c-x4pro.bin", devSha, true);
  int index = -1;
  const auto verdict = bind("xerif-dev-af2169b62e08d8e0c-x4pro.bin", nullptr, entries, 1, &index);
  EXPECT_EQ(verdict, dev_ota::ResumeBind::Exact);
  EXPECT_EQ(index, 0);
}

TEST(DevOtaPolicyBind, EmptyOrUnusableRecordRefuses) {
  OtaDevVersionEntry entries[1]{};
  uint8_t sha[32] = {0xAA};
  makeEntry(entries[0], "xerif-dev-af2169b62e08d8e0c-x4pro.bin", sha, true);
  int index = 0;
  EXPECT_EQ(bind("", nullptr, entries, 1, &index), dev_ota::ResumeBind::EntryMissing);
  EXPECT_EQ(index, -1);
  EXPECT_EQ(bind("x", nullptr, entries, 0, &index), dev_ota::ResumeBind::EntryMissing);
  EXPECT_EQ(bind("x", nullptr, nullptr, 1, &index), dev_ota::ResumeBind::EntryMissing);
}

TEST(DevOtaPolicyBind, ShaUnverifiableWhenFetchedEntryLacksSha) {
  // A recorded digest cannot bind against a sha-less entry (the installer
  // refuses those anyway): fail closed.
  OtaDevVersionEntry entries[1]{};
  strncpy(entries[0].name, "xerif-pr-303-772d2c5-x4pro.bin", sizeof(entries[0].name) - 1);
  uint8_t prSha[32] = {0x77};
  int index = -1;
  const auto verdict = bind("xerif-pr-303-772d2c5-x4pro.bin", prSha, entries, 1, &index);
  EXPECT_EQ(verdict, dev_ota::ResumeBind::ShaUnverifiable);
}

TEST(DevOtaPolicyLabel, PrEntriesLeadWithThePrNumber) {
  // The PR number distinguishes a PR entry from develop builds — even when
  // the entry carries a stamped version, which for a dropped branch fits the
  // same shape as a dev build's stamp.
  OtaDevVersionEntry e{};
  strncpy(e.name, "xerif-pr-303-772d2c5-x4pro.bin", sizeof(e.name) - 1);
  strncpy(e.version, "2.15.0-dev-772d2c5", sizeof(e.version) - 1);
  char label[48] = {};
  dev_ota::entryLabel(e, label, sizeof(label));
  EXPECT_STREQ(label, "pr 303 772d2c5");
}

TEST(DevOtaPolicyLabel, PrLabelFallsBackToVersionWhenNameUnparseable) {
  OtaDevVersionEntry e{};
  strncpy(e.name, "custom-asset.bin", sizeof(e.name) - 1);
  strncpy(e.version, "2.15.0-dev-772d2c5", sizeof(e.version) - 1);
  char label[48] = {};
  dev_ota::entryLabel(e, label, sizeof(label));
  EXPECT_STREQ(label, "2.15.0-dev-772d2c5");
}

TEST(DevOtaPolicyLabel, DevEntryPrefersVersionThenCompactTag) {
  OtaDevVersionEntry e{};
  strncpy(e.name, "xerif-dev-af2169b62e08d8e0c-x4pro.bin", sizeof(e.name) - 1);
  char label[48] = {};
  dev_ota::entryLabel(e, label, sizeof(label));
  EXPECT_STREQ(label, "dev af2169b62e08");  // unstamped, older manifest

  strncpy(e.version, "2.15.0-dev-develop-af2169b6", sizeof(e.version) - 1);
  dev_ota::entryLabel(e, label, sizeof(label));
  EXPECT_STREQ(label, "2.15.0-dev-develop-af2169b6");
}

TEST(DevOtaPolicyLabel, UnrecognizedAssetShowsRawName) {
  OtaDevVersionEntry e{};
  strncpy(e.name, "firmware-payload.bin", sizeof(e.name) - 1);
  char label[48] = {};
  dev_ota::entryLabel(e, label, sizeof(label));
  EXPECT_STREQ(label, "firmware-payload.bin");
}

TEST(DevOtaPolicyLabel, CompactDevDateShape) {
  char out[16] = {};
  ASSERT_TRUE(dev_ota::compactDevDate("2026-11-07T14:03:45+00:00", 0, out, sizeof(out)));
  EXPECT_STREQ(out, "11-07 14:03");
  EXPECT_FALSE(dev_ota::compactDevDate("2026-11-07 14:03", 0, out, sizeof(out)));
}

TEST(DevOtaPolicyDate, ConvertsToTheDeviceOffsetPositive) {
  // +6h rolls the date over: UTC 20:56 on 11-07 is 02:56 on 11-08.
  char out[16] = {};
  ASSERT_TRUE(dev_ota::compactDevDate("2026-11-07T20:56:00+00:00", 360, out, sizeof(out)));
  EXPECT_STREQ(out, "11-08 02:56");
}

TEST(DevOtaPolicyDate, ConvertsToTheDeviceOffsetNegative) {
  char out[16] = {};
  ASSERT_TRUE(dev_ota::compactDevDate("2026-11-07T20:56:00Z", -300, out, sizeof(out)));
  EXPECT_STREQ(out, "11-07 15:56");
}

TEST(DevOtaPolicyDate, OffsetBeyondTheHourRollsBackwards) {
  // DST-style minute offsets (e.g. +5:30, -2h) and backward rolls across
  // midnight: local 01:30-120min on 03-08 is 23:30 the PREVIOUS day (floor,
  // not trunc-toward-zero, must roll the days counter back).
  char out[16] = {};
  ASSERT_TRUE(dev_ota::compactDevDate("2026-03-08T01:30:00Z", -120, out, sizeof(out)));
  EXPECT_STREQ(out, "03-07 23:30");
  ASSERT_TRUE(dev_ota::compactDevDate("2026-11-07T20:56:00Z", 330, out, sizeof(out)));
  EXPECT_STREQ(out, "11-08 02:26");
}

TEST(DevOtaPolicyDate, StampSOwnNonZeroOffsetIsRespected) {
  // dev-build.yml stamps the committer zone (%cI), not always UTC: a
  // +02:00 stamp IS 20:56 UTC, and the device offset applies afterwards.
  char out[16] = {};
  ASSERT_TRUE(dev_ota::compactDevDate("2026-11-07T22:56:00+02:00", 60, out, sizeof(out)));
  EXPECT_STREQ(out, "11-07 21:56");
}

TEST(DevOtaPolicyDate, YearAndMonthBoundaries) {
  char out[16] = {};
  ASSERT_TRUE(dev_ota::compactDevDate("2026-12-31T20:00:00Z", 360, out, sizeof(out)));
  EXPECT_STREQ(out, "01-01 02:00");
  ASSERT_TRUE(dev_ota::compactDevDate("2024-02-29T12:00:00Z", 0, out, sizeof(out)));
  EXPECT_STREQ(out, "02-29 12:00");
}

TEST(DevOtaPolicyDate, DigitsButNotADateFallOpenVerbatim) {
  // Shape-valid but semantically impossible (the pre-conversion display):
  // show something rather than hiding the row date.
  char out[16] = {};
  ASSERT_TRUE(dev_ota::compactDevDate("2026-11-07T25:61:00Z", 360, out, sizeof(out)));
  EXPECT_STREQ(out, "11-07 25:61");
}

namespace {

void makePickerEntry(OtaDevVersionEntry& e, const char* name, const char* date) {
  strncpy(e.name, name, sizeof(e.name) - 1);
  if (date) strncpy(e.date, date, sizeof(e.date) - 1);
}

}  // namespace

TEST(DevOtaPolicyPicker, PrEntriesLeadOrderedByIdDescending) {
  // Owner report: a flat newest-first mix (pr 309, pr 303, dev, dev, pr
  // 306) — PR builds group on top, ascending PR numbers LAST (highest id
  // first), regardless of how the PRs' build dates interleave.
  OtaDevVersionEntry entries[5]{};
  makePickerEntry(entries[0], "xerif-pr-309-aaa-x4pro.bin", "2026-11-08T09:00:00Z");
  makePickerEntry(entries[1], "xerif-pr-303-772d2c5-x4pro.bin", "2026-11-07T21:00:00Z");
  makePickerEntry(entries[2], "xerif-dev-111-x4pro.bin", "2026-11-07T20:56:00Z");
  makePickerEntry(entries[3], "xerif-dev-000-x4pro.bin", "2026-11-07T18:00:00Z");
  makePickerEntry(entries[4], "xerif-pr-306-bbb-x4pro.bin", "2026-11-07T22:00:00Z");
  int order[5];
  dev_ota::pickerOrder(entries, 5, order);
  EXPECT_EQ(order[0], 0);  // pr 309 (highest id first…
  EXPECT_EQ(order[1], 4);  // pr 306 — newest PR date beats 303 despite id
  EXPECT_EQ(order[2], 1);  // pr 303
  EXPECT_EQ(order[3], 2);  // dev newest first…
  EXPECT_EQ(order[4], 3);
  EXPECT_EQ(dev_ota::entryGroup(entries[1]), dev_ota::EntryGroup::Pr);
  EXPECT_EQ(dev_ota::entryGroup(entries[2]), dev_ota::EntryGroup::Dev);
}

TEST(DevOtaPolicyPicker, DevEntriesNewestFirstUndatedRunLastInManifestOrder) {
  OtaDevVersionEntry entries[3]{};
  makePickerEntry(entries[0], "xerif-dev-old-x4pro.bin", "");
  makePickerEntry(entries[1], "xerif-dev-new-x4pro.bin", "2026-11-08T09:00:00Z");
  makePickerEntry(entries[2], "xerif-dev-mid-x4pro.bin", "2026-11-07T20:56:00Z");
  int order[3];
  dev_ota::pickerOrder(entries, 3, order);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
  EXPECT_EQ(order[2], 0);  // undated, but keeps its earlier manifest slot
}

TEST(DevOtaPolicyPicker, PerPrBuildsNewestFirstWithinOnePr) {
  OtaDevVersionEntry entries[2]{};
  makePickerEntry(entries[0], "xerif-pr-303-late-x4pro.bin", "2026-11-09T10:00:00Z");
  makePickerEntry(entries[1], "xerif-pr-303-early-x4pro.bin", "2026-11-09T08:00:00Z");
  int order[2];
  dev_ota::pickerOrder(entries, 2, order);
  EXPECT_EQ(order[0], 0);
  EXPECT_EQ(order[1], 1);
}

TEST(DevOtaPolicyPickerRows, HeadersSplitTheGroupsAndMatchTheOrder) {
  // The rows the user sees: PR header, PR entries in picker order, dev
  // header, dev entries in picker order.
  OtaDevVersionEntry entries[4]{};
  makePickerEntry(entries[0], "xerif-dev-111-x4pro.bin", "2026-11-08T09:00:00Z");
  makePickerEntry(entries[1], "xerif-pr-303-772d2c5-x4pro.bin", "2026-11-07T21:00:00Z");
  makePickerEntry(entries[2], "xerif-dev-000-x4pro.bin", "2026-11-07T18:00:00Z");
  makePickerEntry(entries[3], "xerif-pr-305-bbb-x4pro.bin", "2026-11-07T12:00:00Z");
  dev_ota::PickerRow rows[6];
  const int rowCount = dev_ota::pickerRows(entries, 4, rows);
  ASSERT_EQ(rowCount, 6);  // 4 entries + 2 headers
  EXPECT_EQ(rows[0].entry, -1);
  EXPECT_EQ(rows[0].group, static_cast<uint8_t>(dev_ota::EntryGroup::Pr));
  EXPECT_EQ(rows[1].entry, 3);  // pr 305 ahead of pr 303 (id desc)
  EXPECT_EQ(rows[2].entry, 1);
  EXPECT_EQ(rows[3].entry, -1);
  EXPECT_EQ(rows[3].group, static_cast<uint8_t>(dev_ota::EntryGroup::Dev));
  EXPECT_EQ(rows[4].entry, 0);
  EXPECT_EQ(rows[5].entry, 2);
}

TEST(DevOtaPolicyPickerRows, HeadersNotSelectableEntriesStayPermutation) {
  OtaDevVersionEntry entries[3]{};
  makePickerEntry(entries[0], "xerif-pr-303-772d2c5-x4pro.bin", "2026-11-07T21:00:00Z");
  makePickerEntry(entries[1], "xerif-dev-111-x4pro.bin", "2026-11-08T09:00:00Z");
  makePickerEntry(entries[2], "xerif-dev-000-x4pro.bin", "2026-11-07T18:00:00Z");
  int order[3];
  dev_ota::pickerOrder(entries, 3, order);
  // The ordering is a pure permutation of entry indexes: header rows never
  // enter it, so a selection index can never land on a separator.
  EXPECT_EQ(order[0], 0);
  EXPECT_EQ(order[1], 1);
  EXPECT_EQ(order[2], 2);
  dev_ota::PickerRow rows[5];
  ASSERT_EQ(dev_ota::pickerRows(entries, 3, rows), 5);
  // PR group gone → single dev header; PR entry stays inside the group.
  EXPECT_EQ(rows[0].entry, -1);
  EXPECT_EQ(rows[0].group, static_cast<uint8_t>(dev_ota::EntryGroup::Pr));
  EXPECT_EQ(rows[1].entry, 0);
  EXPECT_EQ(rows[2].entry, -1);
  EXPECT_EQ(rows[2].group, static_cast<uint8_t>(dev_ota::EntryGroup::Dev));
  EXPECT_EQ(rows[3].entry, 1);
  EXPECT_EQ(rows[4].entry, 2);
}

TEST(DevOtaPolicyPickerRows, SingleGroupKeepsJustItsOwnHeader) {
  OtaDevVersionEntry entries[2]{};
  makePickerEntry(entries[0], "xerif-dev-111-x4pro.bin", "2026-11-08T09:00:00Z");
  makePickerEntry(entries[1], "xerif-dev-000-x4pro.bin", "2026-11-07T18:00:00Z");
  dev_ota::PickerRow rows[4];
  ASSERT_EQ(dev_ota::pickerRows(entries, 2, rows), 3);
  EXPECT_EQ(rows[0].entry, -1);
  EXPECT_EQ(rows[0].group, static_cast<uint8_t>(dev_ota::EntryGroup::Dev));
  EXPECT_EQ(rows[1].entry, 0);
  EXPECT_EQ(rows[2].entry, 1);
  const int noRows = dev_ota::pickerRows(entries, 0, rows);
  EXPECT_EQ(noRows, 0);
}

// --- boot-hook resume routing (design §5/§16, owner report 2026-11-09) ------

TEST(DevOtaPolicyResumeRoute, RecordPresentRoutesToTheCompletionOrReEntry) {
  // A pending record routes every boot: DevChannel completing from the
  // worker, or re-entering from the target when the arm's otadata switch
  // never landed (an interrupted arm must not strand its consent).
  EXPECT_EQ(dev_ota::resumeRoute(dev_ota::PendingKind::DevChannel, true), dev_ota::ResumeRoute::CompleteFromWorker);
  EXPECT_EQ(dev_ota::resumeRoute(dev_ota::PendingKind::DevChannel, false), dev_ota::ResumeRoute::ReEnterFromTarget);
  // §16 SD kinds can only re-run flash work from their fixed worker slot: a
  // record seen in the target slot is the pathological completed-not-cleared
  // case — cleared, never a silent re-flash beyond the consent digest.
  const dev_ota::PendingKind sdKinds[] = {dev_ota::PendingKind::SdDev, dev_ota::PendingKind::StableStaged,
                                          dev_ota::PendingKind::StablePicked};
  for (const dev_ota::PendingKind kind : sdKinds) {
    EXPECT_EQ(dev_ota::resumeRoute(kind, true), dev_ota::ResumeRoute::CompleteFromWorker) << static_cast<int>(kind);
    EXPECT_EQ(dev_ota::resumeRoute(kind, false), dev_ota::ResumeRoute::Stale) << static_cast<int>(kind);
  }
}

// --- §20 slot-row identity (brief 2026-11-09 slots screen) ------------------

namespace {

// Stub measure: every character costs kStubPxPerChar px (ctx ignored).
constexpr int kStubPxPerChar = 10;
int stubMeasure(const void*, const char* text) { return static_cast<int>((text ? strlen(text) : 0) * kStubPxPerChar); }
int stubMeasureChars(const char* text) { return stubMeasure(nullptr, text); }

// Composition contract: the output either fits the slot together with the
// suffix (the way slotRowValue reserves the suffix width), or it is the
// fail-safe escape — the unshrunk identity itself, never a placeholder.
void expectComposedFits(const char* value, const char* suffix, const int slotPx, const char* out) {
  const int suffixLen = suffix ? static_cast<int>(strlen(suffix)) : 0;
  if (stubMeasureChars(out) + suffixLen * kStubPxPerChar <= slotPx) {
    SUCCEED();
  } else {
    EXPECT_STREQ(out, value) << "oversized output must be the verbatim stamp, never a placeholder";
  }
}

}  // namespace

TEST(DevOtaPolicySlotStamp, GrammarAcceptsShortShaLengths) {
  // CI shortens as needed (scripts/git_branch.py get_git_short_sha), so
  // 7..12-char trailing segments are all valid stamps. Branches carry
  // slashes and dashes — exactly what the live manifest records.
  const char* valid[] = {
      "2.16.1-dev-develop-772d2c5",                   // 7
      "2.16.1-dev-develop-420c659e",                  // 8 (the live develop stamp)
      "2.16.1-dev-fix/gt911-park-recovery-3a6e50f1",  // 9 + slashes
      "1.1.0-dev-feat-kosync-xpath-05c6cf8ab12",      // 11 (git_branch doc example)
      "2.16.1-dev-x-184cd3a1112f",                    // 12
  };
  for (const char* version : valid) {
    dev_ota::DevStamp stamp;
    ASSERT_TRUE(dev_ota::parseDevStamp(version, &stamp)) << version;
  }
  const char* invalid[] = {
      "2.16.1",                    // stable release
      "",                          // empty
      "2.16.1-develop-420c659e",   // no "-dev-" marker
      "2.16.1-dev-420",            // sha shorter than 4 hex chars
      "2.16.1-dev-ghijkl",         // non-hex sha
      "-dev-420c659e",             // empty base
      "2.16.1-dev-420c659eabcde",  // sha beyond 12 hex chars: not this contract
  };
  for (const char* version : invalid) {
    EXPECT_FALSE(dev_ota::parseDevStamp(version, nullptr)) << version;
  }
  // Out-parameter is optional: the pure predicate works without it.
  EXPECT_TRUE(dev_ota::parseDevStamp("2.16.1-dev-develop-420c659e", nullptr));
}

TEST(DevOtaPolicySlotStamp, GrammarDecodesBaseBranchSha) {
  dev_ota::DevStamp stamp;
  ASSERT_TRUE(dev_ota::parseDevStamp("2.16.1-dev-develop-515801b46", &stamp));
  EXPECT_EQ(stamp.baseLen, 6);
  EXPECT_EQ(strncmp(stamp.base, "2.16.1", 6), 0);
  EXPECT_EQ(stamp.branchLen, 7);
  EXPECT_EQ(strncmp(stamp.branch, "develop", 7), 0);
  EXPECT_EQ(stamp.shaLen, 9);
  EXPECT_EQ(strncmp(stamp.sha, "515801b46", 9), 0);

  // fit_version's branch-dropped form: no branch component to report.
  dev_ota::DevStamp dropped;
  ASSERT_TRUE(dev_ota::parseDevStamp("2.16.1-dev-9d9494e", &dropped));
  EXPECT_EQ(dropped.branchLen, 0);
  EXPECT_EQ(dropped.branch, nullptr);
  EXPECT_EQ(dropped.shaLen, 7);
  EXPECT_EQ(strncmp(dropped.sha, "9d9494e", 7), 0);
}

TEST(DevOtaPolicySlotStamp, PrCrossReferenceRendersThePickerBadge) {
  // The owner's exact scenario: manifest newest-first, PR 303 behind the
  // develop rows, and the flashed PR image carrying its dropped-branch stamp.
  OtaDevVersionEntry entries[2]{};
  makePickerEntry(entries[0], "xerif-dev-a8841f7b5c2d-x4pro.bin", "2026-11-09T16:12:26-05:00");
  makePickerEntry(entries[1], "xerif-pr-303-9d9494e-x4pro.bin", "2026-11-09T21:02:54Z");
  char out[64]{};
  int num = 0;
  dev_ota::slotRowValue("2.16.1-dev-9d9494e", entries, 2, stubMeasure, nullptr, 100000, nullptr, num, out, sizeof(out));
  EXPECT_STREQ(out, "pr 303 9d9494e");
  EXPECT_EQ(num, 303);
}

TEST(DevOtaPolicySlotStamp, PrCrossReferenceResolvesTheRightNumber) {
  // Several PR entries with distinct numbers: only the sha decides.
  OtaDevVersionEntry entries[3]{};
  makePickerEntry(entries[0], "xerif-pr-309-4b9bbc4-x4pro.bin", nullptr);
  makePickerEntry(entries[1], "xerif-pr-309-3a6e50f-x4pro.bin", nullptr);
  makePickerEntry(entries[2], "xerif-pr-303-9d9494e-x4pro.bin", nullptr);
  char out[64]{};
  int num = 0;
  dev_ota::slotRowValue("2.16.1-dev-9d9494e", entries, 3, stubMeasure, nullptr, 100000, nullptr, num, out, sizeof(out));
  EXPECT_STREQ(out, "pr 303 9d9494e");
  EXPECT_EQ(num, 303);
}

TEST(DevOtaPolicySlotStamp, SameShaConflictTakesFirstManifestEntry) {
  OtaDevVersionEntry entries[2]{};
  makePickerEntry(entries[0], "xerif-pr-303-9d9494e-x4pro.bin", nullptr);
  makePickerEntry(entries[1], "xerif-pr-404-9d9494e-x4pro.bin", nullptr);
  char out[64]{};
  int num = 0;
  dev_ota::slotRowValue("2.16.1-dev-9d9494e", entries, 2, stubMeasure, nullptr, 100000, nullptr, num, out, sizeof(out));
  EXPECT_STREQ(out, "pr 303 9d9494e");
  EXPECT_EQ(num, 303);
}

TEST(DevOtaPolicySlotStamp, NoManifestOrNoMatchShowsTheStampItself) {
  char out[64]{};
  int num = 0;
  dev_ota::slotRowValue("2.16.1-dev-develop-515801b46", nullptr, 0, stubMeasure, nullptr, 100000, nullptr, num, out,
                        sizeof(out));
  EXPECT_STREQ(out, "2.16.1-dev-develop-515801b46");
  EXPECT_EQ(num, -1);

  // An empty manifest (fetch failed) and a manifest without the sha behave
  // identically — never a placeholder.
  OtaDevVersionEntry entries[1]{};  // zeroed (empty name) entries
  dev_ota::slotRowValue("2.16.1-dev-develop-515801b46", entries, 1, stubMeasure, nullptr, 100000, nullptr, num, out,
                        sizeof(out));
  EXPECT_STREQ(out, "2.16.1-dev-develop-515801b46");

  OtaDevVersionEntry noMatch[1]{};
  makePickerEntry(noMatch[0], "xerif-pr-303-772d2c5-x4pro.bin", nullptr);
  dev_ota::slotRowValue("2.16.1-dev-develop-515801b46", noMatch, 1, stubMeasure, nullptr, 100000, nullptr, num, out,
                        sizeof(out));
  EXPECT_STREQ(out, "2.16.1-dev-develop-515801b46");
}

TEST(DevOtaPolicySlotStamp, DevelopStampFitsOrClampsByRecognizedShapes) {
  // A long branch survives the manifest but the field's fit_version may well
  // have dropped it; every capacity walk must compose a shape that fits —
  // or refuses to shrink and prints the stamp verbatim.
  const char* value = "2.16.1-dev-feat/ttf-image-quality2-9d9494e";
  for (int capacity = 0; capacity <= 520; capacity += 8) {
    char out[64]{};
    int num = -1;
    dev_ota::slotRowValue(value, nullptr, 0, stubMeasure, nullptr, capacity, nullptr, num, out, sizeof(out));
    expectComposedFits(value, nullptr, capacity, out);
  }
}

TEST(DevOtaPolicySlotStamp, ClampsAgainstEveryManifestWindowCapacity) {
  // kMaxVersions-sized manifest (3 PR + 4 dev + a carried entry): the dev
  // stamp finds no PR sha, so the identity stays the stamp; the composition
  // must still hold the fit-or-verbatim contract.
  OtaDevVersionEntry entries[dev_ota::kMaxVersions]{};
  makePickerEntry(entries[0], "xerif-pr-309-4b9bbc4-x4pro.bin", nullptr);
  makePickerEntry(entries[1], "xerif-pr-303-9d9494e-x4pro.bin", nullptr);
  makePickerEntry(entries[2], "xerif-pr-306-099c814-x4pro.bin", nullptr);
  makePickerEntry(entries[3], "xerif-dev-515801b4604-d41d8-x4pro.bin", nullptr);
  makePickerEntry(entries[4], "xerif-dev-515801b460-d41d8-x4pro.bin", nullptr);
  makePickerEntry(entries[5], "xerif-dev-515801b46-d41d8-x4pro.bin", nullptr);
  makePickerEntry(entries[6], "xerif-dev-515801b4-d41d8-x4pro.bin", nullptr);
  const char* value = "2.16.1-dev-develop-515801b46";
  for (int capacity = 0; capacity <= 520; capacity += 8) {
    char out[64]{};
    int num = -1;
    dev_ota::slotRowValue(value, entries, dev_ota::kMaxVersions, stubMeasure, nullptr, capacity, nullptr, num, out,
                          sizeof(out));
    expectComposedFits(value, nullptr, capacity, out);
    if (stubMeasureChars(out) > capacity) EXPECT_EQ(num, -1);  // escape: unfitted
  }
}

TEST(DevOtaPolicySlotStamp, DropsTheBranchThenShortensShaInShapes) {
  const char* value = "2.16.1-dev-develop-9d9494e";  // 26 chars
  const char* suffix = " · running";                 // 11 bytes (' · ' + 'running')
  char out[64]{};
  int num = -1;
  // Full composition needs 26 + 11 chars; it fits exactly at the boundary.
  dev_ota::slotRowValue(value, nullptr, 0, stubMeasure, nullptr, 370, suffix, num, out, sizeof(out));
  EXPECT_STREQ(out, value);
  // One pixel less and the branch goes first (the fit_version shape).
  dev_ota::slotRowValue(value, nullptr, 0, stubMeasure, nullptr, 369, suffix, num, out, sizeof(out));
  EXPECT_STREQ(out, "2.16.1-dev-9d9494e");
  // A tighter slot starts shortening the shown sha (identity tail intact:
  // nothing else cuts before this stage). 280 - 110(slot) leaves room for
  // exactly base + "-dev-" + a 6-char sha.
  dev_ota::slotRowValue(value, nullptr, 0, stubMeasure, nullptr, 280, suffix, num, out, sizeof(out));
  EXPECT_STREQ(out, "2.16.1-dev-9d9494");
  // A slot that cannot hold even the shortest base-1 shape prints the
  // stamp verbatim (the fail-safe escape, never a placeholder).
  dev_ota::slotRowValue(value, nullptr, 0, stubMeasure, nullptr, 200, suffix, num, out, sizeof(out));
  EXPECT_STREQ(out, value);
}

TEST(DevOtaPolicySlotStamp, BadgeTooWideShrinksItsShaThenFallsToTheStamp) {
  OtaDevVersionEntry entries[1]{};
  makePickerEntry(entries[0], "xerif-pr-303-9d9494e-x4pro.bin", nullptr);
  // Plenty of space: the full badge.
  char out[64]{};
  int num = -1;
  dev_ota::slotRowValue("2.16.1-dev-9d9494e", entries, 1, stubMeasure, nullptr, 100000, nullptr, num, out, sizeof(out));
  EXPECT_STREQ(out, "pr 303 9d9494e");
  // Too narrow for the badge (140px at the stub): the shown sha drops, the
  // number never does.
  dev_ota::slotRowValue("2.16.1-dev-9d9494e", entries, 1, stubMeasure, nullptr, 130, nullptr, num, out, sizeof(out));
  EXPECT_STREQ(out, "pr 303 9d94");
  EXPECT_EQ(num, 303);
  // Sub-badge-room slots fall back to the stamp shapes (the badge refusal
  // is the identity decision, the shapes are the presentation fallback).
  dev_ota::slotRowValue("2.16.1-dev-9d9494e", entries, 1, stubMeasure, nullptr, 60, nullptr, num, out, sizeof(out));
  expectComposedFits("2.16.1-dev-9d9494e", nullptr, 60, out);
}

TEST(DevOtaPolicySlotStamp, EntryLabelAndSlotComposeTheSamePrBadge) {
  // The one-parse invariant: the picker (entryLabel) and the slot chooser
  // (slotRowValue) must never drift into two PR grammars — same entry, same
  // badge text.
  OtaDevVersionEntry entry{};
  makePickerEntry(entry, "xerif-pr-303-9d9494e-x4pro.bin", "2026-11-09T21:02:54Z");
  char picker[64]{};
  dev_ota::entryLabel(entry, picker, sizeof(picker));
  char slots[64]{};
  int num = -1;
  dev_ota::slotRowValue("2.16.1-dev-ttf-image-quality2-9d9494e", &entry, 1, stubMeasure, nullptr, 100000, nullptr, num,
                        slots, sizeof(slots));
  EXPECT_STREQ(picker, slots);
  EXPECT_STREQ(slots, "pr 303 9d9494e");
  EXPECT_EQ(num, 303);
}

TEST(DevOtaPolicySlotStamp, GarbageStampPrintsVerbatim) {
  char out[64]{};
  int num = 0;
  // The prebuilt Arduino core's constant version (unstamped images).
  dev_ota::slotRowValue("6671d0b", nullptr, 0, stubMeasure, nullptr, 10, nullptr, num, out, sizeof(out));
  EXPECT_STREQ(out, "6671d0b");
  EXPECT_EQ(num, -1);

  // Erasure padding: 32 × 0xFF, NUL-terminated — the SlotInfo field order.
  char padded[33]{};
  memset(padded, '\xff', 32);
  dev_ota::slotRowValue(padded, nullptr, 0, stubMeasure, nullptr, 10, nullptr, num, out, sizeof(out));
  EXPECT_STREQ(out, padded);
}
