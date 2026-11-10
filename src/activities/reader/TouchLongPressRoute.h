#pragma once

#include <cstdint>

#include "TouchLongPressMode.h"

// Routing decision for a long press on the reading surface. The single
// SETTINGS.touchLongPressAction value selects exactly one consumer in
// EpubReaderActivity::loop; only the matching gate calls
// mappedInput.wasScreenLongPress(), so the other consumers are structurally
// unable to consume the event (upstream's clip gate used to shadow the
// dictionary branch, v2.6.0-v2.6.1).
enum class TouchLongPressRoute : uint8_t { Dictionary, Footnote, ClipSelection, None };

// Numeric mapping mirrors CrossPointSettings::TOUCH_LONG_PRESS_ACTION,
// persisted by menu-position index and APPEND ONLY (CrossPointSettings.h).
// Duplicated as literals because CrossPointSettings.h is not host-testable
// (ArduinoJson); EpubReaderActivity.cpp static_asserts the correspondence.
// Appended values must update this switch — the default falls back to
// Dictionary, the fork's historical default action. None (the Ignore value)
// matches neither consumer gate, so a screen long press with the Ignore
// setting is a true no-op: wasScreenLongPress() is never read and the press
// cannot be hijacked by a consumer added later (addendum 2026-11-09).
constexpr TouchLongPressRoute resolveTouchLongPressRoute(uint8_t touchLongPressAction) {
  switch (touchLongPressAction) {
    case 3:  // TOUCH_LP_CLIP
      return TouchLongPressRoute::ClipSelection;
    case 2:  // TOUCH_LP_FOOTNOTE
      return TouchLongPressRoute::Footnote;
    case 1:  // TOUCH_LP_IGNORE — real inertness, nothing consumes it
      return TouchLongPressRoute::None;
    default:  // TOUCH_LP_DICTIONARY (0)
      return TouchLongPressRoute::Dictionary;
  }
}
