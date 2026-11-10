#include "VocabularyStore.h"

#include <HalClock.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>
#if __has_include(<common/FsApiConstants.h>)
#include <common/FsApiConstants.h>
#endif
#include <esp_random.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "util/Dictionary.h"
#include "util/HtmlToPlainText.h"

namespace {
constexpr size_t MAX_WORD_BYTES = 160;
constexpr size_t MAX_DEFINITION_BYTES = 2048;
constexpr size_t MAX_HTML_INPUT_BYTES = 4096;
constexpr size_t MAX_HTML_SCAN_BYTES = 16 * 1024;
constexpr size_t MAX_TITLE_BYTES = 256;
constexpr size_t MAX_METADATA_BYTES = 160;

// Buffered output avoids allocating a full JSON record on the X3.
class RecordWriter {
 public:
  explicit RecordWriter(HalFile& output) : file(output) {}

  void raw(const char* text) {
    while (*text) put(*text++);
  }

  void number(uint32_t value) {
    char digits[16];
    snprintf(digits, sizeof(digits), "%lu", static_cast<unsigned long>(value));
    raw(digits);
  }

  bool quoted(const std::string& text, size_t maxBytes, size_t maxInputBytes = static_cast<size_t>(-1)) {
    put('"');
    const size_t limit = std::min({text.size(), maxBytes, maxInputBytes});
    const size_t size = limit < text.size() ? utf8SafeTruncateBuffer(text.data(), limit) : limit;
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    size_t encodedBytes = 0;
    size_t i = 0;
    for (; i < size; ++i) {
      const unsigned char c = static_cast<unsigned char>(text[i]);
      if (c >= 0x80) {
        // The shared decoder expects terminated input. Copy at most four bytes
        // so a truncated field cannot make it read beyond the input budget.
        unsigned char sequence[5] = {};
        const size_t available = std::min<size_t>(4, size - i);
        for (size_t j = 0; j < available; ++j) sequence[j] = static_cast<unsigned char>(text[i + j]);
        const unsigned char* cursor = sequence;
        const uint32_t codepoint = utf8NextCodepoint(&cursor);
        char encoded[4];
        const char* end = utf8AppendCodepoint(encoded, codepoint);
        const size_t encodedLength = static_cast<size_t>(end - encoded);
        if (encodedLength > maxBytes - encodedBytes) break;
        encodedBytes += encodedLength;
        for (const char* p = encoded; p < end; ++p) put(*p);
        i += static_cast<size_t>(cursor - sequence) - 1;
        continue;
      }
      if (encodedBytes == maxBytes) break;
      ++encodedBytes;
      if (c == '"' || c == '\\') {
        put('\\');
        put(static_cast<char>(c));
      } else if (c < 0x20) {
        raw("\\u00");
        put(HEX_DIGITS[c >> 4]);
        put(HEX_DIGITS[c & 0xf]);
      } else {
        put(static_cast<char>(c));
      }
    }
    put('"');
    return i < text.size();
  }

  bool finish() { return flush() && good; }

 private:
  static constexpr size_t BUFFER_BYTES = 192;
  HalFile& file;
  char buffer[BUFFER_BYTES] = {};
  size_t used = 0;
  bool good = true;

  void put(char c) {
    if (!good) return;
    buffer[used++] = c;
    if (used == sizeof(buffer)) flush();
  }

