#include "DictHistoryStore.h"

#include <HalStorage.h>
#include <Logging.h>

#if __has_include(<common/FsApiConstants.h>)
#include <common/FsApiConstants.h>
#endif

#include <algorithm>

namespace {
// Whole-file read budget: the store's own worst case, 50 entries of at most
// MAX_QUERY_BYTES text each plus the newline — a full history must read back
// whole or record()'s next rewrite would drop entries.
constexpr size_t MAX_FILE_BYTES = DictHistoryStore::MAX_ENTRIES * (DictHistoryStore::MAX_QUERY_BYTES + 1);

// Bounded front that never runs past the last UTF-8 lead byte: a cut that
// pushed a full multi-byte codepoint over the budget rolls back to the
// previous lead so truncated query text stays valid UTF-8.
std::string sanitizeQuery(const std::string& query) {
  size_t length = std::min(query.size(), DictHistoryStore::MAX_QUERY_BYTES);
  if (length < query.size()) {
    // A byte-granular cap can land inside a multi-byte codepoint: roll back
    // trailing continuation bytes, then drop the straddling codepoint's lead,
    // so stored query text stays valid UTF-8.
    while (length > 0 && (static_cast<unsigned char>(query[length]) & 0xC0) == 0x80) {
      --length;
    }
    if (length > 0 && (static_cast<unsigned char>(query[length]) & 0x80) != 0) --length;
    if (length == 0) return {};  // defensive: a codepoint cannot fill the cap
  }
  std::string clean;
  clean.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(query[i]);
    if (c < 0x20 || c == 0x7F) return {};  // control byte cannot survive the line shape
    clean.push_back(static_cast<char>(c));
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
  const size_t bytes = static_cast<size_t>(std::max(file.read(content.data(), content.size()), 0));
  file.close();
  if (bytes == 0) return;
  content.resize(bytes);

  size_t lineStart = 0;
  while (lineStart < content.size()) {
    size_t lineEnd = content.find('\n', lineStart);
    if (lineEnd == std::string::npos) {
      // A filled buffer's trailing partial line cannot be trusted and
      // rewriting the file from it would truncate the stored history.
      if (bytes == content.capacity()) break;
      lineEnd = content.size();
    }
    size_t lineLength = lineEnd - lineStart;
    if (lineLength > 0 && content[lineStart + lineLength - 1] == '\r') --lineLength;
    if (lineLength > 0) {
      std::string line = content.substr(lineStart, lineLength);
      if (line.size() > MAX_QUERY_BYTES) {
        line.resize(MAX_QUERY_BYTES);
        while (!line.empty() && (static_cast<unsigned char>(line.back()) & 0xC0) == 0x80) line.pop_back();
      }
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
