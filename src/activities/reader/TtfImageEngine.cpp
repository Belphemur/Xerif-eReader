#include "TtfImageEngine.h"

#include "TtfImageCache.h"

#if defined(CROSSPOINT_TTF_READER)

#include <BitmapHelpers.h>
#include <CrossPointSettings.h>
#include <Epub.h>
#include <Epub/blocks/ImageDecodeGate.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <Logging.h>
#include <Memory.h>
#include <epub/ImageProbe.h>
#include <render/ImageRenderer.h>

#include <cstdio>
#include <cstring>
#include <new>

#include "DpidScaler.h"
#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/GifToFramebufferConverter.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"

namespace {

struct BmpSource;  // defined with the BMP row decoder below

// --- tone codecs -------------------------------------------------------------

// --- tone cache stream -------------------------------------------------------

// --- cache reads --------------------------------------------------------------

}  // namespace

namespace ttfimg {

// The SDK types live in freeink::book; the engine deals in them directly.
using freeink::book::BookStatus;
using freeink::book::ImageInfo;
using freeink::book::PageImage;
using freeink::book::ZipEntry;

// Paints one baked tone row at its placement (the single-pass sink's paint step).
void paintToneRow(GfxRenderer& paint, const ImageSpec& spec, int rowIdx, const uint8_t* toneRow, int width);

namespace {

// Publish a completed ".t" decode stream onto the final cache name. On a
// failed publish nothing valid remains: the final name keeps its previous
// content and the temp is dropped so the next attempt re-decodes.
bool publishResult(const char* finalPath, const char* tmpPath, const bool publishing) {
  if (!publishing) return true;
  if (Storage.replaceFile(tmpPath, finalPath)) return true;
  Storage.remove(tmpPath);
  return false;
}

// Abort tracker for the converter family: the converters report only a
// boolean, so wrapping the caller's hook tells decodeAndCache whether a
// failure was an abort (Busy — foreground takeover) or a genuine decode
// error (Failed).
struct AbortTracker {
  bool (*poll)(void*);
  void* ctx;
  bool fired = false;
};

bool trackAbort(void* user) {
  auto* t = static_cast<AbortTracker*>(user);
  if (t->poll != nullptr && t->poll(t->ctx)) {
    t->fired = true;
    return true;
  }
  return false;
}

// Placement bounds against the live panel: PortraitInverted swaps the axes
// inside DirectPixelWriter (phyY = logical x), so its only internal guard
// cannot catch an off-panel row — the callers must clip both coordinates.
// The bounds are the orientation-aware LOGICAL screen dims
// (getScreenWidth/Height, 480x800 in portrait), NOT the panel accessors:
// getDisplayHeight() is the physical 480, so a panel-dims clip silently
// dropped every image row at logical y >= 480 in portrait — the cropped-
// illustration defect (ImageBlock::render and the GIF converter clip with
// the logical accessors; pinned by ImageFitPaint.*).
bool paintRowOnScreen(const GfxRenderer& paint, const int outY) { return outY >= 0 && outY < paint.getScreenHeight(); }

bool paintColOnScreen(const GfxRenderer& paint, const int outX) { return outX >= 0 && outX < paint.getScreenWidth(); }

// One destination row through the single-pass bake (ttfimg::BakeSinkCtx):
// aborts first (foreground takeover is instant), then toneRow -> cache row
// -> paint. Returns false to stop the decode (abort or a fatal width
// mismatch).
bool bakeSinkRow(void* user, const uint16_t rowIdx, const uint8_t* grayRow, const uint16_t width) {
  auto* ctx = static_cast<BakeSinkCtx*>(user);
  if (ctx->aborted) return false;
  if (ctx->abortPoll != nullptr && ctx->abortPoll(ctx->abortCtx)) {
    ctx->aborted = true;
    return false;
  }
  if (ctx->bake == nullptr || ctx->toneRow == nullptr || ctx->spec == nullptr) return false;
  if (width != ctx->spec->w) {
    LOG_ERR("TIMGE", "Destination row width mismatch: %u != %u", width, ctx->spec->w);
    ctx->aborted = true;
    return false;
  }
  ctx->bake->toneRow(grayRow, ctx->toneRow);
  if (ctx->writer != nullptr && !ctx->writer->writeRow(ctx->toneRow)) {
    // The writer dropped its file (open/write/close failure): keep painting
    // every row and publish nothing — the next pass re-decodes for cache.
    ctx->writer = nullptr;
  }
  if (ctx->paint != nullptr) paintToneRow(*ctx->paint, *ctx->spec, rowIdx, ctx->toneRow, width);
  return true;
}

}  // namespace

// Paints one baked tone row at its placement (the single-pass sink's paint step).
void paintToneRow(GfxRenderer& paint, const ImageSpec& spec, const int rowIdx, const uint8_t* toneRow,
                  const int width) {
  const int outY = spec.y + rowIdx;
  if (!paintRowOnScreen(paint, outY)) return;
  DirectPixelWriter pw;
  pw.init(paint);
  pw.beginRow(outY);
  for (int col = 0; col < width; ++col) {
    const int outX = spec.x + col;
    if (paintColOnScreen(paint, outX)) pw.writePixel(outX, toneRow[col]);
  }
}

// --- BMP row decode (over the PSRAM item buffer; design §5.5 'app BMP path') ---

namespace {

struct BmpSource {
  HalMemoryFile file;
  uint32_t dataOffset = 0;
  int width = 0;
  int height = 0;
  bool topDown = false;
  uint16_t bpp = 0;
  size_t rowBytes = 0;
  uint8_t paletteLuma[256] = {};
  uint16_t paletteCount = 0;

