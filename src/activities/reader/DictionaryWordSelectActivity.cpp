#include "DictionaryWordSelectActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "HapticFeedback.h"
#include "components/UITheme.h"
#include "util/DictionarySelection.h"

namespace {

constexpr unsigned long POPUP_DURATION_MS = 1500;
constexpr size_t MARKER_BUF_SIZE = 64;

void indexBuildYield(void*) { vTaskDelay(1); }

// Normalize a touched word to match the EPUB parser's footnote-number contract
// (ChapterHtmlSlimParser.cpp:1537-1561): strip leading whitespace + '[', and
// trailing whitespace + ']'; also trim any whitespace left INSIDE the brackets
// so "[ 12 ]" matches the parser-stored "12". The parser does NOT strip
// parentheses, so '(1)' stays '(1)' and must NOT collapse into '1' — stripping
// parens would make '[1]' and '(1)' compare equal and turn '(2024)' into a
// numeric no-op. (This function intentionally only touches brackets, never
// parentheses.) entry.number is already normalized this same way by the parser,
// so the touched word is compared against entry.number verbatim (no
// re-normalization of the entry side, which would broaden matches).
// Returns the length of the normalized text in dst.
size_t normalizeMarker(const char* src, char* dst, size_t dstSize) {
  const char* p = src;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  if (*p == '[') {
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;  // trim inside [
  }

  const char* end = p + std::strlen(p);
  if (end > p) end--;
  while (end > p && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end--;
  if (end > p && *end == ']') {
    end--;
    while (end > p && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end--;
  }

  size_t len = static_cast<size_t>(end - p + 1);
  if (len >= dstSize) len = dstSize - 1;
  std::memcpy(dst, p, len);
  dst[len] = '\0';
  return len;
}

// True when the normalized word is non-empty and contains at least one digit.
bool isFootnoteMarker(const char* word) {
  char buf[MARKER_BUF_SIZE];
  normalizeMarker(word, buf, sizeof(buf));
  if (buf[0] == '\0') return false;
  for (const char* p = buf; *p != '\0'; p++) {
    if (*p >= '0' && *p <= '9') return true;
  }
  return false;
}

// True when the normalized word is non-empty and contains only ASCII digits.
bool isNumericMarker(const char* word) {
  char buf[MARKER_BUF_SIZE];
  normalizeMarker(word, buf, sizeof(buf));
  if (buf[0] == '\0') return false;
  for (const char* p = buf; *p != '\0'; p++) {
    if (*p < '0' || *p > '9') return false;
  }
  return true;
}

}  // namespace

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  const unsigned long enterStart = millis();
  LOG_DBG("DICT", "Selector onEnter start");
  fontId = SETTINGS.getReaderFontId();
  lineHeight = renderer.getLineHeight(fontId);
  if (!lookupText.empty()) {
    // Direct lookup (clip-selection Lookup action): skip the selection UI and
    // resolve the preselected text immediately, then hand back to the reader.
    lookupPending = true;
    popupMsg = StrId::STR_DICT_LOOKING_UP;
    popup = Popup::Busy;
    requestUpdate();
    return;
  }
  // No null check: a failed allocation just disables the differential
  // fast path (drawHighlightWithSnapshot skips the read), keeping the
  // full-repaint path as the fallback.
  snapshot = makeUniqueNoThrow<uint8_t[]>(SNAPSHOT_CAPACITY);
  LOG_DBG("DICT", "Selector snapshot allocated=%d; extracting words", snapshot ? 1 : 0);
  extractWords();
  LOG_DBG("DICT", "Selector extraction complete: words=%u groups=%u rows=%u in %lums",
          static_cast<unsigned>(words.size()), static_cast<unsigned>(selections.size()),
          static_cast<unsigned>(rowCount), millis() - enterStart);
  // Start on the middle row's word nearest mid-screen instead of top-left:
  // any word on the page is then at most half a page of moves away.
  if (!selections.empty()) {
    const int initial = closestInRow(rowCount / 2, renderer.getScreenWidth() / 2);
    if (initial >= 0) selected = initial;
  }

  if (initialX >= 0) {
    const int hit = wordAt(initialX, initialY);
    if (hit >= 0) {
      selected = hit;
      // The reader's long-press already suppresses the original touch
      // contact. Look up the selected logical word immediately; the selector
      // remains available as the fallback when no word was hit.
      performLookup();
      return;
    }
  }

  requestUpdate();
  LOG_DBG("DICT", "Selector onEnter complete in %lums", millis() - enterStart);
}

void DictionaryWordSelectActivity::extractWords() {
#if defined(CROSSPOINT_TTF_READER)
  if (ttfMode) {
    extractWordsTtf();
    return;
  }
#endif
  const unsigned long extractStart = millis();
  LOG_DBG("DICT", "extractWords start: elements=%u", static_cast<unsigned>(page->elements.size()));
  words.clear();
  selections.clear();
  selectionSegments.clear();
  words.reserve(128);
  selections.reserve(128);
  selectionSegments.reserve(128);
  rowCount = 0;

  // Single walk: collect the selectable words while accumulating their text
  // and styles (~2KB transient string, freed on return). Widths are measured
  // afterwards: merging the page's codepoints into the SD font's persistent
  // advance table first keeps getTextAdvanceX on the in-RAM path instead of
  // loading glyphs from SD one overflow slot at a time.
  std::string pageText;
  pageText.reserve(2048);
  uint8_t styleMask = 0;

  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto* block = line->getBlock();
    if (!block || !block->valid()) continue;

    bool rowHasWords = false;
    const int ascender = renderer.getFontAscenderSize(fontId);
    const int rubyShift = block->getRubyShift(ascender);
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      const char* text = block->wordText(i);
      if (!DictionarySelection::isSelectableToken(text)) continue;
      const DictionarySelection::TokenSpan span =
          DictionarySelection::trimTokenEdges(std::string_view(text, block->wordTextLen(i)));
      if (span.length == 0) continue;

      WordBox box;
      box.x = static_cast<int16_t>(line->xPos + block->wordXpos(i) + marginLeft);
      box.y = static_cast<int16_t>(line->yPos + marginTop + rubyShift);
      box.style = block->wordStyle(i);
      box.width = 0;  // measured below, once the advance table is ready
      box.row = rowCount;
      box.text = text;
      box.textOffset = static_cast<uint16_t>(span.start);
      box.textLength = static_cast<uint16_t>(span.length);
      // Raw span kept for footnote resolution (see WordBox::rawLength).
      box.rawLength = static_cast<uint16_t>(block->wordTextLen(i));
      box.selectionGroup = block->selectionGroup(i);
      box.syntheticHyphen = block->hasSyntheticHyphen(i);
      box.height = static_cast<int16_t>(lineHeight);
      words.push_back(box);
      rowHasWords = true;

      pageText.append(text);
      pageText.push_back(' ');
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(box.style) & 0x03));
    }
    if (rowHasWords) rowCount++;
  }

  if (styleMask == 0) styleMask = 0x01;  // REGULAR
  LOG_DBG("DICT", "extractWords collected: words=%u rows=%u bytes=%u styles=0x%02X; warming font",
          static_cast<unsigned>(words.size()), static_cast<unsigned>(rowCount), static_cast<unsigned>(pageText.size()),
          styleMask);
  renderer.ensureSdCardFontReady(fontId, pageText.c_str(), styleMask);
  LOG_DBG("DICT", "extractWords font warm complete after %lums", millis() - extractStart);
  for (auto& word : words) {
    const std::string prefix(word.text, word.textOffset);
    const std::string selectedText(word.text + word.textOffset, word.textLength);
    word.x = static_cast<int16_t>(word.x + renderer.getTextAdvanceX(fontId, prefix.c_str(), word.style));
    word.width = static_cast<int16_t>(renderer.getTextAdvanceX(fontId, selectedText.c_str(), word.style));
  }

  std::vector<uint32_t> sourceGroups;
  sourceGroups.reserve(words.size());
  std::transform(words.begin(), words.end(), std::back_inserter(sourceGroups),
                 [](const auto& word) { return word.selectionGroup; });
  const auto grouped = DictionarySelection::groupTokens(sourceGroups);
  std::transform(grouped.groups.begin(), grouped.groups.end(), std::back_inserter(selections), [](const auto& group) {
    return WordSelection{group.sourceGroup, static_cast<uint16_t>(group.segmentStart),
                         static_cast<uint16_t>(group.segmentCount)};
  });
  std::transform(grouped.segments.begin(), grouped.segments.end(), std::back_inserter(selectionSegments),
                 [](const size_t segment) { return static_cast<uint16_t>(segment); });
  LOG_DBG("DICT", "extractWords complete: groups=%u in %lums", static_cast<unsigned>(selections.size()),
          millis() - extractStart);
}

