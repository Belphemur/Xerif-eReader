#include "SdCardFontSystem.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <IndicScripts.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

#include "CrossPointSettings.h"
#include "ReaderFontSizes.h"
#include "fontIds.h"

namespace {

// Point the reader font size at a size the given family actually ships, and
// persist the change so the settings UI and the loaded font never disagree.
// Guarded by the value-change check: a no-op snap must not write SPIFFS.
// cpfont-path-only: unused on native-TTF boards (see ensureLoaded).
#if !defined(CROSSPOINT_TTF_READER)
void snapFontPointSizeTo(const uint8_t availablePointSize) {
  if (availablePointSize == 0 || availablePointSize == SETTINGS.fontPointSize) return;
  LOG_DBG("SDFS", "Font size %u unavailable, snapping to %u", SETTINGS.fontPointSize, availablePointSize);
  SETTINGS.fontPointSize = availablePointSize;
  SETTINGS.saveToFile();
}
#endif  // !CROSSPOINT_TTF_READER

// Built-in UI fonts and their physical point sizes (at 150 DPI, matching the
// SD-font converter) are shared with the TTF UI fallback via kUiFontSizes in
// SdCardFontSystem.h.

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer, const EpdFontFamily& small, const EpdFontFamily& ui10,
                             const EpdFontFamily& ui12) {
  builtinUiFonts_[0] = &small;
  builtinUiFonts_[1] = &ui10;
  builtinUiFonts_[2] = &ui12;
  registry_.discover();
  renderer_ = &renderer;
  // Sized-variant provider on every board: the legacy bitmap reader path
  // (SETTINGS.readerFontEngine == READER_ENGINE_BITMAP — the documented
  // rollback engine on TTF boards) resolves headings/<small>/<big>/drop-cap
  // sizes through it (ChapterHtmlSlimParser::fontIdForScale). On TTF boards
  // resolveVariant serves only the built-in family's registered sizes; the
  // cpfont half is compiled out and `load` stays null (built-in variants are
  // always-registered fonts, never evicted, so re-registration is impossible
  // by construction).
  FontVariantProvider provider;
  provider.ctx = this;
  provider.resolve = [](void* ctx, const int fontId, const float scale) {
    return static_cast<SdCardFontSystem*>(ctx)->resolveVariant(fontId, scale);
  };
#if !defined(CROSSPOINT_TTF_READER)
  provider.load = [](void* ctx, const int fontId) { return static_cast<SdCardFontSystem*>(ctx)->loadVariant(fontId); };
#endif
  renderer.setFontVariantProvider(provider);

#if !defined(CROSSPOINT_TTF_READER)
  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t pointSize) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, pointSize);
  };
  SETTINGS.sdFontResolverCtx = this;
#endif  // !CROSSPOINT_TTF_READER

  // If user has a saved SD font selection, load it
#if !defined(CROSSPOINT_TTF_READER)
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (manager_.loadFamily(*family, renderer, SETTINGS.fontPointSize)) {
        snapFontPointSizeTo(manager_.currentPointSize());
        LOG_DBG("SDFS", "Loaded SD card font family: %s", SETTINGS.sdFontFamilyName);
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.sdFontFamilyName);
        SETTINGS.clearSdFontFamily();
      }
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
      SETTINGS.clearSdFontFamily();
    }
  }
#endif  // !CROSSPOINT_TTF_READER

  // The optional UI faces must fit alongside the saved reader font.
  applyUiFont(renderer, SETTINGS.uiFontFamilyName);
  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::refreshIfDirty() {
  if (registryDirty_.exchange(false, std::memory_order_acquire)) refreshRegistry();
}

void SdCardFontSystem::refreshRegistry() { registry_.discover(); }

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer, [[maybe_unused]] const bool retryUnavailable) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

#if !defined(CROSSPOINT_TTF_READER)
  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      unloadCpfonts(renderer);
    }
    // Back on a built-in family, which exists only at BUILTIN_READER_POINT_SIZES:
    // a size inherited from an SD family has to come back into that set.
    snapFontPointSizeTo(snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES),
                                               SETTINGS.fontPointSize));
    return;
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      unloadCpfonts(renderer);
      SETTINGS.clearSdFontFamily();
      return;
    }
    const auto* selected = family->findNearestSize(SETTINGS.fontPointSize);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    // Snap before the early return: the wanted size can already be loaded while
    // the setting still names a size this family does not ship.
    snapFontPointSizeTo(wantedPt);
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    unloadCpfonts(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, SETTINGS.fontPointSize)) {
      snapFontPointSizeTo(manager_.currentPointSize());
      setupUiFallbacks(renderer);
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.clearSdFontFamily();
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.clearSdFontFamily();
  }