  bool flush() {
    if (good && used != 0) good = file.write(buffer, used) == used;
    used = 0;
    return good;
  }
};

// Scan a bounded HTML window without copying it, stopping at a complete tag
// or entity boundary. A '>' inside a quoted attribute does not end a tag.
// Optionally return the first non-whitespace byte outside markup.
size_t safeHtmlPrefixBytes(const std::string& source, const size_t start, const size_t budget,
                           size_t* firstVisible = nullptr) {
  const size_t limit = start + utf8SafeTruncateBuffer(source.data() + start, std::min(source.size() - start, budget));
  size_t safeEnd = start;
  for (size_t i = start; i < limit;) {
    const char c = source[i];
    if (c == '<') {
      if (i + 1 == limit && limit < source.size()) break;
      if (htmlTagStart(source, i)) {
        const size_t close = htmlTagEnd(source, i, limit);
        if (close == std::string::npos) break;
        safeEnd = close + 1;
        i = close + 1;
        continue;
      }
    }
    if (firstVisible && *firstVisible == std::string::npos && !std::isspace(static_cast<unsigned char>(c)))
      *firstVisible = i;
    safeEnd = ++i;
  }
  if (safeEnd > start && safeEnd < source.size()) {
    const size_t entityStart = source.rfind('&', safeEnd - 1);
    const size_t entityEnd = source.rfind(';', safeEnd - 1);
    if (entityStart != std::string::npos && entityStart >= start &&
        (entityEnd == std::string::npos || entityStart > entityEnd) && safeEnd - entityStart <= 18) {
      safeEnd = entityStart;
    }
  }
  return safeEnd;
}

time_t validLookupTime() {
  // Fork HalClock has no localTime(): read the RTC through getDateTime()
  // instead. The fork seeds time() from the RTC, so the fallback below is
  // RTC-derived too.
  uint16_t year = 0;
  uint8_t month = 0, day = 0, hour = 0, minute = 0;
  if (halClock.getDateTime(year, month, day, hour, minute) && year >= 2020) {
    struct tm local = {};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_min = minute;
    return mktime(&local);
  }
  const time_t systemTime = time(nullptr);
  return systemTime >= 1577836800 ? systemTime : 0;
}
// A bounded structural check lets us preserve a final JSON object without a newline.
// Vocabulary records have a flat JSON schema; the check also respects quoted braces.
bool isCompleteJsonObject(HalFile& file, const size_t start, const size_t end) {
  if (start >= end || !file.seek(start)) return false;
  char buffer[128];
  char nesting[8] = {};
  size_t depth = 0;
  bool inString = false;
  bool escaped = false;
  bool complete = false;
  for (size_t position = start; position < end;) {
    const size_t bytes = std::min(end - position, sizeof(buffer));
    if (file.read(buffer, bytes) != static_cast<int>(bytes)) return false;
    position += bytes;
    for (size_t i = 0; i < bytes; ++i) {
      const char c = buffer[i];
      if (complete) {
        if (c != ' ' && c != '\t' && c != '\r') return false;
      } else if (inString) {
        if (escaped) {
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if (c == '"') {
          inString = false;
        } else if (static_cast<unsigned char>(c) < 0x20) {
          return false;
        }
      } else if (c == '"') {
        if (depth == 0) return false;
        inString = true;
      } else if (c == '{' || c == '[') {
        if (depth == 0 && (c != '{' || position - bytes + i != start)) return false;
        if (depth >= sizeof(nesting)) return false;
        nesting[depth++] = c;
      } else if (c == '}' || c == ']') {
        if (depth == 0 || nesting[depth - 1] != (c == '}' ? '{' : '[')) return false;
        if (--depth == 0) complete = true;
      } else if (depth == 0 || static_cast<unsigned char>(c) < 0x20) {
        return false;
      }
    }
  }
  return complete && !inString && !escaped;
}
bool recoverTail(HalFile& file, const size_t start, const size_t size) {
  if (isCompleteJsonObject(file, start, size)) {
    return file.seek(size) && file.write(static_cast<uint8_t>('\n')) == 1;
  }
  return file.truncate(start) && file.seek(start);
}
bool recoverIncompleteRecord(HalFile& file) {
  const size_t size = file.fileSize();
  if (size == 0) return true;
  if (!file.seek(size - 1)) return false;
  const int last = file.read();
  if (last < 0) return false;
  if (last == '\n') return file.seek(size);

  // Walk backward in bounded chunks even when an externally written tail is
  // longer than a firmware record. recoverTail preserves complete objects and
  // discards invalid partial tails after the last newline.
  constexpr size_t CHUNK_BYTES = 128;
  char chunk[CHUNK_BYTES];
  size_t cursor = size;
  while (cursor > 0) {
    const size_t count = std::min(cursor, CHUNK_BYTES);
    cursor -= count;
    if (!file.seek(cursor) || file.read(chunk, count) != static_cast<int>(count)) return false;
    for (size_t i = count; i > 0; --i) {
      if (chunk[i - 1] == '\n') {
        const size_t newSize = cursor + i;
        return recoverTail(file, newSize, size);
      }
    }
  }
  return recoverTail(file, 0, size);
}

// Publication paths shared with the reader block below (rewrites publish a
// staged temp through the repo's remove+rename replaceFile primitive).
constexpr const char* STAGE_LIVE_PATH = "/Vocabulary.txt";  // VocabularyStore::FILE_PATH
constexpr const char* STAGE_TEMP_PATH = "/Vocabulary.txt.tmp";

// Two stale-free states must settle before any store ACCESS — including the
// append side: an append that creates a fresh live file while an interrupted
// publication holds the only surviving copy in the stage would orphan it (the
// next settle would then remove the stale temp with the whole notebook in
// it). Callers that would otherwise orphan the stage must not proceed while
// adoption fails.
bool settlePendingRewrite() {
  const bool live = Storage.exists(STAGE_LIVE_PATH);
  const bool staged = Storage.exists(STAGE_TEMP_PATH);
  if (live) {
    if (staged) (void)Storage.remove(STAGE_TEMP_PATH);
    return true;
  }
  if (!staged) return true;
  if (!Storage.replaceFile(STAGE_TEMP_PATH, STAGE_LIVE_PATH)) {
    LOG_ERR("VOCAB", "Could not recover an interrupted vocabulary rewrite");
    return false;
  }
  return true;
}
}  // namespace

bool VocabularyStore::append(const VocabularyContext& context, const std::string& word, const std::string& headword,
                             const std::string& definition, const bool htmlDefinition) {
  if (word.empty() || definition.empty()) return false;
  // A pending staged rewrite must be adopted (or publish refused) BEFORE the
  // fresh store is opened — the node above explains the orphaning sequence.
  if (!settlePendingRewrite()) return false;

  std::string plain;
  const std::string* exportedDefinition = &definition;
  bool sourceTruncated = false;
  if (htmlDefinition) {
    // Find the first text within 16 KiB of markup, but keep the actual HTML
    // conversion bounded to 4 KiB even on low-memory devices.
    size_t firstVisible = std::string::npos;
    safeHtmlPrefixBytes(definition, 0, MAX_HTML_SCAN_BYTES, &firstVisible);
    if (firstVisible == std::string::npos) return false;
    const size_t initialEnd = safeHtmlPrefixBytes(definition, 0, MAX_HTML_INPUT_BYTES);
    const size_t start = firstVisible < initialEnd ? 0 : firstVisible;
    const size_t end = start == 0 ? initialEnd : safeHtmlPrefixBytes(definition, start, MAX_HTML_INPUT_BYTES);
    if (end <= start) return false;
    const size_t bytes = end - start;
    sourceTruncated = end < definition.size();
    const auto heap = HalMemory::getDefaultHeap();
    if (heap.largestBlockBytes < bytes + 1 || heap.freeBytes < bytes + 8 * 1024) {
      LOG_ERR("VOCAB", "Not enough memory to export formatted definition");
      return false;
    }
    plain = htmlToPlainText(definition.substr(start, bytes), DICTIONARY_PLAIN_HEADWORD_MARKER);
    std::replace(plain.begin(), plain.end(), DICTIONARY_PLAIN_HEADWORD_MARKER, ' ');
    const bool visible =
        std::any_of(plain.begin(), plain.end(), [](const unsigned char c) { return !std::isspace(c); });
    if (!visible) return false;
    exportedDefinition = &plain;
  }

  HalFile file = Storage.open(FILE_PATH, O_RDWR | O_CREAT | O_AT_END);
  if (!file) {
    LOG_ERR("VOCAB", "Cannot open vocabulary history");
    return false;
  }
  if (!recoverIncompleteRecord(file)) {
    LOG_ERR("VOCAB", "Failed to recover vocabulary file tail");
    return false;
  }
  const size_t originalSize = file.fileSize();

  char id[33];
  snprintf(id, sizeof(id), "%08lx%08lx%08lx%08lx", static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()));
  RecordWriter record(file);
  record.raw("{\"v\":1,\"id\":\"");
  record.raw(id);
  record.raw("\",\"timestamp\":");
  const time_t timestamp = validLookupTime();
  if (timestamp > 0) {
    record.number(static_cast<uint32_t>(timestamp));
  } else {
    record.raw("null");
  }
  record.raw(",\"word\":");
  record.quoted(word, MAX_WORD_BYTES);
  record.raw(",\"headword\":");
  record.quoted(headword, MAX_WORD_BYTES);
  record.raw(",\"definition\":");
  const auto& text = *exportedDefinition;
  size_t definitionBytes = std::min(text.size(), MAX_DEFINITION_BYTES);
  if (definitionBytes < text.size()) definitionBytes = utf8SafeTruncateBuffer(text.data(), definitionBytes);
  bool definitionTruncated = sourceTruncated || definitionBytes < text.size();
  if (definitionBytes < text.size() && definitionBytes > 0) {
    const size_t lastLine = text.rfind('\n', definitionBytes - 1);
    if (lastLine != std::string::npos && lastLine >= definitionBytes / 2) {
      definitionBytes = lastLine;
    } else {
      const size_t lastSpace = text.find_last_of(" \t\r", definitionBytes - 1);
      if (lastSpace != std::string::npos && lastSpace >= definitionBytes / 2) definitionBytes = lastSpace;
    }
  }
  if (record.quoted(text, MAX_DEFINITION_BYTES, definitionBytes)) definitionTruncated = true;
  record.raw(",\"definition_truncated\":");
  record.raw(definitionTruncated ? "true" : "false");
  record.raw(",\"title\":");
  record.quoted(context.title, MAX_TITLE_BYTES);
  record.raw(",\"author\":");
  record.quoted(context.author, MAX_METADATA_BYTES);
  record.raw(",\"series\":");
  record.quoted(context.series, MAX_METADATA_BYTES);
  record.raw(",\"series_index\":");
  if (context.seriesIndex && std::isfinite(*context.seriesIndex)) {
    char number[32];
    snprintf(number, sizeof(number), "%.7g", static_cast<double>(*context.seriesIndex));
    record.raw(number);
  } else {
    record.raw("null");
  }
  record.raw(",\"chapter\":");
  record.quoted(context.chapter, MAX_METADATA_BYTES);
  record.raw(",\"page\":");
  record.number(context.page);
  record.raw(",\"spine\":");
  record.number(context.spine);
  record.raw(",\"start_offset\":");
  if (context.startOffset == UINT32_MAX) {
    record.raw("null");
  } else {
    record.number(context.startOffset);
  }
  record.raw("}\n");

