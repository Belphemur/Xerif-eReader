#include "TextSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderFontSizes.h"
#include "ReaderLineSpacing.h"
#include "SdCardFontSystem.h"
#include "TextSettingsPreview.h"
#include "TtfUiFallback.h"
#include "components/UITheme.h"
#include "fontIds.h"
#if defined(CROSSPOINT_TTF_READER)
#include <BookFontLoader.h>

#include "activities/util/IntervalSelectionActivity.h"
#endif

namespace fui = freeink::ui;

namespace {
// Tab labels for Font | Size | Layout | Style.
constexpr StrId TAB_NAME_IDS[] = {StrId::STR_FONT, StrId::STR_SIZE, StrId::STR_LAYOUT, StrId::STR_STYLE};

constexpr StrId LAYOUT_ROW_NAME_IDS[] = {
    StrId::STR_LINE_SPACING,          StrId::STR_WORD_SPACING, StrId::STR_CHARACTER_SPACING, StrId::STR_EXTRA_SPACING,
    StrId::STR_PARAGRAPH_INDENTATION, StrId::STR_ALIGNMENT,    StrId::STR_SCREEN_MARGIN};
constexpr StrId STYLE_ROW_NAME_IDS[] = {StrId::STR_FOCUS_READING, StrId::STR_HYPHENATION_NO_PREVIEW,
                                        StrId::STR_EMBEDDED_STYLE_NO_PREVIEW, StrId::STR_TEXT_RENDERING_NO_PREVIEW};

#if !defined(CROSSPOINT_TTF_READER)
int findCurrentFontIndex(const SdCardFontRegistry* registry, const char* sdFontFamilyName, uint8_t fontFamily) {
  if (sdFontFamilyName[0] != '\0' && registry) {
    const auto& families = registry->getFamilies();
    // UI list has VISIBLE_BUILTIN_FONT_COUNT built-in entries + SD entries.
    // SD font UI index = VISIBLE_BUILTIN_FONT_COUNT + i (not BUILTIN_FONT_COUNT).
    constexpr int VISIBLE_BUILTIN_FONT_COUNT = 1;
    const auto family = std::find_if(families.begin(), families.end(), [sdFontFamilyName](const auto& candidate) {
      return candidate.name == sdFontFamilyName;
    });
    if (family != families.end()) {
      return VISIBLE_BUILTIN_FONT_COUNT + static_cast<int>(family - families.begin());
    }
  }

  // Built-in: the single Atkinson entry is UI index 0 (NotoSerif became an
  // optional SD download, upstream #4022).
  return 0;
}
#endif  // !CROSSPOINT_TTF_READER

constexpr StrId WORD_SPACING_IDS[] = {StrId::STR_SPACING_50_PERCENT,  StrId::STR_SPACING_75_PERCENT,
                                      StrId::STR_SPACING_100_PERCENT, StrId::STR_SPACING_125_PERCENT,
                                      StrId::STR_SPACING_150_PERCENT, StrId::STR_SPACING_175_PERCENT,
                                      StrId::STR_SPACING_200_PERCENT};
constexpr StrId CHARACTER_SPACING_IDS[] = {StrId::STR_SPACING_MINUS_2, StrId::STR_SPACING_MINUS_1,
                                           StrId::STR_SPACING_ZERO, StrId::STR_SPACING_PLUS_1,
                                           StrId::STR_SPACING_PLUS_2};
constexpr StrId ALIGNMENT_IDS[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                                   StrId::STR_BOOK_S_STYLE};
constexpr int MARGIN_MIN = CrossPointSettings::SCREEN_MARGIN_MIN;
constexpr int MARGIN_MAX = CrossPointSettings::SCREEN_MARGIN_MAX;
constexpr int MARGIN_STEP = CrossPointSettings::SCREEN_MARGIN_STEP;
#if defined(CROSSPOINT_TTF_READER)
// Style-coverage suffix for TTF family rows (§12): the availability is shown
// honestly — "Name · Regular Bold Italic" — so a Regular-only family reads as
// synthetic-bold/italic in the reader.
std::string ttfCoverageSuffix(const freeink::book::FamilyInfo& fam) {
  bool regular = false, bold = false, italic = false, boldItalic = false;
  for (uint8_t i = 0; i < fam.faceCount && i < 4; ++i) {
    switch (fam.faces[i].styleFlags & (freeink::book::StyleBold | freeink::book::StyleItalic)) {
      case freeink::book::StyleBold | freeink::book::StyleItalic:
        boldItalic = true;
        break;
      case freeink::book::StyleBold:
        bold = true;
        break;
      case freeink::book::StyleItalic:
        italic = true;
        break;
      default:
        regular = true;
        break;
    }
  }
  std::string suffix;
  const auto append = [&suffix](bool& first, std::string label) {
    suffix += first ? " · " : " ";
    first = false;
    suffix += label;
  };
  bool first = true;
  if (regular) append(first, I18N.get(StrId::STR_STYLE_REGULAR));
  if (bold) append(first, I18N.get(StrId::STR_STYLE_BOLD));
  if (italic) append(first, I18N.get(StrId::STR_STYLE_ITALIC));
  if (boldItalic) {
    append(first, std::string(I18N.get(StrId::STR_STYLE_BOLD)) + "/" + I18N.get(StrId::STR_STYLE_ITALIC));
  }
  return suffix;
}
#endif
constexpr int WORD_SPACING_MIN = CrossPointSettings::WORD_SPACING_MIN;
constexpr int WORD_SPACING_MAX = CrossPointSettings::WORD_SPACING_MAX;
constexpr int WORD_SPACING_STEP = CrossPointSettings::WORD_SPACING_STEP;
static_assert(std::size(WORD_SPACING_IDS) == (WORD_SPACING_MAX - WORD_SPACING_MIN) / WORD_SPACING_STEP + 1);
}  // namespace

