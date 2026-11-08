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

#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/GifToFramebufferConverter.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"

namespace {

// --- tone codecs -------------------------------------------------------------

// --- tone cache stream -------------------------------------------------------

// --- cache reads --------------------------------------------------------------

// Row sink for the SDK ImageRenderer on the quality JPEG/PNG decode: aborts
// first (foreground takeover is instant), then feeds the adaptive bake's
// collection pass (histogram + gray temp, design 2026-11-08 §2.1 amendment
// 2). No tone, no cache, no paint here — the remap pass after the decode
// owns tone, cache and paint, so every decoder shares one phase-2.
struct GrayCollectCtx {
  ttfimg::ToneBake4* bake = nullptr;
  uint16_t w = 0;
  bool (*abortPoll)(void* ctx) = nullptr;
  void* abortCtx = nullptr;
  bool aborted = false;
  bool collectFailed = false;  // gray temp died mid-collect (not an abort)
};

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

bool grayCollectFn(void* user, const uint16_t rowIdx, const uint8_t* grayRow, const uint16_t width) {
  GrayCollectCtx* ctx = static_cast<GrayCollectCtx*>(user);
  if (ctx->aborted) return false;
  if (ctx->abortPoll != nullptr && ctx->abortPoll(ctx->abortCtx)) {
    ctx->aborted = true;
    return false;
  }
  if (ctx->bake == nullptr) return false;
  if (ctx->bake->grayWriteFailed()) {
    ctx->collectFailed = true;  // stop the render; the engine legacy-passes
    return false;
  }
  if (width != ctx->w) {
    LOG_ERR("TIMGE", "SDK row width mismatch: %u != %u", width, ctx->w);
    ctx->aborted = true;
    return false;
  }
  ctx->bake->collectRow(grayRow);
  return true;
}

}  // namespace

namespace ttfimg {

// The SDK types live in freeink::book; the engine deals in them directly.
using freeink::book::BookStatus;
using freeink::book::ImageInfo;
using freeink::book::PageImage;
using freeink::book::ZipEntry;

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

}  // namespace

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

// Quality BMP decode, phase 1 (design 2026-11-08 §2.1 amendment 2): decode
// to averaged gray rows and feed the adaptive bake's collection pass. No
// tone, cache or paint here — decodeAndCache runs the shared remap pass
// (remapGrayToCache) once the histogram is complete. `bake` carries the
// gray temp (the caller ran beginCollect).
// Paints one baked tone row at its placement (shared by every phase-2 remap
// and the storage-failure legacy fallback; defined below the BMP decoder's
// helpers).
void paintToneRow(GfxRenderer& paint, const ImageSpec& spec, int rowIdx, const uint8_t* toneRow, int width);