#if defined(CROSSPOINT_TTF_READER)
// TTF path: boxes arrive prebuilt from engine run geometry (TtfWordSelect);
// this only maps them into the selector's structures and groups fragments by
// their engine-computed selection groups.
void DictionaryWordSelectActivity::extractWordsTtf() {
  words.clear();
  selections.clear();
  selectionSegments.clear();
  words.reserve(ttfData.boxes.size());
  selections.reserve(64);
  selectionSegments.reserve(128);
  rowCount = 0;

  int16_t lastY = INT16_MIN;
  for (const auto& tb : ttfData.boxes) {
    if (tb.y != lastY) {
      ++rowCount;
      lastY = tb.y;
    }
    WordBox box;
    box.x = tb.x;
    box.y = tb.y;
    box.width = tb.width;
    box.row = static_cast<uint16_t>(rowCount - 1);
    box.text = tb.text;
    box.textOffset = tb.textOffset;
    box.textLength = tb.textLength;
    box.rawLength = tb.rawLength;
    box.style = EpdFontFamily::REGULAR;  // TTF highlight repaint draws no text
    box.selectionGroup = tb.selectionGroup;
    box.syntheticHyphen = tb.syntheticHyphen;
    box.height = tb.height;
    words.push_back(box);
  }

  std::vector<uint32_t> sourceGroups;
  sourceGroups.reserve(words.size());
  std::transform(words.begin(), words.end(), std::back_inserter(sourceGroups),
                 [](const auto& word) { return word.selectionGroup; });
  const auto grouped = DictionarySelection::groupTokens(sourceGroups);
  std::transform(grouped.groups.begin(), grouped.groups.end(), std::back_inserter(selections), [](const auto& group) {
    return WordSelection{group.sourceGroup, static_cast<uint16_t>(group.segmentStart),
                         static_cast<uint16_t>(group.segmentCount)};
  });
  std::transform(grouped.segments.begin(), grouped.segments.end(), std::back_inserter(selectionSegments),
                 [](const size_t segment) { return static_cast<uint16_t>(segment); });
  LOG_DBG("DICT", "extractWordsTtf complete: words=%u groups=%u", static_cast<unsigned>(words.size()),
          static_cast<unsigned>(selections.size()));
}
#endif