TextSettingsActivity::TextSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const SdCardFontRegistry* registry, Tab initialTab)
    : UiTabListActivity("TextSettings", renderer, mappedInput), registry_(registry), tab_(initialTab) {}

const char* TextSettingsActivity::tabLabel(const int index) const { return I18N.get(TAB_NAME_IDS[index]); }

void TextSettingsActivity::onEnter() {
  UiTabListActivity::onEnter();

  metrics_ = UITheme::getInstance().getMetrics();
  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int safeBottom = safeArea.y + safeArea.height;
  afterHeader = safeArea.y + metrics_.topPadding + metrics_.headerHeight + metrics_.verticalSpacing;
  const int usableHeight = safeBottom - afterHeader - metrics_.verticalSpacing;
  previewHeight = usableHeight * metrics_.previewHeightPercent / 100;

  fonts_.clear();
#if defined(CROSSPOINT_TTF_READER)
  // §14.1/§14.2: TTF builds show the native family picker (built-in + §14.4
  // families); the legacy bitmap rows and the SD .cpfont list belong to the
  // legacy class's tab (the #else branch below).
  // Sync the loader's render mode with the persisted setting first: a Crisp
  // mode saved in a previous session must apply before any render.
  freeink::book::fontLoader.applyRenderMode(SETTINGS.textRenderMode == CrossPointSettings::TEXT_RENDER_CRISP);
#if CROSSPOINT_TTF_UI_FALLBACK
  // begin() releases resident face bytes unconditionally (and clears the
  // dirty flag) — registered UI-fallback faces must end FIRST while the
  // bytes they borrow are still valid. Re-registered by the update() that
  // follows the rescan's next ensureLoaded().
  freeink::book::ttfUiFallback.release(renderer);
#endif
  freeink::book::fontLoader.begin();  // rescan: pick up fonts added since boot
  // Apply the persisted family BEFORE registering the UI fallback so
  // update()'s ensureLoaded() resolves the user's family, not families_[0]
  // (a later selectFamily would reload and invalidate the freshly borrowed
  // byte owners — use-after-free on the next UI glyph).
  if (SETTINGS.readerFontEngine == CrossPointSettings::READER_ENGINE_TTF) {
    freeink::book::fontLoader.selectFamily(SETTINGS.ttfFontFamilyName);
  }
#if CROSSPOINT_TTF_UI_FALLBACK
  // Re-register the UI fallback against the rescanned manifest (the release
  // above unregistered it; ensureLoaded inside update() reloads the active
  // family against the fresh manifest).
  freeink::book::ttfUiFallback.update(renderer);
#endif
  fonts_.reserve(1 + static_cast<size_t>(freeink::book::fontLoader.familyCount()));
  fonts_.push_back({I18N.get(StrId::STR_BUILTIN_FONT), true, 0, true});
  for (uint8_t i = 0; i < freeink::book::fontLoader.familyCount(); ++i) {
    const auto& fam = freeink::book::fontLoader.families()[i];
    fonts_.push_back(
        {std::string(fam.name) + ttfCoverageSuffix(fam), false, i, freeink::book::fontLoader.isFamilyAvailable(fam)});
  }

  // Current selection: index 0 = built-in fallback; families follow in scan
  // order. A settings name the scanner no longer finds falls back to built-in.
  currentFamilyIndex_ = 0;
  if (SETTINGS.ttfFontFamilyName[0] != '\0') {
    const auto* fam = freeink::book::fontLoader.findFamily(SETTINGS.ttfFontFamilyName);
    if (fam != nullptr) {
      currentFamilyIndex_ = 1 + static_cast<int>(fam - freeink::book::fontLoader.families());
    }
  }
  // Sync the loader's chain with the persisted selection before the first
  // preview render: the preview rasterizes via getReaderFont(), which uses
  // the loader's current selection — boot can reach this screen before any
  // reader call ever ran selectFamily.
  if (SETTINGS.readerFontEngine == CrossPointSettings::READER_ENGINE_TTF) {
    freeink::book::fontLoader.selectFamily(SETTINGS.ttfFontFamilyName);
  }
  rebuildSizeList();
#else
  constexpr int VISIBLE_BUILTIN_FONT_COUNT = 1;
  fonts_.reserve(VISIBLE_BUILTIN_FONT_COUNT + (registry_ ? registry_->getFamilyCount() : 0));
  // Built-in font: ATKINSON_HN (labeled, enum value NOTOSANS = 0 — the
  // historical persisted ID for the sans family). NotoSerif became an
  // optional SD download (upstream #4022) and is no longer a menu entry.
  fonts_.push_back({I18N.get(StrId::STR_ATKINSON_HN), true, static_cast<uint8_t>(CrossPointSettings::NOTOSANS), true});
  if (registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      fonts_.push_back(
          {families[i].name, false, static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i), true});
    }
  }

  rebuildSizeList();

  currentFamilyIndex_ = findCurrentFontIndex(registry_, SETTINGS.sdFontFamilyName, SETTINGS.fontFamily);
