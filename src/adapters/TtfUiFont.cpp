// TtfUiFont implementation — see TtfUiFont.h for the design contract.

#include "TtfUiFont.h"

#if CROSSPOINT_TTF_UI_FALLBACK

#include <Logging.h>
#include <Memory.h>

#include <cmath>
#include <cstring>

namespace freeink {
namespace book {

namespace {

// Threshold the engine's 8-bit coverage into the EpdFont pipeline's 1bpp
// MSB-first, row-packed bitmap (rows padded to whole bytes).
void packMono1bpp(const GlyphBitmap& src, uint8_t* dst, size_t dstCap) {
  const uint16_t stride = (src.width + 7) / 8;
  memset(dst, 0, dstCap);
  for (uint16_t y = 0; y < src.height; ++y) {
    const uint8_t* row = src.pixels + static_cast<size_t>(y) * src.width;
    uint8_t* out = dst + static_cast<size_t>(y) * stride;
    for (uint16_t x = 0; x < src.width; ++x) {
      if (row[x] >= 0x80) out[x >> 3] |= static_cast<uint8_t>(0x80u >> (x & 7));
    }
  }
}

}  // namespace

TtfUiFont::~TtfUiFont() { end(); }

bool TtfUiFont::begin(const void* const bytes[4], const uint32_t byteSizes[4], const uint8_t faceIndices[4],
                      uint16_t sizePx) {
  end();
  if (sizePx == 0) return false;
  sizePx_ = sizePx;
  // Convert the runtime pixel size to the point size EpdFontFamily stores
  // (pixels at FONT_RASTER_DPI, upstream #4174).
  const uint16_t pointSize = static_cast<uint16_t>(lroundf(sizePx * 72.0f / FONT_RASTER_DPI));
  family_ = EpdFontFamily(pointSize, &slots_[0].font, &slots_[1].font, &slots_[2].font, &slots_[3].font);
  bool any = false;
  for (uint8_t s = 0; s < 4; ++s) {
    Slot& slot = slots_[s];
    slot = {};
    slot.font = EpdFont(&slot.data);
    slot.bytes = bytes[s];
    slot.byteSize = byteSizes[s];
    slot.faceIndex = faceIndices[s];
    slot.owner = this;
    slot.styleSlot = s;
    // Prefer the dedicated style face; fall back to the regular slot when the
    // family lacks that style (UI bold renders un-bolded rather than missing).
    if (slot.bytes == nullptr && s != 0) {
      slot.bytes = bytes[0];
      slot.byteSize = byteSizes[0];
      slot.faceIndex = faceIndices[0];
    }
    if (slot.bytes == nullptr || slot.byteSize == 0) continue;
    any = true;
  }
  if (!any) {
    LOG_DBG("TTFUI", "No resident bytes — UI fallback unavailable");
    return false;
  }

  ring_ = poolMakeBytes(static_cast<size_t>(kRingSlots) * kSlotBytes);
  if (!ring_) ringDram_ = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(kRingSlots) * kSlotBytes);
  uint8_t* ring = ring_ ? ring_.get() : ringDram_.get();
  if (ring == nullptr) {
    LOG_ERR("TTFUI", "OOM: glyph ring %u bytes", static_cast<unsigned>(kRingSlots) * kSlotBytes);
    return false;
  }
  memset(ring, 0, static_cast<size_t>(kRingSlots) * kSlotBytes);
  memset(ringGlyphs_, 0, sizeof(ringGlyphs_));
  ringCursor_ = 0;

  // The regular face is created EAGERLY: its metrics (ascender/descender/line
  // height) fill every style stub — drawText positions the baseline from the
  // resolved font's ascender, so a stub with zero metrics draws on the row's
  // top edge. Other styles stay lazy (created on first use).
  FtFont* regular = ensureFace(slots_[0]);
  if (regular == nullptr) {
    LOG_ERR("TTFUI", "UI regular face unavailable");
    return false;
  }
  FtFont::LineMetrics lm{};
  int16_t lineHeight = sizePx_;
  int16_t ascender = sizePx_ * 3 / 4;  // sane fallback if metrics are missing
  if (regular->lineMetrics26_6(static_cast<uint32_t>(sizePx_) * 64, lm)) {
    ascender = static_cast<int16_t>(lm.ascender26_6 / 64);
    // Baseline-to-baseline distance from the SDK's dedicated height field —
    // it includes the line gap, unlike the ascender-to-descender extent.
    lineHeight = static_cast<int16_t>(lm.height26_6 / 64);
  }
  if (lineHeight <= 0) lineHeight = sizePx_;