// `legacyPaint` (optional) selects the storage-failure fallback: instead of
// collecting gray rows the decoder bakes with the caller-prepared (degenerate)
// map and paints each row — no cache, no temp.
DecodeResult decodeBmpRows(const ImageSpec& spec, const uint8_t* data, const size_t size, ttfimg::ToneBake4* bake,
                           GfxRenderer* legacyPaint, bool (*abortPoll)(void* ctx), void* abortCtx) {
  if (bake == nullptr) return DecodeResult::Failed;
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

  const bool areaScale = (srcW >= 2 * dstW) || (srcH >= 2 * dstH);
  const size_t bitsBytes = src.rowBytes;
  PoolBytes bits{poolMakeBytes(bitsBytes ? bitsBytes : 1)};
  const size_t grayLen = srcW + static_cast<size_t>(dstW) + (legacyPaint != nullptr ? static_cast<size_t>(dstW) : 0);
  PoolBytes gray{poolMakeBytes(grayLen)};  // gray (srcW) + averaged (dstW) + legacy tone (dstW)
  if (!bits || !gray) {
    return DecodeResult::Failed;
  }
  uint8_t* grayRow = gray.get();
  uint8_t* avgRow = grayRow + srcW;
  uint8_t* toneRow = avgRow + dstW;

  std::unique_ptr<uint32_t[]> sums;
  std::unique_ptr<uint16_t[]> counts;
  if (areaScale) {
    sums.reset(new (std::nothrow) uint32_t[dstW]);
    counts.reset(new (std::nothrow) uint16_t[dstW]);
    if (!sums || !counts) {
      return DecodeResult::Failed;
    }
  }

  int loadedRow = -1;  // single gray-row cache: raster-order visits are monotonic
  for (int dstY = 0; dstY < dstH; ++dstY) {
    const int y0 = areaScale ? dstY * srcH / dstH : (dstY * srcH) / dstH;
    const int y1 = (dstY + 1) * srcH / dstH;
    const int yCount = areaScale ? (y1 - y0 == 0 ? 1 : y1 - y0) : 1;
    if (areaScale) {
      memset(sums.get(), 0, dstW * sizeof(uint32_t));
      for (int c = 0; c < dstW; ++c) counts[c] = 0;
    }
    for (int k = 0; k < yCount; ++k) {
      // Descending source-row visits keep the single-row cache valid.
      int sy;
      if (areaScale) {
        sy = (k == 0) ? y0 : ((y1 - k) > y0 ? y1 - k : y0);
      } else {
        sy = (dstY * srcH) / dstH;
        if (sy > srcH - 1) sy = srcH - 1;
      }
      // Foreground takeover aborts at the source-row boundary — the same
      // hook the GIF converter and the SDK row sink honor.
      if (abortPoll != nullptr && abortPoll(abortCtx)) {
        return DecodeResult::Busy;
      }
      if (sy != loadedRow) {
        if (!src.readRowGray(sy, grayRow, bits.get())) {
          return DecodeResult::Failed;
        }
        loadedRow = sy;
      }
      if (areaScale) {
        for (int dstX = 0; dstX < dstW; ++dstX) {
          const int x0 = dstX * srcW / dstW;
          const int x1 = (dstX + 1) * srcW / dstW;
          if (x1 > x0) {
            uint32_t sum = 0;
            for (int x = x0; x < x1; ++x) sum += grayRow[x];
            sums[dstX] += sum / (x1 - x0);
            ++counts[dstX];
          } else {
            // Vertical-only shrink (areaScale turned on by the height axis)
            // leaves an empty box span here — nearest-neighbor the column
            // instead of letting counts stay 0 (which bakes black).
            sums[dstX] += grayRow[x0 < srcW ? x0 : srcW - 1];
            ++counts[dstX];
          }
        }
      } else {
        // Nearest otherwise (the converter family's today-behavior): collect
        // the sampled luma; the row bakes once, below the source-row loop.
        for (int dstX = 0; dstX < dstW; ++dstX) {
          int srcX = dstX * srcW / dstW;
          if (srcX > srcW - 1) srcX = srcW - 1;
          avgRow[dstX] = grayRow[srcX];
        }
      }
    }
    if (areaScale) {
      for (int dstX = 0; dstX < dstW; ++dstX) {
        const uint32_t avg = counts[dstX] != 0 ? sums[dstX] / counts[dstX] : 0;
        avgRow[dstX] = static_cast<uint8_t>(avg > 255 ? 255 : avg);
      }
    }
    if (legacyPaint != nullptr) {
      // Storage-failure fallback: bake with the caller-prepared map and paint.
      bake->toneRow(avgRow, toneRow);
      paintToneRow(*legacyPaint, spec, dstY, toneRow, dstW);
    } else {
      // Phase 1: histogram + gray temp only (the remap pass bakes).
      bake->collectRow(avgRow);
      if (bake->grayWriteFailed()) return DecodeResult::Failed;  // engine legacy-passes
    }
  }
  return DecodeResult::Ok;
}

