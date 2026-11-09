// Host tests for DictHistoryStore (design doc ttf/2026-11-09 §F4): the
// 50-entry move-to-front ring on /DictHistory.txt with staged temp+rename
// writes — a power loss can only leave the previous history, never a
// truncated live file.
#include <DictHistoryStore.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "HalStorage.h"

namespace {
std::vector<std::string> lines() {
  std::vector<std::string> out;
  DictHistoryStore::load(out);
  return out;
}
}  // namespace

TEST(DictHistoryStore, EmptyStoreLoadsNothingAndRecordCreatesFile) {
  fake::reset();
  std::vector<std::string> out{"stale"};
  DictHistoryStore::load(out);
  EXPECT_TRUE(out.empty());

  // An empty query is never recorded (no open, no write).
  EXPECT_FALSE(DictHistoryStore::record(""));
  EXPECT_EQ(fake::writesByPath.size(), 0u);

  EXPECT_TRUE(DictHistoryStore::record("north"));
  const auto history = lines();
  ASSERT_EQ(history.size(), 1u);
  EXPECT_EQ(history.front(), "north");
}

TEST(DictHistoryStore, MoveToFrontDedupeAndCap) {
  fake::reset();
  for (int i = 0; i < 7; ++i) ASSERT_TRUE(DictHistoryStore::record("w" + std::to_string(i)));
  // Re-lookup moves the word to the front instead of duplicating it.
  ASSERT_TRUE(DictHistoryStore::record("w2"));
  auto history = lines();
  ASSERT_EQ(history.size(), 7u);
  EXPECT_EQ(history.front(), "w2");
  EXPECT_EQ(std::count(history.begin(), history.end(), "w2"), 1);

  // Cap 50: the oldest entries fall off, newest stay.
  for (int i = 7; i < 55; ++i) ASSERT_TRUE(DictHistoryStore::record("w" + std::to_string(i)));
  history = lines();
  ASSERT_EQ(history.size(), DictHistoryStore::MAX_ENTRIES);
  EXPECT_EQ(history.front(), "w54");
  // The five oldest-recency entries (w0, w1, w3, w4, w5) fell off; w2 was
  // re-looked-up mid-run so it stayed at its re-record position.
  EXPECT_EQ(history.back(), "w6");
}

TEST(DictHistoryStore, SanitizesControlBytesAndTruncatesLongQueries) {
  fake::reset();
  // A control byte cannot survive the line-per-query shape, so the whole
  // query is refused (keyboard input cannot produce those anyway).
  EXPECT_FALSE(DictHistoryStore::record("bad\tword"));
  EXPECT_FALSE(DictHistoryStore::record("semi\nword"));
  EXPECT_TRUE(DictHistoryStore::record(std::string(200, 'x')));
  auto history = lines();
  ASSERT_EQ(history.size(), 1u);
  EXPECT_EQ(history[0].size(), DictHistoryStore::MAX_QUERY_BYTES);
}

TEST(DictHistoryStore, Utf8TruncationEndsOnCodepointBoundary) {
  fake::reset();
  // "café" with é (2 bytes): a cut that would land inside é rolls back to
  // the codepoint boundary.
  const std::string longUtf8 = std::string(128, 'a') + "\xc3\xa9" + "x";
  EXPECT_TRUE(DictHistoryStore::record(longUtf8));
  auto history = lines();
  ASSERT_EQ(history.size(), 1u);
  EXPECT_EQ(history[0].size(), 127u);  // the é's lead byte straddles the cap and is dropped
  EXPECT_EQ(history[0].back(), 'a');
  // A control byte mid-query refuses the whole record.
  EXPECT_FALSE(DictHistoryStore::record(std::string(50, 'a') + '\n' + std::string(50, 'b')));
  EXPECT_EQ(lines().size(), 1u);
}

TEST(DictHistoryStore, TempPublishFailureKeepsRecoverableStage) {
  fake::reset();
  ASSERT_TRUE(DictHistoryStore::record("original"));
  // Publish failure (rename refused): the live file is consumed by the repo's
  // remove+rename primitive and the staged file survives as the recovery
  // source; the NEXT record recovers the ring from it instead of starting a
  // fresh one-entry history.
  fake::failRename = 0;
  EXPECT_FALSE(DictHistoryStore::record("never-appears"));
  fake::failRename = -1;
  EXPECT_EQ(fake::files.count("/DictHistory.txt"), 0u);
  EXPECT_EQ(fake::files.count("/DictHistory.txt.tmp"), 1u);
  ASSERT_TRUE(DictHistoryStore::record("recovered"));
  auto history = lines();
  // The recovered stage was the full post-MTF ring of the failed publish
  // (the original lookup plus the one whose publish failed), so the ring
  // keeps both entries instead of collapsing to a single query.
  ASSERT_EQ(history.size(), 3u);
  EXPECT_EQ(history.front(), "recovered");
  EXPECT_TRUE(std::any_of(history.begin(), history.end(), [](const std::string& q) { return q == "original"; }));
}