// Index of the word whose box (with finger-sized slop) contains the touch
// point; -1 when the touch lands on no word. Boxes never overlap after the
// slop grows them, at worst they touch, so first hit wins.
int DictionaryWordSelectActivity::wordAt(const int x, const int y) const {
  constexpr int SLOP = 4;  // matches the highlight box (+2) plus finger error
  for (int selection = 0; selection < static_cast<int>(selections.size()); selection++) {
    const auto& group = selections[selection];
    for (uint16_t i = 0; i < group.segmentCount; i++) {
      const WordBox& word = words[selectionSegments[group.segmentStart + i]];
      if (x >= word.x - SLOP && x < word.x + word.width + SLOP && y >= word.y - SLOP &&
          y < word.y + word.height + SLOP) {
        return selection;
      }
    }
  }
  return -1;
}

std::string DictionaryWordSelectActivity::selectionText(const int selectionIndex) const {
  std::string text;
  if (selectionIndex < 0 || selectionIndex >= static_cast<int>(selections.size())) return text;

  const auto& group = selections[selectionIndex];
  size_t totalBytes = 0;
  for (uint16_t i = 0; i < group.segmentCount; i++) {
    const WordBox& word = words[selectionSegments[group.segmentStart + i]];
    totalBytes += word.textLength;
  }
  text.reserve(totalBytes);
  for (uint16_t i = 0; i < group.segmentCount; i++) {
    const WordBox& word = words[selectionSegments[group.segmentStart + i]];
    const std::string_view wordView(word.text + word.textOffset, word.textLength);
    const size_t length = DictionarySelection::logicalSegmentLength(wordView, word.syntheticHyphen);
    const char* wordText = word.text + word.textOffset;
    text.append(wordText, length);
  }
  return text;
}

std::string DictionaryWordSelectActivity::selectionRawText(const int selectionIndex) const {
  std::string text;
  if (selectionIndex < 0 || selectionIndex >= static_cast<int>(selections.size())) return text;

  const auto& group = selections[selectionIndex];
  size_t totalBytes = 0;
  for (uint16_t i = 0; i < group.segmentCount; i++) {
    const WordBox& word = words[selectionSegments[group.segmentStart + i]];
    totalBytes += word.rawLength;
  }
  text.reserve(totalBytes);
  for (uint16_t i = 0; i < group.segmentCount; i++) {
    const WordBox& word = words[selectionSegments[group.segmentStart + i]];
    text.append(word.text, word.rawLength);
  }
  return text;
}