  bool parse() {
    if (file.size() < 26) return false;  // smallest legal file: 14-byte header + 12-byte core DIB
    uint8_t fh[14];
    if (file.read(fh, 14) != 14) return false;
    if (fh[0] != 'B' || fh[1] != 'M') return false;
    dataOffset = static_cast<uint32_t>(fh[10]) | (static_cast<uint32_t>(fh[11]) << 8) |
                 (static_cast<uint32_t>(fh[12]) << 16) | (static_cast<uint32_t>(fh[13]) << 24);
    if (!file.seek(14)) return false;
    uint8_t dibSizeBytes[4];
    if (file.read(dibSizeBytes, 4) != 4) return false;
    const uint32_t dibSize = static_cast<uint32_t>(dibSizeBytes[0]) | (static_cast<uint32_t>(dibSizeBytes[1]) << 8) |
                             (static_cast<uint32_t>(dibSizeBytes[2]) << 16) |
                             (static_cast<uint32_t>(dibSizeBytes[3]) << 24);
    if (dibSize == 12) {
      uint8_t core[8];  // LE16 width, LE16 height, LE16 planes, LE16 bpp
      if (file.read(core, 8) != 8) return false;
      width = static_cast<uint16_t>(core[0]) | (static_cast<uint16_t>(core[1]) << 8);
      height = static_cast<uint16_t>(core[2]) | (static_cast<uint16_t>(core[3]) << 8);
      bpp = static_cast<uint16_t>(core[6]) | (static_cast<uint16_t>(core[7]) << 8);
      // OS/2 core headers carry 3-byte (B,G,R) palette entries.
      paletteCount = static_cast<uint16_t>(dataOffset > 14 + 12 ? (dataOffset - 14 - 12) / 3 : 0);
      if (paletteCount > 256) paletteCount = 256;
      if (!loadCorePalette()) return false;
    } else if (dibSize >= 40 && dibSize <= 124) {
      uint8_t info[36];  // w(4) h(4) planes(2) bpp(2) compression(4) sizeImage(4) ppm(4x2) clrUsed(4) clrImportant(4)
      if (file.read(info, 36) != 36) return false;
      const int32_t w =
          static_cast<int32_t>(static_cast<uint32_t>(info[0]) | (static_cast<uint32_t>(info[1]) << 8) |
                               (static_cast<uint32_t>(info[2]) << 16) | (static_cast<uint32_t>(info[3]) << 24));
      const int32_t h =
          static_cast<int32_t>(static_cast<uint32_t>(info[4]) | (static_cast<uint32_t>(info[5]) << 8) |
                               (static_cast<uint32_t>(info[6]) << 16) | (static_cast<uint32_t>(info[7]) << 24));
      bpp = static_cast<uint16_t>(info[10]) | (static_cast<uint16_t>(info[11]) << 8);
      // `info` starts at biWidth: biCompression lives at info[12..15].
      const uint32_t compression = static_cast<uint32_t>(info[12]) | (static_cast<uint32_t>(info[13]) << 8) |
                                   (static_cast<uint32_t>(info[14]) << 16) | (static_cast<uint32_t>(info[15]) << 24);
      const uint32_t clrUsed = static_cast<uint32_t>(info[28]) | (static_cast<uint32_t>(info[29]) << 8) |
                               (static_cast<uint32_t>(info[30]) << 16) | (static_cast<uint32_t>(info[31]) << 24);
      // Mirror the positive bound on the negative (top-down) side: negating
      // INT32_MIN would be signed-overflow UB before the magnitude check.
      if (w <= 0 || h == 0 || w > 0xFFFF || h > 0xFFFF || h < -0xFFFF) return false;
      width = w;
      topDown = h < 0;
      height = h < 0 ? -h : h;
      // BI_RGB only — RLE/BITFIELDS forms are refused (blank reserved space).
      if (compression != 0) return false;
      if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) return false;
      // clrUsed == 0 means "format default", not 256: monochrome 2, 4-bit 16,
      // 8-bit 256 (BITMAPINFOHEADER spec). Reading 256 entries into a 16-entry
      // palette consumes pixel bytes and rejects valid images.
      if (bpp <= 8) {
        const uint16_t defaultEntries = bpp == 1 ? 2 : (bpp == 4 ? 16 : 256);
        paletteCount = clrUsed == 0 ? defaultEntries : (clrUsed > 256 ? 256 : static_cast<uint16_t>(clrUsed));
      } else {
        paletteCount = 0;
      }
      if (!loadInfoPalette(dibSize)) return false;
    } else {
      return false;
    }
    if (dataOffset == 0 || file.size() <= dataOffset) return false;
    rowBytes = (static_cast<size_t>(width) * bpp + 31) / 32 * 4;
    // The payload must cover every row.
    if (file.size() < static_cast<size_t>(dataOffset) + rowBytes * static_cast<uint32_t>(height)) return false;
    return true;
  }

  bool loadCorePalette() {
    if (paletteCount == 0) return true;
    if (!file.seek(14 + 12)) return false;
    for (uint16_t i = 0; i < paletteCount; ++i) {
      uint8_t bgr[3];
      if (file.read(bgr, 3) != 3) return false;
      paletteLuma[i] = static_cast<uint8_t>((bgr[2] * 77 + bgr[1] * 150 + bgr[0] * 29) >> 8);
    }
    return true;
  }

  bool loadInfoPalette(const uint32_t dibSize) {
    if (paletteCount == 0) return true;
    if (!file.seek(14 + dibSize)) return false;
    for (uint16_t i = 0; i < paletteCount; ++i) {
      uint8_t bgra[4];  // BGRA on the INFO and V4/V5 header family
      if (file.read(bgra, 4) != 4) return false;
      paletteLuma[i] = static_cast<uint8_t>((bgra[2] * 77 + bgra[1] * 150 + bgra[0] * 29) >> 8);
    }
    return true;
  }

  bool readRowGray(const int y, uint8_t* gray, uint8_t* bits) {
    const uint32_t fileRow = topDown ? static_cast<uint32_t>(y) : static_cast<uint32_t>(height - 1 - y);
    if (!file.seek(dataOffset + fileRow * rowBytes)) return false;
    if (file.read(bits, rowBytes) != static_cast<int>(rowBytes)) return false;
    switch (bpp) {
      case 24:
      case 32: {
        const uint16_t step = bpp / 8;
        for (int x = 0; x < width; ++x) {
          // Byte order is B,G,R ([,A]) — luma weights for the RGB channels.
          gray[x] =
              static_cast<uint8_t>((bits[x * step + 2] * 77 + bits[x * step + 1] * 150 + bits[x * step] * 29) >> 8);
        }
        break;
      }
      case 8:
        for (int x = 0; x < width; ++x) gray[x] = paletteLuma[bits[x] & 0xFF];
        break;
      case 4:
        for (int x = 0; x < width; ++x) {
          const uint8_t idx =
              (x & 1) == 0 ? static_cast<uint8_t>(bits[x >> 1] >> 4) : static_cast<uint8_t>(bits[x >> 1] & 0x0F);
          gray[x] = paletteLuma[idx & 0x0F];
        }
        break;
      case 1:
        for (int x = 0; x < width; ++x) {
          const uint8_t idx = (bits[x >> 3] >> (7 - (x & 7))) & 0x01;
          gray[x] = paletteLuma[idx & 0x01];
        }
        break;
      default:
        return false;
    }
    return true;
  }
};

}  // namespace

