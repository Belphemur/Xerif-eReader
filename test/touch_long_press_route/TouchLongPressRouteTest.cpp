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
// Since the 2026-11-09 addendum the map has four values (persisted by
// menu-position index, APPEND ONLY): 0 = TOUCH_LP_DICTIONARY,
// 1 = TOUCH_LP_IGNORE, 2 = TOUCH_LP_FOOTNOTE, 3 = TOUCH_LP_CLIP. Ignore (1)
// maps to None: today's build only *looks* inert because startClipSelection
// early-returns on TTF books — the PR's TTF branch would otherwise hijack
// that value instead of leaving "Ignore" a true no-op (no consumer gate
// matches, wasScreenLongPress() is never read). EpubReaderActivity.cpp
// static_asserts the correspondence.

TEST(TouchLongPressRoute, DictionarySettingRoutesToDictionary) {
  // TOUCH_LP_DICTIONARY => the dictionary word selector, never clip selection.
  EXPECT_EQ(resolveTouchLongPressRoute(0), TouchLongPressRoute::Dictionary);
}

TEST(TouchLongPressRoute, FootnoteSettingRoutesToFootnote) {
  // TOUCH_LP_FOOTNOTE => footnote mode of the word selector.
  EXPECT_EQ(resolveTouchLongPressRoute(2), TouchLongPressRoute::Footnote);
}

TEST(TouchLongPressRoute, ClipSettingRoutesToClipSelection) {
  // TOUCH_LP_CLIP (appended value, never renumbered) => the clip selector.
  EXPECT_EQ(resolveTouchLongPressRoute(3), TouchLongPressRoute::ClipSelection);
}

TEST(TouchLongPressRoute, IgnoreSettingIsATrueNoOp) {
  // TOUCH_LP_IGNORE => None: neither the word-select gate nor the clip gate
  // matches, so nothing consumes the long press. The upstream-v1 meaning of
  // this value (clip selection) is gone; Clipping got its own appended value
  // so no user's stored index silently changes meaning.
  EXPECT_EQ(resolveTouchLongPressRoute(1), TouchLongPressRoute::None);
}

TEST(TouchLongPressRoute, EveryActionValueRoutesToExactlyOneConsumer) {
  // Structural exclusivity: for every persisted action value exactly one of
  // the consumer groups matches, so only one gate in the reader loop can
  // call wasScreenLongPress() per tick — or none at all (Ignore), which is
  // the point of None.
  for (uint8_t action = 0; action <= 3; ++action) {
    const TouchLongPressRoute route = resolveTouchLongPressRoute(action);
    const bool isClip = route == TouchLongPressRoute::ClipSelection;
    const bool isWordSelect = route == TouchLongPressRoute::Dictionary || route == TouchLongPressRoute::Footnote;
    const bool isInert = route == TouchLongPressRoute::None;
    // Exactly one of {clip, word-select, inert} holds.
    const int matched = static_cast<int>(isClip) + static_cast<int>(isWordSelect) + static_cast<int>(isInert);
    EXPECT_EQ(matched, 1) << "action " << static_cast<int>(action) << " must route to exactly one consumer group";
  }
}

TEST(TouchLongPressRoute, FootnoteRouteDrivesFootnoteMode) {
  // The word-select gate derives TouchLongPressMode from the route, so the
  // Footnote route must map to TouchLongPressMode::Footnote (and nothing else
  // may): the word-select activity never reads the mutable SETTINGS global
  // (docs/design/touch-long-press-dictionary.md).
  for (uint8_t action = 0; action <= 3; ++action) {
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