std::string DictionaryWordSelectActivity::resolveFootnoteHref(const char* word) const {
  char normalized[MARKER_BUF_SIZE];
  normalizeMarker(word, normalized, sizeof(normalized));
  if (normalized[0] == '\0') return {};

#if defined(CROSSPOINT_TTF_READER)
  if (ttfMode) {
    // entry.number is already normalized by the TTF footnote collection —
    // compare against it verbatim, same as the legacy page list.
    const auto it = std::find_if(ttfData.footnotes.begin(), ttfData.footnotes.end(), [&](const FootnoteEntry& entry) {
      return std::strcmp(entry.number, normalized) == 0;
    });
    if (it != ttfData.footnotes.end()) {
      return std::string(it->href);
    }
    return {};
  }
#endif
  // entry.number is already normalized by the parser (whitespace + '['/'['
  // stripped, parentheses preserved), so compare against it verbatim.
  const auto it = std::find_if(page->footnotes.begin(), page->footnotes.end(),
                               [&](const FootnoteEntry& entry) { return std::strcmp(entry.number, normalized) == 0; });
  if (it != page->footnotes.end()) {
    return std::string(it->href);
  }
  return {};
}

bool DictionaryWordSelectActivity::resolveFootnoteOrFinish(const char* raw, const char* trimmed) {
  // Try both views for the href: the raw token keeps parentheses ("(1)" must
  // match the parser-stored "(1)" and never "[1]"), the trimmed token handles
  // markers the raw form carries attached punctuation on ("[1]." -> "1").
  if (isFootnoteMarker(raw)) {
    if (const std::string href = resolveFootnoteHref(raw); !href.empty()) {
      setResult(ActivityResult(FootnoteResult{href}));
      finish();
      return true;
    }
  }
  if (trimmed != raw && isFootnoteMarker(trimmed)) {
    if (const std::string href = resolveFootnoteHref(trimmed); !href.empty()) {
      setResult(ActivityResult(FootnoteResult{href}));
      finish();
      return true;
    }
  }
  if (!isFootnoteMarker(raw) && !(trimmed != raw && isFootnoteMarker(trimmed))) return false;
  // A bare numeric marker that matches no footnote closes the lookup instead
  // of searching the dictionary for a year. Accept either form: the raw text
  // keeps parentheses ("(2024)"), the trimmed text loses them ("2024").
  if (isNumericMarker(raw) || isNumericMarker(trimmed)) {
    finish();
    return true;
  }
  return false;
}

// Index of the word in `row` whose horizontal center is closest to centerX;
// -1 when the row has no words (or only `excludeSelection`'s own segments).
int DictionaryWordSelectActivity::closestInRow(const uint16_t row, const int centerX,
                                               const int excludeSelection) const {
  int best = -1;
  int bestDistance = INT_MAX;
  for (int selection = 0; selection < static_cast<int>(selections.size()); selection++) {
    if (selection == excludeSelection) continue;
    const auto& group = selections[selection];
    for (uint16_t i = 0; i < group.segmentCount; i++) {
      const WordBox& word = words[selectionSegments[group.segmentStart + i]];
      if (word.row != row) continue;
      const int distance = std::abs(word.x + word.width / 2 - centerX);
      if (distance < bestDistance) {
        bestDistance = distance;
        best = selection;
      }
    }
  }
  return best;
}

void DictionaryWordSelectActivity::moveVertical(const int direction) {
  if (selected < 0 || selected >= static_cast<int>(selections.size())) return;
  const auto& currentGroup = selections[selected];
  if (currentGroup.segmentCount == 0) return;
  // A logical word can wrap across rows. When moving down, its own second
  // segment sits in the next row, so anchor on the LAST segment (first when
  // moving up) — otherwise the target row is the row we're already on.
  const uint16_t anchorIdx =
      selectionSegments[currentGroup.segmentStart + (direction > 0 ? currentGroup.segmentCount - 1 : 0)];
  const WordBox& current = words[anchorIdx];
  int targetRow = static_cast<int>(current.row) + direction;

  while (targetRow >= 0 && targetRow < static_cast<int>(rowCount)) {
    const int best = closestInRow(static_cast<uint16_t>(targetRow), current.x + current.width / 2, selected);
    if (best >= 0 && best != selected) {
      selected = best;
      requestUpdate();
      return;
    }
    // The row only contained this selection's own wrapped segment(s): keep
    // scanning in the movement direction.
    targetRow += direction;
  }
}