#endif  // !CROSSPOINT_TTF_READER
}

void SdCardFontSystem::setupUiFallbacks(GfxRenderer& renderer) {
  // cpfont-path-only: on native-TTF boards no SD family is ever loaded, so the
  // UI fallback registration is dead and the TTF UI fallback covers CJK UI.
#if !defined(CROSSPOINT_TTF_READER)
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty() || activeUiFontName_[0] != '\0') return;

  const auto* family = registry_.findFamily(familyName);
  if (!family) return;

  // Probe the already-loaded reader-size font before paying for the UI sizes:
  // resolveTextFontId only redirects on codepoints the built-in UI fonts lack,
  // so a family with no coverage beyond theirs can never act as a fallback and
  // its UI sizes would be dead weight in RAM.
  const auto readerIt = renderer.getFontMap().find(manager_.getFontId(familyName));
  if (readerIt == renderer.getFontMap().end()) return;
  // One representative codepoint per script the built-in fonts may lack,
  // matching the non-Latin interval presets the SD-font converter ships
  // (docs/sd-card-fonts.md).
  static constexpr uint32_t kFallbackProbes[] = {
      0x4E00,  // CJK Unified Ideographs (Han)
      0x3042,  // Hiragana
      0x30A2,  // Katakana
      0xAC00,  // Hangul Syllables
      0x03B1,  // Greek
      0x0430,  // Cyrillic
      0x05D0,  // Hebrew
      0x0627,  // Arabic
      0x0531,  // Armenian
      0x10D0,  // Georgian
      0x1200,  // Ethiopic
      0x13A0,  // Cherokee
      0x2D30,  // Tifinagh
      0x0259,  // IPA Extensions (ə)
  };
  bool hasFallbackScript = false;
  for (const uint32_t cp : kFallbackProbes) {
    if (readerIt->second.hasCodepoint(cp)) {
      hasFallbackScript = true;
      break;
    }
  }
  // Every Indic script too.
  hasFallbackScript = hasFallbackScript ||
                      std::any_of(std::begin(indic::SCRIPTS), std::end(indic::SCRIPTS),
                                  [&](const indic::ScriptInfo& s) { return readerIt->second.hasCodepoint(s.probe); });
  if (!hasFallbackScript) {
    LOG_DBG("SDFS", "%s has no fallback-script coverage - skipping UI fallback sizes", familyName.c_str());
    return;
  }

  for (size_t i = 0; i < std::size(kUiFontSizes); ++i) {
    const auto& ui = kUiFontSizes[i];
    // Use the persisted CSS variant id so UI and reader text share one prewarm set.
    const int id = variantFontId(manager_.getFontId(familyName), ui.pointSize);
    const int sdFontId = manager_.loadFamilyExtraSize(*family, renderer, ui.pointSize, id);
    if (sdFontId != 0) {
      cpfontUiIds_[i] = sdFontId;
      renderer.setFallbackFont(ui.fontId, sdFontId);
    } else {
      LOG_DBG("SDFS", "No %u pt SD glyphs for UI fallback in %s", ui.pointSize, familyName.c_str());
    }
  }
#endif  // !CROSSPOINT_TTF_READER
}

// Sized variants of the reader font (CSS font-size, headings, drop caps).
// Live on every board: the built-in family's sizes are registered even on
// native-TTF boards, where the legacy bitmap rollback engine still walks this
// provider. The cpfont fallback below is compiled out there.

// Custom UI font binding (upstream #4563, cpfont path): built-ins are
// restored through the EpdFontFamily handles begin() captured, so the vector
// engine that upstream pairs this with stays out (the SDK FreeInkBook engine
// owns native-TTF rendering on the fork).
void SdCardFontSystem::releaseForStorage(GfxRenderer& renderer) {
#if !defined(CROSSPOINT_TTF_READER)
  unloadCpfonts(renderer);
#endif
  applyUiFont(renderer, "");
}

