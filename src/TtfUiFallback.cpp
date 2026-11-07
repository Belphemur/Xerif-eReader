// TtfUiFallback implementation — see TtfUiFallback.h for the design contract.

#include "TtfUiFallback.h"

#if CROSSPOINT_TTF_UI_FALLBACK

#include <GfxRenderer.h>
#include <HalMemory.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"

namespace freeink {
namespace book {
namespace {

// PSRAM floor for funding the feature: three instances (lazy faces + 16-slot
// 1bpp rings each). Faces allocate PSRAM-first via FontAlloc; below this
// largest-block the allocation cascade would evict reader working set.
constexpr size_t kMinPsramLargestBlock = 256u * 1024u;

// One representative codepoint per script the built-in UI fonts may lack
// (same probe set as the SD-font UI fallbacks): Han, Hiragana, Katakana,
// Hangul, Greek, Cyrillic, Hebrew, Arabic, Thai, Devanagari.
constexpr uint32_t kFallbackProbes[] = {0x4E00, 0x3042, 0x30A2, 0xAC00, 0x03B1, 0x0430, 0x05D0, 0x0627, 0x0E01, 0x0905};

bool familyCoversFallbackScripts(FontChain& chain) {
  for (const uint32_t cp : kFallbackProbes) {
    if (chain.covers(cp)) return true;
  }
  return false;
}

// kUiFontSizes pointSize is a physical POINT size at 150 DPI (the SD-font
// converter convention the UI font ids share). FtFont sizes are PIXELS, so
// convert — otherwise the fallback CJK/Greek/Cyrillic glyphs render at about
// half the height of the surrounding built-in Latin text.
constexpr uint16_t pointToPx(const uint8_t pointSize) {
  // pt * 150 DPI / 72, rounded to nearest.
  return static_cast<uint16_t>((pointSize * 150u + 36u) / 72u);
}

}  // namespace

void TtfUiFallback::update(GfxRenderer& renderer) {
  // Engine gate: TTF UI fallback only exists on the native-TTF path with a
  // selected family. The built-in bitmap fonts stay the UI font otherwise.
  if (SETTINGS.readerFontEngine != CrossPointSettings::READER_ENGINE_TTF || SETTINGS.ttfFontFamilyName[0] == '\0') {
    release(renderer);
    return;
  }
  // Release BEFORE any reload: a dirty loader frees the borrowed bytes on
  // ensureLoaded(), which must not happen while live faces still reference
  // them. The fingerprint fast path below only covers reload-free calls.
  if (registeredCount_ > 0 && fontLoader.isDirty()) {
    release(renderer);
  }
  fontLoader.ensureLoaded();
  const uint32_t fingerprint = fontLoader.fontFingerprint();
  if (fingerprint == 0) {
    release(renderer);  // fallback chain active — nothing TTF to lend
    return;
  }
  if (registeredCount_ > 0 && registeredFingerprint_ == fingerprint) {
    // Buffer-address guard: an ensureLoaded() reload with identical content
    // keeps the fingerprint but re-creates the byte owners — stale faces
    // would point at freed memory. Release + re-register on any move.
    bool borrowed = true;
    for (uint8_t s = 0; s < 4; ++s) {
      if (fontLoader.slotFaceBytes(s) != borrowedSnapshot_[s]) {
        borrowed = false;
        break;
      }
    }
    if (borrowed) return;
    release(renderer);
  } else {
    release(renderer);
  }

  // Negative cache: this fingerprint already failed the fallback-script probe.
  // Without it the probe re-ran (and logged) on EVERY redraw — hundreds of
  // TTFUI DBG lines per reader session and a wasted chain walk each time.
  if (noCoverageFingerprint_ == fingerprint) {
    return;
  }

  // Heap gate: FreeType faces + glyph rings need PSRAM headroom.
  const HalMemory::HeapStats psram = HalMemory::getPsramHeap();
  if (psram.totalBytes == 0 || psram.largestBlockBytes < kMinPsramLargestBlock) {
    LOG_DBG("TTFUI", "PSRAM too tight for UI fallback (largest %zu)", psram.largestBlockBytes);
    return;
  }

  // Script gate: probe against the LOADED reader chain (no new faces) — a
  // family whose coverage matches the built-ins can never act as a fallback
  // and its UI faces would be dead weight.
  FontChain* chain = fontLoader.getReaderFont();
  if (chain == nullptr || !familyCoversFallbackScripts(*chain)) {
    LOG_DBG("TTFUI", "Family '%s' has no fallback-script coverage", SETTINGS.ttfFontFamilyName);
    noCoverageFingerprint_ = fingerprint;  // cache the failed probe for this content
    return;
  }

  for (size_t i = 0; i < kUiSizeCount; ++i) {
    const UiFontSize& ui = kUiFontSizes[i];
    const int ttfId = computeTtfUiFontId(SETTINGS.ttfFontFamilyName, ui.pointSize);
    if (renderer.getFontMap().count(ttfId) != 0) {
      LOG_ERR("TTFUI", "Font id %d collision — skipping UI size %u", ttfId, static_cast<unsigned>(ui.pointSize));
      continue;
    }
    const void* bytes[4] = {};
    uint32_t sizes[4] = {};
    uint8_t faceIndices[4] = {};
    bool any = false;
    for (uint8_t s = 0; s < 4; ++s) {
      bytes[s] = fontLoader.slotFaceBytes(s);
      sizes[s] = fontLoader.slotFaceByteSize(s);
      faceIndices[s] = fontLoader.slotFaceIndex(s);
      any = any || bytes[s] != nullptr;
    }
    if (!any) {
      LOG_DBG("TTFUI", "Family '%s' has no resident bytes (streamed?) — no UI fallback", SETTINGS.ttfFontFamilyName);
      return;
    }
    if (!instances_[i].begin(bytes, sizes, faceIndices, pointToPx(ui.pointSize))) {
      LOG_DBG("TTFUI", "UI size %u not available in '%s'", static_cast<unsigned>(ui.pointSize),
              SETTINGS.ttfFontFamilyName);
      continue;
    }
    renderer.insertFont(ttfId, instances_[i].family());
    renderer.setFallbackFont(ui.fontId, ttfId);
    registeredIds_[registeredCount_] = ttfId;
    primaryIds_[registeredCount_] = ui.fontId;
    ++registeredCount_;
    LOG_DBG("TTFUI", "UI fallback %s @%upt -> id %d", SETTINGS.ttfFontFamilyName, static_cast<unsigned>(ui.pointSize),
            ttfId);
  }
  registeredFingerprint_ = fingerprint;
  noCoverageFingerprint_ = 0;  // the family now registered — clear any stale negative cache
  for (uint8_t s = 0; s < 4; ++s) borrowedSnapshot_[s] = fontLoader.slotFaceBytes(s);
  LOG_INF("TTFUI", "%u TTF UI fallback size(s) registered", static_cast<unsigned>(registeredCount_));
}

void TtfUiFallback::release(GfxRenderer& renderer) {
  for (uint8_t i = 0; i < registeredCount_; ++i) {
    renderer.clearFallbackFont(primaryIds_[i]);
    renderer.removeFont(registeredIds_[i]);
  }
  // Faces release AFTER the font map entries are gone — no draw path can
  // reach the borrowed views while they are torn down. End EVERY instance,
  // not just the first registeredCount_: a size whose begin() failed leaves
  // no registration, but an instance at a later index may still hold live
  // faces borrowing loader bytes.
  for (size_t i = 0; i < kUiSizeCount; ++i) {
    instances_[i].end();
  }
  if (registeredCount_ > 0) {
    LOG_DBG("TTFUI", "Released %u TTF UI fallback size(s)", static_cast<unsigned>(registeredCount_));
  }
  registeredCount_ = 0;
  registeredFingerprint_ = 0;
  for (auto& snap : borrowedSnapshot_) snap = nullptr;
}

}  // namespace book
}  // namespace freeink

#endif  // CROSSPOINT_TTF_UI_FALLBACK