  const bool written = record.finish();
  if (!written && !file.truncate(originalSize)) LOG_ERR("VOCAB", "Failed to restore partial vocabulary record");
  const bool closed = file.close();
  if (!closed) LOG_ERR("VOCAB", "Failed to flush vocabulary record");
  if (!written) LOG_ERR("VOCAB", "Could not append vocabulary record");
  return written && closed;
}

// ---------------------------------------------------------------------------
// Read and delete sides (addendum 2026-11-09: one Vocabulary Notebook).
//
// The notebook was write-only until the UI existed; the reader below is a
// bounded window over the file, never a whole-file load — the store grows
// unbounded on a 380 KB-RAM baseline. Records are the '\n'-terminated lines
// the append side writes; indexing counts newlines, so a partial (interrupted
// or externally truncated) trailing record is neither listed nor counted,
// exactly the tail the next append's recoverIncompleteRecord would discard.
namespace {
// The append side caps one record's content far below this (definition 2 KiB
// plus the bounded metadata fields). The window covers several max-size
// records and grows no further: a record larger than the window is skipped
// rather than parsed partially.
constexpr size_t TAIL_WINDOW_BYTES = 16 * 1024;
// Single-record scan cap for loadRecord: beyond this a record cannot be
// materialized safely, so it is refused instead of truncated silently.
constexpr size_t RECORD_SCAN_BYTES = 16 * 1024;
constexpr size_t STREAM_CHUNK_BYTES = 512;

struct RecordSpan {
  size_t start;  // first byte of the record line
  size_t end;    // one past the terminating '\n'
};

// Counts '\n' in [0, limit) by streaming: gives the file-order index of the
// first complete record inside a tail window that starts at `limit`. Read
// errors are REPORTED — a silently short count would misaddress every
// recordIndex the list hands to loadRecord/removeRecord.
bool countNewlinesBefore(HalFile& file, const size_t limit, uint32_t& newlinesOut) {
  uint8_t chunk[STREAM_CHUNK_BYTES];
  uint32_t newlines = 0;
  size_t pos = 0;
  if (!file.seek(0)) return false;
  while (pos < limit) {
    const size_t bytes = std::min(sizeof(chunk), limit - pos);
    if (file.read(chunk, bytes) != static_cast<int>(bytes)) return false;
    for (size_t i = 0; i < bytes; ++i) newlines += chunk[i] == '\n';
    pos += bytes;
  }
  newlinesOut = newlines;
  return true;
}

// Streams the file counting newline-terminated records and, when `span` is
// requested, captures the byte range of file-order record `index`.
// totalRecords (optional) receives the record count; a trailing partial
// record is not one. Returns false only on read errors.
bool locateRecord(HalFile& file, const uint32_t index, RecordSpan* span, uint32_t* totalRecords = nullptr) {
  uint8_t chunk[STREAM_CHUNK_BYTES];
  const size_t size = file.fileSize();
  uint32_t records = 0;
  size_t recordStart = 0;
  bool found = span == nullptr;
  if (!file.seek(0)) return false;
  size_t pos = 0;
  while (pos < size) {
    const size_t bytes = std::min(sizeof(chunk), size - pos);
    if (file.read(chunk, bytes) != static_cast<int>(bytes)) return false;
    for (size_t i = 0; i < bytes; ++i, ++pos) {
      if (chunk[i] != '\n') continue;
      ++records;
      if (!found && records == index + 1) {
        *span = {recordStart, pos + 1};
        found = true;
      }
      recordStart = pos + 1;
    }
  }
  if (totalRecords) *totalRecords = records;
  return found;
}

// Copies one byte range between two open files in bounded chunks.
bool copyRange(HalFile& source, HalFile& output, const size_t start, const size_t end) {
  uint8_t chunk[STREAM_CHUNK_BYTES];
  size_t pos = start;
  if (!source.seek(start)) return false;
  while (pos < end) {
    const size_t bytes = std::min(sizeof(chunk), end - pos);
    if (source.read(chunk, bytes) != static_cast<int>(bytes)) return false;
    if (output.write(chunk, bytes) != bytes) return false;
    pos += bytes;
  }
  return true;
}

// Flat-JSON quoted-field scan: one linear pass over the record line with a
// small state machine, copying and decoding the value of the first matching
// key — the same no-parser discipline the append-side RecordWriter uses in
// the opposite direction. Decodes exactly what the writer emits (the
// escaped quote, the escaped backslash, \u00xx control bytes) plus the
// plain UTF-8 pass-through.
bool scanQuotedField(const std::string_view record, const char* key, std::string& out) {
  const size_t keyLength = strlen(key);
  for (size_t i = 0; i + keyLength + 3 <= record.size(); ++i) {
    if (record[i] != '"' || record[i + keyLength + 1] != '"' || record[i + keyLength + 2] != ':') continue;
    if (record.compare(i + 1, keyLength, key) != 0) continue;
    size_t value = i + keyLength + 3;
    while (value < record.size() && (record[value] == ' ' || record[value] == '\t')) ++value;
    if (value >= record.size() || record[value] != '"') continue;
    ++value;
    out.clear();
    while (value < record.size()) {
      const char c = record[value];
      if (c == '"') return true;
      if (c == '\\') {
        ++value;
        if (value >= record.size()) return false;
        const char escape = record[value];
        if (escape == 'u') {
          if (value + 4 >= record.size()) return false;
          uint32_t codepoint = 0;
          for (size_t d = 1; d <= 4; ++d) {
            const char digit = record[value + d];
            const uint32_t nibble = digit < 'A' ? digit - '0' : (digit | 0x20) - 'a' + 10;
            if (nibble > 0xF) return false;
            codepoint = (codepoint << 4) | nibble;
          }
          char encoded[4];
          const char* encodedEnd = utf8AppendCodepoint(encoded, codepoint);
          out.append(encoded, static_cast<size_t>(encodedEnd - encoded));
          value += 4;
        } else if (escape == 'n') {
          out += '\n';
        } else if (escape == 't') {
          out += '\t';
        } else {
          out += escape;
        }
      } else {
        out += c;
      }
      ++value;
    }
    return false;  // unterminated value: malformed record
  }
  return false;
}

// Scalar field scan: copies the digits between ':' and the next delimiter.
// "null" and absence leave `out` untouched (the caller's default stands).
void scanNumberField(const std::string_view record, const char* key, uint32_t& out) {
  const size_t keyLength = strlen(key);
  for (size_t i = 0; i + keyLength + 3 <= record.size(); ++i) {
    if (record[i] != '"' || record[i + keyLength + 1] != '"' || record[i + keyLength + 2] != ':') continue;
    if (record.compare(i + 1, keyLength, key) != 0) continue;
    size_t value = i + keyLength + 3;
    while (value < record.size() && (record[value] == ' ' || record[value] == '\t')) ++value;
    unsigned long long parsed = 0;
    size_t digits = 0;
    while (value < record.size() && record[value] >= '0' && record[value] <= '9') {
      parsed = parsed * 10 + static_cast<unsigned long long>(record[value] - '0');
      if (parsed > UINT32_MAX) parsed = UINT32_MAX;  // clamp a corrupt/huge field
      ++value;
      ++digits;
    }
    if (digits > 0) out = static_cast<uint32_t>(parsed);
    return;
  }
}

// A record line is listed only when it looks like a notebook record: the
// same "word" field the append side writes first among the cheap fields.
// Skipped lines (partial tails, foreign bytes, oversized straddles) simply
// do not produce an entry — the rest keeps its indexes.
bool parseEntry(const std::string_view line, const uint32_t recordIndex, NotebookEntry& out) {
  if (!scanQuotedField(line, "word", out.word) || out.word.empty()) return false;
  (void)scanQuotedField(line, "headword", out.headword);
  out.timestamp = 0;
  scanNumberField(line, "timestamp", out.timestamp);
  out.recordIndex = recordIndex;
  return true;
}
}  // namespace