void DictionaryWordSelectActivity::performLookup() {
  if (mode != TouchLongPressMode::Footnote) {
    performLookup(selectionText(selected));
    return;
  }
  // Footnote resolution uses the RAW token text: the parser stores footnote
  // numbers with their parentheses preserved, and trimTokenEdges strips them,
  // so a trimmed "(1)" could never match. The dictionary query keeps the
  // TRIMMED text.
  performLookup(selectionRawText(selected), selectionText(selected));
}

void DictionaryWordSelectActivity::performLookup(const std::string& query) {
  performLookup(selectionRawText(selected), query);
}

void DictionaryWordSelectActivity::performLookup(const std::string& raw, const std::string& trimmed) {
  if (mode == TouchLongPressMode::Footnote && resolveFootnoteOrFinish(raw.c_str(), trimmed.c_str())) {
    return;
  }
  // DEBUG(dict-debug): lookup diagnostics, visible at LOG_LEVEL >= 1/2.
  LOG_INF("DICT", "Long-press lookup: raw='%s' trimmed='%s' headword_mode=%s", raw.c_str(), trimmed.c_str(),
          mode == TouchLongPressMode::Footnote ? "FOOTNOTE" : "DICTIONARY");
  {
    const char* q = lookupText.empty() ? trimmed.c_str() : lookupText.c_str();
    LOG_DBG("DICT", "lookup key='%s' (using %s)", q, lookupText.empty() ? "trimmed" : "lookupText(explicit)");
    (void)q;  // keep defined when logging is compiled out
  }
  {
    RenderLock lock;
    popupMsg = StrId::STR_DICT_LOOKING_UP;
    popup = Popup::Busy;
  }
  if (!dictOpenAttempted) {
    dictOpenAttempted = true;
    dictOpenOk = dict.open(SETTINGS.dictionaryName);
    // needsIndex() opens and validates the .qidx sidecar, so ask it once per
    // open rather than once per word: the answer only changes when we build
    // the sidecar ourselves, which is handled below.
    dictNeedsIndex = dictOpenOk && dict.needsIndex();
  }
  {
    RenderLock lock;
    popupMsg = dictNeedsIndex ? StrId::STR_DICT_INDEXING : StrId::STR_DICT_LOOKING_UP;
  }
  requestUpdateAndWait();  // paint the page + busy popup before blocking on SD

  bool ok = dictOpenOk;
  Dictionary::IndexResult indexResult = Dictionary::IndexResult::Ok;
  if (ok && dictNeedsIndex) {
    ok = dict.buildIndex(&indexBuildYield, nullptr, &indexResult);
    dictNeedsIndex = !ok;  // a successful build leaves the sidecar fresh; a failed one retries
  }

  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  const bool found =
      ok && dict.lookup(lookupText.empty() ? trimmed.c_str() : lookupText.c_str(), definition, headword, &result);

  // DEBUG(dict-debug): lookup diagnostics, visible at LOG_LEVEL >= 1/2.
  LOG_DBG("DICT", "lookup found=%d htmlDefinitions=%d headword='%s' definition_bytes=%u", found,
          dict.definitionsAreHtml(), headword.c_str(), static_cast<unsigned>(definition.size()));
  if (!definition.empty()) {
    LOG_DBG("DICT", "definition head (first 120 bytes): %.120s", definition.c_str());
    LOG_DBG("DICT", "definition tail (last 80 bytes): %.80s",
            definition.size() > 80 ? definition.c_str() + definition.size() - 80 : definition.c_str());
  }
  if (!found)
    LOG_DBG("DICT", "lookup MISS: ok=%d dictNeedsIndex=%d indexResult=%d lookupResult=%d", ok, dictNeedsIndex,
            static_cast<int>(indexResult), static_cast<int>(result));

  if (found) {
    {
      RenderLock lock;
      popup = Popup::None;
      // Direct lookup returns to the reader; this page is no longer needed.
      if (!lookupText.empty()) page.reset();
    }
    auto activity = makeUniqueNoThrow<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(headword),
                                                                    std::move(definition), SETTINGS.dictionaryName,
                                                                    dict.definitionsAreHtml());
    // enableLookup moves the query into the definition activity's lookup state.
    // Pass a copy of the actual lookup key — the same string the call above
    // resolved — because the callback below still reads lookupText to decide
    // whether to return straight to the reader.
    if (!activity ||
        !activity->enableLookup(dict, lookupText.empty() ? std::string(trimmed) : std::string(lookupText))) {
      LOG_ERR("DICT", "OOM: definition activity");
      popup = Popup::Error;
      popupMsg = StrId::STR_DICT_LOW_MEMORY;
      popupTime = millis();
      requestUpdate();
      return;
    }
    startActivityForResult(std::move(activity), [this](const ActivityResult& result) {
      if (!result.isCancelled && std::holds_alternative<DictionarySearchResult>(result.data)) {
        performLookup(std::get<DictionarySearchResult>(result.data).text);
      } else if (!lookupText.empty() || (initialX >= 0 && initialY >= 0)) {
        setResult(ActivityResult{result});
        finish();
      } else {
        popup = Popup::None;
        requestUpdate();
      }
    });
    return;
  }
  // Name the failure: a genuine miss is "Not found"; a word that WAS found but
  // couldn't be read is a real error — and we distinguish decompression from a
  // low-memory allocation from a generic read error.
  if (!ok) {
    popup = Popup::Error;
    // An index build allocates a scan buffer, so it fails the same way lookups
    // do on a fragmented heap — name that rather than a generic error.
    switch (indexResult) {
      case Dictionary::IndexResult::LowMemory:
        popupMsg = StrId::STR_DICT_LOW_MEMORY;
        break;
      case Dictionary::IndexResult::ReadError:
        popupMsg = StrId::STR_DICT_READ_FAILED;
        break;
      case Dictionary::IndexResult::Ok:
      default:
        popupMsg = StrId::STR_DICT_ERROR;  // dict.open() failed, not the index
        break;
    }
  } else {
    switch (result) {
      case Dictionary::LookupResult::Decompress:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_DECOMPRESS_ERROR;
        break;
      case Dictionary::LookupResult::LowMemory:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_LOW_MEMORY;
        break;
      case Dictionary::LookupResult::ReadError:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_READ_FAILED;
        break;
      case Dictionary::LookupResult::NotFound:
      default:
        popup = Popup::NotFound;
        popupMsg = StrId::STR_DICT_NOT_FOUND;
        break;
    }
  }

  // A long-press miss should still lead to an editable dictionary lookup.
  // Show the same definition overlay and open its keyboard with the failed
  // word prefilled; this keeps correction in one interaction instead of
  // forcing the user back to the reader first.
  if (initialX >= 0 && initialY >= 0 && ok && result == Dictionary::LookupResult::NotFound) {
    popup = Popup::None;
    startActivityForResult(
        std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, trimmed, tr(STR_DICT_NOT_FOUND),
                                                       SETTINGS.dictionaryName, false, true),
        [this](const ActivityResult& result) {
          if (!result.isCancelled && std::holds_alternative<DictionarySearchResult>(result.data)) {
            performLookup(std::get<DictionarySearchResult>(result.data).text);
          } else {
            finish();
          }
        });
    return;
  }

  popupTime = millis();
  requestUpdate();
}