#endif
  // Per-tab ring positions (0 = tab bar, 1..N = row). The base reset each
  // tab's nav with followOnBuild armed, so each tab's first build shows its
  // remembered selection (Family/Size open on the current item).
  for (auto& n : tabNavs) n.selected = 1;  // default to the first list row
  tabNavs[static_cast<int>(Tab::Family)].selected = currentFamilyIndex_ + 1;
  tabNavs[static_cast<int>(Tab::Size)].selected = currentSizeIndex_ + 1;
  tabNavs[static_cast<int>(tab_)].selected = 0;  // screen opens with the tab bar focused, not a list row

  rebuildRowItems();
}

// Rebuilds rowItems_ (label + actionValue) for the active tab. Structural —
// call only when tab_ or its backing data (fonts_/sizes_) changes, never from
// buildScreen(), which just refreshes rowValues_/rowItems_[].value in place.
void TextSettingsActivity::rebuildRowItems() {
  const int count = listCount();
  rowValues_.assign(count, std::string());
  rowItems_.clear();
  rowItems_.reserve(count);
  for (int i = 0; i < count; i++) {
    fui::ListItem item;
    switch (tab_) {
      case Tab::Family:
        item.label = fonts_[i].name.c_str();
        break;
      case Tab::Size:
        item.label = sizes_[i].name.c_str();
        break;
      case Tab::Layout:
        item.label = I18N.get(LAYOUT_ROW_NAME_IDS[i]);
        break;
      case Tab::Style:
        item.label = I18N.get(STYLE_ROW_NAME_IDS[i]);
        break;
      default:
        break;
    }
    item.actionValue = static_cast<int16_t>(i);
#if defined(CROSSPOINT_TTF_READER)
    // §12: families failing the static gates stay visible but disabled; a
    // Confirm/tap shows the one-line reason instead of applying.
    if (tab_ == Tab::Family && !fonts_[i].available) item.enabled = false;
#endif
    rowItems_.push_back(item);
  }
}