bool VocabularyStore::loadRecent(std::vector<NotebookEntry>& out, const size_t limit) {
  out.clear();
  if (limit == 0) return true;
  if (!settlePendingRewrite()) {
    out.clear();
    return false;
  }
  HalFile file = Storage.open(STAGE_LIVE_PATH, O_RDONLY);
  if (!file) return true;  // absent file = an empty fresh notebook, not a failure
  const size_t size = file.fileSize();
  if (size == 0) return true;

  const size_t window = std::min(size, TAIL_WINDOW_BYTES);
  const size_t windowStart = size - window;
  auto buffer = makeUniqueNoThrow<char[]>(window);
  if (!buffer) {
    LOG_ERR("VOCAB", "Not enough memory for notebook tail window");
    return false;
  }
  if (!file.seek(windowStart) || file.read(buffer.get(), window) != static_cast<int>(window)) {
    LOG_ERR("VOCAB", "Failed to read notebook tail");
    return false;
  }
  const std::string_view content(buffer.get(), window);
  // Index of the first complete record visible in the window: the count of
  // newlines before the window, plus one more when the window straddles a
  // record — its terminating newline (the first one inside the window) also
  // finishes a record in front of it. A window that starts exactly on a
  // record boundary (byte before the window is '\n') is NOT a straddle.
  uint32_t firstRecordIndex = 0;
  if (!countNewlinesBefore(file, windowStart, firstRecordIndex)) {
    LOG_ERR("VOCAB", "Failed to read notebook record base");
    return false;
  }

  // The segment before the first '\n' in the window is the tail of the
  // straddled record (never fully visible) — skipped. With window == size
  // the whole file is visible, so nothing is skipped.
  size_t scanStart = 0;
  if (window < size && windowStart > 0) {
    if (!file.seek(windowStart - 1)) {
      LOG_ERR("VOCAB", "Failed to read notebook tail boundary");
      return false;
    }
    const int beforeWindow = file.read();
    const size_t firstNewline = content.find('\n');
    if (firstNewline == std::string_view::npos) {
      // The tail window holds no complete record at all.
      return true;
    }
    if (beforeWindow >= 0 && static_cast<char>(beforeWindow) != '\n' && firstNewline > 0) {
      scanStart = firstNewline + 1;
      firstRecordIndex += 1;
    }
  } else if (window < size) {
    // windowStart == 0 cannot happen with window < size, kept for symmetry.
    const size_t firstNewline = content.find('\n');
    if (firstNewline == std::string_view::npos) return true;
  }

  // Collect complete-record spans window-locally, then parse only the newest
  // `limit` of them in reverse — the caller never materializes more rows
  // than asked and never parses a window block worth of discards.
  struct Span {
    uint32_t start;
    uint32_t end;  // local: one past the '\n'
  };
  static_assert(TAIL_WINDOW_BYTES <= UINT32_MAX, "spans address the window with 32-bit offsets");
  std::vector<Span> spans;
  for (size_t i = scanStart; i < content.size();) {
    const size_t newline = content.find('\n', i);
    if (newline == std::string_view::npos) break;  // trailing partial record: skipped
    spans.push_back({static_cast<uint32_t>(i), static_cast<uint32_t>(newline + 1)});
    i = newline + 1;
  }

  const size_t keep = std::min(limit, spans.size());
  out.reserve(keep);
  for (size_t s = spans.size() - keep; s < spans.size(); ++s) {
    NotebookEntry entry;
    const std::string_view line = content.substr(spans[s].start, spans[s].end - spans[s].start - 1);
    if (parseEntry(line, firstRecordIndex + s, entry)) out.push_back(std::move(entry));
  }
  std::reverse(out.begin(), out.end());
  // Tolerated skips are not failures: a tail full of malformed records just
  // leaves the list empty while the file itself remains readable.
  return true;
}

