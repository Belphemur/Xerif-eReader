#include <gtest/gtest.h>

#include <cstdint>

#include "src/activities/reader/TouchLongPressRoute.h"

// Regression contract for the touch long-press routing in
// EpubReaderActivity::loop (src/activities/reader/EpubReaderActivity.cpp).
//
// Since v2.6.0 the clip-selection long-press consumer (upstream d7fcc7ce,
// #3589) sits above the fork's dictionary consumer (94e4a6f5, PR #10) and both
// read mappedInput.wasScreenLongPress(): the clip gate consumed every screen
// long press and returned, so the owner's configured touchLongPressAction was
// dead code. The fix routes the setting through the pure helper under test
// and gates BOTH consumers on its single result, so exactly one consumer can
// fire per long press.
//
// Numeric mapping mirrors CrossPointSettings::TOUCH_LONG_PRESS_ACTION
// (persisted by menu-position index, APPEND ONLY): 0 = TOUCH_LP_DICTIONARY,
// 1 = TOUCH_LP_IGNORE, 2 = TOUCH_LP_FOOTNOTE. EpubReaderActivity.cpp
// static_asserts the correspondence.

TEST(TouchLongPressRoute, DictionarySettingRoutesToDictionary) {
  // TOUCH_LP_DICTIONARY => the dictionary word selector, never clip selection.
  EXPECT_EQ(resolveTouchLongPressRoute(0), TouchLongPressRoute::Dictionary);
}

TEST(TouchLongPressRoute, FootnoteSettingRoutesToFootnote) {
  // TOUCH_LP_FOOTNOTE => footnote mode of the word selector.
  EXPECT_EQ(resolveTouchLongPressRoute(2), TouchLongPressRoute::Footnote);
}

TEST(TouchLongPressRoute, IgnoreSettingRoutesToClipSelection) {
  // TOUCH_LP_IGNORE keeps its meaning: upstream's clip (highlight) selection.
  EXPECT_EQ(resolveTouchLongPressRoute(1), TouchLongPressRoute::ClipSelection);
}

TEST(TouchLongPressRoute, EveryActionValueRoutesToExactlyOneConsumer) {
  // Structural exclusivity: for every persisted action value exactly one of
  // the two consumer groups matches, so only one gate in the reader loop can
  // call wasScreenLongPress() per tick.
  for (uint8_t action = 0; action <= 2; ++action) {
    const TouchLongPressRoute route = resolveTouchLongPressRoute(action);
    const bool isClip = route == TouchLongPressRoute::ClipSelection;
    const bool isWordSelect = route == TouchLongPressRoute::Dictionary || route == TouchLongPressRoute::Footnote;
    EXPECT_NE(isClip, isWordSelect) << "action " << static_cast<int>(action) << " must route to exactly one consumer";
  }
}

TEST(TouchLongPressRoute, FootnoteRouteDrivesFootnoteMode) {
  // The word-select gate derives TouchLongPressMode from the route, so the
  // Footnote route must map to TouchLongPressMode::Footnote (and nothing else
  // may): the word-select activity never reads the mutable SETTINGS global
  // (docs/design/touch-long-press-dictionary.md).
  for (uint8_t action = 0; action <= 2; ++action) {
    const TouchLongPressRoute route = resolveTouchLongPressRoute(action);
    const TouchLongPressMode mode =
        route == TouchLongPressRoute::Footnote ? TouchLongPressMode::Footnote : TouchLongPressMode::Dictionary;
    if (action == 2) {
      EXPECT_EQ(mode, TouchLongPressMode::Footnote);
    } else {
      EXPECT_EQ(mode, TouchLongPressMode::Dictionary);
    }
  }
}
