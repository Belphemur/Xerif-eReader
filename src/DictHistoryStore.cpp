#include "DictHistoryStore.h"

#include <HalStorage.h>
#include <Logging.h>

#if __has_include(<common/FsApiConstants.h>)
#include <common/FsApiConstants.h>
#endif
#include <algorithm>
#include <cstdio>

namespace {
// Whole-file read budget: 50 entries × (query + newline) with headroom.
constexpr size_t MAX_FILE_BYTES = 4 * 1024 + 512;

// Queries are single lines. The keyboard cannot submit control bytes, but a
// defensive cut at the first one keeps a stray newline from corrupting the
// line-per-query file shape.
std::string sanitizeQuery(const std::string& query) {
  std::string clean;
  clean.reserve(std::min(query.size(), DictHistoryStore::MAX_QUERY_BYTES + 1));
  for (const char c : query) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) break;
    if (clean.size() == DictHistoryStore::MAX_QUERY_BYTES) break;
    clean.push_back(c);
  }
  return clean;
}
}  // namespace

void DictHistoryStore::load(std::vector<std::string>& out) {
  out.clear();
  HalFile file = Storage.open(FILE_PATH, O_RDONLY);
  if (!file) return;  // no history yet is not an error

  std::string content;
  content.resize(MAX_FILE_BYTES);
  const size_t bytes = static_cast<size_t>(file.read(content.data(), content.size()));
  file.close();
  if (bytes == 0) return;
  content.resize(bytes);

  size_t lineStart = 0;
  while (lineStart < content.size()) {
    size_t lineEnd = content.find('\n', lineStart);
    if (lineEnd == std::string::npos) lineEnd = content.size();
    size_t lineLength = lineEnd - lineStart;
    if (lineLength > 0 && content[lineStart + lineLength - 1] == '\r') --lineLength;
    if (lineLength > 0) {
      std::string line = content.substr(lineStart, lineLength);
      if (line.size() > MAX_QUERY_BYTES) line.resize(MAX_QUERY_BYTES);
      out.push_back(std::move(line));
      if (out.size() >= MAX_ENTRIES) return;
    }
    lineStart = lineEnd + 1;
  }
}

bool DictHistoryStore::record(const std::string& query) {
  std::string clean = sanitizeQuery(query);
  if (clean.empty()) return false;

  std::vector<std::string> entries;
  load(entries);
  // Move-to-front dedupe: a re-looked-up word jumps to the top.
  const auto existing = std::find(entries.begin(), entries.end(), clean);
  if (existing != entries.end()) entries.erase(existing);
  entries.insert(entries.begin(), std::move(clean));
  if (entries.size() > MAX_ENTRIES) entries.resize(MAX_ENTRIES);

  constexpr const char* TEMP_PATH = "/DictHistory.txt.tmp";
  HalFile file = Storage.open(TEMP_PATH, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    LOG_ERR("DICTH", "Could not open history temp file");
    return false;
  }
  bool ok = true;
  for (const auto& entry : entries) {
    const size_t written = file.write(entry.data(), entry.size());
    if (written != entry.size()) {
      ok = false;
      break;
    }
    if (file.write("\n", 1) != 1) {
      ok = false;
      break;
    }
  }
  // The stage must complete before publication; a short write or a failed
  // sync leaves the live history untouched.
  if (ok && !file.sync()) ok = false;
  file.close();
  if (ok) {
    ok = Storage.replaceFile(TEMP_PATH, FILE_PATH);
    if (!ok) LOG_ERR("DICTH", "Could not publish history temp file");
  }
  if (!ok) (void)Storage.remove(TEMP_PATH);
  return ok;
}
