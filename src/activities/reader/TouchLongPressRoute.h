#pragma once

#include <cstdint>

#include "TouchLongPressMode.h"

// Routing decision for a long press on the reading surface. The single
// SETTINGS.touchLongPressAction value selects exactly one consumer in
// EpubReaderActivity::loop; only the matching gate calls
// mappedInput.wasScreenLongPress(), so the other consumers are structurally
// unable to consume the event (upstream's clip gate used to shadow the
// dictionary branch, v2.6.0-v2.6.1).
enum class TouchLongPressRoute : uint8_t { Dictionary, Footnote, ClipSelection };

// Numeric mapping mirrors CrossPointSettings::TOUCH_LONG_PRESS_ACTION,
// persisted by menu-position index and APPEND ONLY (CrossPointSettings.h).
// Duplicated as literals because CrossPointSettings.h is not host-testable
// (ArduinoJson); EpubReaderActivity.cpp static_asserts the correspondence.
// Appended values must update this switch — the default falls back to
// Dictionary, the fork's historical default action.
constexpr TouchLongPressRoute resolveTouchLongPressRoute(uint8_t touchLongPressAction) {
  switch (touchLongPressAction) {
    case 2:  // TOUCH_LP_FOOTNOTE
      return TouchLongPressRoute::Footnote;
    case 1:  // TOUCH_LP_IGNORE (upstream's clip/highlight selection behavior)
      return TouchLongPressRoute::ClipSelection;
    default:  // TOUCH_LP_DICTIONARY (0)
      return TouchLongPressRoute::Dictionary;
  }
}
