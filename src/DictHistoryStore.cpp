#include "DictHistoryStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#if __has_include(<common/FsApiConstants.h>)
#include <common/FsApiConstants.h>
#endif

#include <algorithm>
#include <cstdio>
#include <string_view>

namespace {
// Whole-file read budget: the store's own worst case, 50 entries of at most
// MAX_QUERY_BYTES text each plus the newline — a full history must read back
// whole or record()'s next rewrite would drop entries.
constexpr size_t MAX_FILE_BYTES = DictHistoryStore::MAX_ENTRIES * (DictHistoryStore::MAX_QUERY_BYTES + 1);

// Staged write target. Publication (replaceFile) is the repo's remove+rename
// FAT primitive: a failed rename leaves the live file gone and this temp
// still on the card, so the next record() recovers the ring from it.
constexpr const char* TEMP_PATH = "/DictHistory.txt.tmp";

// Splits the ring file into caller-owned lines. A filled buffer's trailing
// partial line is dropped (it cannot be trusted); byte-granular truncation
// rolls back to the previous UTF-8 codepoint boundary.
void parseRingLines(const std::string_view content, std::vector<std::string>& out) {
  size_t lineStart = 0;
  while (lineStart < content.size() && out.size() < DictHistoryStore::MAX_ENTRIES) {
    size_t lineEnd = content.find('\n', lineStart);
    if (lineEnd == std::string::npos) {
      if (content.size() >= MAX_FILE_BYTES) break;  // truncated tail: refuse
      lineEnd = content.size();
    }
    size_t lineLength = lineEnd - lineStart;
    if (lineLength > 0 && content[lineStart + lineLength - 1] == '\r') --lineLength;
    if (lineLength > 0) {
      std::string line(content.substr(lineStart, lineLength));
      if (line.size() > DictHistoryStore::MAX_QUERY_BYTES) {
        line.resize(DictHistoryStore::MAX_QUERY_BYTES);
        while (!line.empty() && (static_cast<unsigned char>(line.back()) & 0xC0) == 0x80) line.pop_back();
      }
      out.push_back(std::move(line));
    }
    lineStart = lineEnd + 1;
  }
}

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

// Reads one path into a caller-buffer and parses the ring lines. False when
// the path is unreadable or present but OOM-stranded (callers must not treat
// that as an empty history and rewrite).
bool readRingPath(const char* path, std::vector<std::string>& out) {
  HalFile file = Storage.open(path, O_RDONLY);
  if (!file) return false;
  auto buffer = makeUniqueNoThrow<uint8_t[]>(MAX_FILE_BYTES);
  if (!buffer) return false;
  const size_t bytes = static_cast<size_t>(std::max(file.read(buffer.get(), MAX_FILE_BYTES), 0));
  file.close();
  parseRingLines(std::string_view(reinterpret_cast<const char*>(buffer.get()), bytes), out);
  return true;
}
}  // namespace

void DictHistoryStore::load(std::vector<std::string>& out) {
  out.clear();
  readRingPath(FILE_PATH, out);
}

bool DictHistoryStore::record(const std::string& query) {
  std::string clean = sanitizeQuery(query);
  if (clean.empty()) return false;

  // Load recent-first. An existing-but-unreadable live file must never be
  // replaced by a single-query ring: an empty load from a PRESENT file means
  // a transient storage error, so the update refuses instead.
  const bool historyPresent = Storage.exists(FILE_PATH);
  std::vector<std::string> entries;
  load(entries);
  if (historyPresent && entries.empty()) {
    LOG_ERR("DICTH", "History exists but is unreadable; refusing to rewrite");
    return false;
  }
  if (entries.empty()) {
    // Previous publication may have failed (remove+rename leaves the staged
    // file behind and no live file); recover the ring from it rather than
    // starting a fresh one-entry history.
    std::vector<std::string> staged;
    if (readRingPath(TEMP_PATH, staged) && !staged.empty()) entries = std::move(staged);
  }
  // Move-to-front dedupe: a re-looked-up word jumps to the top.
  const auto existing = std::find(entries.begin(), entries.end(), clean);
  if (existing != entries.end()) entries.erase(existing);
  entries.insert(entries.begin(), std::move(clean));
  if (entries.size() > MAX_ENTRIES) entries.resize(MAX_ENTRIES);

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
  bool staged = ok;  // stage completed before the sync/publish attempts
  if (ok && !file.sync()) ok = false;
  file.close();
  if (ok) {
    ok = Storage.replaceFile(TEMP_PATH, FILE_PATH);
    if (!ok) LOG_ERR("DICTH", "Could not publish history temp file");
  }
  if (ok) return true;
  if (staged) {
    // The stage completed; publication failed. Keep the temp as the recovery
    // source for the next record (remove+rename may have consumed the live
    // file) — deleting it here would strand the ring empty.
    return false;
  }
  (void)Storage.remove(TEMP_PATH);  // interrupted stage is not a recovery source
  return false;
}
