// TtfWordSelect — see TtfWordSelect.h. Gated to CROSSPOINT_TTF_READER: engine
// pages only exist on TTF builds (§14.1 compile-time split).

#if defined(CROSSPOINT_TTF_READER)

#include "TtfWordSelect.h"

extern "C" {
#include <minibidi.h>  // bidi_class — strong-class scan over a bounded run prefix
}

#include <BidiUtils.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "util/DictionarySelection.h"

namespace freeink {
namespace book {
namespace {

// Byte-level whitespace test — UTF-8 continuation and lead bytes never
// collide with ASCII whitespace, same argument as the layout's isWsByte.
inline bool isWsByte(const char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }

// Decodes the UTF-8 codepoint at text[i..to), advancing i past its bytes.
inline uint32_t decodeCp(const char* text, const uint16_t to, uint16_t& i) {
  if (i >= to) return 0;  // defensive: callers loop on i < to
  const auto b0 = static_cast<uint8_t>(text[i]);
  uint32_t cp = b0;
  uint32_t extra = 0;
  if (b0 >= 0xF0) {
    cp = b0 & 0x07;
    extra = 3;
  } else if (b0 >= 0xE0) {
    cp = b0 & 0x0F;
    extra = 2;
  } else if (b0 >= 0xC0) {
    cp = b0 & 0x1F;
    extra = 1;
  }
  ++i;
  while (extra > 0 && i < to && (static_cast<uint8_t>(text[i]) & 0xC0) == 0x80) {
    cp = (cp << 6) | (static_cast<uint8_t>(text[i]) & 0x3F);
    ++i;
    --extra;
  }
  return cp;
}

// Chapter-codepoint weight of one re-encoded run codepoint. The layout folds
// ligature chains (ff→FB00 = 2 cps, FB00+i/l → FB03/FB04 = 3 cps) and lam-alef
// pairs (FEF5..FEFC = 2 cps) into a single baked codepoint before storing run
// text (ChapterLayout decodeShaped re-encode), so a baked cp covers more
// chapter codepoints than its own byte decode reveals. A value the active
// font could have baked is counted as baked; a raw source occurrence of the
// same codepoint — vanishingly rare EPUB text — drifts one codepoint,
// bounded to the run (design doc §F3: log-once rule). Soft hyphens drop in
// the same re-encode and are invisible here; their absence also drifts the
// anchor within the affected run only.
inline uint32_t chapterCpWeight(FontChain& fonts, const uint32_t cp, const uint8_t styleFlags) {
  if (cp >= 0xFEF5 && cp <= 0xFEFC) return fonts.covers(cp) ? 2 : 1;
  switch (cp) {
    case 0xFB00:
      return fonts.ligature('f', 'f', styleFlags) == cp ? 2 : 1;
    case 0xFB01:
      return fonts.ligature('f', 'i', styleFlags) == cp ? 2 : 1;
    case 0xFB02:
      return fonts.ligature('f', 'l', styleFlags) == cp ? 2 : 1;
    case 0xFB03:
      return fonts.ligature(0xFB00, 'i', styleFlags) == cp ? 3 : 1;
    case 0xFB04:
      return fonts.ligature(0xFB00, 'l', styleFlags) == cp ? 3 : 1;
    default:
      return 1;
  }
}

// True when any codepoint in the run has a strong RTL class: engine segments
// are level-homogeneous, so a run holding one strong RTL character was
// re-encoded in visual order and NO token in the run carries a logical
// chapter anchor. Token-level startsWithRtl cannot see this: a digits-only
// token inside a reversed run classifies as non-RTL while its bytes are
// still reversed. The scan is linear in the run length (runs are one visual
// line; bidi_class is a bounded table lookup) — a bounded prefix would miss
// reversed runs whose strong characters start past it (long neutral/digit
// visual prefixes).
bool runStoresVisualOrder(const char* text, const uint16_t to) {
  uint16_t i = 0;
  while (i < to) {
    const uint32_t cp = decodeCp(text, to, i);
    const uchar cls = bidi_class(cp);
    if (cls == R || cls == AL) return true;
  }
  return false;  // no strong RTL class anywhere in the run: expect LTR order
}

}  // namespace

bool buildTtfWordSelectData(const Page& page, FontChain& fonts, TtfWordSelectData& out,
                            const int16_t characterSpacingPx, const int16_t wordSpacingPx) {
  out.boxes.clear();
  out.footnotes.clear();

  // Pass 1: token boundary scan (no storage) so the pooled token table and
  // text arena can be allocated once, sized exactly.
  struct TokenRange {
    uint16_t run;
    uint16_t start;
    uint16_t end;
  };
  size_t tokenCount = 0;
  size_t arenaBytes = 0;
  for (uint16_t r = 0; r < page.runCount; ++r) {
    const PageTextRun& run = page.runs[r];
    uint16_t i = 0;
    while (i < run.len) {
      while (i < run.len && isWsByte(run.text[i])) ++i;
      if (i >= run.len) break;
      const uint16_t start = i;
      while (i < run.len && !isWsByte(run.text[i])) ++i;
      ++tokenCount;
      arenaBytes += static_cast<size_t>(i - start) + 1;  // NUL-terminated
    }
  }
  if (tokenCount == 0) return true;

  // Token boundaries and word text are page-scale derived data: PSRAM-backed
  // pool on PSRAM builds, plain heap on DRAM-only/host builds.
  auto tokenBytes = poolMakeBytes(tokenCount * sizeof(TokenRange));
  if (!tokenBytes) {
    LOG_ERR("TTFWS", "OOM: token table %u bytes", static_cast<unsigned>(tokenCount * sizeof(TokenRange)));
    return false;
  }
  auto arena = poolMakeBytes(arenaBytes);
  if (!arena) {
    LOG_ERR("TTFWS", "OOM: token arena %u bytes", static_cast<unsigned>(arenaBytes));
    return false;
  }
  auto* tokens = reinterpret_cast<TokenRange*>(tokenBytes.get());

  // Pass 2: fill the token table (same boundary walk as pass 1).
  {
    size_t t = 0;
    for (uint16_t r = 0; r < page.runCount; ++r) {
      const PageTextRun& run = page.runs[r];
      uint16_t i = 0;
      while (i < run.len) {
        while (i < run.len && isWsByte(run.text[i])) ++i;
        if (i >= run.len) break;
        const uint16_t start = i;
        while (i < run.len && !isWsByte(run.text[i])) ++i;
        tokens[t++] = TokenRange{r, start, i};
      }
    }
  }

  // Pass 3: copy text, measure, and group. A logical word's group id is the
  // index of its first fragment — a fragment continuing a previous run's
  // word reuses the carried group, which is exactly the pairing
  // DictionarySelection::groupTokens joins on.
  out.boxes.reserve(tokenCount);
  char* write = reinterpret_cast<char*>(arena.get());

  // Width of a byte range inside a run: advance + kerning + the reader's
  // spacing knobs replayed exactly like the paint walk (PagePaint's spacingFor:
  // word spacing rides the space glyph, character spacing everything else), so
  // token geometry matches painted glyph positions. Justify slack lives in
  // run.x and needs no replay here.
  auto measure = [&](const char* text, uint16_t from, uint16_t to, uint16_t sizePx, uint8_t styleFlags) {
    int32_t w = 0;
    uint32_t prev = 0;
    for (uint16_t i = from; i < to;) {
      const uint32_t cp = decodeCp(text, to, i);
      if (prev != 0) w += fonts.kerning(prev, cp, sizePx, styleFlags);
      w += fonts.advance(cp, sizePx, styleFlags);
      w += cp == ' ' ? wordSpacingPx : characterSpacingPx;
      prev = cp;
    }
    return w;
  };

  bool carry = false;  // previous run's last token ends mid-word
  uint32_t carriedGroup = 0;
  // Rolling pen position: each inter-token gap is measured once. Measuring
  // from the run start for every token would be O(runLen·tokens) font
  // metric work (kody, PR #113).
  int32_t penX = 0;
  uint16_t measuredTo = 0;
  uint16_t anchorFrom = 0;
  uint32_t anchorCps = 0;
  bool anchorSafe = true;
  bool rtlRun = false;
  for (size_t t = 0; t < tokenCount; ++t) {
    const TokenRange& token = tokens[t];
    const PageTextRun& run = page.runs[token.run];
    const char* text = run.text + token.start;
    const uint16_t rawLen = token.end - token.start;
    const std::string_view rawView(text, rawLen);

    const bool runFirst = t == 0 || tokens[t - 1].run != token.run;
    const bool runLast = t + 1 == tokenCount || tokens[t + 1].run != token.run;
    const bool headFlag = runFirst && (run.layoutFlags & PageTextRun::LayoutFirstContinues) != 0;
    const bool tailFlag = runLast && token.end == run.len && (run.layoutFlags & PageTextRun::LayoutLastContinues) != 0;

    // Advance the rolling pen to this token's start (tokens are ordered by
    // run, then start; a run switch resets the pen). Skipped tokens keep the
    // pen correct for the next one — the next gap measure starts here.
    if (runFirst) {
      penX = run.x;
      measuredTo = 0;
      anchorFrom = 0;
      anchorCps = 0;
      // Re-encode drops soft hyphens, which the consumer cannot see. A run
      // whose weighted decode count disagrees with charLen has dropped
      // characters — mark its anchors unavailable rather than persist
      // guessed ranges (a mismatched clip can highlight the wrong token
      // after relayout); the drift stays bounded to the run either way.
      anchorSafe = true;
      uint16_t effLen = run.len;
      if ((run.layoutFlags & PageTextRun::LayoutHyphenated) != 0 && effLen > 0 && run.text[effLen - 1] == '-') {
        --effLen;  // baked synthetic '-' is presentation-only, outside charLen
      }
      uint32_t decoded = 0;
      for (uint16_t i = 0; i < effLen;) {
        decoded += chapterCpWeight(fonts, decodeCp(run.text, effLen, i), run.styleFlags);
      }
      anchorSafe = decoded == run.charLen;
      if (!anchorSafe) {
        LOG_DBG("TTFWS", "run charCount mismatch (decoded %u vs %u): anchors disabled for this run",
                static_cast<unsigned>(decoded), static_cast<unsigned>(run.charLen));
      }
      rtlRun = runStoresVisualOrder(run.text, effLen);
    }
    penX += measure(run.text, measuredTo, token.start, run.sizePx, run.styleFlags);
    measuredTo = token.start;
    // Same monotone walk for the chapter-anchor cursor: codepoints (weighted)
    // up to this token's start, so a skipped token's bytes are counted once.
    if (anchorFrom < token.start) {
      for (uint16_t i = anchorFrom; i < token.start;) {
        anchorCps += chapterCpWeight(fonts, decodeCp(run.text, token.start, i), run.styleFlags);
      }
      anchorFrom = token.start;
    }

    // Selection group: a continuation head joins the carried group; any
    // other token starts a fresh logical word. A pairing mismatch (head
    // without carry, carry without head — neither should happen) leaves the
    // fragment standing alone.
    uint32_t group = static_cast<uint32_t>(t);
    if (carry && headFlag) {
      group = carriedGroup;
    }
    carry = tailFlag;
    carriedGroup = group;

    // Trim after the group decision: the box stores the raw token; the
    // trimmed range is what gets looked up and measured.
    const DictionarySelection::TokenSpan span = DictionarySelection::trimTokenEdges(rawView);
    if (span.length == 0 || !DictionarySelection::isSelectableToken(rawView)) continue;

    TtfWordBox box{};
    box.text = write;
    memcpy(write, rawView.data(), rawView.size());
    write[rawView.size()] = '\0';
    write += rawView.size() + 1;
    // Chapter anchor: decode-walk the run's re-encoded text from its charStart
    // (weights fold baked ligatures; soft hyphens stay drift-bounded), gated
    // to LTR tokens. RTL runs are stored visual order — no anchor, their
    // clips degrade to text-only per the design doc. Leading punctuation the
    // trim strips is real chapter text and counts toward the anchor.
    box.charStart = UINT32_MAX;
    box.charLen = 0;
    if (anchorSafe && !rtlRun) {
      const uint16_t spanFrom = static_cast<uint16_t>(token.start + span.start);
      const uint16_t spanTo = static_cast<uint16_t>(spanFrom + span.length);
      for (uint16_t i = token.start; i < spanFrom;) {
        anchorCps += chapterCpWeight(fonts, decodeCp(run.text, spanFrom, i), run.styleFlags);
      }
      anchorFrom = spanFrom;
      uint32_t len = 0;
      for (uint16_t i = spanFrom; i < spanTo;) {
        len += chapterCpWeight(fonts, decodeCp(run.text, spanTo, i), run.styleFlags);
      }
      // The encode trim strips trailing punctuation (including a baked
      // trailing synthetic '-'), so the span never covers it; a mid-token
      // hyphen is a real chapter codepoint and stays counted.
      if (len > 0) {
        box.charStart = run.charStart + anchorCps;
        box.charLen = static_cast<uint16_t>(len);
      }
    }
    // The box covers the trimmed token only: skip leading punctuation the
    // trim stripped, so touch/highlight geometry matches the lookup text.
    box.x = static_cast<int16_t>(penX + measure(text, 0, span.start, run.sizePx, run.styleFlags));
    box.y = static_cast<int16_t>(run.baselineY - fonts.ascent(run.sizePx));
    box.height = fonts.lineHeight(run.sizePx);
    box.width = static_cast<int16_t>(measure(text, span.start, span.start + span.length, run.sizePx, run.styleFlags));
    box.textOffset = static_cast<uint16_t>(span.start);
    box.textLength = static_cast<uint16_t>(span.length);
    box.rawLength = rawLen;
    box.styleFlags = run.styleFlags;
    box.selectionGroup = group;
    // A synthetic hyphen is the baked '-' that terminates a hyphenated run:
    // presentation-only, stripped from the dictionary query.
    box.syntheticHyphen = (run.layoutFlags & PageTextRun::LayoutHyphenated) != 0 && runLast && token.end == run.len &&
                          rawLen > 0 && text[rawLen - 1] == '-';
    out.boxes.push_back(box);
  }

  out.arena = std::move(arena);
  return true;
}

}  // namespace book
}  // namespace freeink

#endif  // CROSSPOINT_TTF_READER