// cpfont-path-only: the SD manager's load/unload machinery is compiled out on
// native-TTF boards (SdCardFontManager.cpp), so the UI cpfont binding runs only
// where that stack exists. TTF boards keep built-in UI fonts + TtfUiFallback.
#if !defined(CROSSPOINT_TTF_READER)
void SdCardFontSystem::unloadCpfonts(GfxRenderer& renderer) {
  renderer.clearFallbackFonts();
  std::fill(std::begin(cpfontUiIds_), std::end(cpfontUiIds_), 0);
  cpfontVariants_.clear();  // the manager owns and frees the fonts themselves
  manager_.unloadAll(renderer);
}

void SdCardFontSystem::clearCpfontUiFallbacks(GfxRenderer& renderer) {
  const int readerId = manager_.getFontId(manager_.currentFamilyName());
  for (int& id : cpfontUiIds_) {
    if (id != 0 && id != readerId) {
      std::erase_if(cpfontVariants_, [id](const CpfontVariant& variant) { return variant.fontId == id; });
      manager_.unloadFont(renderer, id);
    }
    id = 0;
  }
}
#endif  // !CROSSPOINT_TTF_READER

const char* SdCardFontSystem::activeUiFontName() const { return activeUiFontName_; }

bool SdCardFontSystem::supportsUiFont(const SdCardFontFamilyInfo& family) {
  return std::all_of(std::begin(kUiFontSizes), std::end(kUiFontSizes),
                     [&](const UiFontSize& ui) { return family.hasSize(ui.pointSize); });
}

#if !defined(CROSSPOINT_TTF_READER)
bool SdCardFontSystem::loadUiCpfonts(const SdCardFontFamilyInfo& family, GfxRenderer& renderer) {
  static constexpr size_t kMinInternalFree = 50 * 1024;
  for (size_t i = 0; i < std::size(kUiFontSizes); ++i) {
    const auto& ui = kUiFontSizes[i];
    renderer.removeFont(ui.fontId);
    if (!uiCpfonts_.loadFamilyVariant(family, renderer, ui.pointSize, ui.fontId, builtinUiFonts_[i])) return false;
    if (HalMemory::getInternalHeap().freeBytes < kMinInternalFree) {
      LOG_ERR("SDFS", "Not enough internal heap for the UI bitmap font");
      return false;
    }
  }
  return true;
}

#endif  // !CROSSPOINT_TTF_READER

bool SdCardFontSystem::applyUiFont(GfxRenderer& renderer, const char* want) {
#if defined(CROSSPOINT_TTF_READER)
  // Native-TTF boards have no SD cpfont stack: nothing can bind, so only the
  // "Default" (empty) selection succeeds — a real name fails honestly.
  (void)renderer;
  return *want == '\0';
#else
  uiCpfonts_.unloadAll(renderer);
  activeUiFontName_[0] = '\0';
  if (*want) {
    const auto* family = registry_.findFamily(want);
    if (family && supportsUiFont(*family)) {
      // An explicit UI family uses the built-ins for missing glyphs. Release
      // reader-based fallbacks first to make room for the new font.
      renderer.clearFallbackFonts();
      clearCpfontUiFallbacks(renderer);
      const bool loaded = loadUiCpfonts(*family, renderer);
      if (loaded) {
        snprintf(activeUiFontName_, sizeof(activeUiFontName_), "%s", want);
        LOG_DBG("SDFS", "UI font: %s", want);
        return true;
      }
    }
    LOG_ERR("SDFS", "UI font '%s' missing or failed to load; using built-in", want);
    uiCpfonts_.unloadAll(renderer);
  }
  restoreBuiltinUiFonts(renderer);
  setupUiFallbacks(renderer);
  return *want == '\0';
#endif  // CROSSPOINT_TTF_READER
}

void SdCardFontSystem::restoreBuiltinUiFonts(GfxRenderer& renderer) {
  for (size_t i = 0; i < std::size(kUiFontSizes); ++i) {
    renderer.removeFont(kUiFontSizes[i].fontId);
    renderer.insertFont(kUiFontSizes[i].fontId, *builtinUiFonts_[i]);
  }
}

