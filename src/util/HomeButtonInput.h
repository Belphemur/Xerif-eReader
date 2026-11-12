#pragma once

#include <cstdint>

// Persisted indices: append actions without reordering existing values.
// Values 0..10 come from upstream; 11..13 are fork extensions.
enum class HomeButtonAction : uint8_t {
  Home,
  Ignore,
  NextPage,
  Refresh,
  Footnotes,
  Confirm,
  Sync,
  Bookmark,
  Dictionary,
  ReaderMenu,
  ToggleFrontlight,
  Sleep,
  Screenshot,
  GoBack,
  CreateClipping,
  FileBrowser,
  Count
};

// Which classifier transition produced the action. The gesture is needed by
// the fork invariant that a single tap (but not a double tap or hold) is inert
// on the device home screen.
enum class HomeButtonGesture : uint8_t { None, Tap, DoubleTap, Hold };

class HomeButtonInput {
 public:
  static constexpr uint32_t DOUBLE_TAP_MS = 350;

  HomeButtonAction update(uint32_t now, bool tapped, bool held, bool swiped, bool pressed, HomeButtonAction tap,
                          HomeButtonAction doubleTap, HomeButtonAction longPress) {
    gesture = HomeButtonGesture::None;
    if (swiped) {
      reset();
      return HomeButtonAction::Ignore;
    }
    if (held) {
      reset();
      gesture = HomeButtonGesture::Hold;
      return longPress;
    }
    if (pending && !secondContact && now - tappedAt > DOUBLE_TAP_MS) {
      pending = tapped;
      tappedAt = now;
      gesture = HomeButtonGesture::Tap;
      return tap;
    }
    if (pending && pressed) secondContact = true;
    if (!tapped) return HomeButtonAction::Ignore;
    if (doubleTap == HomeButtonAction::Ignore) {
      reset();
      gesture = HomeButtonGesture::Tap;
      return tap;
    }
    if (pending) {
      reset();
      gesture = HomeButtonGesture::DoubleTap;
      return doubleTap;
    }
    pending = true;
    tappedAt = now;
    return HomeButtonAction::Ignore;
  }

  void reset() {
    pending = false;
    secondContact = false;
  }

  HomeButtonGesture lastGesture() const { return gesture; }

 private:
  uint32_t tappedAt = 0;
  bool pending = false;
  bool secondContact = false;
  HomeButtonGesture gesture = HomeButtonGesture::None;
};