bool blitFromCache(GfxRenderer& renderer, const ImageSpec& spec, const char* dir, const bool qualityTone) {
  char path[256];
  if (!cacheNameFor(spec, dir, qualityTone, path, sizeof(path))) return false;
  HalFile cacheFile;
  if (!Storage.openFileForRead("TIMGE", path, cacheFile)) return false;
  uint16_t cachedWidth = 0;
  uint16_t cachedHeight = 0;
  if (!readValidCacheHeader(cacheFile, spec.w, spec.h, cachedWidth, cachedHeight)) {
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

// Paints one baked tone row at its placement (shared by every phase-2 remap).
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

// Legacy single-pass paint sink (review round 1): the storage-failure
// fallback bakes with the degenerate map straight from the decode rows —
// cache storage availability must not be a prerequisite for displaying an
// image (the pre-#301 paint-uncached contract).
struct LegacyPaintCtx {
  ttfimg::ToneBake4* bake = nullptr;
  const ttfimg::ImageSpec* spec = nullptr;
  GfxRenderer* paint = nullptr;
  uint8_t* toneRow = nullptr;  // [spec->w]
  bool (*abortPoll)(void* ctx) = nullptr;
  void* abortCtx = nullptr;
  bool aborted = false;
};

bool legacyPaintFn(void* user, const uint16_t rowIdx, const uint8_t* grayRow, const uint16_t width) {
  LegacyPaintCtx* ctx = static_cast<LegacyPaintCtx*>(user);
  if (ctx->aborted) return false;
  if (ctx->abortPoll != nullptr && ctx->abortPoll(ctx->abortCtx)) {
    ctx->aborted = true;
    return false;
  }
  if (ctx->bake == nullptr || ctx->toneRow == nullptr) return false;
  if (width != ctx->spec->w) {
    LOG_ERR("TIMGE", "SDK row width mismatch: %u != %u", width, ctx->spec->w);
    ctx->aborted = true;
    return false;
  }
  ctx->bake->toneRow(grayRow, ctx->toneRow);
  if (ctx->paint != nullptr) paintToneRow(*ctx->paint, *ctx->spec, rowIdx, ctx->toneRow, width);
  return true;
}

// Phase 2 of the adaptive bake (design 2026-11-08 §2.1 amendment 2): stream
// the .g gray temp back through the adaptive map + serpentine FS bake,
// appending the 2bpp cache rows and painting each row. ONE implementation for
// all four decoders — worker/foreground cache parity is this code path.
// Consumes the gray temp (removed on every exit path). `writePath` is the
// .t publish temp when publishing, else the final name (no-publish decode).
// `cacheStreamOk` (optional) reports whether the tone-cache stream ran to a
// finalized end — a degraded stream paints every row but produces no cache,
// so the caller must not publish (and reports success: the image displayed).
DecodeResult remapGrayToCache(const ImageSpec& spec, ttfimg::ToneBake4& bake, const char* writePath, const bool caching,
                              GfxRenderer* paint, const bool paintToTarget, bool (*abortPoll)(void* ctx),
                              void* abortCtx, bool* cacheStreamOk) {
  if (cacheStreamOk != nullptr) *cacheStreamOk = false;
  uint8_t toneRowStack[256];
  uint8_t* toneRow = toneRowStack;
  PoolBytes toneRowHeap;
  if (spec.w > static_cast<int>(sizeof(toneRowStack))) {
    toneRowHeap = poolMakeBytes(static_cast<size_t>(spec.w));
    toneRow = toneRowHeap.get();
  }
  uint8_t grayStack[256];
  uint8_t* grayRow = grayStack;
  PoolBytes grayRowHeap;
  if (spec.w > static_cast<int>(sizeof(grayStack))) {
    grayRowHeap = poolMakeBytes(static_cast<size_t>(spec.w));
    grayRow = grayRowHeap.get();
  }
  if (toneRow == nullptr || grayRow == nullptr) {
    bake.grayDiscard();
    return DecodeResult::Failed;
  }
  bake.beginRemap();

  ToneCacheWriter writer;
  bool writerActive = caching && writer.begin(writePath, spec.w, spec.h);
  if (caching && !writerActive) {
    // Storage failure on the cache stream is not a decode failure: keep
    // painting the rows and skip the cache (the next pass re-decodes).
    LOG_ERR("TIMGE", "Image cache open failed — painting uncached: %s", spec.href);
  }
  HalFile gray;
  if (!Storage.openFileForRead("TIMGE", bake.grayPath(), gray)) {
    if (writerActive) writer.abort();
    bake.grayDiscard();
    return DecodeResult::Failed;
  }
  for (int row = 0; row < spec.h; ++row) {
    if (abortPoll != nullptr && abortPoll(abortCtx)) {
      if (writerActive) writer.abort();
      gray.close();  // FAT refuses remove on an open handle
      bake.grayDiscard();
      return DecodeResult::Busy;
    }
    if (gray.read(grayRow, static_cast<size_t>(spec.w)) != spec.w) {
      LOG_ERR("TIMGE", "Gray temp read failed at row %d: %s", row, spec.href);
      if (writerActive) writer.abort();
      gray.close();
      bake.grayDiscard();
      return DecodeResult::Failed;
    }
    // Serpentine row bake over the adaptive map (beginRemap reset the error
    // state; the byte-exact contract moves to the 's' cache identity).
    bake.toneRow(grayRow, toneRow);
    if (writerActive && !writer.writeRow(toneRow)) {
      // The writer dropped the file; the remaining rows still paint (the
      // same degraded-paint contract the BMP path had).
      writerActive = false;
    }
    if (paint != nullptr && paintToTarget) paintToneRow(*paint, spec, row, toneRow, spec.w);
  }
  const bool streamOk = writerActive && writer.finalize();
  gray.close();  // FAT refuses remove on an open handle
  bake.grayDiscard();
  if (cacheStreamOk != nullptr) *cacheStreamOk = streamOk;
  if (writerActive && !streamOk) return DecodeResult::Failed;
  return DecodeResult::Ok;  // rows painted; cache written when writerActive
}

// Legacy single-pass paint fallback (review round 1): when the gray temp is
// unavailable (open failure, or a failed/short write), the image still
// paints — re-decoding with the deterministic degenerate map (fixed contrast
// 1.3, gamma off) straight to the target, no cache, no temp. A cache-only
// job (worker) has nothing to produce without storage: Ok, no-op.
DecodeResult legacyPaintPass(const ImageSpec& spec, const ImageInfo::Kind kind, freeink::book::BookSource& source,
                             const freeink::book::ZipCatalog& zip, freeink::book::Arena& scratch, Epub* epub,
                             const uint8_t* psramImage, const size_t imageSize, ttfimg::ToneBake4& bake,
                             GfxRenderer* paint, const bool paintToTarget, bool (*abortPoll)(void* ctx),
                             void* abortCtx) {
  bake.beginLegacyFallback();
  if (paint == nullptr || !paintToTarget) return DecodeResult::Ok;  // worker: no cache producible
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
    LegacyPaintCtx ctx;
    ctx.bake = &bake;
    ctx.spec = &spec;
    ctx.paint = paint;
    ctx.toneRow = toneRow;
    ctx.abortPoll = abortPoll;
    ctx.abortCtx = abortCtx;
    const BookStatus st = freeink::book::ImageRenderer::render(source, zip, sdkImage, scratch, &legacyPaintFn, &ctx);
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
  // BMP: the row decoder paints in legacy mode.
  return decodeBmpRows(spec, psramImage, imageSize, &bake, paint, abortPoll, abortCtx);
}

DecodeResult decodeAndCache(const ImageSpec& spec, const char* cachePath, freeink::book::BookSource& source,
                            const freeink::book::ZipCatalog& zip, freeink::book::Arena& scratch, Epub* epub,
                            GfxRenderer* paint, const bool qualityTone, const bool caching,
                            bool (*abortPoll)(void* ctx), void* abortCtx, const bool paintToTarget) {
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

  freeink::book::PageImage sdkImage{};
  sdkImage.href = href;
  sdkImage.x = spec.x;
  sdkImage.y = spec.y;
  sdkImage.width = static_cast<uint16_t>(spec.w);
  sdkImage.height = static_cast<uint16_t>(spec.h);

  // Abort tracking lives in the file anon namespace (see trackAbort).

  // JPEG/PNG under the legacy policy keep today's behavior exactly (§5.6):
  // no tone cache, SDK error-diffusion at paint — the worker declines them.
  if ((info.kind == ImageInfo::Kind::Png || info.kind == ImageInfo::Kind::Jpeg) && !qualityTone) {
    return DecodeResult::Skip;
  }

  if (info.kind == ImageInfo::Kind::Png || info.kind == ImageInfo::Kind::Jpeg) {
    // Quality decode (§5.3 corrected + §2.1 amendment 2): phase 1 — SDK gray
    // rows (area-averaged exactly as the foreground fallback's SDK stack)
    // feed the adaptive bake's collection (histogram + gray temp); then the
    // shared remap pass bakes, caches and paints. Decode cost on first view
    // is unchanged (one decode) plus one small disk pass; steady state is
    // the cache as before.
    ttfimg::ToneBake4 bake(spec.w);
    if (!bake.ok()) return DecodeResult::Failed;
    char grayPath[280];
    const int grayPathLen = snprintf(grayPath, sizeof(grayPath), "%s.g", cachePath);
    // Storage failure on the gray temp is not a decode failure: the image
    // still paints through the legacy single-pass fallback (review round 1).
    const bool collecting =
        grayPathLen > 0 && grayPathLen < static_cast<int>(sizeof(grayPath)) && bake.beginCollect(grayPath);
    if (!collecting) LOG_ERR("TIMGE", "Gray temp unavailable — painting uncached: %s", spec.href);

    GrayCollectCtx ctx;
    ctx.bake = &bake;
    ctx.w = static_cast<uint16_t>(spec.w);
    ctx.abortPoll = abortPoll;
    ctx.abortCtx = abortCtx;

    if (collecting) {
      ctx.bake = &bake;
      ctx.w = static_cast<uint16_t>(spec.w);
      ctx.abortPoll = abortPoll;
      ctx.abortCtx = abortCtx;

      const BookStatus st = freeink::book::ImageRenderer::render(source, zip, sdkImage, scratch, &grayCollectFn, &ctx);
      if (st != BookStatus::Ok || ctx.aborted) {
        bake.grayDiscard();
        if (ctx.aborted) return DecodeResult::Busy;
        // Gray temp died mid-collect: the phase-1 render stopped early and
        // the legacy pass still paints (storage-failure doctrine, round 1).
        if (ctx.collectFailed)
          return legacyPaintPass(spec, info.kind, source, zip, scratch, epub, nullptr, 0, bake, paint, paintToTarget,
                                 abortPoll, abortCtx);
        return st == BookStatus::Unsupported ? DecodeResult::Skip : DecodeResult::Failed;
      }
      if (bake.grayDone()) {
        bool cacheStreamOk = false;
        const DecodeResult remap =
            remapGrayToCache(spec, bake, writePath, caching, paint, paintToTarget, abortPoll, abortCtx, &cacheStreamOk);
        if (remap == DecodeResult::Busy) return remap;  // foreground takeover: stay out of the way
        if (remap != DecodeResult::Ok) {
          // Phase-2 storage failure (gray re-open/read): still paint — the
          // legacy pass repaints the whole box, replacing any remap rows.
          return legacyPaintPass(spec, info.kind, source, zip, scratch, epub, nullptr, 0, bake, paint, paintToTarget,
                                 abortPoll, abortCtx);
        }
        // Degraded cache stream: rows painted, no cache — skip the publish.
        if (publishing && !cacheStreamOk) return DecodeResult::Ok;
        if (!publishResult(cachePath, writePath, publishing)) return DecodeResult::Failed;
        return DecodeResult::Ok;
      }
      // A failed/short gray write: re-decode with the degenerate map and paint.
    }
    return legacyPaintPass(spec, info.kind, source, zip, scratch, epub, nullptr, 0, bake, paint, paintToTarget,
                           abortPoll, abortCtx);
  }

  if (info.kind == ImageInfo::Kind::Gif || info.kind == ImageInfo::Kind::Bmp) {
    // New formats route through the same adaptive two-phase bake (§2.1
    // amendment 2): phase 1 collects gray rows (the GIF converter in
    // collect-only mode, the BMP row decoder directly), then the shared
    // remap pass bakes/caches/paints. qualityTone=false keeps today's
    // behavior — nothing was rendered for these kinds before, so nothing
    // decodes.
    if (!qualityTone) return DecodeResult::Skip;
    if (epub == nullptr) return DecodeResult::Failed;

    size_t imageSize = 0;
    PoolBytes image{epub->extractItemToPsram(href, &imageSize)};
    if (!image) {
      LOG_ERR("TIMGE", "PSRAM extraction failed: %s", href);
      return DecodeResult::Failed;
    }

    char grayPath[280];
    ttfimg::ToneBake4 bake(spec.w);
    if (!bake.ok()) return DecodeResult::Failed;
    const int grayPathLen = snprintf(grayPath, sizeof(grayPath), "%s.g", cachePath);
    // Gray-temp unavailability is not a decode failure: the legacy fallback
    // below still paints (review round 1).
    const bool collecting =
        grayPathLen > 0 && grayPathLen < static_cast<int>(sizeof(grayPath)) && bake.beginCollect(grayPath);
    if (!collecting) {
      LOG_ERR("TIMGE", "Gray temp unavailable — painting uncached: %s", spec.href);
      bake.beginLegacyFallback();  // the legacy paint below uses the degenerate map
    }

    bool decoded = false;
    bool aborted = false;
    if (info.kind == ImageInfo::Kind::Gif) {
      // Select the converter by the SNIFFED kind, not the href: ImageProbe
      // identified this GIF from its bytes, so a mismatched or extensionless
      // href must still reach the GIF converter (it would otherwise get no
      // decoder and stay blank). The shared ToneBake4 collects; the converter
      // paints nothing and writes no cache in this mode (the remap pass owns
      // both). The renderer stays live for the converter's geometry reads;
      // it is simply not drawn through in collect mode.
      RenderConfig config;
      config.x = spec.x;
      config.y = spec.y;
      config.maxWidth = spec.w;
      config.maxHeight = spec.h;
      config.useGrayscale = true;
      config.useDithering = true;
      config.performanceMode = false;
      config.useExactDimensions = true;
      config.abortPoll = abortPoll;
      config.abortCtx = abortCtx;
      config.qualityTone = true;        // GIF bake policy (§5.6)
      config.toneCollect = collecting;  // §2.1: phase 1 only; false = legacy paint
      config.toneBake = &bake;
      AbortTracker tracker{abortPoll, abortCtx};
      config.abortPoll = &trackAbort;
      config.abortCtx = &tracker;
      ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(".gif");
      decoded = decoder != nullptr && paint != nullptr &&
                decoder->decodeToFramebuffer(image.get(), imageSize, *paint, config);
      aborted = tracker.fired;
    } else {
      const DecodeResult bmpRes = decodeBmpRows(spec, image.get(), imageSize, &bake,
                                                collecting || !paintToTarget ? nullptr : paint, abortPoll, abortCtx);
      decoded = bmpRes == DecodeResult::Ok;
      aborted = bmpRes == DecodeResult::Busy;
    }
    if (!decoded) {
      bake.grayDiscard();
      if (aborted) return DecodeResult::Busy;
      // Collect-mode decode failure painted nothing (e.g. a low-heap GIF
      // buffer failure): the legacy pass gets its own decode attempt before
      // the placement is given up. In legacy mode (!collecting) the failed
      // decode itself was the paint attempt — Failed stands.
      if (!collecting) return DecodeResult::Failed;
      return legacyPaintPass(spec, info.kind, source, zip, scratch, epub, image.get(), imageSize, bake, paint,
                             paintToTarget, abortPoll, abortCtx);
    }
    if (collecting && bake.grayDone()) {
      bool cacheStreamOk = false;
      const DecodeResult remap =
          remapGrayToCache(spec, bake, writePath, caching, paint, paintToTarget, abortPoll, abortCtx, &cacheStreamOk);
      if (remap == DecodeResult::Busy) return remap;  // foreground takeover: stay out of the way
      if (remap != DecodeResult::Ok) {
        // Phase-2 storage failure (gray re-open/read): still paint.
        return legacyPaintPass(spec, info.kind, source, zip, scratch, epub, image.get(), imageSize, bake, paint,
                               paintToTarget, abortPoll, abortCtx);
      }
      // Degraded cache stream: rows painted, no cache — skip the publish.
      if (publishing && !cacheStreamOk) return DecodeResult::Ok;
      if (!publishResult(cachePath, writePath, publishing)) return DecodeResult::Failed;
      return DecodeResult::Ok;
    }
    // !collecting: the decode above already painted through the legacy path
    // (converter quality paint / BMP legacy mode) — Ok with no cache.
    // collecting but a failed/short gray write: re-decode with the
    // deterministic degenerate map and paint.
    if (!collecting) return DecodeResult::Ok;
    return legacyPaintPass(spec, info.kind, source, zip, scratch, epub, image.get(), imageSize, bake, paint,
                           paintToTarget, abortPoll, abortCtx);
  }

  return DecodeResult::Skip;  // Kind::Unknown — not decodable (SVG/WebP)
}

bool decodeGatesOk() {
  // Per-image worker heap gates (#292 §3.4 rule 5 precedents): a decode that
  // would under-run the loop task's working set is deferred, not forced.
  const HalMemory::HeapStats internal = HalMemory::getInternalHeap();
  if (internal.freeBytes < 24 * 1024 || internal.largestBlockBytes < 8 * 1024) return false;
  if (HalMemory::getPsramHeap().freeBytes < 256 * 1024) return false;
  return true;
}

}  // namespace ttfimg

#endif  // CROSSPOINT_TTF_READER
