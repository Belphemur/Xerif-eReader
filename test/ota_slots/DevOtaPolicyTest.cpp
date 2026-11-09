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

// OtaDevVersionEntry lives beside the policy, in the OTA JSON module.
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