// The selectable sizes belong to the active family, so this runs on entry and
// again after every family change. A family change goes through ensureLoaded(),
// which snaps SETTINGS.fontPointSize into the new family's set — but entry does
// not, so the highlight is resolved by snapping rather than by exact match.
void TextSettingsActivity::rebuildSizeList() {
#if defined(CROSSPOINT_TTF_READER)
  // §14.2: TTF builds carry ONE continuous point size — the Size tab is a
  // single row that opens the slider; there is no per-family discrete set.
  sizes_.clear();
  sizes_.reserve(1);
  char label[12];
  snprintf(label, sizeof(label), "%u pt", SETTINGS.ttfFontPointSize);
  sizes_.push_back({label, SETTINGS.ttfFontPointSize});
  currentSizeIndex_ = 0;
  return;
#endif
  const std::vector<uint8_t> points = readerFontPointSizes(registry_, SETTINGS.sdFontFamilyName);

  // The stored size can still sit outside this family's set — e.g. the family
  // was deleted while selected, or the card was swapped. Highlight the size the
  // reader actually renders, which getReaderFontId() resolves the same way.
  const uint8_t selectedPt = snapToNearestPointSize(points, SETTINGS.fontPointSize);

  sizes_.clear();
  sizes_.reserve(points.size());
  currentSizeIndex_ = 0;
  for (const uint8_t pt : points) {
    // "pt" is deliberately not translated: it is the typographic unit symbol,
    // written the same way in every language CrossPoint ships.
    char label[12];
    snprintf(label, sizeof(label), "%u pt", pt);
    if (pt == selectedPt) currentSizeIndex_ = static_cast<int>(sizes_.size());
    sizes_.push_back({label, pt});
  }
}

void TextSettingsActivity::onTabAction(const int index) {
  if (optionPopup_.isActive()) return;
  if (tab_ != static_cast<Tab>(index)) {
    tab_ = static_cast<Tab>(index);
    rebuildRowItems();
    auto& n = activeNav();
    n.selected = 0;          // tab taps land with the tab bar focused (legacy tap behavior)
    n.followOnBuild = true;  // pull the new tab's viewport to its remembered selection
    requestUpdate();
  }
  // The switched-to tab repaints as the selected pill; a flash overlay on top
  // of it just repaints the pill in the focused style.
  app.clearTapFlash();
}

void TextSettingsActivity::activateIndex(const int index) {
  if (optionPopup_.isActive()) return;
  // Most rows repaint a different surface (popup, preview, new value);
  // a lingering tap flash would gray an unrelated element.
  app.clearTapFlash();
  activateRow(index);
}

bool TextSettingsActivity::handleCustomInput() {
  return optionPopup_.handleInput(mappedInput, [this] { requestUpdate(); });
}

bool TextSettingsActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ringPos() == 0) {
      switchTab();
    } else {
      activateRow(ringPos() - 1);
    }
    return true;
  }

  return false;
}

void TextSettingsActivity::buildScreen(UiScreen& screen) {
  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int tabTop = afterHeader + previewHeight;
  const int rightInset = renderer.getScreenWidth() - (safeArea.x + safeArea.width);
  const int bottomInset = renderer.getScreenHeight() - (safeArea.y + safeArea.height);
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(tabTop), static_cast<int16_t>(rightInset),
                                                static_cast<int16_t>(bottomInset), static_cast<int16_t>(safeArea.x)});

  buildTabBar(screen);

  // rowItems_ (label/actionValue) was built by rebuildRowItems() when the tab
  // was last switched; only the live value text needs refreshing here, by
  // assigning into the existing rowValues_ strings (no vector growth) rather
  // than building a new items/values vector on every render.
  const int count = listCount();
  for (int i = 0; i < count; i++) {
    switch (tab_) {
      case Tab::Family:
        rowValues_[i] = (i == currentFamilyIndex_) ? tr(STR_SELECTED) : "";
        break;
      case Tab::Size:
        rowValues_[i] = (i == currentSizeIndex_) ? tr(STR_SELECTED) : "";
        break;
      case Tab::Layout:
        rowValues_[i] = layoutValueText(i);
        break;
      default:
        break;
    }
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
    if (tab_ == Tab::Style) {
      if (i == static_cast<int>(StyleRow::TextRendering)) {
        // A two-mode ring (Smooth/Crisp), not a boolean — renders as value text.
        rowValues_[i] =
            SETTINGS.textRenderMode == CrossPointSettings::TEXT_RENDER_SMOOTH ? tr(STR_SMOOTH) : tr(STR_CRISP);
        rowItems_[i].value = rowValues_[i].c_str();
      } else {
        GUI.setCheckboxRow(rowItems_[i], styleRowChecked(i));
      }
    } else if (tab_ == Tab::Layout && i == static_cast<int>(LayoutRow::ParaSpacing)) {
      GUI.setCheckboxRow(rowItems_[i], SETTINGS.extraParagraphSpacing);
    }
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  // Titles match the value's font size (smallText) so both sides of a row
  // read as one unit; labels that still don't fit wrap onto a second line.
  // maxLines=2 also marks the style explicitly set (see SettingsActivity).
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncTabListViewport(screen, props);
  screen.list(props);
}