  for (uint8_t s = 0; s < 4; ++s) {
    Slot& slot = slots_[s];
    if (slot.bytes == nullptr) continue;
    slot.data.bitmap = ring;
    slot.data.glyph = nullptr;
    slot.data.intervals = nullptr;
    slot.data.intervalCount = 0;
    slot.data.advanceY = 0;
    slot.data.ascender = 0;  // filled from the face's metrics on first use
    slot.data.descender = 0;
    slot.data.is2Bit = false;
    slot.data.groups = nullptr;
    slot.data.groupCount = 0;
    slot.data.glyphToGroup = nullptr;
    slot.data.kernLeftClasses = nullptr;
    slot.data.kernRightClasses = nullptr;
    slot.data.kernLeftCodepoints = nullptr;
    slot.data.kernLeftClassIds = nullptr;
    slot.data.kernRightCodepoints = nullptr;
    slot.data.kernRightClassIds = nullptr;
    slot.data.kernMatrix = nullptr;
    slot.data.kernRowOffsets = nullptr;
    slot.data.kernLeftClassCount = 0;
    slot.data.kernRightClassCount = 0;
    slot.data.ligaturePairs = nullptr;
    slot.data.ligaturePairCount = 0;
    slot.data.glyphMissHandler = &TtfUiFont::missThunk;
    slot.data.glyphMissCtx = &slot;
    slot.data.coverageHandler = &TtfUiFont::coverageThunk;
    slot.data.missKind = MISS_CTX_RING;
    slot.data.ascender = ascender;
    slot.data.descender = 0;
    slot.data.advanceY = static_cast<uint8_t>(lineHeight);
  }
  bound_ = true;
  return true;
}

void TtfUiFont::end() {
  for (uint8_t s = 0; s < 4; ++s) {
    Slot& slot = slots_[s];
    slot.face.reset();  // main-thread only (FreeType shared-library rule)
    slot.faceTried = false;
    slot.data = {};
    slot.font = EpdFont(nullptr);
    slot.bytes = nullptr;
    slot.byteSize = 0;
  }
  ring_.reset();
  ringDram_.reset();
  ringCursor_ = 0;
  bound_ = false;
}

FtFont* TtfUiFont::ensureFace(Slot& slot) {
  if (slot.face != nullptr || slot.faceTried) return slot.face.get();
  slot.faceTried = true;
  if (slot.bytes == nullptr || slot.byteSize == 0) return nullptr;
  // Faces are main-thread-only; every TtfUiFont call site runs on loopTask.
  auto face = makeUniqueNoThrow<FtFont>();
  if (face == nullptr) {
    LOG_ERR("TTFUI", "OOM: UI face style %u", static_cast<unsigned>(slot.styleSlot));
    return nullptr;
  }
  // Borrowed bytes: same lifetime rules as the loader's faces — released
  // only through end(), which the owner calls before dropping the loader's
  // bytes (family change / releaseResidentCaches).
  if (!face->init(static_cast<const uint8_t*>(slot.bytes), slot.byteSize, sizePx_, 400, false, slot.faceIndex)) {
    LOG_ERR("TTFUI", "UI face init failed (style %u)", static_cast<unsigned>(slot.styleSlot));
    return nullptr;
  }
  // Degrade funnel (same discipline as the loader's P2 stack probe): Light
  // + monochrome for the 1bpp EpdFont pipeline; on refusal (module not
  // compiled in) drop monochrome, then hinting, then proceed with the
  // engine default. setRenderOptions stores the REQUESTED options even when
  // it reports false, so a refused request must never be left in place —
  // every subsequent rasterize would fail (e.g. mono with the mono module
  // compiled out fails every FT_Render_Glyph).
  FtFont::RenderOptions options = BookFontLoader::kRenderOptions;
  options.monochrome = true;
  if (!face->setRenderOptions(options)) {
    options.hinting = freeink::font::FtFont::HintingMode::Default;
    if (!face->setRenderOptions(options)) {
      options.monochrome = false;
      face->setRenderOptions(options);  // last rung: pure engine default
    }
  }
  slot.face = std::move(face);
  return slot.face.get();
}

bool TtfUiFont::covered(Slot& slot, uint32_t codepoint) {
  FtFont* face = ensureFace(slot);
  return face != nullptr && face->hasGlyph(codepoint);
}

bool TtfUiFont::coverageThunk(void* ctx, uint32_t codepoint) {
  auto* slot = static_cast<Slot*>(ctx);
  if (slot == nullptr || slot->owner == nullptr) return false;
  return slot->owner->covered(*slot, codepoint);
}

const EpdGlyph* TtfUiFont::missThunk(void* ctx, uint32_t codepoint) {
  auto* slot = static_cast<Slot*>(ctx);
  if (slot == nullptr || slot->owner == nullptr) return nullptr;
  return slot->owner->miss(*slot, codepoint);
}

const EpdGlyph* TtfUiFont::miss(Slot& slot, uint32_t codepoint) {
  FtFont* face = ensureFace(slot);
  if (face == nullptr) return nullptr;

  const GlyphBitmap* g = face->rasterize26_6(codepoint, static_cast<uint32_t>(sizePx_) * 64);
  if (g == nullptr) return nullptr;

  auto* ring = const_cast<uint8_t*>(slot.data.bitmap);  // adapter-owned buffer
  const uint8_t slotIndex = ringCursor_++;
  if (ringCursor_ >= kRingSlots) ringCursor_ = 0;
  uint8_t* slotBase = ring + static_cast<size_t>(slotIndex) * kSlotBytes;

  EpdGlyph& out = ringGlyphs_[slotIndex];
  out.width = static_cast<uint8_t>(g->width);
  out.height = static_cast<uint8_t>(g->height);
  out.advanceX = static_cast<uint16_t>(g->advance * 16);  // px → 12.4 fixed-point
  out.left = g->xoff;
  out.top = static_cast<int16_t>(-g->yoff);  // EpdGlyph: distance ABOVE baseline
  if (g->width > 0 && g->height > 0) {
    const uint16_t stride = (g->width + 7) / 8;
    const size_t needed = static_cast<size_t>(stride) * g->height;
    if (needed > kSlotBytes) {
      LOG_ERR("TTFUI", "Glyph %u x %u exceeds ring slot", static_cast<unsigned>(g->width),
              static_cast<unsigned>(g->height));
      return nullptr;
    }
    packMono1bpp(*g, slotBase, kSlotBytes);
    out.dataLength = static_cast<uint16_t>(needed);
    out.dataOffset = static_cast<uint32_t>(slotIndex) * kSlotBytes;
  } else {
    // Zero-box glyph (e.g. space): keep the advance, no bitmap bytes.
    out.dataLength = 0;
    out.dataOffset = static_cast<uint32_t>(slotIndex) * kSlotBytes;
  }
  return &out;
}

int computeTtfUiFontId(const char* familyName, uint16_t sizePx) {
  // FNV-1a over family name + UI salt + size (upstream's "\x01ui" pattern).
  uint32_t h = 0x811c9dc5;
  for (const char* p = familyName; *p != '\0'; ++p) {
    h ^= static_cast<uint8_t>(*p);
    h *= 0x01000193;
  }
  h ^= 0x01u;
  h *= 0x01000193;
  const uint16_t salted = static_cast<uint16_t>(sizePx ^ 0x5A17u);
  h ^= salted >> 8;
  h *= 0x01000193;
  h ^= salted & 0xFFu;
  h *= 0x01000193;
  // Keep the result positive and non-zero (0 is the manager's failure sentinel).
  const int id = static_cast<int>(h & 0x7FFFFFFFu);
  return id == 0 ? 1 : id;
}

}  // namespace book
}  // namespace freeink

#endif  // CROSSPOINT_TTF_UI_FALLBACK