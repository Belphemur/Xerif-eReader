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
// first (foreground takeover is instant), then bakes tone, appends the cache
// row, and paints the same tone through the target renderer.
struct RowBakeCtx {
  ttfimg::ToneCacheWriter* cache = nullptr;
  GfxRenderer* paint = nullptr;
  ttfimg::FloydSteinberg4* bake = nullptr;
  uint8_t* toneRow = nullptr;  // decode-local scratch [width]
  int16_t x = 0;
  int16_t y = 0;
  uint16_t w = 0;
  bool (*abortPoll)(void* ctx) = nullptr;
  void* abortCtx = nullptr;
  bool aborted = false;
};

bool rowBakeFn(void* user, const uint16_t rowIdx, const uint8_t* grayRow, const uint16_t width) {
  RowBakeCtx* ctx = static_cast<RowBakeCtx*>(user);
  if (ctx->aborted) return false;
  if (ctx->abortPoll != nullptr && ctx->abortPoll(ctx->abortCtx)) {
    ctx->aborted = true;
    return false;
  }
  if (ctx->bake == nullptr || ctx->cache == nullptr || ctx->toneRow == nullptr) return false;
  if (width != ctx->w) {
    LOG_ERR("TIMGE", "SDK row width mismatch: %u != %u", width, ctx->w);
    ctx->aborted = true;
    return false;
  }
  for (uint16_t x = 0; x < width; ++x) {
    ctx->toneRow[x] = ctx->bake->tone(static_cast<int>(x), grayRow[x]);
  }
  ctx->bake->nextRow();
  if (!ctx->cache->writeRow(ctx->toneRow)) {
    ctx->aborted = true;  // cache write failed: stop, the writer already dropped the file
    return false;
  }
  if (ctx->paint != nullptr) {
    DirectPixelWriter pw;
    pw.init(*ctx->paint);
    pw.beginRow(ctx->y + static_cast<int>(rowIdx));
    for (uint16_t col = 0; col < width; ++col) {
      pw.writePixel(ctx->x + static_cast<int>(col), ctx->toneRow[col]);
    }
  }
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
      const uint32_t compression = static_cast<uint32_t>(info[14]) | (static_cast<uint32_t>(info[15]) << 8) |
                                   (static_cast<uint32_t>(info[16]) << 16) | (static_cast<uint32_t>(info[17]) << 24);
      const uint32_t clrUsed = static_cast<uint32_t>(info[28]) | (static_cast<uint32_t>(info[29]) << 8) |
                               (static_cast<uint32_t>(info[30]) << 16) | (static_cast<uint32_t>(info[31]) << 24);
      if (w <= 0 || h == 0 || w > 0xFFFF || h > 0xFFFF) return false;
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

// Quality BMP decode: area-averaged downscale for ≥2x reductions (§5.6),
// nearest-neighbor otherwise (the converter family's today-behavior),
// Floyd–Steinberg tone bake, cache rows + paint through `paint`.
DecodeResult decodeBmpRows(const ImageSpec& spec, const uint8_t* data, const size_t size, GfxRenderer* paint,
                           const char* cachePath, const bool caching) {
  if (paint == nullptr && !caching) return DecodeResult::Failed;  // nothing to produce
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
  ttfimg::ToneCacheWriter writer;
  const bool writerActive = caching && writer.begin(cachePath, dstW, dstH);
  if (caching && !writerActive) return DecodeResult::Failed;

  const size_t bitsBytes = src.rowBytes;
  PoolBytes bits{poolMakeBytes(bitsBytes ? bitsBytes : 1)};
  PoolBytes gray{poolMakeBytes(srcW + dstW)};  // gray row (srcW) + tone row (dstW) tail
  ttfimg::FloydSteinberg4 bake(dstW);
  if (!bits || !gray || !bake.ok()) {
    writer.abort();
    return DecodeResult::Failed;
  }
  uint8_t* grayRow = gray.get();
  uint8_t* toneRow = grayRow + srcW;

  std::unique_ptr<uint32_t[]> sums;
  std::unique_ptr<uint16_t[]> counts;
  if (areaScale) {
    sums.reset(new (std::nothrow) uint32_t[dstW]);
    counts.reset(new (std::nothrow) uint16_t[dstW]);
    if (!sums || !counts) {
      writer.abort();
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
      if (sy != loadedRow) {
        if (!src.readRowGray(sy, grayRow, bits.get())) {
          writer.abort();
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
        // Nearest otherwise (the converter family's today-behavior).
        for (int dstX = 0; dstX < dstW; ++dstX) {
          int srcX = dstX * srcW / dstW;
          if (srcX > srcW - 1) srcX = srcW - 1;
          toneRow[dstX] = bake.tone(dstX, grayRow[srcX]);
        }
        bake.nextRow();
        if (writerActive && !writer.writeRow(toneRow)) return DecodeResult::Failed;
        if (paint != nullptr) {
          DirectPixelWriter pw;
          pw.init(*paint);
          pw.beginRow(spec.y + dstY);
          for (int col = 0; col < dstW; ++col) {
            pw.writePixel(spec.x + col, toneRow[col]);
          }
        }
        continue;
      }
    }
    if (!areaScale) continue;  // row already written by the nearest branch
    for (int dstX = 0; dstX < dstW; ++dstX) {
      const uint32_t avg = counts[dstX] != 0 ? sums[dstX] / counts[dstX] : 0;
      toneRow[dstX] = bake.tone(dstX, static_cast<int>(avg > 255 ? 255 : avg));
    }
    bake.nextRow();
    if (writerActive && !writer.writeRow(toneRow)) return DecodeResult::Failed;
    if (paint != nullptr) {
      DirectPixelWriter pw;
      pw.init(*paint);
      pw.beginRow(spec.y + dstY);
      for (int col = 0; col < dstW; ++col) {
        pw.writePixel(spec.x + col, toneRow[col]);
      }
    }
  }
  if (!writerActive || writer.finalize()) return DecodeResult::Ok;
  return DecodeResult::Failed;
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
    pw.beginRow(spec.y + row);
    for (int col = 0; col < cachedWidth; ++col) {
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      const uint8_t tone = (rowBytes[col >> 2] >> bitShift) & 0x03;
      pw.writePixel(spec.x + col, tone);
    }
  }
  return true;
}

DecodeResult decodeAndCache(const ImageSpec& spec, const char* cachePath, freeink::book::BookSource& source,
                            const freeink::book::ZipCatalog& zip, freeink::book::Arena& scratch, Epub* epub,
                            GfxRenderer* paint, const bool qualityTone, const bool caching,
                            bool (*abortPoll)(void* ctx), void* abortCtx, const bool paintToTarget) {
  const ZipEntry* entry = zip.find(spec.href);
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
  sdkImage.href = spec.href;
  sdkImage.x = spec.x;
  sdkImage.y = spec.y;
  sdkImage.width = static_cast<uint16_t>(spec.w);
  sdkImage.height = static_cast<uint16_t>(spec.h);

  // JPEG/PNG under the legacy policy keep today's behavior exactly (§5.6):
  // no tone cache, SDK error-diffusion at paint — the worker declines them.
  if ((info.kind == ImageInfo::Kind::Png || info.kind == ImageInfo::Kind::Jpeg) && !qualityTone) {
    return DecodeResult::Skip;
  }

  if (info.kind == ImageInfo::Kind::Png || info.kind == ImageInfo::Kind::Jpeg) {
    // Quality decode (§5.3 corrected): SDK gray rows — area-averaged exactly
    // as the foreground fallback's SDK stack — baked to 4-level tone while
    // appending the cache, and painted through the target row by row. A
    // cache-only job (worker) skips the placement writes entirely.
    ttfimg::ToneCacheWriter writer;
    if (caching && !writer.begin(writePath, spec.w, spec.h)) return DecodeResult::Failed;
    ttfimg::ToneCacheWriter* writerPtr = caching ? &writer : nullptr;

    ttfimg::FloydSteinberg4 bake(spec.w);
    uint8_t toneRowStack[256];
    uint8_t* toneRow = toneRowStack;
    PoolBytes toneRowHeap;
    if (spec.w > static_cast<int>(sizeof(toneRowStack))) {
      toneRowHeap = poolMakeBytes(static_cast<size_t>(spec.w));
      toneRow = toneRowHeap.get();
    }
    if (toneRow == nullptr || !bake.ok()) {
      if (writerPtr != nullptr) writerPtr->abort();
      return DecodeResult::Failed;
    }

    RowBakeCtx ctx;
    ctx.cache = writerPtr;
    ctx.paint = paintToTarget ? paint : nullptr;
    ctx.bake = &bake;
    ctx.toneRow = toneRow;
    ctx.x = spec.x;
    ctx.y = spec.y;
    ctx.w = static_cast<uint16_t>(spec.w);
    ctx.abortPoll = abortPoll;
    ctx.abortCtx = abortCtx;

    const BookStatus st = freeink::book::ImageRenderer::render(source, zip, sdkImage, scratch, &rowBakeFn, &ctx);
    if (st == BookStatus::Ok && !ctx.aborted) {
      if (writerPtr != nullptr && !writerPtr->finalize()) return DecodeResult::Failed;
      if (!publishResult(cachePath, writePath, publishing)) return DecodeResult::Failed;
      return DecodeResult::Ok;
    }
    if (writerPtr != nullptr) writerPtr->abort();
    return ctx.aborted ? DecodeResult::Busy
                       : (st == BookStatus::Unsupported ? DecodeResult::Skip : DecodeResult::Failed);
  }

  if (info.kind == ImageInfo::Kind::Gif || info.kind == ImageInfo::Kind::Bmp) {
    // New formats: the converters/BMP decode writes the pixel cache AND
    // paints through the target renderer. qualityTone=false keeps today's
    // behavior — nothing was rendered for these kinds before, so nothing
    // decodes.
    if (!qualityTone) return DecodeResult::Skip;
    if (epub == nullptr || paint == nullptr) return DecodeResult::Failed;

    size_t imageSize = 0;
    PoolBytes image{epub->extractItemToPsram(spec.href, &imageSize)};
    if (!image) {
      LOG_ERR("TIMGE", "PSRAM extraction failed: %s", spec.href);
      return DecodeResult::Failed;
    }

    RenderConfig config;
    config.x = spec.x;
    config.y = spec.y;
    config.maxWidth = spec.w;
    config.maxHeight = spec.h;
    config.useGrayscale = true;
    config.useDithering = true;
    config.performanceMode = false;
    config.useExactDimensions = true;
    if (caching) {
      config.cachePath = writePath;
    }
    config.abortPoll = abortPoll;
    config.abortCtx = abortCtx;
    config.qualityTone = qualityTone;  // GIF bake policy (§5.6)

    bool decoded = false;
    if (info.kind == ImageInfo::Kind::Gif) {
      // Select the converter by the SNIFFED kind, not the href: ImageProbe
      // identified this GIF from its bytes, so a mismatched or extensionless
      // href must still reach the GIF converter (it would otherwise get no
      // decoder and stay blank).
      ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(".gif");
      decoded = decoder != nullptr && decoder->decodeToFramebuffer(image.get(), imageSize, *paint, config);
    } else {
      const DecodeResult bmpRes = decodeBmpRows(spec, image.get(), imageSize, paintToTarget ? paint : nullptr,
                                                caching ? writePath : nullptr, caching);
      // Lossless BMP: leave rows EMPTY in the cache on decode failure so a
      // later view re-decodes rather than trusting a torn cache.
      decoded = bmpRes == DecodeResult::Ok;
    }
    if (!decoded) return DecodeResult::Failed;
    if (!publishResult(cachePath, writePath, publishing)) return DecodeResult::Failed;
    return DecodeResult::Ok;
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
