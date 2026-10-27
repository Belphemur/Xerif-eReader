#include "FontPreviewActivity.h"

#include <BookFontLoader.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "QuickPageRelayout.h"
#include "ReaderLineSpacing.h"
#include "TtfBookRuntime.h"
#include "TtfUiFallback.h"
#include "activities/ActivityResult.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"

#if defined(CROSSPOINT_TTF_READER)

FontPreviewActivity::FontPreviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         freeink::book::TtfBookRuntime* ttf, const uint16_t spineIndex,
                                         const uint32_t anchorChar, const uint32_t pageEndChar, const bool autoPageTurn,
                                         Host host)
    : Activity("FontPreview", renderer, mappedInput),
      ttf_(ttf),
      spineIndex_(spineIndex),
      anchorChar_(anchorChar),
      pageEndChar_(pageEndChar),
      autoPageTurn_(autoPageTurn),
      host_(host) {}

void FontPreviewActivity::onEnter() {
  Activity::onEnter();
  snprintf(entryFamily_, sizeof(entryFamily_), "%s", SETTINGS.ttfFontFamilyName);
  entrySize_ = SETTINGS.ttfFontPointSize;
  entryLineSpacing_ = SETTINGS.lineSpacing;
  entryWordSpacing_ = SETTINGS.wordSpacing;
  entryCharacterSpacing_ = SETTINGS.characterSpacing;
  chrome_ = makeUniqueNoThrow<ReaderToolbarUi>(renderer);
  if (chrome_) {
    chrome_->begin();
  } else {
    LOG_ERR("FPR", "OOM: preview chrome");
  }
  needsRelayout_ = true;
  // Pushed activities must requestUpdate() here or the screen sits blank for
  // up to a minute (the previous screen's pending notify was consumed).
  requestUpdate();
}

void FontPreviewActivity::onExit() {
  familyPopup_.dismiss();
  popup_.dismiss();
  chrome_.reset();
  preview_.attach(nullptr, 0);  // buffer freed below
  previewBuf_.reset();
  relayoutFont_ = nullptr;
  Activity::onExit();
}

void FontPreviewActivity::relayout() {
  needsRelayout_ = false;
  if (ttf_ == nullptr) return;
  const auto out =
      quickRelayoutPage(*ttf_, renderer, spineIndex_, anchorChar_, pageEndChar_, autoPageTurn_, preview_, previewBuf_);
  relayoutFont_ = out.font;
  relayoutCharSpacingPx_ = out.characterSpacingPx;
  relayoutWordSpacingPx_ = out.wordSpacingPx;
  // An unconfirmed capture (layout failure, anchor past the chapter's last
  // char) must not be displayed as if it were the current page: drop it and
  // let the render keep the frame the preview opened over until close reflows.
  if (!out.reachedAnchor) preview_.reset();
}

void FontPreviewActivity::applySize(const uint32_t value) {
  const uint8_t clamped = static_cast<uint8_t>(std::clamp<uint32_t>(value, CrossPointSettings::TTF_FONT_POINT_SIZE_MIN,
                                                                    CrossPointSettings::TTF_FONT_POINT_SIZE_MAX));
  if (clamped == SETTINGS.ttfFontPointSize) return;
  SETTINGS.ttfFontPointSize = clamped;
  // Persist outside RenderLock, mirroring Text settings' Size tab.
  if (!SETTINGS.saveToFile()) {
    LOG_ERR("FPR", "font preview: settings save failed");
  }
  needsRelayout_ = true;
  requestUpdate();
}

void FontPreviewActivity::openSizeSlider() {
  // Same dialog Text settings' Size tab uses — one sizing mechanism
  // everywhere (design decision 2026-09-27).
  auto sizeDialog = makeUniqueNoThrow<IntervalSelectionActivity>(
      renderer, mappedInput, "TtfPointSize", StrId::STR_FONT_SIZE, SETTINGS.ttfFontPointSize,
      CrossPointSettings::TTF_FONT_POINT_SIZE_MIN, CrossPointSettings::TTF_FONT_POINT_SIZE_MAX, 1, 2,
      StrId::STR_FONT_SIZE_VALUE);
  if (!sizeDialog) {
    LOG_ERR("FPR", "OOM: size slider");
    return;
  }
  startActivityForResult(std::move(sizeDialog), [this](const ActivityResult& result) {
    if (result.isCancelled || !std::holds_alternative<IntervalResult>(result.data)) return;
    applySize(std::get<IntervalResult>(result.data).value);
  });
}

