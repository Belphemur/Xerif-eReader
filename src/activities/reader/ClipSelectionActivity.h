#pragma once

#include <Epub/Page.h>

#if defined(CROSSPOINT_TTF_READER)
#include "TtfWordSelect.h"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "components/themes/BaseTheme.h"
#include "util/WordSelectionInput.h"

class ClipSelectionActivity final : public Activity {
 public:
  enum class Mode : uint8_t { Clip, Lookup, DefinitionLookup };

  struct Selection {
    uint16_t firstPage = 0;
    uint16_t lastPage = 0;
    uint16_t firstWord = 0;
    uint16_t lastWord = 0;
    bool valid = false;
  };

  ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        std::vector<std::unique_ptr<Page>> pages, int marginLeft, int marginTop, int initialX = -1,
                        int initialY = -1, Mode mode = Mode::Clip);

  // The definition viewer retains these pages until this child exits.
  ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        std::span<const std::unique_ptr<Page>> borrowedPages, int marginLeft, int marginTop,
                        Selection selection, int initialX = -1, int initialY = -1);

#if defined(CROSSPOINT_TTF_READER)
  // TTF path (design doc ttf/2026-11-09 §F2): per-chapter-page word payloads
  // built by the reader from the engine (buildTtfWordSelectData, one entry
  // per selection-window page, in window order), with the reader repainting
  // the shown page through the render hook (offset = that entry's index).
  // Token text arenas are owned here until the activity exits.
  struct PageRenderFn {
    void* ctx;
    void (*renderPage)(void*, GfxRenderer&, uint16_t pageOffset);
  };
  ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        std::vector<freeink::book::TtfWordSelectData> ttfPages, PageRenderFn renderPage,
                        int initialX = -1, int initialY = -1, Mode mode = Mode::Clip);
#endif

  // Restore a previous selection (page offsets relative to the first page) on enter.
  void setInitialSelection(const Selection& selection) { initialSelection = selection; }

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool handleHomeGesture() override;

 private:
  struct WordBox {
    int16_t x = 0;
    int16_t y = 0;
    int16_t width = 0;
    int16_t height = 0;
    uint16_t row = 0;
    uint8_t pageOffset = 0;
    uint16_t pageWordIndex = 0;
    uint32_t startOffset = UINT32_MAX;
    uint32_t endOffset = UINT32_MAX;
    const char* text = nullptr;
    // Trimmed token span inside `text` (0..full-length unless the TTF payload
    // provides a trim): buildSelectedText exports the trimmed range so box
    // geometry and lookup text agree.
    uint16_t textOffset = 0;
    uint16_t textLength = 0;
    bool paragraphStart = false;
    bool isRtl = false;
    bool discretionaryHyphen = false;
  };

  static constexpr size_t MAX_SELECTABLE_WORDS = 240;

  bool extractWords();
#if defined(CROSSPOINT_TTF_READER)
  // Maps the per-page engine payloads into the flat WordBox array
  // (design doc ttf/2026-11-09 §F2). Shared downstream logic sees no
  // difference between the two data sources.
  void extractWordsTtf();
#endif
  size_t pageCount() const {
#if defined(CROSSPOINT_TTF_READER)
    if (ttfMode) return ttfPages.size();  // swipe page navigation over the window
#endif
    return borrowedPages.empty() ? pages.size() : borrowedPages.size();
  }
  const Page& pageAt(size_t index) const { return *(borrowedPages.empty() ? pages[index] : borrowedPages[index]); }
  bool isLookup() const { return mode != Mode::Clip; }
  int firstAction() const { return lookupAvailable ? 0 : 1; }
  int actionCount() const { return mode == Mode::DefinitionLookup ? 1 : 3 - firstAction(); }
  int actionAt(int x, int y) const;
  int closestInRow(uint16_t row, int centerX) const;
  int wordAt(int x, int y) const;
  int dragWordAt(int x, int y) const;
  bool selectionContains(int x, int y) const;
  int nextPageStartIndexForTouchDrag() const;
  bool isWithinCurrentPageEndDwellSlop(int x, int y) const;
  void moveVertical(int direction);
  void selectIndex(int index);
  void moveToPage(int pageOffset);
  void confirmSelection(ClippingResult::Action action = ClippingResult::Action::Clip);
  void loopButtons();
  bool handleButtons(uint8_t buttons);
  Rect handleRect(int index, bool start) const;
  Rect actionRect() const;
  int selectionTop() const;
  int textOffset() const;
  int textXOffset() const;
  void cancel();
  bool buildSelectedText(int first, int last, std::string& text) const;
  void paintPage(GfxRenderer& renderer, int offsetX, int offset) const;
  void drawSelection() const;
  bool renderIncremental();
  void ditherGapBetween(const WordBox& a, const WordBox& b, int offsetX, int offset) const;

  std::vector<std::unique_ptr<Page>> pages;
  std::span<const std::unique_ptr<Page>> borrowedPages;
#if defined(CROSSPOINT_TTF_READER)
  bool ttfMode = false;
  std::vector<freeink::book::TtfWordSelectData> ttfPages;  // owns the token text
  PageRenderFn ttfRender{};
#endif
  Selection initialSelection;
  const int marginLeft;
  const int marginTop;
  const int initialX;
  const int initialY;
  const Mode mode;
  bool lookupAvailable = false;
  OptionPopup<> actionPopup;
  // ponytail: retain 16 button frames during refresh; increase only if overflow is observed.
  std::array<uint8_t, 16> pendingButtons{};
  size_t pendingButtonCount = 0;
  WordSelectionInput selectionInput;
  int fontId = 0;
  int lineHeight = 0;
  std::unique_ptr<WordBox[]> words;
  size_t wordCount = 0;
  int selected = 0;
  int rangeStart = -1;
  uint8_t currentPageOffset = 0;
  uint16_t rowCount = 0;
  bool touchDragSelecting = false;
  bool ignoreInitialTouch = false;
  int dragOffsetX = 0;
  int dragOffsetY = 0;
  bool touchDragHasMoved = false;
  int touchDragStartX = 0;
  int touchDragStartY = 0;
  int touchDragPageEndIndex = -1;
  unsigned long touchDragPageEndHeldSince = 0;
  int lastRenderedSelected = -1;
  int lastRenderedRangeStart = -1;
  int lastRenderedPageOffset = -1;
  int lastRenderedTextOffset = 0;
  int lastRenderedTextXOffset = 0;
};
