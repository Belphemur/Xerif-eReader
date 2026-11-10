// Host tests for the Vocabulary Notebook store (addendum 2026-11-09): the
// bounded-tail reader over /Vocabulary.txt and the rewrite-without-the-record
// delete paths. Uses the fake HalStorage (temp+replaceFile publication,
// injectable write/rename failures) shared with the clipping and library
// suites; records are crafted line-for-line like the append side writes them
// (flat JSON, one record per '\n'-terminated line, newest last).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "HalStorage.h"
#include "vocabulary/VocabularyStore.h"

namespace {

std::string record(const std::string& word, const std::string& headword, const std::string& definition,
                   const uint32_t timestamp) {
  // One record per line, exactly like the append side publishes them.
  return "{\"v\":1,\"id\":\"0000000000000000000000000000000" + std::to_string(timestamp) +
         "\",\"timestamp\":" + std::to_string(timestamp) + ",\"word\":\"" + word + "\",\"headword\":\"" + headword +
         "\",\"definition\":\"" + definition +
         "\",\"definition_truncated\":false,\"title\":\"\",\"author\":\"\",\"series\":\"\",\"series_index\":null,"
         "\"chapter\":\"\",\"page\":3,\"spine\":0,\"start_offset\":null}\n";
}

// Newest record of a multi-record file.
std::string newestContent(const std::vector<std::string>& words) {
  std::string content;
  for (size_t i = 0; i < words.size(); ++i) content += record(words[i], "head-" + words[i], "def " + words[i], i + 1);
  return content;
}

std::vector<NotebookEntry> loadRecent(const size_t limit) {
  // Seeded with a sentinel row the store must clear out (empty-file asserts
  // rely on the clear happening).
  std::vector<NotebookEntry> out;
  NotebookEntry stale;
  stale.word = "stale";
  out.push_back(stale);
  VocabularyStore::loadRecent(out, limit);
  return out;
}

}  // namespace

TEST(VocabNotebook, MultiRecordNewestFirstWithFileOrderIndexes) {
  fake::reset();
  fake::add(VocabularyStore::FILE_PATH, newestContent({"alpha", "beta", "gamma"}));
  const auto entries = loadRecent(50);
  ASSERT_EQ(entries.size(), 3u);
  EXPECT_EQ(entries[0].word, "gamma");
  EXPECT_EQ(entries[0].headword, "head-gamma");
  EXPECT_EQ(entries[0].recordIndex, 2u);
  EXPECT_EQ(entries[0].timestamp, 3u);
  EXPECT_EQ(entries[1].word, "beta");
  EXPECT_EQ(entries[1].recordIndex, 1u);
  EXPECT_EQ(entries[2].word, "alpha");
  EXPECT_EQ(entries[2].recordIndex, 0u);

  NotebookRecord full;
  ASSERT_TRUE(VocabularyStore::loadRecord(1, full));
  EXPECT_EQ(full.word, "beta");
  EXPECT_EQ(full.definition, "def beta");
  EXPECT_EQ(full.timestamp, 2u);
  EXPECT_EQ(full.page, 3u);

  EXPECT_EQ(VocabularyStore::count(), 3u);
}

TEST(VocabNotebook, PartialTrailingRecordIsSkippedNotCounted) {
  fake::reset();
  // A power loss mid-append leaves a truncated tail: one complete record,
  // then an unterminated fragment.
  const std::string partial = record("alpha", "head-alpha", "def alpha", 1) + "{\"v\":1,\"id\":\"ff\",\"word\":\"bet";
  fake::add(VocabularyStore::FILE_PATH, partial);
  const auto entries = loadRecent(50);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].word, "alpha");
  EXPECT_EQ(entries[0].recordIndex, 0u);
  EXPECT_EQ(VocabularyStore::count(), 1u);
  // Addressing beyond the complete records fails cleanly.
  NotebookRecord full;
  EXPECT_FALSE(VocabularyStore::loadRecord(1, full));
  EXPECT_FALSE(VocabularyStore::removeRecord(1));
}

TEST(VocabNotebook, LimitKeepsTheNewestRowsOfABiggerStore) {
  fake::reset();
  std::vector<std::string> words;
  for (int i = 0; i < 40; ++i) words.push_back("w" + std::to_string(i));
  fake::add(VocabularyStore::FILE_PATH, newestContent(words));
  const auto entries = loadRecent(10);
  ASSERT_EQ(entries.size(), 10u);
  EXPECT_EQ(entries.front().word, "w39");
  EXPECT_EQ(entries.front().recordIndex, 39u);
  EXPECT_EQ(entries.back().word, "w30");
  EXPECT_EQ(entries.back().recordIndex, 30u);
}

