#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Dictionary search history (design doc ttf/2026-11-09 §F4): the last 50
// successful dictionary lookups on the SD card, newest first, one query per
// line. Successful lookups only — the record call sits in the lookup success
// branch next to the vocabulary export. Re-looking up a word moves it to the
// front; the cap keeps the file under ~2 KB. Writes are staged to a temp
// file first, so a power loss mid-write cannot truncate the live history;
// publication rides the repo's remove+rename FAT primitive (documented on
// HalStorage::replaceFile), as all store writers here do.
class DictHistoryStore {
 public:
  static constexpr size_t MAX_ENTRIES = 50;
  // One line per query. A query that would exceed this byte budget is
  // truncated at a codepoint boundary; the keyboard cannot submit newlines,
  // but input is sanitized anyway (write cuts at the first control byte).
  static constexpr size_t MAX_QUERY_BYTES = 128;
  static constexpr const char* FILE_PATH = "/DictHistory.txt";

  static DictHistoryStore& getInstance() { return instance; }

  // Records one query (move-to-front dedupe, capped). Returns false on storage
  // failure; callers treat that as non-fatal, exactly like the vocabulary
  // export — a failed save must never block the lookup.
  bool record(const std::string& query);
  // Caller-owned list, most recent first. Missing/corrupt file → empty.
  void load(std::vector<std::string>& out) const;

 private:
  static DictHistoryStore instance;
};