void FontPreviewActivity::openFamilyPicker() {
  // Same enum-picker pattern as the reader's Text panel: one modal over the
  // page. Built-in is index 0; scanned families follow in loader order.
  const uint8_t familyCount = freeink::book::fontLoader.familyCount();
  std::vector<std::string> options;
  options.reserve(1U + familyCount);
  options.emplace_back(tr(STR_BUILTIN_FONT));
  for (uint8_t i = 0; i < familyCount; ++i) {
    options.emplace_back(freeink::book::fontLoader.families()[i].name);
  }

  int currentIndex = 0;
  if (SETTINGS.ttfFontFamilyName[0] != '\0') {
    const auto* current = freeink::book::fontLoader.findFamily(SETTINGS.ttfFontFamilyName);
    if (current != nullptr) currentIndex = 1 + (current - freeink::book::fontLoader.families());
  }

  familyPopup_.show(StrId::STR_FONT_FAMILY, options, currentIndex, [this](const int idx) {
    if (idx <= 0) {
      SETTINGS.ttfFontFamilyName[0] = '\0';
    } else if (idx <= freeink::book::fontLoader.familyCount()) {
      const auto& family = freeink::book::fontLoader.families()[idx - 1];
      if (!freeink::book::fontLoader.isFamilyAvailable(family)) return;
      strncpy(SETTINGS.ttfFontFamilyName, family.name, sizeof(SETTINGS.ttfFontFamilyName) - 1);
      SETTINGS.ttfFontFamilyName[sizeof(SETTINGS.ttfFontFamilyName) - 1] = '\0';
    } else {
      return;
    }
    // The reload on the next makeLayoutParams() frees the loader's resident
    // bytes; the reader pauses machinery whose faces borrow them first.
    if (host_.onFamilyChanging != nullptr) host_.onFamilyChanging(host_.ctx);
    SETTINGS.readerFontEngine = CrossPointSettings::READER_ENGINE_TTF;
    if (!SETTINGS.saveToFile()) {
      LOG_ERR("FPR", "font preview: settings save failed");
    }
    needsRelayout_ = true;
  });
  requestUpdate();
}

// Spacing rows. Each re-lays the captured page live through the same
// makeLayoutParams() the committed build reads, so the preview reflects the
// setting exactly as the book will after the close reindex.
void FontPreviewActivity::openLineSpacingPicker() {
  // LINE_SPACING_IDS follows the persisted LINE_COMPRESSION order (ReaderLineSpacing.h) —
  // the same labels the Text settings row shows.
  static_assert(std::size(LINE_SPACING_IDS) == CrossPointSettings::LINE_COMPRESSION_COUNT, "line spacing labels");
  popup_.show(StrId::STR_LINE_SPACING, LINE_SPACING_IDS, static_cast<int>(std::size(LINE_SPACING_IDS)),
              SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT, [this](const int idx) {
                if (idx < 0 || idx >= CrossPointSettings::LINE_COMPRESSION_COUNT) return;
                SETTINGS.lineSpacing = static_cast<uint8_t>(idx);
                if (!SETTINGS.saveToFile()) {
                  LOG_ERR("FPR", "font preview: settings save failed");
                }
                needsRelayout_ = true;
              });
  requestUpdate();
}

void FontPreviewActivity::openWordSpacingSlider() {
  // Indexed picker over the same discrete steps the Text panel uses, so both
  // surfaces offer identical choices.
  static constexpr StrId kWordIds[] = {StrId::STR_SPACING_50_PERCENT,  StrId::STR_SPACING_75_PERCENT,
                                       StrId::STR_SPACING_100_PERCENT, StrId::STR_SPACING_125_PERCENT,
                                       StrId::STR_SPACING_150_PERCENT, StrId::STR_SPACING_175_PERCENT,
                                       StrId::STR_SPACING_200_PERCENT};
  static_assert(std::size(kWordIds) == (CrossPointSettings::WORD_SPACING_MAX - CrossPointSettings::WORD_SPACING_MIN) /
                                               CrossPointSettings::WORD_SPACING_STEP +
                                           1,
                "word spacing steps");
  const int current = (std::clamp<int>(SETTINGS.wordSpacing, CrossPointSettings::WORD_SPACING_MIN,
                                       CrossPointSettings::WORD_SPACING_MAX) -
                       CrossPointSettings::WORD_SPACING_MIN) /
                      CrossPointSettings::WORD_SPACING_STEP;
  popup_.show(StrId::STR_WORD_SPACING, kWordIds, static_cast<int>(std::size(kWordIds)), current, [this](const int idx) {
    if (idx < 0 || idx >= static_cast<int>(std::size(kWordIds))) return;
    SETTINGS.wordSpacing =
        static_cast<uint8_t>(CrossPointSettings::WORD_SPACING_MIN + idx * CrossPointSettings::WORD_SPACING_STEP);
    if (!SETTINGS.saveToFile()) {
      LOG_ERR("FPR", "font preview: settings save failed");
    }
    needsRelayout_ = true;
  });
  requestUpdate();
}

