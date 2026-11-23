#pragma once

#include <Epub/blocks/TextBlock.h>  // MAX_WORD_FONTS caps the variant caches
#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

#include <atomic>
#include <cstdint>
#include <vector>

#include "fontIds.h"

class GfxRenderer;
class EpdFontFamily;

/// One built-in UI font paired with the point size of the SD/TTF fallback
/// that covers scripts the built-in lacks (CJK, Greek, Cyrillic, ...).
struct UiFontSize {
  int fontId;
  uint8_t pointSize;
};

/// The built-in UI fonts that render UI chrome text. Each is paired with a
/// same-size script fallback (SD .cpfont via SdCardFontSystem::setupUiFallbacks,
/// or the active TTF family via TtfUiFallback) so UI text in scripts the
/// built-ins lack matches the surrounding Latin.
inline constexpr UiFontSize kUiFontSizes[] = {
    {SMALL_FONT_ID, 8},
    {UI_10_FONT_ID, 10},
    {UI_12_FONT_ID, 12},
};

/// Facade that owns the SD card font registry, manager, and resolver logic.
/// Hides implementation details behind a single begin() + ensureLoaded() API.
class SdCardFontSystem {
 public:
  SdCardFontSystem() = default;
  SdCardFontSystem(const SdCardFontSystem&) = delete;
  SdCardFontSystem& operator=(const SdCardFontSystem&) = delete;
  /// Discover SD card fonts and load user's saved selection. Call once during setup.
  void begin(GfxRenderer& renderer, const EpdFontFamily& small, const EpdFontFamily& ui10, const EpdFontFamily& ui12);

  /// Ensure the correct SD font family is loaded for the current settings.
  /// Call before entering the reader or after settings change.
  /// Also re-discovers if the registry has been marked dirty (e.g. by web upload).
  void ensureLoaded(GfxRenderer& renderer);

  static bool supportsUiFont(const SdCardFontFamilyInfo& family);

  /// Bind the UI font to an installed family or the built-ins (empty name).
  /// Returns false on failure, with the built-ins restored. The caller holds
  /// RenderLock after startup.
  bool applyUiFont(GfxRenderer& renderer, const char* name);
  const char* activeUiFontName() const;
#if CROSSPOINT_VECTOR_FONTS
  bool uiTtfFailed() const { return uiTtfFailed_.load(std::memory_order_relaxed); }
  void recoverUiFont(GfxRenderer& renderer);
#endif

  /// Release SD font handles and restore built-ins before storage leaves firmware control.
  /// The caller holds RenderLock.
  void releaseForStorage(GfxRenderer& renderer);

  /// Resolve an SD card font ID from family name + reader point size.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t pointSize) const;

  /// Non-const access to the registry (for FontInstaller).
  SdCardFontRegistry& registry() { return registry_; }

  /// Mark the registry as needing re-discovery.
  /// Thread-safe: can be called from the web server task.
  void markRegistryDirty() { registryDirty_.store(true, std::memory_order_release); }

  /// If the registry is dirty, re-scan the SD card now and clear the flag.
  /// Used by the web UI so uploaded/deleted fonts appear in the list
  /// without waiting for the reader activity to run ensureLoaded().
  void refreshIfDirty() {
    if (registryDirty_.exchange(false, std::memory_order_acquire)) {
      registry_.discover();
    }
  }

 private:
  bool loadUiCpfonts(const SdCardFontFamilyInfo& family, GfxRenderer& renderer);
  void restoreBuiltinUiFonts(GfxRenderer& renderer);
  void clearCpfontUiFallbacks(GfxRenderer& renderer);

  // Load the active SD family at the built-in UI point sizes and register each
  // as a size-matched script fallback for the corresponding UI font, so book
  // titles/list rows in scripts the built-ins lack (CJK, Greek, Cyrillic, ...)
  // render at the same size as the surrounding Latin UI text. No-op when no SD
  // family is loaded. Safe to call repeatedly (sizes already loaded are
  // reused).
  void setupUiFallbacks(GfxRenderer& renderer);

  // --- Sized variants of the reader font (CSS font-size, headings, drop caps) ---
  // Served to the renderer through FontVariantProvider. Variant ids derive from the
  // reader font id and point size, so ids cached in section files can be reloaded after
  // eviction or a reboot. Least recently used variants are evicted at the caps below.
  int resolveVariant(int fontId, float scale);
  bool loadVariant(int fontId);
  bool loadCpfontVariant(const SdCardFontFamilyInfo& family, uint8_t pointSize, int fontId);
  void unloadCpfonts(GfxRenderer& renderer);
  static int variantFontId(int readerFontId, uint8_t pointSize);

  struct CpfontVariant {
    int fontId;
    uint32_t lastUse;
  };
  static constexpr size_t MAX_CPFONT_VARIANTS = TextBlock::MAX_WORD_FONTS + 2;  // Inline, block, and drop-cap fonts.
  std::vector<CpfontVariant> cpfontVariants_;
  uint32_t variantClock_ = 0;
  GfxRenderer* renderer_ = nullptr;

  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  SdCardFontManager uiCpfonts_;
  int cpfontUiIds_[3] = {};
  const EpdFontFamily* builtinUiFonts_[3] = {};
  char activeUiFontName_[SdCardFontRegistry::FAMILY_NAME_CAPACITY] = "";
  std::atomic<bool> registryDirty_{false};
};

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;
