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
  EXPECT_TRUE(DictHistoryStore::record("bad\tword"));   // tab control byte cuts the tail
  EXPECT_TRUE(DictHistoryStore::record("semi\nword"));  // embedded newline terminates
  EXPECT_TRUE(DictHistoryStore::record(std::string(200, 'x')));
  auto history = lines();
  ASSERT_EQ(history.size(), 3u);
  EXPECT_EQ(history[2], "bad");  // first control byte ends the query
  EXPECT_EQ(history[1], "semi");
  EXPECT_EQ(history[0].size(), DictHistoryStore::MAX_QUERY_BYTES);
}

TEST(DictHistoryStore, TempPublishFailureDamagesOnlyTheRing) {
  fake::reset();
  ASSERT_TRUE(DictHistoryStore::record("original"));
  // A failed publication uses the repo's remove+rename FAT primitive
  // (HalStorage::replaceFile contract): the live file is removed first and
  // the rename refuses, so the ring is empty and the temp is cleaned up for
  // the next record. The staging phase itself is what the temp file
  // protects — a power loss DURING the write leaves the previous history.
  fake::failRename = 0;
  EXPECT_FALSE(DictHistoryStore::record("never-appears"));
  fake::failRename = -1;
  auto history = lines();
  EXPECT_TRUE(history.empty());
  EXPECT_EQ(fake::files.count("/DictHistory.txt"), 0u);
  // The failed temp is cleaned up for the next record.
  EXPECT_EQ(fake::files.count("/DictHistory.txt.tmp"), 0u);
  ASSERT_TRUE(DictHistoryStore::record("after-recovery"));
  history = lines();
  ASSERT_EQ(history.size(), 1u);
  EXPECT_EQ(history.front(), "after-recovery");
}

TEST(DictHistoryStore, ShortStageWriteRefusesToPublish) {
  fake::reset();
  ASSERT_TRUE(DictHistoryStore::record("kept"));
  fake::shortWrite = 0;  // first write of the next stage comes up short
  EXPECT_FALSE(DictHistoryStore::record("incomplete"));
  fake::shortWrite = -1;
  auto history = lines();
  ASSERT_EQ(history.size(), 1u);
  EXPECT_EQ(history.front(), "kept");
  EXPECT_EQ(fake::files.count("/DictHistory.txt.tmp"), 0u);
}