void FontPreviewActivity::openCharacterSpacingPicker() {
  static constexpr StrId kSpacingIds[] = {StrId::STR_SPACING_MINUS_2, StrId::STR_SPACING_MINUS_1,
                                          StrId::STR_SPACING_ZERO, StrId::STR_SPACING_PLUS_1,
                                          StrId::STR_SPACING_PLUS_2};
  // characterSpacing is stored as the raw 0..4 picker index; the -2 px offset
  // lives in getCharacterSpacing(). TextSettingsActivity indexes the picker the
  // same way, so applying the offset here would shift every label and store
  // values outside the enum.
  popup_.show(StrId::STR_CHARACTER_SPACING, kSpacingIds, static_cast<int>(std::size(kSpacingIds)),
              static_cast<int>(SETTINGS.characterSpacing), [this](const int idx) {
                if (idx < 0 || idx >= static_cast<int>(std::size(kSpacingIds))) return;
                SETTINGS.characterSpacing = static_cast<uint8_t>(idx);
                if (!SETTINGS.saveToFile()) {
                  LOG_ERR("FPR", "font preview: settings save failed");
                }
                needsRelayout_ = true;
              });
  requestUpdate();
}

void FontPreviewActivity::close() {
  // Net-effect close contract (design decision log): compare the final values
  // with the entry snapshots, so a bounced change (A→B→A) on ANY row closes
  // silently — zero reflow, zero further SD writes.
  const bool changed = SETTINGS.ttfFontPointSize != entrySize_ ||
                       strncmp(SETTINGS.ttfFontFamilyName, entryFamily_, sizeof(entryFamily_)) != 0 ||
                       SETTINGS.lineSpacing != entryLineSpacing_ || SETTINGS.wordSpacing != entryWordSpacing_ ||
                       SETTINGS.characterSpacing != entryCharacterSpacing_;
  ActivityResult result;
  if (changed) {
    result = QuickFontPreviewResult{true};
  } else {
    result.isCancelled = true;
  }
  setResult(std::move(result));
  finish();
}

void FontPreviewActivity::loop() {
  // An open modal owns all input (same shape as the reader's overlayPopup
  // branch). The selection callback applies the setting; the dismiss callback
  // repaints so the relayout runs exactly once.
  if (familyPopup_.isActive() || popup_.isActive()) {
    const bool family = familyPopup_.isActive();
    const auto done = [this, family] {
      if (family ? familyPopup_.isActive() : popup_.isActive()) {
        requestUpdate();  // highlight moved
        return;
      }
      requestUpdate();  // selected or dismissed: render() re-lays if pending
    };
    // Dispatch to the popup that is actually open — handleInput() on an
    // inactive popup would swallow the input.
    if (family) {
      familyPopup_.handleInput(mappedInput, done);
    } else {
      popup_.handleInput(mappedInput, done);
    }
    return;
  }

  const auto routed = chrome_ ? chrome_->route(mappedInput) : ReaderToolbarUi::Routed{};
  if (routed.routed) {
    switch (routed.event) {
      case ReaderToolbarUi::Event::Dismiss:
        close();
        return;
      case ReaderToolbarUi::Event::FontRow:
        cursorRow_ = routed.value;
        openRow(routed.value);
        return;
      default:
        break;
    }
    if (routed.routed) return;
  }

  constexpr int kLastRow = 4;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    cursorRow_ = cursorRow_ <= 0 ? kLastRow : cursorRow_ - 1;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    cursorRow_ = cursorRow_ >= kLastRow ? 0 : cursorRow_ + 1;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openRow(cursorRow_);
    return;
  }
}