TEST(VocabNotebook, RecordsBeyondTheTailWindowAreOutOfViewButIndexed) {
  fake::reset();
  // 500 small records: the 16 KiB window starts inside record ~280, so the
  // base counts the newlines before it and the kept rows keep absolute
  // file-order indexes.
  std::vector<std::string> words;
  for (int i = 0; i < 500; ++i) words.push_back("w" + std::to_string(i));
  fake::add(VocabularyStore::FILE_PATH, newestContent(words));
  EXPECT_GT(fake::files[VocabularyStore::FILE_PATH]->bytes.size(), 16u * 1024u);
  const auto entries = loadRecent(60);
  ASSERT_EQ(entries.size(), 60u);
  EXPECT_EQ(entries.front().word, "w499");
  EXPECT_EQ(entries.front().recordIndex, 499u);
  EXPECT_EQ(entries.back().word, "w440");
  EXPECT_EQ(entries.back().recordIndex, 440u);
  EXPECT_EQ(VocabularyStore::count(), 500u);
  // The list still addresses any record by its file-order index.
  NotebookRecord full;
  ASSERT_TRUE(VocabularyStore::loadRecord(0, full));
  EXPECT_EQ(full.word, "w0");
}

TEST(VocabNotebook, OversizedRecordIsSkippedByTheWindowAndRefusedByLoadRecord) {
  fake::reset();
  const std::string oversized = "{\"v\":1,\"timestamp\":1,\"word\":\"" + std::string(20 * 1024, 'x') + "\"}\n";
  fake::add(VocabularyStore::FILE_PATH, oversized + record("small", "head-small", "d", 2));
  EXPECT_EQ(VocabularyStore::count(), 2u);
  const auto entries = loadRecent(50);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].word, "small");
  EXPECT_EQ(entries[0].recordIndex, 1u);
  NotebookRecord full;
  EXPECT_TRUE(VocabularyStore::loadRecord(1, full));
  // Materializing the oversized record is refused, never truncated silently.
  EXPECT_FALSE(VocabularyStore::loadRecord(0, full));
}

TEST(VocabNotebook, MissingAndEmptyFilesLoadEmptyWithoutError) {
  fake::reset();
  EXPECT_TRUE(loadRecent(50).empty());
  EXPECT_EQ(VocabularyStore::count(), 0u);
  fake::add(VocabularyStore::FILE_PATH, "");
  auto entries = loadRecent(50);
  EXPECT_TRUE(entries.empty());
  NotebookRecord full;
  EXPECT_FALSE(VocabularyStore::loadRecord(0, full));
  EXPECT_FALSE(VocabularyStore::removeRecord(0));
  // Deleting a record from a missing store fails without creating files.
  fake::reset();
  EXPECT_FALSE(VocabularyStore::removeRecord(0));
  EXPECT_EQ(fake::files.count("/Vocabulary.txt.tmp"), 0u);
}

TEST(VocabNotebook, MalformedInteriorRecordsAreSkippedKeepingIndexes) {
  fake::reset();
  const std::string content = record("alpha", "h1", "d1", 1) + "not-json-at-all\n" + record("beta", "h2", "d2", 2);
  fake::add(VocabularyStore::FILE_PATH, content);
  const auto entries = loadRecent(50);
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].word, "beta");
  EXPECT_EQ(entries[0].recordIndex, 2u);
  EXPECT_EQ(entries[1].word, "alpha");
  EXPECT_EQ(entries[1].recordIndex, 0u);
}

TEST(VocabNotebook, CorruptHugeTimestampClamps) {
  fake::reset();
  fake::add(VocabularyStore::FILE_PATH, "{\"v\":1,\"timestamp\":99999999999999,\"word\":\"w\",\"headword\":\"w\"}\n");
  const auto entries = loadRecent(50);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].timestamp, UINT32_MAX);
}

TEST(VocabNotebook, RemoveRecordPreservesEveryOtherByte) {
  fake::reset();
  const std::string original = record("a", "h", "d", 1) + record("b", "h", "d", 2) + record("c", "h", "d", 3);
  fake::add(VocabularyStore::FILE_PATH, original);
  ASSERT_TRUE(VocabularyStore::removeRecord(1));
  const std::string remaining = record("a", "h", "d", 1) + record("c", "h", "d", 3);
  EXPECT_EQ(fake::files.at(VocabularyStore::FILE_PATH)->bytes,
            std::vector<uint8_t>(remaining.begin(), remaining.end()));
  EXPECT_EQ(fake::files.count("/Vocabulary.txt.tmp"), 0u);
  const auto entries = loadRecent(50);
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].word, "c");
  EXPECT_EQ(entries[0].recordIndex, 1u);
  EXPECT_EQ(entries[1].recordIndex, 0u);
}