void DictionaryWordSelectActivity::loop() {
  if (lookupPending) {
    lookupPending = false;
    performLookup(lookupText);
    return;
  }
  if (popup == Popup::NotFound || popup == Popup::Error) {
    if (millis() - popupTime >= POPUP_DURATION_MS) {
      if (!lookupText.empty() || (initialX >= 0 && initialY >= 0)) {
        // An Error popup expiring cancels the lookup result; a NotFound one
        // does not (the parent distinguishes a failed lookup from an error).
        result.isCancelled = popup == Popup::Error;
        finish();
      } else {
        popup = Popup::None;
        requestUpdate();
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !selections.empty()) {
    performLookup();
    return;
  }

  if (selections.empty()) return;

  // Touch: a touch-down moves the highlight to the touched word (differential
  // repaint), a tap on a word selects and looks it up in one go.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTouchDown(tx, ty)) {
    const int hit = wordAt(tx, ty);
    if (hit >= 0 && hit != selected) {
      selected = hit;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const int hit = wordAt(tx, ty);
    if (hit >= 0) {
      haptic_feedback::touchAction();
      selected = hit;
      performLookup();
    }
    return;
  }

  const bool hasNextWord = selected + 1 < static_cast<int>(selections.size());
  if (mappedInput.wasPressed(MappedInputManager::Button::ScreenLeft) && selected > 0) {
    selected--;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenRight) && hasNextWord) {
    selected++;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenUp)) {
    moveVertical(-1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenDown)) {
    moveVertical(1);
  }
}

