#pragma once

#include <array>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "vocabulary/VocabularyStore.h"

// Vocabulary Notebook list (addendum 2026-11-09): the newest-first view over
// the notebook store, complementing the keyboard chips (last lookups on
// touch only). Buttons and touch both drive it. Rows carry the store's
// file-order record index, so activate / delete / clear act on the very
// records loadRecent() listed. The list is a bounded recency view (the most
// recent ROW_LIMIT rows) — never an unbounded materialization of the whole
// notebook file on the 380 KB-RAM baseline; the file keeps everything.
class VocabularyListActivity final : public UiListActivity {
 public:
  VocabularyListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;

 private:
  // How many recent rows one screen load keeps (matches the retired ring's
  // cap the chips migrated from).
  static constexpr size_t ROW_LIMIT = 50;
  static constexpr int ROW_WINDOW = 8;

  void loadEntries();
  void refreshRowWindow(int start);
  void showRowMenu();
  void deleteSelected();
  void clearAll();
  void relookupSelected();

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  const char* headerTitle() const override;

  std::vector<NotebookEntry> entries;
  std::array<freeink::ui::ListItem, ROW_WINDOW> rowItems;
  int windowStart = -1;
  int windowCount = 0;
  OptionPopup<> confirmPopup;
  bool menuOpen = false;
};