TEST(VocabNotebook, RemoveRecordPublishFailureRecoversTheStage) {
  fake::reset();
  const std::string original = record("a", "h", "d", 1) + record("b", "h", "d", 2);
  fake::add(VocabularyStore::FILE_PATH, original);
  fake::failRename = 0;
  EXPECT_FALSE(VocabularyStore::removeRecord(0));
  // The FAT replace removed the live file before the rename failed: the only
  // surviving copy is the completed stage, which is left in place and
  // adopted by the next read. Records other than the deleted one survive.
  EXPECT_EQ(fake::files.count(VocabularyStore::FILE_PATH), 0u);
  const auto entries = loadRecent(50);
  printf("DBG entries=%zu\n", entries.size());
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].word, "b");
  EXPECT_EQ(entries[0].recordIndex, 0u);
  // The next append publishes the adopted stage instead of starting fresh.
  VocabularyContext context;
  ASSERT_TRUE(VocabularyStore::append(context, "c", "he", "de", false));
  const auto after = loadRecent(50);
  ASSERT_EQ(after.size(), 2u);
  EXPECT_EQ(after[0].word, "c");
  EXPECT_EQ(after[1].word, "b");
}

TEST(VocabNotebook, RemoveRecordStageWriteFailsLeavesTheStoreUntouched) {
  fake::reset();
  const std::string original = record("a", "h", "d", 1) + record("b", "h", "d", 2);
  fake::add(VocabularyStore::FILE_PATH, original);
  fake::failWritePath = "/Vocabulary.txt.tmp";
  EXPECT_FALSE(VocabularyStore::removeRecord(1));
  EXPECT_EQ(fake::files[VocabularyStore::FILE_PATH]->bytes, std::vector<uint8_t>(original.begin(), original.end()));
}

TEST(VocabNotebook, ClearEmptiesThroughThePublicationPath) {
  fake::reset();
  fake::add(VocabularyStore::FILE_PATH, record("a", "h", "d", 1));
  ASSERT_TRUE(VocabularyStore::clear());
  const auto node = fake::files[VocabularyStore::FILE_PATH];
  ASSERT_TRUE(node != nullptr);
  EXPECT_TRUE(node->bytes.empty());
  EXPECT_TRUE(loadRecent(50).empty());
  EXPECT_EQ(VocabularyStore::count(), 0u);
  EXPECT_EQ(fake::files.count("/Vocabulary.txt.tmp"), 0u);
  // Clearing a missing store is a success and creates nothing.
  fake::reset();
  EXPECT_TRUE(VocabularyStore::clear());
  EXPECT_EQ(fake::files.count(VocabularyStore::FILE_PATH), 0u);
}

TEST(VocabNotebook, ClearPublishFailureRecoversTheEmptyStage) {
  fake::reset();
  fake::add(VocabularyStore::FILE_PATH, record("a", "h", "d", 1));
  fake::failRename = 0;
  EXPECT_FALSE(VocabularyStore::clear());
  EXPECT_EQ(fake::files.count(VocabularyStore::FILE_PATH), 0u);
  // The empty stage survives and completes the clear on the next access.
  EXPECT_GT(fake::files.count("/Vocabulary.txt.tmp"), 0u);
  EXPECT_EQ(VocabularyStore::count(), 0u);
  EXPECT_EQ(fake::files.count("/Vocabulary.txt.tmp"), 0u);
}

TEST(VocabNotebook, EscapesRoundTripThroughTheScanner) {
  fake::reset();
  // The append side escapes ", \, and control bytes as \u00xx; the scanner
  // must decode them back (word below carries a quote, a backslash, a
  // newline and a UTF-8 accent).
  VocabularyContext context;
  ASSERT_TRUE(VocabularyStore::append(context, "voi\"la\\ce", "voilà", "line1\nline2", false));
  NotebookRecord full;
  ASSERT_TRUE(VocabularyStore::loadRecord(0, full));
  EXPECT_EQ(full.word, "voi\"la\\ce");
  EXPECT_EQ(full.headword, "voilà");
  EXPECT_EQ(full.definition, "line1\nline2");
  const auto entries = loadRecent(50);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].word, "voi\"la\\ce");
}
