#include "QuickPageCapture.h"

#include <cstring>

uint32_t QuickPageCapture::pageEndChar(const freeink::book::Page& page) {
  // Same metric as the engine's window delivery (ChapterLayout emitPage):
  // the furthest run end, floored at the page's char start.
  uint32_t end = page.charStart;
  for (uint16_t i = 0; i < page.runCount; ++i) {
    const uint32_t runEnd = page.runs[i].charStart + page.runs[i].charLen;
    if (runEnd > end) end = runEnd;
  }
  return end;
}

namespace {
// Byte length of the first `chars` codepoints of text[0..len). Tolerates
// malformed UTF-8 the same way the engine's countChars() counts it: every
// non-continuation byte opens a codepoint, continuation bytes extend it.
uint32_t byteLenOfChars(const char* text, const uint32_t len, const uint32_t chars) {
  uint32_t bytes = 0;
  for (uint32_t seen = 0; seen < chars && bytes < len; ++seen) {
    do {
      ++bytes;
    } while (bytes < len && (static_cast<uint8_t>(text[bytes]) & 0xC0) == 0x80);
  }
  return bytes;
}
}  // namespace

bool QuickPageCapture::capture(const freeink::book::Page& pageData, const uint32_t endChar) {
  valid_ = false;
  stringUsed_ = 0;
  if (buffer_ == nullptr) return false;
  if (pageData.runCount > kMaxRuns || pageData.linkCount > kMaxLinks || pageData.imageCount > kMaxImages ||
      pageData.ruleCount > kMaxRules || pageData.rubyCount > kMaxRubies) {
    return false;
  }

  // Validate the string budget up front so a failed capture cannot leave a
  // partially overwritten bump region behind. Null strings contribute
  // nothing and stay null through copyString; the 64-bit accumulator keeps
  // the bound check overflow-free. Runs beyond endChar contribute only
  // their kept prefix.
  uint64_t stringNeed = 0;
  for (uint16_t i = 0; i < pageData.runCount; ++i) {
    if (pageData.runs[i].text == nullptr) continue;
    if (endChar > 0 && pageData.runs[i].charStart >= endChar) continue;
    uint32_t len = pageData.runs[i].len;
    if (endChar > 0 && pageData.runs[i].charStart + pageData.runs[i].charLen > endChar) {
      const uint32_t cutChars = endChar - pageData.runs[i].charStart;
      len = byteLenOfChars(pageData.runs[i].text, len, cutChars);
    }
    stringNeed += len;
  }
  for (uint16_t i = 0; i < pageData.linkCount; ++i) {
    if (pageData.links[i].target != nullptr) stringNeed += strlen(pageData.links[i].target) + 1;
    if (pageData.links[i].fragment != nullptr) stringNeed += strlen(pageData.links[i].fragment) + 1;
  }
  for (uint16_t i = 0; i < pageData.imageCount; ++i) {
    if (pageData.images[i].href != nullptr) stringNeed += strlen(pageData.images[i].href) + 1;
  }
  for (uint16_t i = 0; i < pageData.rubyCount; ++i) {
    if (pageData.rubies[i].text != nullptr) stringNeed += strlen(pageData.rubies[i].text) + 1;
  }
  if (cap_ < kStringsOff || stringNeed > cap_ - kStringsOff) return false;

  auto* runs = reinterpret_cast<freeink::book::PageTextRun*>(buffer_ + kRunsOff);
  auto* links = reinterpret_cast<freeink::book::PageLink*>(buffer_ + kLinksOff);
  auto* images = reinterpret_cast<freeink::book::PageImage*>(buffer_ + kImagesOff);
  auto* rules = reinterpret_cast<freeink::book::PageRule*>(buffer_ + kRulesOff);
  auto* rubies = reinterpret_cast<freeink::book::PageRuby*>(buffer_ + kRubiesOff);

  uint16_t runOut = 0;
  for (uint16_t i = 0; i < pageData.runCount; ++i) {
    freeink::book::PageTextRun run = pageData.runs[i];
    if (endChar > 0) {
      if (run.charStart >= endChar) continue;  // beyond the entry span
      const uint32_t runEnd = run.charStart + run.charLen;
      if (runEnd > endChar) {
        // Straddles the span end: keep only the leading codepoints.
        const uint32_t cutChars = endChar - run.charStart;
        run.len = byteLenOfChars(run.text, run.len, cutChars);
        run.charLen = cutChars;
        // The bit describes the run's LAST byte; a truncation that removes
        // the synthetic hyphen makes it a lie.
        if ((run.layoutFlags & freeink::book::PageTextRun::LayoutHyphenated) != 0 &&
            (run.len == 0 || run.text[run.len - 1] != '-')) {
          run.layoutFlags &= ~freeink::book::PageTextRun::LayoutHyphenated;
        }
      }
    }
    runs[runOut] = run;
    if (run.text != nullptr) {  // stays null, like copyString
      char* dst = reinterpret_cast<char*>(buffer_ + kStringsOff + stringUsed_);
      memcpy(dst, run.text, run.len);
      stringUsed_ += run.len;
      runs[runOut].text = dst;
    }
    ++runOut;
  }
  for (uint16_t i = 0; i < pageData.linkCount; ++i) {
    links[i] = pageData.links[i];
    links[i].target = copyString(pageData.links[i].target);
    links[i].fragment = pageData.links[i].fragment != nullptr ? copyString(pageData.links[i].fragment) : nullptr;
  }
  for (uint16_t i = 0; i < pageData.imageCount; ++i) {
    images[i] = pageData.images[i];
    images[i].href = copyString(pageData.images[i].href);
  }
  // Rules carry no strings — geometry only.
  for (uint16_t i = 0; i < pageData.ruleCount; ++i) rules[i] = pageData.rules[i];
  for (uint16_t i = 0; i < pageData.rubyCount; ++i) {
    rubies[i] = pageData.rubies[i];
    rubies[i].text = copyString(pageData.rubies[i].text);
  }

  page_ = freeink::book::Page{runs,
                              runOut,
                              images,
                              pageData.imageCount,
                              links,
                              pageData.linkCount,
                              rules,
                              pageData.ruleCount,
                              rubies,
                              pageData.rubyCount,
                              pageData.pageIndex,
                              pageData.wordCount,
                              pageData.charStart};
  valid_ = true;
  return true;
}

const char* QuickPageCapture::copyString(const char* src) {
  if (src == nullptr) return nullptr;
  const size_t len = strlen(src) + 1;
  char* dst = reinterpret_cast<char*>(buffer_ + kStringsOff + stringUsed_);
  memcpy(dst, src, len);
  stringUsed_ += len;
  return dst;
}
