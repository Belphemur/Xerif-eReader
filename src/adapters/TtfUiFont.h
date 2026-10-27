#pragma once

// TtfUiFont — EpdFontFamily view over the ACTIVE native-TTF family at one
// built-in UI size (design §14.6, ported from upstream #3646's
// setupTtfUiFallbacks). Lets GfxRenderer route UI strings in scripts the
// built-in bitmap UI fonts lack (CJK titles, list rows, menus) to the reader's
// TTF family through the EXISTING fallback plumbing
// (GfxRenderer::setFallbackFont / resolveTextFontId) — no new draw path.
//
// Every style stub borrows the loader's already-resident font bytes
// (BookFontLoader::slotFaceBytes) — no copies of font bytes (DRY). Per-style
// FreeType faces are created LAZILY on first use (a UI screen that never draws
// bold CJK never pays for a bold face) and carry their own RenderOptions with
// the monochrome target: the EpdFont pipeline draws 1bpp MSB-first bitmaps, and
// the adapter must never flip the reader chain faces' AA/Crisp mode (a
// Smooth/Crisp mismatch blanks the page).
//
// Glyphs fault on demand through the EpdFontData::glyphMissHandler seam (the
// same hook the SD-card fonts use): each miss rasterizes into a small ring of
// 1bpp slots; the returned EpdGlyph is valid until the next miss on that style.
//
// Whole unit is compile-gated to TTF builds with the FreeType backend; on
// PSRAM-less builds the loader never loads faces anyway (design §14.1).

#include <BookFontLoader.h>
#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <FtFont.h>
#include <Memory.h>

#include <cstddef>
#include <cstdint>

#if CROSSPOINT_TTF_UI_FALLBACK

namespace freeink {
namespace book {

using freeink::font::FtFont;
using freeink::font::GlyphBitmap;

class TtfUiFont {
 public:
  // 1bpp ring capacity: one UI screen's worth of distinct fallback glyphs.
  // 16 slots × up to a 32×32 box (12pt @150DPI ≈ 25px tall) ≈ 2.25KB per instance.
  static constexpr uint8_t kRingSlots = 16;
  static constexpr uint8_t kMaxGlyphBox = 32;
  // Rows of (width+7)/8 bytes, MSB-first — the EpdFont bitmap format.
  static constexpr uint16_t kSlotBytes = ((kMaxGlyphBox + 7) / 8) * kMaxGlyphBox;

  TtfUiFont() = default;
  ~TtfUiFont();
  TtfUiFont(const TtfUiFont&) = delete;
  TtfUiFont& operator=(const TtfUiFont&) = delete;

  // Bind to a loaded family: borrows per-style resident font bytes
  // (bytes[slot] for slots REGULAR/BOLD/ITALIC/BOLD_ITALIC; a null slot
  // resolves to the regular slot on use). `bytes` must outlive this
  // instance — release via end() BEFORE the loader releases its bytes.
  // Bind to a loaded family: borrows per-style resident font bytes
  // (bytes[slot] for slots REGULAR/BOLD/ITALIC/BOLD_ITALIC; a null slot
  // resolves to the regular slot on use). `bytes` must outlive this
  // instance — release via end() BEFORE the loader releases its bytes.
  // faceIndices[slot] selects the embedded face for collections (.ttc): the
  // same index the loader discovered for the reader, since index 0 can carry
  // different coverage.
  bool begin(const void* const bytes[4], const uint32_t byteSizes[4], const uint8_t faceIndices[4], uint16_t sizePx);

  // Release faces and ring. The EpdFontFamily view goes dead (nullptr data).
  void end();

  bool bound() const { return bound_; }
  const EpdFontFamily& family() const { return family_; }

 private:
  // Per-style slot: the EpdFontData stub + its lazy FreeType face.
  struct Slot {
    EpdFontData data = {};
    EpdFont font{nullptr};
    // RAII: the lazily created FreeType face is owned here, so no error path
    // in begin()/miss()/end() can leak it. Main-thread-only lifetime.
    std::unique_ptr<FtFont> face = nullptr;
    const void* bytes = nullptr;
    uint32_t byteSize = 0;
    uint8_t faceIndex = 0;   // .ttc embedded face (loader-discovered)
    bool faceTried = false;  // failed creation is not retried
    // Back-links for the static handlers (EpdFontData carries one void* ctx).
    TtfUiFont* owner = nullptr;
    uint8_t styleSlot = 0;
  };

  static const EpdGlyph* missThunk(void* ctx, uint32_t codepoint);
  static bool coverageThunk(void* ctx, uint32_t codepoint);
  const EpdGlyph* miss(Slot& slot, uint32_t codepoint);
  bool covered(Slot& slot, uint32_t codepoint);
  FtFont* ensureFace(Slot& slot);

  Slot slots_[4] = {};
  // EpdFontFamily's ctor takes the point size (upstream #4174); a TtfUiFont's
  // size is a runtime pixel size, so begin() re-binds the family with the
  // converted value once the slots exist.
  EpdFontFamily family_{0, nullptr, nullptr, nullptr, nullptr};
  uint16_t sizePx_ = 0;
  bool bound_ = false;

  // 1bpp glyph ring: kRingSlots slots of kSlotBytes. data->bitmap points at
  // the base; faulted glyphs set glyph->dataOffset = slot * kSlotBytes.
  PoolBytes ring_;
  std::unique_ptr<uint8_t[]> ringDram_;
  uint8_t ringCursor_ = 0;
  EpdGlyph ringGlyphs_[kRingSlots] = {};
};

// FNV-1a id for a TTF UI fallback registration (upstream's "\x01ui" salt
// pattern): family name + salt + pixel size. Distinct from reader and
// .cpfont ids by construction.
int computeTtfUiFontId(const char* familyName, uint16_t sizePx);

}  // namespace book
}  // namespace freeink

#endif  // CROSSPOINT_TTF_UI_FALLBACK