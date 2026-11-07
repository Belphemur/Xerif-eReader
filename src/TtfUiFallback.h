#pragma once

// TtfUiFallback — TTF-backed CJK/script UI fallback (design §14.6, ported
// from upstream #3646's setupTtfUiFallbacks): when the ACTIVE native-TTF
// reader family covers scripts the built-in bitmap UI fonts lack, register
// size-free EpdFontFamily views of that family (TtfUiFont) as the fallback
// for the built-in UI font ids through the EXISTING GfxRenderer fallback
// plumbing (setFallbackFont/resolveTextFontId). The reader's already-resident
// font bytes are borrowed — no copies of font bytes (DRY). Glyphs fault on
// demand through the EpdFontData miss seam; there is no SD prewarm to pay.
//
// Heap-gated: skipped when PSRAM cannot fund the FreeType faces + glyph
// rings (the built-in bitmap fonts keep covering Latin UI either way).

#include <BookFontLoader.h>

class GfxRenderer;

#if CROSSPOINT_TTF_UI_FALLBACK

#include <cstdint>

#include "adapters/TtfUiFont.h"

namespace freeink {
namespace book {

class TtfUiFallback {
 public:
  // Re-sync the registrations with the loader's current state. Idempotent and
  // cheap when nothing changed (fingerprint fast path). Call from the same
  // places the SD-font fallbacks are kept fresh (boot, settings screens,
  // reader entry).
  void update(GfxRenderer& renderer);

  // Unregister every TTF UI fallback font. Must run BEFORE the loader drops
  // the borrowed bytes (family change / releaseResidentCaches / shutdown).
  void release(GfxRenderer& renderer);

  bool active() const { return registeredCount_ > 0; }

 private:
  // One instance per built-in UI size (kUiFontSizes in SdCardFontSystem.h).
  static constexpr size_t kUiSizeCount = 3;

  TtfUiFont instances_[kUiSizeCount];
  int registeredIds_[kUiSizeCount] = {};
  int primaryIds_[kUiSizeCount] = {};
  uint8_t registeredCount_ = 0;
  uint32_t registeredFingerprint_ = 0;  // loader fingerprint the registrations borrow
  // Loader fingerprint whose fallback-script probe FAILED. Negative result
  // cache for update(): without it a no-coverage family re-probed and
  // re-logged on every redraw (TtfUiFallback.cpp negative-cache comment).
  // Keyed on the fingerprint, so a font swap invalidates it for free; NOT
  // reset in release() — release runs on the empty path every update call.
  uint32_t noCoverageFingerprint_ = 0;
  // Borrowed byte owners per style slot, captured at registration. The
  // fingerprint does NOT identify buffer addresses: an ensureLoaded() reload
  // with identical content re-creates the byte owners (possibly at different
  // pool addresses) while the fingerprint stays equal, so the fast path must
  // verify the borrowed pointers are still the loader's CURRENT ones.
  const void* borrowedSnapshot_[4] = {};
};

// Global instance (one inline variable for every build class — the #else
// branch defines the matching no-op singleton).
inline TtfUiFallback ttfUiFallback;

}  // namespace book
}  // namespace freeink

#else

// Non-TTF builds: wiring sites keep compiling with a stateless no-op. The
// inline global keeps one symbol across all build classes.
namespace freeink {
namespace book {

class TtfUiFallback {
 public:
  // Static bodies: the no-op never touches members (cppcheck functionStatic).
  static void update(GfxRenderer& /*renderer*/) {}
  static void release(GfxRenderer& /*renderer*/) {}
  static bool active() { return false; }
};
inline TtfUiFallback ttfUiFallback;

}  // namespace book
}  // namespace freeink

#endif  // CROSSPOINT_TTF_UI_FALLBACK