const char* TextSettingsActivity::confirmLabelText() const {
  if (ringPos() == 0) {
    // Confirm on the tab bar advances to the next tab.
    return I18N.get(TAB_NAME_IDS[(static_cast<int>(tab_) + 1) % static_cast<int>(Tab::Count)]);
  }
  switch (tab_) {
    case Tab::Layout:
      return ringPos() - 1 == static_cast<int>(LayoutRow::ParaSpacing) ? tr(STR_TOGGLE) : tr(STR_SELECT);
    case Tab::Style:
      return tr(STR_TOGGLE);
    default:
      return tr(STR_SELECT);
  }
}

void TextSettingsActivity::drawChrome() {
  GUI.drawHeader(renderer, UITheme::getInstance().getHeaderRect(renderer, true, false), tr(STR_TEXT_SETTINGS));

  const char* familyName = (currentFamilyIndex_ >= 0 && currentFamilyIndex_ < static_cast<int>(fonts_.size()))
                               ? fonts_[currentFamilyIndex_].name.c_str()
                               : "";
  const char* sizeName = (currentSizeIndex_ >= 0 && currentSizeIndex_ < static_cast<int>(sizes_.size()))
                             ? sizes_[currentSizeIndex_].name.c_str()
                             : "";
  char labelBuf[128];
  snprintf(labelBuf, sizeof(labelBuf), "%s \"%s, %s\"", tr(STR_PREVIEW), familyName, sizeName);
  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect previewBounds{safeArea.x, afterHeader, safeArea.width, previewHeight};
  const auto [savedX, savedY, savedWidth, savedHeight] = renderer.getClipRect();
  const int clipLeft = std::max(previewBounds.x, savedX);
  const int clipTop = std::max(previewBounds.y, savedY);
  const int clipRight = std::min(previewBounds.x + previewBounds.width, savedX + savedWidth);
  const int clipBottom = std::min(previewBounds.y + previewBounds.height, savedY + savedHeight);
  renderer.setClipRect(clipLeft, clipTop, std::max(0, clipRight - clipLeft), std::max(0, clipBottom - clipTop));
  textsettings::renderPreview(renderer, previewLayout_, metrics_.previewPadding, metrics_.verticalSpacing,
                              previewBounds, labelBuf);
  renderer.setClipRect(savedX, savedY, savedWidth, savedHeight);
}

void TextSettingsActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabelText(), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TextSettingsActivity::render(RenderLock&& lock) {
  if (optionPopup_.processRender(renderer, mappedInput)) return;  // picker draws over everything
  UiListActivity::render(std::move(lock));
}