// DpidScaler plumbing shared by the SDK JPEG/PNG streaming decode and the
// BMP row decoder (design 2026-11-09-ttf-image-dpid-clahe.md): the scaler's
// destination rows stream through the single-pass bake sink; the scratch
// block is a call-scoped PSRAM PoolBytes (decision log 6). Degradation
// ladder inside attachScaler: detail+clahe -> clahe only (detail off) -> λ0
// fractional box (no image buffer), each attempt a fresh allocation; the
// mode is fixed before the first row, so no partial-image state can exist.
struct DpidCtx {
  ttfimg::DpidScaler scaler;
  PoolBytes block;
  BakeSinkCtx* sink = nullptr;  // the single-pass bake sink (tone/cache/paint)
  const ttfimg::ImageSpec* spec = nullptr;
  const freeink::book::ImageInfo* info = nullptr;  // probe result (progressive frame dims)
  bool clahe = false;                              // CLAHE stage off — see QuickPageRelayout.cpp (design 2026-11-13)
  bool attached = false;                           // dims hook ran; begin() succeeded
  bool dimsFired = false;                          // the source dims hook ran (GIF converter)
  // True when the decode's ladder stepped down from the requested rung —
  // DPID detail or CLAHE dropped after the scratch allocation failed.
  // A degraded bake must never be published under the requested quality's
  // cache name (review 2026-11-10) — the engine skips the publish and the
  // next pass re-decodes.
  bool degraded = false;
  // A progressive stream that fell back to the DC-only first scan reports
  // 1/8-scale dims: the decode is a thumbnail — bilinear class, never
  // cached (fidelity campaign 2026-11-10).
  bool dcFallback = false;
  // BMP random-access provider state
  BmpSource* bmp = nullptr;
  uint8_t* bits = nullptr;  // BmpSource bit scratch (rowBytes)
};

// Degrade bookkeeping shared by every dims/attach site: a degraded decode
// never publishes under the requested quality's cache name (a cached
// degraded rung would permanently block the real page — review 2026-11-10),
// so the cache stream detaches BEFORE the first row — every degrade fact
// (the ladder stepdown, the DC fallback, the scale-reduced arena retry) is
// an attach-time dims-hook fact.
void detachWriterIfDegraded(DpidCtx& c, const char* why) {
  if (!c.degraded || c.sink == nullptr) return;
  if (c.sink->writer != nullptr) {
    LOG_INF("TIMGE", "DPID degraded rung (%s) - painting uncached: %s", why, c.spec->href);
  }
  c.sink->detachWriter();
}

bool dpidPollFn(void* user) {
  auto* c = static_cast<DpidCtx*>(user);
  if (c->sink != nullptr && c->sink->abortPoll != nullptr && c->sink->abortPoll(c->sink->abortCtx)) {
    c->sink->aborted = true;
    return true;
  }
  return false;
}

bool dpidEmitFn(void* user, const int rowIdx, const uint8_t* dstRow) {
  auto* c = static_cast<DpidCtx*>(user);
  if (c->sink == nullptr) return false;
  return bakeSinkRow(c->sink, static_cast<uint16_t>(rowIdx), dstRow, c->scaler.dstWidth());
}

// BMP provider: reads the requested source row out of the resident PSRAM
// item buffer (no decode involved — bit reuse of the row decoder). Foreground
// takeover polls here: the provider is the only per-row hook the encoder loop
// sees.
bool dpidBmpRowFn(void* user, const int srcY, uint8_t* out) {
  auto* c = static_cast<DpidCtx*>(user);
  if (dpidPollFn(user)) return false;
  return c->bmp->readRowGray(srcY, out, c->bits);
}

// Host-test seam (review 2026-11-13): forces the DPID attach ladder to refuse
// every rung so the ladder-exhaustion contracts are testable without a real
// allocation failure. Never set in production.
bool g_attachLadderFailForTest = false;

void setAttachLadderFailForTest(const bool fail) { g_attachLadderFailForTest = fail; }