bool VocabularyStore::loadRecord(const uint32_t index, NotebookRecord& out) {
  if (!settlePendingRewrite()) return false;
  HalFile file = Storage.open(STAGE_LIVE_PATH, O_RDONLY);
  if (!file) return false;
  RecordSpan span{};
  uint32_t total = 0;
  if (!locateRecord(file, index, &span, &total) || index >= total) return false;
  const size_t length = span.end - span.start - 1;  // excluding the '\n'
  if (length > RECORD_SCAN_BYTES) {
    LOG_ERR("VOCAB", "Record %u exceeds the scan bound", static_cast<unsigned>(index));
    return false;
  }
  auto buffer = makeUniqueNoThrow<char[]>(length);
  if (!buffer) {
    LOG_ERR("VOCAB", "Not enough memory for notebook record");
    return false;
  }
  if (!file.seek(span.start) || file.read(buffer.get(), length) != static_cast<int>(length)) {
    LOG_ERR("VOCAB", "Failed to read notebook record");
    return false;
  }
  const std::string_view line(buffer.get(), length);
  NotebookRecord record;
  if (!scanQuotedField(line, "word", record.word) || record.word.empty()) return false;
  (void)scanQuotedField(line, "headword", record.headword);
  (void)scanQuotedField(line, "definition", record.definition);
  (void)scanQuotedField(line, "title", record.title);
  (void)scanQuotedField(line, "author", record.author);
  (void)scanQuotedField(line, "chapter", record.chapter);
  record.timestamp = 0;
  scanNumberField(line, "timestamp", record.timestamp);
  uint32_t page = 0;
  scanNumberField(line, "page", page);
  record.page = static_cast<uint16_t>(std::min<uint32_t>(page, UINT16_MAX));
  out = std::move(record);
  return true;
}

