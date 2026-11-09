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
  // é begins AT byte 128 (bytes 128-129): the 128-byte cap lands on its lead
  // byte, which is a clean boundary — all 128 in-limit ASCII bytes survive.
  const std::string cleanCut = std::string(128, 'a') + "\xc3\xa9" + "x";
  EXPECT_TRUE(DictHistoryStore::record(cleanCut));
  auto history = lines();
  ASSERT_EQ(history.size(), 1u);
  EXPECT_EQ(history[0].size(), 128u);
  EXPECT_EQ(history[0].back(), 'a');
  // A cap landing INSIDE é (its lead at byte 127, continuation at 128) rolls
  // back to the codepoint boundary: the incomplete character is dropped and
  // the stored query stays valid UTF-8.
  const std::string splitCut = std::string(127, 'a') + "\xc3\xa9" + "x";
  EXPECT_TRUE(DictHistoryStore::record(splitCut));
  history = lines();
  ASSERT_EQ(history.size(), 2u);
  EXPECT_EQ(history.front().size(), 127u);
  EXPECT_EQ(history.front().back(), 'a');
  // A control byte mid-query refuses the whole record.
  EXPECT_FALSE(DictHistoryStore::record(std::string(50, 'a') + '\n' + std::string(50, 'b')));
  EXPECT_EQ(lines().size(), 2u);
}

TEST(DictHistoryStore, TempPublishFailureKeepsRecoverableStage) {
  fake::reset();
  ASSERT_TRUE(DictHistoryStore::record("original"));
  // Publish failure (rename refused): the live file is consumed by the repo's
  // remove+rename primitive and the staged file survives as the recovery
  // source; the NEXT record recovers the ring from it instead of starting a
  // fresh one-entry history.
  // Rotate consumes one rename (LIVE→.bak); the publish consumes the next:
  // fake::failRename=1 fails exactly the publish step after the rotation.
  fake::failRename = 1;
  EXPECT_FALSE(DictHistoryStore::record("never-appears"));
  fake::failRename = -1;
  // The rotated backup holds the last known-good ring; the live path is
  // momentarily absent (the stage failed to deploy over it).
  EXPECT_EQ(fake::files.count("/DictHistory.txt"), 0u);
  EXPECT_EQ(fake::files.count("/DictHistory.txt.tmp"), 0u);
  EXPECT_GT(fake::files.count("/DictHistory.txt.bak"), 0u);
  ASSERT_TRUE(DictHistoryStore::record("recovered"));
  auto history = lines();
  ASSERT_EQ(history.size(), 2u);
  EXPECT_EQ(history.front(), "recovered");
  EXPECT_TRUE(std::any_of(history.begin(), history.end(), [](const std::string& q) { return q == "original"; }));
}