int SdCardFontSystem::resolveVariant(const int fontId, const float scale) {
  if (const int builtin = builtinReaderFontVariant(fontId, scale); builtin != 0) {
    return renderer_->getFontMap().count(builtin) ? builtin : 0;
  }
#if !defined(CROSSPOINT_TTF_READER)
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty() || fontId != manager_.getFontId(familyName)) return 0;
  const auto* family = registry_.findFamily(familyName);
  if (!family) return 0;

  // Pick among the sizes installed for the family: shrinking takes the largest size at or
  // below the target so small print never rounds back up to body size; growing takes the
  // nearest.
  const uint8_t base = manager_.currentPointSize();
  const float target = base * scale;
  uint8_t chosen = base;
  if (scale < 1.0f) {
    uint8_t atOrBelow = 0;
    uint8_t smallest = base;
    for (const auto& file : family->files) {
      if (file.pointSize <= target && file.pointSize > atOrBelow) atOrBelow = file.pointSize;
      smallest = std::min(smallest, file.pointSize);
    }
    chosen = atOrBelow != 0 ? atOrBelow : smallest;
  } else {
    for (const auto& file : family->files) {
      if (std::fabs(file.pointSize - target) < std::fabs(chosen - target)) chosen = file.pointSize;
    }
  }
  if (chosen == base) return fontId;
  const int id = variantFontId(fontId, chosen);
  return loadCpfontVariant(*family, chosen, id) ? id : 0;
#endif  // !CROSSPOINT_TTF_READER
  return 0;
}

// cpfont-path-only: variant ids, sized variants, and unload paths only ever
// run for a registered SD reader font, which native-TTF boards never have.
#if !defined(CROSSPOINT_TTF_READER)
int SdCardFontSystem::variantFontId(const int readerFontId, const uint8_t pointSize) {
  uint32_t hash = 2166136261u;
  for (int shift = 0; shift < 32; shift += 8) {
    hash ^= static_cast<uint8_t>(static_cast<uint32_t>(readerFontId) >> shift);
    hash *= 16777619u;
  }
  hash ^= pointSize;
  hash *= 16777619u;
  hash ^= 0x56415200u;  // "VAR\0" salt keeps variants apart from reader and UI ids
  const int id = static_cast<int>(hash);
  return id != 0 ? id : 1;
}

bool SdCardFontSystem::loadVariant(const int fontId) {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return false;
  const auto* family = registry_.findFamily(familyName);
  if (!family) return false;
  const int readerFontId = manager_.getFontId(familyName);
  for (const auto& file : family->files) {
    if (variantFontId(readerFontId, file.pointSize) == fontId) {
      return loadCpfontVariant(*family, file.pointSize, fontId);
    }
  }
  return false;
}

bool SdCardFontSystem::loadCpfontVariant(const SdCardFontFamilyInfo& family, const uint8_t pointSize,
                                         const int fontId) {
  for (auto& variant : cpfontVariants_) {
    if (variant.fontId == fontId) {
      variant.lastUse = ++variantClock_;
      return true;
    }
  }
  // UI fallback sizes stay resident and must not enter the variant eviction list.
  if (manager_.getFontId(family.name, pointSize) == fontId) return true;
  // Each size keeps its interval tables resident (small for Latin, tens of KB for broad
  // CJK subsets), so skip it on a tight heap: the text then stays at the reader size.
  static constexpr size_t MIN_VARIANT_FREE_BLOCK = 48 * 1024;
  if (ESP.getMaxAllocHeap() < MIN_VARIANT_FREE_BLOCK) {
    LOG_DBG("SDFS", "Skipping %upt variant: largest free block %u", pointSize,
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return false;
  }
  if (cpfontVariants_.size() >= MAX_CPFONT_VARIANTS) {
    const auto oldest = std::min_element(cpfontVariants_.begin(), cpfontVariants_.end(),
                                         [](const auto& a, const auto& b) { return a.lastUse < b.lastUse; });
    manager_.unloadFont(*renderer_, oldest->fontId);
    cpfontVariants_.erase(oldest);
  }
  if (!manager_.loadFamilyVariant(family, *renderer_, pointSize, fontId)) return false;
  cpfontVariants_.push_back({fontId, ++variantClock_});
  LOG_DBG("SDFS", "Loaded %s %upt variant (heap free %u)", family.name.c_str(), pointSize,
          static_cast<unsigned>(ESP.getFreeHeap()));
  return true;
}
#endif  // !CROSSPOINT_TTF_READER

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*pointSize*/) const {
#if !defined(CROSSPOINT_TTF_READER)
  // The manager holds exactly one reader-size font, already selected for
  // SETTINGS.fontPointSize, so the size argument is implicit — always return
  // that font's ID. ensureLoaded() must have run for the current settings first.
  return manager_.getFontId(familyName);
#else
  // No SD reader font on TTF boards; begin() never wires this resolver.
  (void)familyName;
  return 0;
#endif
}
