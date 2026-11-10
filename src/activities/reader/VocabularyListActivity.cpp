#include "VocabularyListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr int DELETE_HOLD_MS = 700;  // same hold as the clippings list
}

VocabularyListActivity::VocabularyListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("VocabularyList", renderer, mappedInput, /*wantsTouchLongPress=*/true) {}

void VocabularyListActivity::onEnter() {
  UiListActivity::onEnter();
  loadEntries();
}

void VocabularyListActivity::loadEntries() {
  if (!VocabularyStore::loadRecent(entries, ROW_LIMIT)) {
    LOG_ERR("VOCAB", "Could not read the notebook store");
    entries.clear();
  }
}

int VocabularyListActivity::listCount() const { return static_cast<int>(entries.size()); }

const char* VocabularyListActivity::headerTitle() const { return tr(STR_VOCAB_NOTEBOOK); }

// Visible rows only: the loaded entries are sliced into the row window the
// list viewport shows, same shape as the clippings list.
void VocabularyListActivity::refreshRowWindow(const int start) {
  const int total = listCount();
  int clamped = std::max(0, start);
  if (clamped > total - ROW_WINDOW) clamped = std::max(0, total - ROW_WINDOW);
  if (clamped == windowStart) return;

  windowCount = std::min(ROW_WINDOW, total - clamped);
  for (int slot = 0; slot < windowCount; ++slot) {
    const NotebookEntry& entry = entries[static_cast<size_t>(clamped + slot)];
    fui::ListItem item;
    item.label = entry.word;
    // Headword disambiguates an inflected lookup ("ran" -> "run"); omitted
    // when identical.
    item.subtitle = entry.headword != entry.word ? entry.headword : "";
    item.icon = listIconFor(UIIcon::Book, 32);  // subtitle rows carry the larger icon
    item.actionValue = static_cast<int16_t>(clamped + slot);
    rowItems[slot] = item;
  }
  windowStart = clamped;
}

void VocabularyListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (listCount() == 0) {
    screen.centeredText(tr(STR_NO_LOOKUPS), screen.theme().bodyText);
    return;
  }

  if (!mappedInput.hasTouch()) {
    const int helpHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const fui::Rect band = screen.takeBottom(static_cast<int16_t>(helpHeight + metrics.verticalSpacing));
    GUI.drawHelpText(renderer, Rect{band.x, band.y + metrics.verticalSpacing, band.width, helpHeight},
                     tr(STR_HOLD_SELECT_TO_DELETE));
  }

  fui::ListProps props;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  syncListViewport(screen, props);
  refreshRowWindow(nav.top);
  props.items = rowItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(windowCount);
  screen.list(props);
}

void VocabularyListActivity::relookupSelected() {
  if (confirmPopup.isActive() || nav.selected < 0 || nav.selected >= listCount()) return;
  app.clearTapFlash();
  // Hand the word back to the reader's own lookup funnel; the definition
  // screen records the re-lookup into the notebook like any fresh lookup.
  setResult(ActivityResult(DictionarySearchResult{entries[static_cast<size_t>(nav.selected)].word}));
  finish();
}

void VocabularyListActivity::activateIndex(const int index) {
  if (confirmPopup.isActive() || index < 0 || index >= listCount()) return;
  nav.selected = index;
  relookupSelected();
}

void VocabularyListActivity::onRowLongPress(const int index) {
  if (confirmPopup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  showRowMenu();
}

void VocabularyListActivity::showRowMenu() {
  if (listCount() == 0 || confirmPopup.isActive()) return;
  menuOpen = true;
  static constexpr StrId OPTIONS[] = {StrId::STR_CANCEL, StrId::STR_DELETE, StrId::STR_CLEAR_ALL};
  confirmPopup.show(tr(STR_VOCAB_NOTEBOOK), OPTIONS, 3, 1, [this](const int index) {
    menuOpen = false;
    if (index == 1) {
      deleteSelected();
      return;
    }
    if (index == 2) {
      clearAll();
      return;
    }
    requestUpdate();
  });
  requestUpdate();
}

void VocabularyListActivity::deleteSelected() {
  const int selected = nav.selected;
  if (selected < 0 || selected >= listCount()) return;
  if (!VocabularyStore::removeRecord(entries[static_cast<size_t>(selected)].recordIndex)) {
    LOG_ERR("VOCAB", "Could not delete the notebook record");
    return;
  }
  loadEntries();
  if (listCount() == 0) {
    finish();
    return;
  }
  if (nav.selected >= listCount()) nav.selected = listCount() - 1;
  nav.follow(listCount());
}

void VocabularyListActivity::clearAll() {
  if (!VocabularyStore::clear()) {
    LOG_ERR("VOCAB", "Could not clear the notebook store");
    return;
  }
  loadEntries();
  finish();
}

bool VocabularyListActivity::handleCustomInput() {
  if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  if (menuOpen) {
    menuOpen = false;
    requestUpdate();
  }
  return false;
}

bool VocabularyListActivity::handleButtons() {
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, DELETE_HOLD_MS)) {
    showRowMenu();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    relookupSelected();
    return true;
  }
  return false;
}