// Saves the pixels under all rendered segments of selections[selected], then
// draws the highlight over them. A logically whole word can span two lines
// after pagination, so the snapshot is the union rectangle of those segments.
// Returns false when the pixels could not be saved (no buffer / oversize box) —
// the highlight is drawn regardless, but the next selection move must do a full
// repaint.
bool DictionaryWordSelectActivity::drawHighlightWithSnapshot() {
  if (selected < 0 || selected >= static_cast<int>(selections.size())) return false;
  const auto& group = selections[selected];
  if (group.segmentCount == 0) return false;

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  int left = screenWidth;
  int top = screenHeight;
  int right = 0;
  int bottom = 0;
  for (uint16_t i = 0; i < group.segmentCount; i++) {
    const WordBox& word = words[selectionSegments[group.segmentStart + i]];
    left = std::min(left, std::max(0, static_cast<int>(word.x) - 2));
    top = std::min(top, std::max(0, static_cast<int>(word.y) - 2));
    right = std::max(right, std::min(screenWidth, static_cast<int>(word.x) + word.width + 2));
    bottom = std::max(bottom, std::min(screenHeight, static_cast<int>(word.y) + lineHeight + 2));
  }
  const int hx = left;
  const int hy = top;
  const int hw = right - left;
  const int hh = bottom - top;
  if (hw <= 0 || hh <= 0) return false;

  bool saved = false;
  if (snapshot) {
    saved = renderer.readFramebufferRegion(hx, hy, hw, hh, snapshot.get(), SNAPSHOT_CAPACITY) > 0;
  }
  snapshotX = static_cast<int16_t>(hx);
  snapshotY = static_cast<int16_t>(hy);
  snapshotW = static_cast<int16_t>(hw);
  snapshotH = static_cast<int16_t>(hh);
  snapshotIdx = saved ? selected : -1;

  for (uint16_t i = 0; i < group.segmentCount; i++) {
    const WordBox& word = words[selectionSegments[group.segmentStart + i]];
    const int wordX = std::max(0, static_cast<int>(word.x) - 2);
    const int wordY = std::max(0, static_cast<int>(word.y) - 2);
    const int wordRight = std::min(screenWidth, static_cast<int>(word.x) + word.width + 2);
    const int wordBottom = std::min(screenHeight, static_cast<int>(word.y) + word.height + 2);
#if defined(CROSSPOINT_TTF_READER)
    if (ttfMode) {
      // Outline only: the engine font draws the page, and redrawing a token
      // through GfxRenderer's SD-font drawText would paint the wrong glyphs.
      renderer.drawRect(wordX, wordY, wordRight - wordX, wordBottom - wordY, true);
      continue;
    }
#endif
    renderer.fillRect(wordX, wordY, wordRight - wordX, wordBottom - wordY, true);
    const std::string selectedText(word.text + word.textOffset, word.textLength);
    renderer.drawText(fontId, word.x, word.y, selectedText.c_str(), false, word.style);
  }
  return saved;
}

void DictionaryWordSelectActivity::drawHighlightRange() const {
  if (selected < 0 || selected >= static_cast<int>(selections.size())) return;
  const auto& group = selections[selected];
  for (uint16_t i = 0; i < group.segmentCount; i++) {
    const WordBox& word = words[selectionSegments[group.segmentStart + i]];
    const std::string selectedText(word.text + word.textOffset, word.textLength);
    if (word.width <= 0 || selectedText.empty()) continue;
#if defined(CROSSPOINT_TTF_READER)
    if (ttfMode) {
      // Outline highlight — see drawHighlightWithSnapshot.
      renderer.drawRect(std::max(0, static_cast<int>(word.x) - 2), std::max(0, static_cast<int>(word.y) - 2),
                        word.width + 4, word.height + 4, true);
      continue;
    }
#endif
    renderer.fillRect(std::max(0, static_cast<int>(word.x) - 2), std::max(0, static_cast<int>(word.y) - 2),
                      word.width + 4, lineHeight + 4, true);
    renderer.drawText(fontId, word.x, word.y, selectedText.c_str(), false, word.style);
  }
}