void FontPreviewActivity::openRow(const int row) {
  switch (row) {
    case 0:
      openSizeSlider();
      return;
    case 1:
      openFamilyPicker();
      return;
    case 2:
      openLineSpacingPicker();
      return;
    case 3:
      openWordSpacingSlider();
      return;
    case 4:
      openCharacterSpacingPicker();
      return;
    default:
      return;
  }
}

void FontPreviewActivity::render(RenderLock&&) {
  if (needsRelayout_) {
    relayout();
  }
#if CROSSPOINT_TTF_UI_FALLBACK
  // AFTER the relayout: a family change reloads the loader's resident bytes
  // inside makeLayoutParams(); the reader's seam ended the borrowed fallback
  // faces before that, and update() re-registers them against the fresh
  // bytes BEFORE any chrome text draws (issue #168). Fast pointer-compare
  // path when clean.
  freeink::book::ttfUiFallback.update(renderer);
#endif
  if (preview_.ready() && relayoutFont_ != nullptr) {
    renderer.clearScreen(0xFF);
    paintCapturedPage(preview_.page(), relayoutFont_, renderer, *ttf_, relayoutCharSpacingPx_, relayoutWordSpacingPx_);
  } else {
    // No confirmed capture (scan failed / anchor out of reach): keep the
    // frame the preview opened over rather than showing a wrong page.
    LOG_DBG("FPR", "preview: no confirmed capture, keeping previous frame");
  }

  if (chrome_) {
    ReaderToolbarUi::Model model;
    model.quickFont = true;
    model.panelTitle = tr(STR_FONT);
    model.quickSelected = cursorRow_;
    model.bottomReserve = mappedInput.hasTouch() ? 0 : UITheme::getInstance().getMetrics().buttonHintsHeight;
    std::string sizeText = std::to_string(SETTINGS.ttfFontPointSize) + " pt";
    std::string familyText = SETTINGS.ttfFontFamilyName[0] != '\0' ? SETTINGS.ttfFontFamilyName : tr(STR_BUILTIN_FONT);
    model.sizeText = sizeText.c_str();
    model.familyText = familyText.c_str();
    static constexpr StrId kCharIds[] = {StrId::STR_SPACING_MINUS_2, StrId::STR_SPACING_MINUS_1,
                                         StrId::STR_SPACING_ZERO, StrId::STR_SPACING_PLUS_1, StrId::STR_SPACING_PLUS_2};
    static constexpr StrId kWordIds[] = {StrId::STR_SPACING_50_PERCENT,  StrId::STR_SPACING_75_PERCENT,
                                         StrId::STR_SPACING_100_PERCENT, StrId::STR_SPACING_125_PERCENT,
                                         StrId::STR_SPACING_150_PERCENT, StrId::STR_SPACING_175_PERCENT,
                                         StrId::STR_SPACING_200_PERCENT};
    const std::string lineText =
        I18N.get(LINE_SPACING_IDS[SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT]);
    const int wordIndex = (std::clamp<int>(SETTINGS.wordSpacing, CrossPointSettings::WORD_SPACING_MIN,
                                           CrossPointSettings::WORD_SPACING_MAX) -
                           CrossPointSettings::WORD_SPACING_MIN) /
                          CrossPointSettings::WORD_SPACING_STEP;
    const std::string wordText =
        I18N.get(kWordIds[wordIndex >= 0 && wordIndex < static_cast<int>(std::size(kWordIds)) ? wordIndex : 2]);
    const int charIndex = std::clamp<int>(SETTINGS.characterSpacing, 0, static_cast<int>(std::size(kCharIds)) - 1);
    const std::string charText = I18N.get(kCharIds[charIndex]);
    model.lineSpacingText = lineText.c_str();
    model.wordSpacingText = wordText.c_str();
    model.characterSpacingText = charText.c_str();
    chrome_->setModel(model);
    chrome_->render();
    if (!mappedInput.hasTouch()) {
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  }
  if (familyPopup_.isActive()) {
    familyPopup_.render(renderer);
  }
  if (popup_.isActive()) {
    popup_.render(renderer);
  }
  renderer.displayBuffer();
}

#endif  // CROSSPOINT_TTF_READER