// Attaches a DpidScaler to `ctx` for one decode, walking the degradation
// ladder. false = even the tiny λ0 block could not be allocated (the caller
// fails the decode; nothing has been written yet).
bool attachScaler(DpidCtx& c, const int srcW, const int srcH, const int dstW, const int dstH, const bool clahe,
                  const bool randomAccess) {
  if (g_attachLadderFailForTest) return false;
  DpidScaler::Plan p{srcW, srcH, dstW, dstH, clahe, true, randomAccess, false, c.dcFallback};
  size_t need = DpidScaler::scratchBytes(p);
  // 8-aligned block: the scaler carves uint64/uint32/uint16 views at internal
  // offsets and poolMakeBytes only guarantees 4 (review 2026-11-11).
  c.block = poolMakeAlignedBytes(8, need);
  if (!c.block) {
    // Keep CLAHE (the Enhanced promise) and drop only the detail pass.
    DpidScaler::Plan p2 = p;
    p2.detail = false;
    need = DpidScaler::scratchBytes(p2);
    c.block = poolMakeAlignedBytes(8, need);
    if (!c.block && p2.clahe) {
      p2.clahe = false;  // λ0 fractional box: the small accumulator block
      need = DpidScaler::scratchBytes(p2);
      c.block = poolMakeAlignedBytes(8, need);
    }
    if (!c.block) return false;
    p = p2;
  }
  // On-device budget probe (PSRAM budget 2026-11-09 in
  // docs/design/2026-11-08-ttf-image-text-in-image.md): every decode states
  // its scratch need, the rung the ladder placed and the live PSRAM free, so
  // flash tests read the degradation directly from serial instead of inferring
  // it from the picture.
  const char* rung;
  if (p.detail && p.clahe) {
    rung = "detail+clahe";
  } else if (p.detail) {
    rung = "detail";
  } else if (p.clahe) {
    rung = "clahe";
  } else {
    rung = "box";
  }
  LOG_INF("TIMGE", "DPID scratch %u KiB -> %s (psram free %u KiB)", static_cast<unsigned>(need / 1024), rung,
          static_cast<unsigned>(HalMemory::getPsramHeap().freeBytes / 1024));
  const bool okBegin = c.scaler.begin(p, c.block.get(), need, &dpidEmitFn, &c, &dpidPollFn, &c,
                                      randomAccess ? &dpidBmpRowFn : nullptr, randomAccess ? &c : nullptr);
  if (okBegin) {
    // Degradation = the placed plan lost a rung the REQUESTED plan carries:
    // the attach ladder (pool failed -> detail/clahe dropped). The small
    // class's own nearest/1:1 classing is design, not degradation — and
    // neither is reset4's streaming policy (pass 2 never runs on streamed
    // rows), so the intent plan carries it too.
    DpidScaler::Plan intent =
        DpidScaler::plan(DpidScaler::Plan{srcW, srcH, dstW, dstH, clahe, true, randomAccess, false, c.dcFallback});
    if (!randomAccess) intent.detail = false;  // reset4 streaming policy, not a degradation
    const DpidScaler::Plan placed = c.scaler.placedPlan();
    c.degraded = placed.detail != intent.detail || placed.clahe != intent.clahe;
  }
  return okBegin;
}

// Streaming row entry for renderSourceRows (the width comes from the SDK;
// anything other than the placed plan's source width means a different
// decode — refuse it). Polls the abort hook per row: the removed
// GrayCollectCtx-based hook used to poll at SDK row cadence, so a foreground
// takeover must keep that granularity instead of waiting for the next
// destination band (review 2026-11-10).
bool dpidRowFn(void* user, const uint16_t y, const uint8_t* gray, const uint16_t width) {
  auto* c = static_cast<DpidCtx*>(user);
  if (dpidPollFn(user)) return false;
  if (width != c->scaler.srcWidth()) {
    if (c->sink != nullptr) c->sink->aborted = true;
    return false;
  }
  c->scaler.feedSourceRow(y, gray);
  // An aborted sink makes every further source row useless — stop the SDK
  // decode at this callback boundary instead of streaming the rest of the
  // image into a dead scaler (review 2026-11-11).
  return !(c->sink != nullptr && c->sink->aborted);
}

// Dims hook: single call before the first row; allocates the block here so
// the JPEG power-of-two pre-scale (which sets srcW/srcH) is already known.
bool dpidDimsFn(void* user, const uint16_t srcW, const uint16_t srcH, const bool scaleReduced) {
  auto* c = static_cast<DpidCtx*>(user);
  // A progressive stream whose full decode fell back to the DC-only first
  // scan reports 1/8-scale dims (blocksW x blocksH < the probe's frame
  // width). Such a decode is a thumbnail: bilinear class, and degraded —
  // never published under the requested quality's cache name (a cached
  // DC-thumbnail would permanently block the real page — fidelity
  // campaign 2026-11-10).
  c->dcFallback = c->info != nullptr && c->info->progressive && srcW < c->info->width;
  c->attached = attachScaler(*c, srcW, srcH, c->spec->w, c->spec->h, c->clahe, false);
  if (c->attached && c->dcFallback) {
    LOG_WRN("TIMGE", "DC fallback decode %ux%u < frame %u, ncomp %u - bilinear, uncached: %s", srcW, srcH,
            c->info->width, static_cast<unsigned>(c->info->jpegComponents), c->spec->href);
    c->degraded = true;
  }
  if (c->attached && scaleReduced) {
    // The SDK's arena-retry stepped the decode below the fidelity-preferred
    // pre-scale (SDK reports it via the dims hook, 2026-11-11 review). The
    // reduced-scale result still paints, but it must not publish under the
    // full-quality cache key — a later full-detail decode would otherwise
    // be blocked by the cache forever.
    LOG_WRN("TIMGE", "Decode scale reduced below the preferred pick - painting uncached: %s", c->spec->href);
    c->degraded = true;
  }
  detachWriterIfDegraded(*c, c->dcFallback ? "dc fallback" : "scale reduced");
  return c->attached;
}

// GIF converter hooks: the RenderConfig callbacks have the (int, const
// uint8_t*) row shape, the DpidScaler expects the same.
bool dpidGifDimsFn(void* user, const int srcW, const int srcH) {
  auto* c = static_cast<DpidCtx*>(user);
  c->dimsFired = true;
  c->attached = attachScaler(*c, srcW, srcH, c->spec->w, c->spec->h, c->clahe, false);
  detachWriterIfDegraded(*c, "ladder stepdown");
  return c->attached;
}

bool dpidGifRowFn(void* user, const int srcY, const uint8_t* gray) {
  auto* c = static_cast<DpidCtx*>(user);
  if (!c->attached) return false;
  if (dpidPollFn(user)) return false;
  c->scaler.feedSourceRow(srcY, gray);
  // Same dead-sink stop as dpidRowFn.
  return !(c->sink != nullptr && c->sink->aborted);
}

