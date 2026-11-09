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

// Staged write path. Publication (replaceFile) is the repo's remove+rename
// FAT primitive: publish() rotates the live ring into BACKUP_PATH before the
// destructive step, so a failed publish can consume the live file and the
// stage without losing history (the next record() recovers from the backup;
// the stage, and the remove+rename Publication temp — kept as recovery
// paths for interrupted/raced states).
constexpr const char* TEMP_PATH = "/DictHistory.txt.tmp";
constexpr const char* BACKUP_PATH = "/DictHistory.txt.bak";
constexpr const char* STAGE_PATH = "/DictHistory.txt.stage";

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
        // Same boundary rule as sanitizeQuery: roll back continuation bytes,
        // then a cut that left an orphaned multi-byte LEAD byte drops it too
        // (a dangling 0xC2..0xF4 prefix is not valid UTF-8 either).
        while (!line.empty() && (static_cast<unsigned char>(line.back()) & 0xC0) == 0x80) line.pop_back();
        if (!line.empty() && static_cast<unsigned char>(line.back()) >= 0xC0 &&
            line.size() == DictHistoryStore::MAX_QUERY_BYTES) {
          // Only when the byte AT the cap was this lead's continuation... the
          // cap landed right after a lead byte: drop it.
          line.pop_back();
        }
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
    // Roll back ONLY when the cap lands inside a multi-byte codepoint (the
    // byte at the cap is a continuation: its codepoint started before the
    // cap). A cap landing on a lead byte or ASCII is already a clean
    // boundary — the straddling codepoint lies entirely past the limit.
    while (length > 0 && (static_cast<unsigned char>(query[length]) & 0xC0) == 0x80) {
      --length;
    }
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
// Reads one path and parses the ring lines. Coding:
// - returns false                 → path missing (or storage-level open error)
// - returns true, readable=false  → open OK but read failed / OOM: present yet
//   unreadable; callers must never treat that as an empty ring and rewrite
// - returns true, readable=true   → parsed OK (entries possibly EMPTY: a
//   blank file, or a filled buffer's truncated tail refused)
bool readRingPath(const char* path, std::vector<std::string>& out, bool& readable) {
  readable = false;
  HalFile file = Storage.open(path, O_RDONLY);
  if (!file) return false;
  auto buffer = makeUniqueNoThrow<uint8_t[]>(MAX_FILE_BYTES);
  if (!buffer) return true;  // stranded an open file unreadably
  const int raw = file.read(buffer.get(), MAX_FILE_BYTES);
  file.close();
  if (raw < 0) return true;  // read error (present, unreadable)
  readable = true;
  parseRingLines(std::string_view(reinterpret_cast<const char*>(buffer.get()), static_cast<size_t>(raw)), out);
  return true;
}
}  // namespace

// Recovery sources in newest-complete-first order: the stage holds a fully
// written+synced ring that merely failed to publish; the backup holds the
// pre-publish ring; the legacy temp is the pre-stage-rename fallback.
constexpr const char* kRecoveryOrder[] = {STAGE_PATH, BACKUP_PATH, TEMP_PATH};

// Loads the newest available ring: the live file when readable, otherwise the
// newest recovery copy (the UI's recent chips must survive a failed publish).
// liveOpened/liveReadable report the LIVE file's state for callers that gate
// rewrites on it.
void loadNewestRing(std::vector<std::string>& out, bool& liveReadable, bool& liveOpened) {
  out.clear();
  liveReadable = false;
  liveOpened = readRingPath(DictHistoryStore::FILE_PATH, out, liveReadable);
  if (liveOpened && liveReadable) return;  // readable live file (maybe empty): honor it
  if (liveOpened) return;                  // present-but-unreadable: caller decides
  for (const char* path : kRecoveryOrder) {
    std::vector<std::string> staged;
    bool stagedReadable = false;
    readRingPath(path, staged, stagedReadable);
    if (!staged.empty()) {
      out = std::move(staged);
      return;
    }
  }
}

void DictHistoryStore::load(std::vector<std::string>& out) {
  bool liveReadable = false;
  bool liveOpened = false;
  loadNewestRing(out, liveReadable, liveOpened);
}

bool DictHistoryStore::record(const std::string& query) {
  std::string clean = sanitizeQuery(query);
  if (clean.empty()) return false;

  // Load recent-first. An existing-but-unreadable live file must never be
  // replaced by a single-query ring: an empty load from a PRESENT file means
  // a transient storage error, so the update refuses instead.
  std::vector<std::string> entries;
  bool liveReadable = false;
  bool liveOpened = false;
  loadNewestRing(entries, liveReadable, liveOpened);
  // A live file that exists but could not be OPENED (storage-level error) or
  // opened-but-unreadable must never be replaced by a recovery/empty ring.
  if ((liveOpened && !liveReadable) || (!liveOpened && Storage.exists(FILE_PATH))) {
    LOG_ERR("DICTH", "History exists but is unreadable; refusing to rewrite");
    return false;
  }
  if (entries.empty()) {
    // A PRESENT recovery file that reads as unreadable refuses the rewrite
    // (a missing one is just no recovery source for that path).
    for (const char* path : kRecoveryOrder) {
      std::vector<std::string> staged;
      bool stagedReadable = false;
      readRingPath(path, staged, stagedReadable);
      if (Storage.exists(path) && !stagedReadable) {
        LOG_ERR("DICTH", "Recovery file unreadable; refusing to rewrite");
        return false;
      }
      if (!staged.empty()) {
        entries = std::move(staged);
        break;
      }
    }
  }
  // Move-to-front dedupe: a re-looked-up word jumps to the top.
  const auto existing = std::find(entries.begin(), entries.end(), clean);
  if (existing != entries.end()) entries.erase(existing);
  entries.insert(entries.begin(), std::move(clean));
  if (entries.size() > MAX_ENTRIES) entries.resize(MAX_ENTRIES);

  HalFile file = Storage.open(STAGE_PATH, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    LOG_ERR("DICTH", "Could not open history stage file");
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
  // sync leaves the live history untouched (an interrupted stage is removed,
  // its content is not a recovery source).
  if (ok && !file.sync()) ok = false;
  file.close();
  if (ok) {
    // Rotate the current live ring into the backup BEFORE the destructive
    // publish: the FAT replace is remove+rename, a failed publish can leave
    // the live file gone, and the backup must hold the last known-good ring.
    if (Storage.exists(FILE_PATH) && !Storage.replaceFile(FILE_PATH, BACKUP_PATH)) {
      LOG_ERR("DICTH", "Could not rotate history backup");
    }
    ok = Storage.replaceFile(STAGE_PATH, FILE_PATH);
    if (!ok) LOG_ERR("DICTH", "Could not publish history stage file");
  } else {
    (void)Storage.remove(STAGE_PATH);
  }
  return ok;
}