// Font switching runs on the main task from loop(), which deliberately holds no
// RenderLock. ensureLoaded() deletes the resident SdCardFont before loading the
// next one, and the render task walks that same object inside the preview's
// prewarmCache() — so without this lock a font switch can free the mini glyph
// arrays out from under prewarmStyle() (crash: null s.miniGlyphs mid-read/sort).
void TextSettingsActivity::applyFamily(int listIndex) {
  RenderLock lock;
#if defined(CROSSPOINT_TTF_READER)
  {
    const auto& font = fonts_[listIndex];
    if (font.isBuiltin) {
      // §14.3: built-in = the Atkinson fallback chain (end-of-chain tail),
      // never a scanned family — clearing the selection returns to it.
      SETTINGS.ttfFontFamilyName[0] = '\0';
    } else {
      if (!font.available) return;  // disabled rows: ListItem gates taps too
      const auto& fam = freeink::book::fontLoader.families()[font.settingIndex];
      strncpy(SETTINGS.ttfFontFamilyName, fam.name, sizeof(SETTINGS.ttfFontFamilyName) - 1);
      SETTINGS.ttfFontFamilyName[sizeof(SETTINGS.ttfFontFamilyName) - 1] = '\0';
    }
    SETTINGS.readerFontEngine = CrossPointSettings::READER_ENGINE_TTF;
    // Load the selection now: the preview pane's next render calls getReaderFont
    // (ensureLoaded inside), and the reader picks the same family on relayout.
    freeink::book::fontLoader.selectFamily(SETTINGS.ttfFontFamilyName);
#if CROSSPOINT_TTF_UI_FALLBACK
    // Re-sync the UI fallback registrations with the new family (or release
    // them for the built-in fallback selection — update() handles both).
    freeink::book::ttfUiFallback.update(renderer);
#endif
    currentFamilyIndex_ = listIndex;
    rebuildSizeList();
    tabNavs[static_cast<int>(Tab::Size)].selected = 1;
    return;
  }
#endif
  const auto& font = fonts_[listIndex];
  if (font.isBuiltin) {
    SETTINGS.fontFamily = font.settingIndex;
    SETTINGS.sdFontFamilyName[0] = '\0';
    sdFontSystem.ensureLoaded(renderer);  // unloads the previously resident SD font
    currentFamilyIndex_ = listIndex;
  } else if (registry_) {
    const int sdIdx = font.settingIndex - CrossPointSettings::BUILTIN_FONT_COUNT;
    const auto& families = registry_->getFamilies();
    if (sdIdx < static_cast<int>(families.size())) {
      strncpy(SETTINGS.sdFontFamilyName, families[sdIdx].name.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
      SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      sdFontSystem.ensureLoaded(renderer);
      currentFamilyIndex_ = listIndex;
    }
  }

  if (currentFamilyIndex_ != listIndex) return;  // switch failed — keep the old size list

  // The new family ships its own set of point sizes, and ensureLoaded() may have
  // snapped the selection into it, so the Size tab's list and its nav position
  // both have to be rebuilt.
  rebuildSizeList();
  tabNavs[static_cast<int>(Tab::Size)].selected = currentSizeIndex_ + 1;
}

void TextSettingsActivity::activateRow(int row) {
  switch (tab_) {
    case Tab::Family:
      if (row != currentFamilyIndex_) {
        applyFamily(row);
        // Persist immediately (like SettingsActivity's per-change saves): the
        // parent's result callback only runs on a normal finish(), so relying
        // on it loses the change when this screen is left via the home
        // gesture/key or a sleep. Saved here, not inside applyFamily, so the
        // SD write happens outside its RenderLock.
        if (currentFamilyIndex_ == row) {
          SETTINGS.saveToFile();
        }
        requestUpdate();
      }
      break;
    case Tab::Size: {
#if defined(CROSSPOINT_TTF_READER)
      // §14.2: continuous point size — the slider dialog (IntervalSelection)
      // applies on OK; the e-ink refresh between stops is the debounce.
      auto sizeDialog = makeUniqueNoThrow<IntervalSelectionActivity>(
          renderer, mappedInput, "TtfPointSize", StrId::STR_FONT_SIZE, SETTINGS.ttfFontPointSize,
          CrossPointSettings::TTF_FONT_POINT_SIZE_MIN, CrossPointSettings::TTF_FONT_POINT_SIZE_MAX, 1, 2,
          StrId::STR_FONT_SIZE_VALUE);
      if (!sizeDialog) {
        LOG_ERR("TSET", "OOM: IntervalSelectionActivity");
        break;
      }
      startActivityForResult(std::move(sizeDialog), [this](const ActivityResult& result) {
        if (result.isCancelled || !std::holds_alternative<IntervalResult>(result.data)) return;
        const auto size = std::get<IntervalResult>(result.data).value;
        SETTINGS.ttfFontPointSize = static_cast<uint8_t>(std::clamp<uint32_t>(
            size, CrossPointSettings::TTF_FONT_POINT_SIZE_MIN, CrossPointSettings::TTF_FONT_POINT_SIZE_MAX));
        SETTINGS.saveToFile();
        rebuildSizeList();
        requestUpdate();
      });
      break;
#endif
      if (row != currentSizeIndex_) {
        applySize(row);
        SETTINGS.saveToFile();
        requestUpdate();
      }
      break;
    }
    case Tab::Layout:
      confirmLayoutRow(row);
      break;
    case Tab::Style:
      confirmStyleRow(row);
      break;
    default:
      break;
  }
}

// Same RenderLock rationale as applyFamily(): a size change reloads the SD font
// file, which frees and replaces the SdCardFont the render task may be reading.
void TextSettingsActivity::applySize(int listIndex) {
  RenderLock lock;

  currentSizeIndex_ = listIndex;
  SETTINGS.fontPointSize = sizes_[listIndex].pointSize;
  sdFontSystem.ensureLoaded(renderer);
}

void TextSettingsActivity::confirmLayoutRow(int row) {
  switch (static_cast<LayoutRow>(row)) {
    case LayoutRow::ParaSpacing:
      SETTINGS.extraParagraphSpacing = !SETTINGS.extraParagraphSpacing;
      SETTINGS.saveToFile();
      requestUpdate();
      break;
    case LayoutRow::ParaIndentation: {
      std::vector<std::string> options;
      options.reserve(6);
      options.push_back(tr(STR_STATE_OFF));
      for (int spaces = 1; spaces <= 5; ++spaces) options.push_back(std::to_string(spaces));
      optionPopup_.show(StrId::STR_PARAGRAPH_INDENTATION, options, SETTINGS.paragraphIndentSpaces, [](int idx) {
        SETTINGS.paragraphIndentSpaces = static_cast<uint8_t>(idx);
        SETTINGS.saveToFile();
      });
      requestUpdate();
      break;
    }
    case LayoutRow::LineSpacing:
      optionPopup_.show(StrId::STR_LINE_SPACING, LINE_SPACING_IDS, static_cast<int>(std::size(LINE_SPACING_IDS)),
                        SETTINGS.lineSpacing, [](int idx) {
                          SETTINGS.lineSpacing = static_cast<uint8_t>(idx);
                          SETTINGS.saveToFile();
                        });
      requestUpdate();
      break;
    case LayoutRow::Alignment:
      optionPopup_.show(StrId::STR_ALIGNMENT, ALIGNMENT_IDS, static_cast<int>(std::size(ALIGNMENT_IDS)),
                        SETTINGS.paragraphAlignment, [](int idx) {
                          SETTINGS.paragraphAlignment = static_cast<uint8_t>(idx);
                          SETTINGS.saveToFile();
                        });
      requestUpdate();
      break;
    case LayoutRow::WordSpacing: {
      const int cur = (std::clamp<int>(SETTINGS.wordSpacing, WORD_SPACING_MIN, WORD_SPACING_MAX) - WORD_SPACING_MIN) /
                      WORD_SPACING_STEP;
      optionPopup_.show(StrId::STR_WORD_SPACING, WORD_SPACING_IDS, static_cast<int>(std::size(WORD_SPACING_IDS)), cur,
                        [](int idx) {
                          SETTINGS.wordSpacing = static_cast<uint8_t>(WORD_SPACING_MIN + idx * WORD_SPACING_STEP);
                          SETTINGS.saveToFile();
                        });
      requestUpdate();
      break;
    }
    case LayoutRow::CharacterSpacing:
      optionPopup_.show(StrId::STR_CHARACTER_SPACING, CHARACTER_SPACING_IDS,
                        static_cast<int>(std::size(CHARACTER_SPACING_IDS)), SETTINGS.characterSpacing, [](int idx) {
                          SETTINGS.characterSpacing = static_cast<uint8_t>(idx);
                          SETTINGS.saveToFile();
                        });
      requestUpdate();
      break;
    case LayoutRow::ScreenMargin: {
      std::vector<std::string> options;
      options.reserve((MARGIN_MAX - MARGIN_MIN) / MARGIN_STEP + 1);
      for (int m = MARGIN_MIN; m <= MARGIN_MAX; m += MARGIN_STEP) options.push_back(std::to_string(m));
      const int cur = (std::clamp<int>(SETTINGS.screenMargin, MARGIN_MIN, MARGIN_MAX) - MARGIN_MIN) / MARGIN_STEP;
      optionPopup_.show(StrId::STR_SCREEN_MARGIN, options, cur, [](int idx) {
        SETTINGS.screenMargin = static_cast<uint8_t>(MARGIN_MIN + idx * MARGIN_STEP);
        SETTINGS.saveToFile();
      });
      requestUpdate();
      break;
    }

    default:
      break;
  }
}

std::string TextSettingsActivity::layoutValueText(int row) const {
  switch (static_cast<LayoutRow>(row)) {
    case LayoutRow::LineSpacing: {
      const uint8_t v = SETTINGS.lineSpacing;
      return I18N.get(LINE_SPACING_IDS[v < std::size(LINE_SPACING_IDS) ? v : CrossPointSettings::LINE_SPACING_120]);
    }
    case LayoutRow::ParaIndentation: {
      const int spaces = std::clamp<int>(SETTINGS.paragraphIndentSpaces, 0, 5);
      return spaces == 0 ? tr(STR_STATE_OFF) : std::to_string(spaces);
    }
    case LayoutRow::Alignment: {
      const uint8_t v = SETTINGS.paragraphAlignment;
      return v < std::size(ALIGNMENT_IDS) ? I18N.get(ALIGNMENT_IDS[v]) : I18N.get(StrId::STR_JUSTIFY);
    }
    case LayoutRow::WordSpacing:
      return std::to_string(SETTINGS.wordSpacing) + "%";
    case LayoutRow::CharacterSpacing: {
      const uint8_t v = SETTINGS.characterSpacing;
      return v < std::size(CHARACTER_SPACING_IDS) ? I18N.get(CHARACTER_SPACING_IDS[v])
                                                  : I18N.get(StrId::STR_SPACING_ZERO);
    }
    case LayoutRow::ScreenMargin:
      return std::to_string(SETTINGS.screenMargin);

    default:
      return "";
  }
}

void TextSettingsActivity::confirmStyleRow(int row) {
  switch (static_cast<StyleRow>(row)) {
    case StyleRow::FocusReading:
      SETTINGS.focusReadingEnabled = !SETTINGS.focusReadingEnabled;
      break;
    case StyleRow::Hyphenation:
      SETTINGS.hyphenationEnabled = !SETTINGS.hyphenationEnabled;
      break;
    case StyleRow::EmbeddedStyle:
      SETTINGS.embeddedStyle = !SETTINGS.embeddedStyle;
      break;
    case StyleRow::TextRendering:
      // Smooth -> Crisp -> Smooth. The mode change re-derives the FT faces'
      // render options through setRenderOptions() (the P1 glyph-cache flush
      // point) and the fingerprint tag folds the mode, so FIBP identity
      // invalidates without a face reload.
      SETTINGS.textRenderMode = SETTINGS.textRenderMode == CrossPointSettings::TEXT_RENDER_SMOOTH
                                    ? CrossPointSettings::TEXT_RENDER_CRISP
                                    : CrossPointSettings::TEXT_RENDER_SMOOTH;
#if defined(CROSSPOINT_TTF_READER)
      // Flush the FT faces' render options (P1 glyph-cache flush point) and
      // refresh the fingerprint tag so FIBP identity invalidates — no face
      // reload. The legacy bitmap path reads the mode per render directly.
      freeink::book::fontLoader.applyRenderMode(SETTINGS.textRenderMode == CrossPointSettings::TEXT_RENDER_CRISP);
#endif
      break;

    default:
      return;
  }
  SETTINGS.saveToFile();
  requestUpdate();
}

bool TextSettingsActivity::styleRowChecked(int row) const {
  switch (static_cast<StyleRow>(row)) {
    case StyleRow::FocusReading:
      return SETTINGS.focusReadingEnabled;
    case StyleRow::Hyphenation:
      return SETTINGS.hyphenationEnabled;
    case StyleRow::EmbeddedStyle:
      return SETTINGS.embeddedStyle;

    default:
      return false;
  }
}

void TextSettingsActivity::switchTab(const int direction) {
  const bool onTabBar = ringPos() == 0;
  constexpr int count = static_cast<int>(Tab::Count);
  tab_ = static_cast<Tab>((static_cast<int>(tab_) + direction + count) % count);
  rebuildRowItems();
  auto& n = activeNav();
  if (onTabBar) n.selected = 0;
  n.followOnBuild = true;  // pull the new tab's viewport to its remembered selection
  requestUpdate();
}

int TextSettingsActivity::listCount() const {
  switch (tab_) {
    case Tab::Family:
      return static_cast<int>(fonts_.size());
    case Tab::Size:
      return static_cast<int>(sizes_.size());
    case Tab::Layout:
      return static_cast<int>(LayoutRow::Count);
    case Tab::Style:
      return static_cast<int>(StyleRow::Count);

    default:
      return 0;
  }
}