// Quality BMP decode (DPID design 2026-11-09 §Contract 2): the source rows
// re-read out of the resident PSRAM item buffer feed the app-side DpidScaler
// in random-access mode; the scaler's destination rows go through the same
// single-pass sink as every decoder (tone → cache row → paint). `sink` is
// fully caller-owned: the production flow passes the writer + the paint
// target, the re-decode paint fallback passes paint without a writer.
DecodeResult decodeBmpRows(const ImageSpec& spec, const uint8_t* data, const size_t size, BakeSinkCtx* sink,
                           const bool clahe, bool* degradedOut = nullptr, bool* ladderFailedOut = nullptr) {
  if (sink == nullptr || sink->bake == nullptr || sink->toneRow == nullptr) return DecodeResult::Failed;
  BmpSource src;
  src.file.attach(data, size);
  if (!src.parse()) {
    LOG_ERR("TIMGE", "BMP parse failed: %s", spec.href);
    return DecodeResult::Skip;
  }

  const int srcW = src.width;
  const int srcH = src.height;
  const int dstW = spec.w;
  const int dstH = spec.h;
  if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return DecodeResult::Failed;

  const size_t bitsBytes = src.rowBytes;
  PoolBytes bits{poolMakeBytes(bitsBytes ? bitsBytes : 1)};
  if (!bits) return DecodeResult::Failed;

  DpidCtx dpid;
  dpid.sink = sink;
  dpid.spec = &spec;
  dpid.clahe = clahe;
  dpid.bmp = &src;
  dpid.bits = bits.get();
  if (!attachScaler(dpid, srcW, srcH, dstW, dstH, clahe, true)) {
    // Every rung refused: memory failure, not a decode failure — a worker
    // job defers instead of reporting silent success (review 2026-11-13,
    // same contract as the GIF/JPEG branches).
    if (ladderFailedOut != nullptr) *ladderFailedOut = true;
    return DecodeResult::Failed;
  }
  detachWriterIfDegraded(dpid, "ladder stepdown");
  if (degradedOut != nullptr) *degradedOut = dpid.degraded;

  if (!dpid.scaler.runRandom(&dpidBmpRowFn, &dpid)) {
    return sink->aborted ? DecodeResult::Busy : DecodeResult::Failed;
  }
  if (!dpid.scaler.finish()) {
    if (sink->aborted) return DecodeResult::Busy;
    return DecodeResult::Failed;  // row-read/emit failure: never cache garbage rows
  }
  if (sink->aborted) return DecodeResult::Busy;
  return DecodeResult::Ok;
}

bool blitFromCache(GfxRenderer& renderer, const ImageSpec& spec, const char* dir, const bool qualityTone,
                   const bool clahe) {
  char path[256];
  if (!cacheNameFor(spec, dir, qualityTone, clahe, path, sizeof(path))) return false;
  HalFile cacheFile;
  if (!Storage.openFileForRead("TIMGE", path, cacheFile)) return false;
  uint16_t cachedWidth = 0;
  uint16_t cachedHeight = 0;
  if (!readValidCacheHeader(cacheFile, spec.w, spec.h, cacheIdentity(spec, qualityTone, clahe), cachedWidth,
                            cachedHeight)) {
    LOG_ERR("TIMGE", "Invalid image cache: %s", path);
    return false;
  }

  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
  constexpr size_t kBulkBytes = 4096;
  int rowsPerRead = static_cast<int>(kBulkBytes / static_cast<size_t>(bytesPerRow));
  if (rowsPerRead < 1) rowsPerRead = 1;
  uint8_t bulk[kBulkBytes];

  DirectPixelWriter pw;
  pw.init(renderer);

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = 0; row < cachedHeight; ++row) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (cachedHeight - row < rowsPerRead) ? (cachedHeight - row) : rowsPerRead;
      const size_t want = static_cast<size_t>(toRead) * static_cast<size_t>(bytesPerRow);
      if (cacheFile.read(bulk, want) != static_cast<int>(want)) {
        LOG_ERR("TIMGE", "Cache read error at row %d: %s", row, path);
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }
    const uint8_t* rowBytes = bulk + static_cast<size_t>(bufferRow) * static_cast<size_t>(bytesPerRow);
    ++bufferRow;
    const int outY = spec.y + row;
    if (outY < 0 || outY >= renderer.getScreenHeight()) continue;  // logical bounds (see paintRowOnScreen)
    pw.beginRow(outY);
    for (int col = 0; col < cachedWidth; ++col) {
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      const uint8_t tone = (rowBytes[col >> 2] >> bitShift) & 0x03;
      const int outX = spec.x + col;
      if (outX >= 0 && outX < renderer.getScreenWidth()) pw.writePixel(outX, tone);
    }
  }
  return true;
}

// Re-decode paint fallback (review round 1): when the quality decode could
// not produce rows (SDK decode failure, whole allocation ladder refused)
// the image still paints — re-decoding through the legacy scaled render /
// converter / BMP legacy paint straight to the target, no cache. A
// cache-only job (worker) has nothing to produce: `workerResult` selects
// what it gets — storage-class failures keep the documented no-op Ok (a
// bounded deferral storm against a full card buys nothing), while
// memory-class failures pass Failed so processJob defers and a calmer pass
// re-decodes (review 2026-11-11). `bake.resetError()` first: rows may have
// been baked before the failure, and the repaint starts a fresh plane.
DecodeResult legacyPaintPass(const ImageSpec& spec, const ImageInfo::Kind kind, freeink::book::BookSource& source,
                             const freeink::book::ZipCatalog& zip, freeink::book::Arena& scratch, Epub* epub,
                             const uint8_t* psramImage, const size_t imageSize, ttfimg::ToneBake4& bake,
                             GfxRenderer* paint, const bool paintToTarget, bool (*abortPoll)(void* ctx), void* abortCtx,
                             const bool clahe, const DecodeResult workerResult = DecodeResult::Ok) {
  bake.resetError();
  if (paint == nullptr || !paintToTarget) return workerResult;  // worker: no cache producible
  if (kind == ImageInfo::Kind::Png || kind == ImageInfo::Kind::Jpeg) {
    freeink::book::PageImage sdkImage{};
    sdkImage.href = (spec.hrefExt != nullptr && spec.hrefExt[0] != '\0') ? spec.hrefExt : spec.href;
    sdkImage.x = spec.x;
    sdkImage.y = spec.y;
    sdkImage.width = static_cast<uint16_t>(spec.w);
    sdkImage.height = static_cast<uint16_t>(spec.h);
    uint8_t toneRowStack[256];
    uint8_t* toneRow = toneRowStack;
    PoolBytes toneRowHeap;
    if (spec.w > static_cast<int>(sizeof(toneRowStack))) {
      toneRowHeap = poolMakeBytes(static_cast<size_t>(spec.w));
      toneRow = toneRowHeap.get();
    }
    if (toneRow == nullptr) return DecodeResult::Failed;
    BakeSinkCtx ctx;
    ctx.bake = &bake;
    ctx.spec = &spec;
    ctx.paint = paint;
    ctx.toneRow = toneRow;
    ctx.abortPoll = abortPoll;
    ctx.abortCtx = abortCtx;
    const BookStatus st = freeink::book::ImageRenderer::render(source, zip, sdkImage, scratch, &bakeSinkRow, &ctx);
    if (st != BookStatus::Ok || ctx.aborted) {
      return ctx.aborted ? DecodeResult::Busy
                         : (st == BookStatus::Unsupported ? DecodeResult::Skip : DecodeResult::Failed);
    }
    return DecodeResult::Ok;
  }
  if (kind == ImageInfo::Kind::Gif) {
    if (epub == nullptr) return DecodeResult::Failed;
    RenderConfig config;
    config.x = spec.x;
    config.y = spec.y;
    config.maxWidth = spec.w;
    config.maxHeight = spec.h;
    config.useGrayscale = true;
    config.useDithering = true;
    config.performanceMode = false;
    config.useExactDimensions = true;
    config.qualityTone = true;
    config.toneCollect = false;  // legacy quality paint (the converter's existing path)
    config.toneBake = &bake;
    ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(".gif");
    const bool decoded =
        decoder != nullptr && decoder->decodeToFramebuffer(const_cast<uint8_t*>(psramImage), imageSize, *paint, config);
    return decoded ? DecodeResult::Ok : DecodeResult::Failed;
  }
  // BMP: the row decoder paints through the paint-only sink.
  BakeSinkCtx ctx;
  ctx.bake = &bake;
  ctx.spec = &spec;
  ctx.paint = paint;
  ctx.toneRow = nullptr;  // decodeBmpRows owns its row buffer
  ctx.abortPoll = abortPoll;
  ctx.abortCtx = abortCtx;
  uint8_t toneRowStack[256];
  uint8_t* toneRow = toneRowStack;
  PoolBytes toneRowHeap;
  if (spec.w > static_cast<int>(sizeof(toneRowStack))) {
    toneRowHeap = poolMakeBytes(static_cast<size_t>(spec.w));
    toneRow = toneRowHeap.get();
  }
  ctx.toneRow = toneRow;
  if (toneRow == nullptr) return DecodeResult::Failed;
  return decodeBmpRows(spec, psramImage, imageSize, &ctx, clahe);
}

