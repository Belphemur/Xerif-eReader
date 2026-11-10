#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct VocabularyContext {
  std::string title;
  std::string author;
  std::string series;
  std::optional<float> seriesIndex;
  std::string chapter;
  uint16_t page = 0;
  uint16_t spine = 0;
  uint32_t startOffset = UINT32_MAX;
};

// Recency-view row of a notebook record: only the cheap head fields.
// recordIndex is the record's file-order index (0 = oldest written), so a
// caller can address loadRecord()/removeRecord() straight from a row the
// newest-first list returned.
struct NotebookEntry {
  std::string word;
  std::string headword;
  uint32_t timestamp = 0;
  uint32_t recordIndex = 0;
};

// One full notebook record. The definition is the heavy field; nothing else
// in the file needs it, so it is only loaded for a single record at a time.
struct NotebookRecord {
  std::string word;
  std::string headword;
  std::string definition;
  uint32_t timestamp = 0;  // 0 = the record's "null" timestamp (clock unset)
  std::string title;
  std::string author;
  std::string chapter;
  uint16_t page = 0;
};

class VocabularyStore {
 public:
  static constexpr const char* FILE_PATH = "/Vocabulary.txt";

  // A failed export must never prevent a successful dictionary lookup.
  static bool append(const VocabularyContext& context, const std::string& word, const std::string& headword,
                     const std::string& definition, bool htmlDefinition);

  // --- read side (addendum 2026-11-09: the notebook was write-only) ---------
  // Newest `limit` records (or all of them when the file holds fewer), FIRST
  // RESULT = NEWEST. Bounded tail window: the store grows unbounded on a
  // 380 KB-RAM baseline, so no call ever loads the whole file for parsing.
  // Records outside the window, malformed records and a partial trailing
  // record are skipped, keeping the rest (same tolerance the append-side
  // recoverIncompleteRecord shows). Missing/unreadable file -> false with
  // the log emitted once; callers present the empty state.
  static bool loadRecent(std::vector<NotebookEntry>& out, size_t limit);
  // Full record at file-order `index` (0 = oldest). False when the record
  // does not exist, is malformed, or exceeds the single-record scan bound.
  static bool loadRecord(uint32_t index, NotebookRecord& out);
  // Number of complete records in the store (0 also covers a missing file;
  // the log path is loadRecent's, the count is only used to gate UI entry).
  static uint32_t count();

  // --- delete side ----------------------------------------------------------
  // Both publish through the same stage-to-temp + HalStorage::replaceFile
  // path the append side recovers through: an interruption leaves either the
  // old file or the new one, never a half-spliced notebook (SOLID: the
  // in-place splice would corrupt the only copy on power loss).
  static bool removeRecord(uint32_t index);
  static bool clear();
};