uint32_t VocabularyStore::count() {
  if (!settlePendingRewrite()) return 0;
  HalFile file = Storage.open(STAGE_LIVE_PATH, O_RDONLY);
  if (!file) return 0;
  uint32_t total = 0;
  if (!locateRecord(file, UINT32_MAX, nullptr, &total)) return 0;
  return total;
}

bool VocabularyStore::removeRecord(const uint32_t index) {
  if (!settlePendingRewrite()) return false;
  // A pending adoption that cannot publish must not strand its records under
  // a fresh append-created store.
  if (!Storage.exists(STAGE_LIVE_PATH)) return false;
  HalFile file = Storage.open(STAGE_LIVE_PATH, O_RDONLY);
  if (!file) {
    LOG_ERR("VOCAB", "Cannot open vocabulary file to delete a record");
    return false;
  }
  RecordSpan span{};
  if (!locateRecord(file, index, &span)) return false;
  const size_t size = file.fileSize();

  // Rewrite without the record through the same publication path the store
  // uses elsewhere: stage a temp file, then replaceFile (SOLID over the
  // in-place splice, which could corrupt the only copy of the user's data).
  HalFile staged = Storage.open(STAGE_TEMP_PATH, O_RDWR | O_CREAT | O_TRUNC);
  if (!staged) {
    LOG_ERR("VOCAB", "Cannot stage vocabulary rewrite");
    return false;
  }
  const bool copied = copyRange(file, staged, 0, span.start) && copyRange(file, staged, span.end, size);
  if (!file.close()) LOG_ERR("VOCAB", "Failed to close vocabulary file after rewrite copy");
  const bool stagedOk = copied && staged.close();
  if (!stagedOk) {
    staged.close();
    (void)Storage.remove(STAGE_TEMP_PATH);  // incomplete stage: the live copy is intact
    LOG_ERR("VOCAB", "Failed to stage vocabulary rewrite");
    return false;
  }
  if (!Storage.replaceFile(STAGE_TEMP_PATH, STAGE_LIVE_PATH)) {
    // The stage is complete and is the only surviving copy after the FAT
    // remove — leave it for settlePendingRewrite() to adopt; never delete it.
    LOG_ERR("VOCAB", "Failed to publish vocabulary rewrite");
    return false;
  }
  return true;
}

bool VocabularyStore::clear() {
  if (!settlePendingRewrite()) return false;
  if (!Storage.exists(STAGE_LIVE_PATH)) return true;
  HalFile staged = Storage.open(STAGE_TEMP_PATH, O_RDWR | O_CREAT | O_TRUNC);
  if (!staged) {
    LOG_ERR("VOCAB", "Cannot stage vocabulary clear");
    return false;
  }
  if (!staged.close()) {
    staged.close();
    (void)Storage.remove(STAGE_TEMP_PATH);
    LOG_ERR("VOCAB", "Failed to flush vocabulary clear");
    return false;
  }
  if (!Storage.replaceFile(STAGE_TEMP_PATH, STAGE_LIVE_PATH)) {
    // An empty stage is the only surviving copy after the FAT remove — leave
    // it: adopting it completes the requested clear.
    LOG_ERR("VOCAB", "Failed to publish vocabulary clear");
    return false;
  }
  return true;
}