DecodeResult decodeAndCache(const ImageSpec& spec, const char* cachePath, freeink::book::BookSource& source,
                            const freeink::book::ZipCatalog& zip, freeink::book::Arena& scratch, Epub* epub,
                            GfxRenderer* paint, const bool qualityTone, const bool caching,
                            bool (*abortPoll)(void* ctx), void* abortCtx, const bool paintToTarget, const bool clahe,
                            PoolBytes* arenaFallback) {
  const char* const href = (spec.hrefExt != nullptr && spec.hrefExt[0] != '\0') ? spec.hrefExt : spec.href;
  const ZipEntry* entry = zip.find(href);
  if (entry == nullptr) return DecodeResult::Skip;

  // Atomic publish: every writer streams to "<cachePath>.t" and the engine
  // renames it onto the final name only after a complete, closed decode. An
  // interrupted decode (power loss, abort, write failure) leaves the previous
  // final cache untouched and the .t dropped.
  char tmpPath[280];
  const bool publishing = caching;
  if (publishing && snprintf(tmpPath, sizeof(tmpPath), "%s.t", cachePath) >= static_cast<int>(sizeof(tmpPath))) {
    return DecodeResult::Failed;
  }
  const char* const writePath = publishing ? tmpPath : cachePath;

  ImageInfo info;
  const BookStatus probe = probeImage(source, *entry, scratch, &info);
  if (probe != BookStatus::Ok) return DecodeResult::Failed;

  // Abort tracking lives in the file anon namespace (see trackAbort).

  // JPEG/PNG under the legacy policy keep today's behavior exactly (§5.6):
  // no tone cache, SDK error-diffusion at paint — the worker declines them.
  if ((info.kind == ImageInfo::Kind::Png || info.kind == ImageInfo::Kind::Jpeg) && !qualityTone) {
    return DecodeResult::Skip;
  }

  if (info.kind == ImageInfo::Kind::Png || info.kind == ImageInfo::Kind::Jpeg) {
    // Quality decode (DPID design 2026-11-09 §Contract 2): the SDK streams
    // UNSCALED source gray rows (renderSourceRows; the legacy RowScaler box
    // stays untouched for the PageRenderer path) and the app-side
    // DpidScaler turns them into destination rows — fractional pass 1
    // (truncation bug gone by construction), detail-weighted pass 2, then
    // optional CLAHE — which stream single-pass through the legacy-parity
    // bake into the cache writer and the paint (amendment 9).
    //
    // Cold-decode arena parity (design amendment 7, reviewed 2026-11-11):
    // the 128 KiB fallback is reserved HERE, after the probe named the kind.
    // GIF/BMP decoders pull their source into a separate PSRAM item buffer
    // and never read the decode arena, so an earlier reservation (before
    // the probe) could starve that extraction of the very PSRAM it needs —
    // an extraction OOM leaves a cold GIF/BMP unpainted. The block stays
    // alive for the whole SDK decode through the caller's PoolBytes; on
    // pool exhaustion the decode proceeds on `scratch` as before.
    ttfimg::ToneBake4 bake(spec.w);
    if (!bake.ok()) return DecodeResult::Failed;
    // Single-pass cache stream (atomic .t publish): the sink writes the 2bpp
    // rows as the scaler emits them. A failed open paints uncached — a
    // storage failure is not a decode failure (review round 1 doctrine).
    ttfimg::ToneCacheWriter writer;
    const bool writerActive =
        caching && writer.begin(writePath, spec.w, spec.h, cacheIdentity(spec, qualityTone, clahe));
    if (caching && !writerActive) {
      LOG_ERR("TIMGE", "Image cache open failed - painting uncached: %s", spec.href);
    }
    // A worker leg with no producible stream (cache open failed) has no
    // work: painting is the foreground's contract (storage doctrine — a
    // bounded deferral storm against a full card buys nothing).
    if (!paintToTarget && !writerActive) return DecodeResult::Ok;

    uint8_t toneRowStack[256];
    uint8_t* toneRow = toneRowStack;
    PoolBytes toneRowHeap;
    if (spec.w > static_cast<int>(sizeof(toneRowStack))) {
      toneRowHeap = poolMakeBytes(static_cast<size_t>(spec.w));
      toneRow = toneRowHeap.get();
    }
    if (toneRow == nullptr) {
      writer.abort();
      return DecodeResult::Failed;
    }

    BakeSinkCtx sink;
    sink.bake = &bake;
    sink.spec = &spec;
    sink.writer = writerActive ? &writer : nullptr;
    sink.paint = paintToTarget ? paint : nullptr;
    sink.toneRow = toneRow;
    sink.abortPoll = abortPoll;
    sink.abortCtx = abortCtx;

    freeink::book::Arena fallbackArena;
    freeink::book::Arena* decodeArena = &scratch;
    if (arenaFallback != nullptr && scratch.capacity() - scratch.used() < ttfimg::kDecodeArenaBytes) {
      *arenaFallback = poolMakeBytes(ttfimg::kDecodeArenaBytes);
      if (*arenaFallback) {
        fallbackArena.init((*arenaFallback).get(), ttfimg::kDecodeArenaBytes);
        decodeArena = &fallbackArena;
      } else {
        LOG_DBG("TIMGE", "OOM: image decode arena fallback (%u KiB)",
                static_cast<unsigned>(ttfimg::kDecodeArenaBytes / 1024));
      }
    }

    DpidCtx dpid;
    dpid.sink = &sink;
    dpid.spec = &spec;
    dpid.info = &info;
    dpid.clahe = clahe;
    const BookStatus st = freeink::book::ImageRenderer::renderSourceRows(
        source, zip, href, static_cast<uint16_t>(spec.w), static_cast<uint16_t>(spec.h), *decodeArena, &dpidDimsFn,
        &dpid, &dpidRowFn, &dpid, &dpidPollFn, &dpid);
    if (st != BookStatus::Ok || sink.aborted) {
      writer.abort();
      return sink.aborted ? DecodeResult::Busy
                          : (st == BookStatus::Unsupported ? DecodeResult::Skip : DecodeResult::Failed);
    }
    if (!dpid.attached) {
      // The whole allocation ladder failed (even the ~8 KB λ0 block): a
      // memory failure. The worker defers; the foreground still paints
      // through the legacy pass (review round 1).
      writer.abort();
      if (!paintToTarget && caching) return DecodeResult::Failed;
      return legacyPaintPass(spec, info.kind, source, zip, *decodeArena, epub, nullptr, 0, bake, paint, paintToTarget,
                             abortPoll, abortCtx, clahe);
    }
    // finish() applies pass 2 + CLAHE and emits the remaining destination
    // rows; the streamed ones already went through the sink.
    const bool finished = dpid.scaler.finish();
    if (sink.aborted) {
      writer.abort();
      return DecodeResult::Busy;
    }
    if (!finished) {
      writer.abort();
      return DecodeResult::Failed;
    }
    if (dpid.degraded) {
      // The writer was detached at the dims hook (before the first row) —
      // nothing was published. A dcFallback decode is a deterministic 1/8-
      // scale bilinear thumbnail that can never publish under the requested
      // quality's key (fidelity campaign 2026-11-10): drop the worker job
      // (Skip = processJob logs and drops) instead of burning its deferral
      // budget on identical decodes that hold the bus. Other degrades are
      // transient (the SDK's arena-retry scale): the worker defers. The
      // foreground keeps the paint-uncached contract (rows painted).
      if (!paintToTarget && caching) {
        if (dpid.dcFallback) {
          LOG_INF("TIMGE", "DC fallback decode - worker job skipped: %s", spec.href);
          return DecodeResult::Skip;
        }
        LOG_INF("TIMGE", "Worker decode degraded - deferring: %s", spec.href);
        return DecodeResult::Failed;
      }
      return DecodeResult::Ok;
    }
    const bool streamOk = writerActive && writer.finalize();
    if (!streamOk) {
      // A dead cache stream still painted every row: a foreground decode
      // must report the display success, or a full card turns a painted
      // image into a file error (review 2026-11-11). A worker job produced
      // nothing and must keep failing so the job defers and retries.
      return paintToTarget ? DecodeResult::Ok : DecodeResult::Failed;
    }
    if (!publishResult(cachePath, writePath, publishing)) {
      // Publish failure is storage, not decode: the rows are already on
      // screen (paintToTarget), so report the display and let the next
      // pass re-decode for the cache. The worker produced nothing and
      // keeps failing (deferral retry).
      return paintToTarget ? DecodeResult::Ok : DecodeResult::Failed;
    }
    return DecodeResult::Ok;
  }

  if (info.kind == ImageInfo::Kind::Gif || info.kind == ImageInfo::Kind::Bmp) {
    // New formats route through the same single-pass bake (amendment 9):
    // the converter hands RAW source rows to the app-side DpidScaler (one
    // resize for every decoder — design 2026-11-09 §Decision log 1), the
    // BMP row decoder runs random-access, and both stream destination rows
    // through the shared sink. qualityTone=false keeps today's behavior —
    // nothing was rendered for these kinds before, so nothing decodes.
    if (!qualityTone) return DecodeResult::Skip;
    if (epub == nullptr) return DecodeResult::Failed;

    size_t imageSize = 0;
    PoolBytes image{epub->extractItemToPsram(href, &imageSize)};
    if (!image) {
      LOG_ERR("TIMGE", "PSRAM extraction failed: %s", href);
      return DecodeResult::Failed;
    }

    ttfimg::ToneBake4 bake(spec.w);
    if (!bake.ok()) return DecodeResult::Failed;
    ttfimg::ToneCacheWriter writer;
    const bool writerActive =
        caching && writer.begin(writePath, spec.w, spec.h, cacheIdentity(spec, qualityTone, clahe));
    if (caching && !writerActive) {
      LOG_ERR("TIMGE", "Image cache open failed - painting uncached: %s", spec.href);
    }
    if (!paintToTarget && !writerActive) return DecodeResult::Ok;

    uint8_t toneRowStack[256];
    uint8_t* toneRow = toneRowStack;
    PoolBytes toneRowHeap;
    if (spec.w > static_cast<int>(sizeof(toneRowStack))) {
      toneRowHeap = poolMakeBytes(static_cast<size_t>(spec.w));
      toneRow = toneRowHeap.get();
    }
    if (toneRow == nullptr) {
      writer.abort();
      return DecodeResult::Failed;
    }

    BakeSinkCtx sink;
    sink.bake = &bake;
    sink.spec = &spec;
    sink.writer = writerActive ? &writer : nullptr;
    sink.paint = paintToTarget ? paint : nullptr;
    sink.toneRow = toneRow;
    sink.abortPoll = abortPoll;
    sink.abortCtx = abortCtx;

    bool decoded = false;
    bool aborted = false;
    // A degraded rung never publishes under the requested name (the writer
    // was detached at the dims hook); the pre-blur gate uses the PROBE dims
    // (the true source — baseline JPEG's power-of-two pre-scale is inside
    // the SDK renderer, review 2026-11-10 steer).
    bool planDegraded = false;
    // The DPID ladder refused every rung (dims hook ran, attachScaler
    // returned false — the converter then fails the collect decode).
    // Memory failure, not a decode failure: a worker job must defer.
    bool ladderFailed = false;
    if (info.kind == ImageInfo::Kind::Gif) {
      // Select the converter by the SNIFFED kind, not the href: ImageProbe
      // identified this GIF from its bytes, so a mismatched or extensionless
      // href must still reach the GIF converter (it would otherwise get no
      // decoder and stay blank). The converter routes the RAW source rows
      // through the app-side DpidScaler and paints nothing — the sink owns
      // tone, cache and paint. The renderer stays live for the converter's
      // geometry reads; it is simply not drawn through.
      RenderConfig config;
      config.x = spec.x;
      config.y = spec.y;
      config.maxWidth = spec.w;
      config.maxHeight = spec.h;
      config.useGrayscale = true;
      config.useDithering = true;
      config.performanceMode = false;
      config.useExactDimensions = true;
      config.qualityTone = true;  // GIF bake policy (§5.6)
      config.toneCollect = true;  // feed the scaler; the sink owns the emit
      config.toneBake = &bake;
      AbortTracker tracker{abortPoll, abortCtx};
      config.abortPoll = &trackAbort;
      config.abortCtx = &tracker;
      DpidCtx dpid;
      dpid.sink = &sink;
      dpid.spec = &spec;
      dpid.clahe = clahe;
      if (paint != nullptr) {
        config.sourceDimsFn = &dpidGifDimsFn;
        config.sourceRowFn = &dpidGifRowFn;
        config.sourceUser = &dpid;
      }
      ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(".gif");
      decoded = decoder != nullptr && paint != nullptr &&
                decoder->decodeToFramebuffer(image.get(), imageSize, *paint, config);
      aborted = tracker.fired;
      if (decoded && !dpid.attached) {
        // Dims hook never fired (empty GIF / zero rows): nothing streamed.
        decoded = false;
      }
      if (!dpid.attached && dpid.dimsFired) ladderFailed = true;
      if (decoded && dpid.attached) {
        // Pass 2 + optional CLAHE + the emit walk through the sink.
        decoded = dpid.scaler.finish();
        if (!decoded && sink.aborted) aborted = true;
      }
      if (sink.aborted) aborted = true;
      planDegraded = dpid.attached && dpid.degraded;
    } else {
      bool bmpLadderFailed = false;
      const DecodeResult bmpRes =
          decodeBmpRows(spec, image.get(), imageSize, &sink, clahe, &planDegraded, &bmpLadderFailed);
      if (bmpLadderFailed) ladderFailed = true;
      decoded = bmpRes == DecodeResult::Ok;
      aborted = bmpRes == DecodeResult::Busy;
    }
    if (!decoded) {
      writer.abort();
      if (aborted) return DecodeResult::Busy;
      // A collect decode failure painted nothing (e.g. a low-heap GIF
      // buffer failure): the legacy pass gets its own decode attempt before
      // the placement is given up. DPID ladder exhaustion is a memory
      // failure, not a decode failure: a worker job has nothing to paint
      // or cache — defer (review 2026-11-11, same contract as the JPEG/PNG
      // branch). The foreground keeps the legacy-pass second attempt.
      const DecodeResult workerResult =
          ladderFailed && !paintToTarget && caching ? DecodeResult::Failed : DecodeResult::Ok;
      return legacyPaintPass(spec, info.kind, source, zip, scratch, epub, image.get(), imageSize, bake, paint,
                             paintToTarget, abortPoll, abortCtx, clahe, workerResult);
    }
    if (planDegraded) {
      // The writer was detached at the dims hook — nothing published. The
      // degraded cache-only job defers instead of reporting silent success;
      // the foreground keeps the paint-uncached contract (rows painted).
      if (!paintToTarget && caching) {
        LOG_INF("TIMGE", "Worker decode degraded - deferring: %s", spec.href);
        return DecodeResult::Failed;
      }
      return DecodeResult::Ok;
    }
    const bool streamOk = writerActive && writer.finalize();
    if (!streamOk) {
      // A dead cache stream still painted every row: the foreground keeps
      // the display success; the worker produced nothing and defers.
      return paintToTarget ? DecodeResult::Ok : DecodeResult::Failed;
    }
    if (!publishResult(cachePath, writePath, publishing)) {
      // Same paint-vs-publish contract as the JPEG/PNG branch above.
      return paintToTarget ? DecodeResult::Ok : DecodeResult::Failed;
    }
    return DecodeResult::Ok;
  }

  return DecodeResult::Skip;  // Kind::Unknown — not decodable (SVG/WebP)
}

bool decodeGatesOk() {
  // Per-image worker heap gates (#292 §3.4 rule 5 precedents): a decode that
  // would under-run the loop task's working set is deferred, not forced.
  const HalMemory::HeapStats internal = HalMemory::getInternalHeap();
  if (internal.freeBytes < 24 * 1024 || internal.largestBlockBytes < 8 * 1024) return false;
  const HalMemory::HeapStats psram = HalMemory::getPsramHeap();
  if (!psramBudgetOk(psram.totalBytes, psram.freeBytes)) return false;
  return true;
}

}  // namespace ttfimg

#endif  // CROSSPOINT_TTF_READER