// Front-button bar (Back/Confirm/Left/Right). Drawn last on every repaint
// path, including the differential highlight-only path, so it always ends
// up as the top layer even when a highlighted word's box falls under a
// hint's screen area. No side-button hints: the full-bleed reader page has no
// spare gutter for them, so a hint box there would hide text.
void DictionaryWordSelectActivity::drawHints() const {
  // No selectable word on this page: Confirm and navigation are all no-ops
  // (guarded by selections.empty() in loop()/performLookup), so only Back does
  // anything and only Back is hinted.
  if (selections.empty()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  const auto labels = mappedInput.mapDirectionalLabels(tr(STR_BACK), tr(STR_LOOKUP), tr(STR_DIR_LEFT),
                                                       tr(STR_DIR_RIGHT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  const unsigned long renderStart = millis();
  LOG_DBG("DICT", "Selector render start: words=%u groups=%u popup=%u", static_cast<unsigned>(words.size()),
          static_cast<unsigned>(selections.size()), static_cast<unsigned>(popup));
  // Differential fast path: only the highlight moved and the framebuffer
  // still holds a clean page (no popup or sub-activity since the last full
  // repaint). Restore the pixels under the old highlight, draw the new one,
  // and push — skipping the two-pass page render entirely.
  if (popup == Popup::None && snapshotIdx >= 0 && !selections.empty() && selected != snapshotIdx) {
    renderer.writeFramebufferRegion(snapshotX, snapshotY, snapshotW, snapshotH, snapshot.get());
#if !defined(CROSSPOINT_TTF_READER)
    // The full path's PrewarmScope cleared the glyph cache on exit; batch-load
    // just the highlighted word's glyphs before drawing them white-on-black.
    const auto& group = selections[selected];
    for (uint16_t i = 0; i < group.segmentCount; i++) {
      const WordBox& word = words[selectionSegments[group.segmentStart + i]];
      renderer.getFontCacheManager()->prewarmCache(
          fontId, word.text, static_cast<uint8_t>(1u << (static_cast<uint8_t>(word.style) & 0x03)));
    }
#else
    if (!ttfMode) {
      // The full path's PrewarmScope cleared the glyph cache on exit; batch-load
      // just the highlighted word's glyphs before drawing them white-on-black.
      const auto& group = selections[selected];
      for (uint16_t i = 0; i < group.segmentCount; i++) {
        const WordBox& word = words[selectionSegments[group.segmentStart + i]];
        renderer.getFontCacheManager()->prewarmCache(
            fontId, word.text, static_cast<uint8_t>(1u << (static_cast<uint8_t>(word.style) & 0x03)));
      }
    }
#endif
    if (drawHighlightWithSnapshot()) {
      drawHints();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
    // Snapshot failed (oversize box) — fall through to a full repaint.
  }

  renderer.clearScreen();
  LOG_DBG("DICT", "Selector render cleared screen");

#if defined(CROSSPOINT_TTF_READER)
  if (ttfMode) {
    // Engine glyphs are RAM-cached by the FontChain — one pass, no SD-font
    // prewarm scan.
    ttfRender.renderPage(ttfRender.ctx, renderer);
  } else
#endif
  {
    // Same prewarm-scan-then-render pass the reader uses, so SD-card fonts hit
    // the in-RAM glyph cache during the real draw.
    auto* fcm = renderer.getFontCacheManager();
    auto scope = fcm->createPrewarmScope();
    if (page) page->render(renderer, fontId, marginLeft, marginTop);
    LOG_DBG("DICT", "Selector render first page pass complete in %lums", millis() - renderStart);
    scope.endScanAndPrewarm();
    LOG_DBG("DICT", "Selector render font prewarm complete in %lums", millis() - renderStart);
    if (page) page->render(renderer, fontId, marginLeft, marginTop);
    LOG_DBG("DICT", "Selector render second page pass complete in %lums", millis() - renderStart);
  }

  if (!selections.empty()) drawHighlightRange();

  drawHints();

  if (popup != Popup::None) {
    // The popup overdraws the page, so the snapshot no longer matches the
    // framebuffer — force the next render onto the full-repaint path.
    snapshotIdx = -1;
    // drawPopup overlays the framebuffer and refreshes the display itself.
    // I18N.get directly: tr() only accepts literal key names.
    GUI.drawPopup(renderer, I18N.get(popupMsg));
    return;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  LOG_DBG("DICT", "Selector render complete in %lums", millis() - renderStart);
}
