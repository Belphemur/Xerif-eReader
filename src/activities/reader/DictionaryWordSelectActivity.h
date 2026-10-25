#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <vector>

#include "TouchLongPressMode.h"
#include "activities/Activity.h"
#include "activities/ActivityResult.h"
#include "util/Dictionary.h"
#if defined(CROSSPOINT_TTF_READER)
#include "TtfWordSelect.h"
#endif

// Word selection over the current reader page: Left/Right step through words
// in reading order, Up/Down jump rows, Confirm looks the word up and opens
// DictionaryDefinitionActivity, Back returns to the reader. On touch devices a
// touch-down moves the highlight and a tap on a word looks it up directly.
class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int marginLeft, int marginTop, int initialX = -1,
                                        int initialY = -1, TouchLongPressMode mode = TouchLongPressMode::Dictionary,
                                        std::string lookupText = {})
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop),
        initialX(initialX),
        initialY(initialY),
        mode(mode),
        lookupText(std::move(lookupText)) {}

#if defined(CROSSPOINT_TTF_READER)
  // TTF path: word boxes come prebuilt from engine run geometry
  // (TtfWordSelect), and the page is re-rasterized through the reader's
  // render hook — engine run text lives in the reader's scratch arena and
  // cannot outlive the openDictionaryWordSelect call.
  struct PageRenderFn {
    void* ctx;
    void (*renderPage)(void*, GfxRenderer&);
  };
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        freeink::book::TtfWordSelectData&& ttfData, PageRenderFn renderPage,
                                        int initialX = -1, int initialY = -1,
                                        TouchLongPressMode mode = TouchLongPressMode::Dictionary,
                                        std::string lookupText = {})
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        marginLeft(0),
        marginTop(0),
        initialX(initialX),
        initialY(initialY),
        mode(mode),
        ttfMode(true),
        ttfData(std::move(ttfData)),
        ttfRender(renderPage),
        lookupText(std::move(lookupText)) {}
#endif

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Screen box of one selectable word. `text` points into the owned Page's
  // TextBlock arena (NUL-terminated), valid for this activity's lifetime.
  struct WordBox {
    int16_t x = 0;
    int16_t y = 0;
    int16_t width = 0;
    uint16_t row = 0;
    const char* text = nullptr;
    uint16_t textOffset = 0;
    uint16_t textLength = 0;
    // The token's full raw span in `text` (pre-trim): footnote resolution must
    // see the parentheses the trim strips, since the parser stores footnote
    // numbers with them ([1] and (1) must not compare equal). rawLength starts
    // at offset 0 of `text`.
    uint16_t rawLength = 0;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    uint32_t selectionGroup = 0;
    bool syntheticHyphen = false;
    // Line-box height of this word's row: the legacy font's line height, or
    // the engine font's per-run height on the TTF path (headings differ).
    int16_t height = 0;
  };

  // A logical word may contain several rendered boxes when pagination split it
  // across lines. The segment indexes are kept in one flat vector to avoid a
  // heap allocation per word on the X4 Pro.
  struct WordSelection {
    uint32_t group;
    uint16_t segmentStart;
    uint16_t segmentCount;
  };

  enum class Popup : uint8_t { None, Busy, NotFound, Error };

  void extractWords();
#if defined(CROSSPOINT_TTF_READER)
  void extractWordsTtf();
#endif
  int closestInRow(uint16_t row, int centerX, int excludeSelection = -1) const;
  int wordAt(int x, int y) const;
  std::string selectionText(int selectionIndex) const;
  // The selection's raw token text (pre-trim): for a multi-segment word the
  // segments are joined directly, so punctuation between segments survives.
  // Footnote resolution needs the parentheses the trimmed text loses.
  std::string selectionRawText(int selectionIndex) const;
  std::string resolveFootnoteHref(const char* word) const;
  // Footnote attempt shared by both lookup entry points. Returns true when the
  // selection was consumed (footnote resolved or bare numeric marker) and the
  // activity finished — the caller must then not run a dictionary lookup.
  bool resolveFootnoteOrFinish(const char* raw, const char* trimmed);
  void moveVertical(int direction);
  void performLookup();
  void performLookup(const std::string& query);
  void performLookup(const std::string& raw, const std::string& trimmed);
  void drawHighlightRange() const;
  bool drawHighlightWithSnapshot();
  void drawHints() const;

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  const int initialX;
  const int initialY;
  const TouchLongPressMode mode;
  // Direct-lookup mode (upstream #3589): when set on construction the activity
  // skips the selection UI, resolves this text against the dictionary, and
  // finishes back to the reader.
  std::string lookupText;
  bool lookupPending = false;
  int fontId = 0;
  int lineHeight = 0;

#if defined(CROSSPOINT_TTF_READER)
  bool ttfMode = false;                      // boxes prebuilt from engine runs
  freeink::book::TtfWordSelectData ttfData;  // owns the token text
  PageRenderFn ttfRender{};                  // reader's page repaint hook
#endif

  std::vector<WordBox> words;
  std::vector<WordSelection> selections;
  std::vector<uint16_t> selectionSegments;
  int selected = 0;
  uint16_t rowCount = 0;

  Dictionary dict;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;
  bool dictNeedsIndex = false;

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;

  // Differential highlight repaint: the pixels under the current highlight
  // box, so a selection move restores them and repaints only the two affected
  // boxes instead of re-running the full two-pass page render (which also
  // reloads every SD-font glyph on the page). snapshotIdx is the word whose
  // under-pixels are saved; -1 means the framebuffer no longer holds a clean
  // page (popup drawn, sub-activity shown) and the next render must be full.
  static constexpr size_t SNAPSHOT_CAPACITY = 4096;
  std::unique_ptr<uint8_t[]> snapshot;
  int16_t snapshotX = 0;
  int16_t snapshotY = 0;
  int16_t snapshotW = 0;
  int16_t snapshotH = 0;
  int snapshotIdx = -1;
};
