#include "GifToFramebufferConverter.h"

#include <AnimatedGIF.h>
#include <Arduino.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <GifCommon.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"

namespace {

struct GifContext {
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};
  int srcWidth{0};
  int srcHeight{0};
  int dstWidth{0};
  int dstHeight{0};
  float scale{1.f};
  int lastDstY{-1};
  int nextExpectedSrcY{0};
  const uint8_t* activePalette24{nullptr};
  uint8_t grayPalette[256] = {0};
  std::unique_ptr<uint8_t[]> grayRow;
  std::unique_ptr<uint8_t[]> whiteRow;
  PixelCache cache;
  bool caching{false};
  bool success{true};
  uint32_t lastYieldMs{0};
  // Quality tone mode (RenderConfig::qualityTone): area-averaged downscale
  // + the injected ToneBaker (RenderConfig::toneBake — the shared bake,
  // design 2026-11-08 §5.2) instead of nearest + Bayer. Allocation failure
  // or a missing bake silently falls back to the legacy path.
  bool quality{false};
  std::unique_ptr<uint32_t[]> qSums;    // [dstWidth] gray accumulation
  std::unique_ptr<uint16_t[]> qCounts;  // [dstWidth] samples per column
  std::unique_ptr<uint8_t[]> qGrayRow;  // [dstWidth] averaged luma handed to the bake
  std::unique_ptr<uint8_t[]> qToneRow;  // [dstWidth] baked 4-level tones
  ToneBaker* bake{nullptr};             // not owned; from RenderConfig::toneBake
  bool collect{false};                  // §2.1 phase 1: feed only, no paint/cache
  int qAccumDstY = -1;                  // dst row currently accumulating (-1 = none)
  int qFlushedRows = 0;                 // next destination row to bake (in order)
};

// Retired inline Floyd–Steinberg (qToneAt/qNextErrorRow): the bake comes from
// RenderConfig::toneBake so all four quality decodes share one implementation.

void updateGrayPalette(const GIFDRAW* pDraw, GifContext& ctx) {
  const uint8_t* palette24 = pDraw->pPalette24;
  if (palette24 == nullptr || palette24 == ctx.activePalette24) return;

  ctx.activePalette24 = palette24;
  for (int i = 0; i < 256; ++i) {
    const uint8_t* pixel = &palette24[i * 3];
    ctx.grayPalette[i] = static_cast<uint8_t>((pixel[0] * 77 + pixel[1] * 150 + pixel[2] * 29) >> 8);
  }
}

// --- quality tone mode (RenderConfig::qualityTone) ---------------------------

void qResetAccum(GifContext& ctx) {
  memset(ctx.qSums.get(), 0, static_cast<size_t>(ctx.dstWidth) * sizeof(uint32_t));
  memset(ctx.qCounts.get(), 0, static_cast<size_t>(ctx.dstWidth) * sizeof(uint16_t));
}

// Bakes one accumulated destination row (counts == 0 → white) and emits it
// to the pixel cache and the renderer. In collect mode (design 2026-11-08
// §2.1 amendment 2) the row only feeds the bake's phase-1 collection — the
// engine's remap pass owns tone, cache and paint.
void qFlushRow(const int dstY, GifContext& ctx) {
  for (int dstX = 0; dstX < ctx.dstWidth; ++dstX) {
    const uint32_t avg = ctx.qCounts[dstX] != 0 ? ctx.qSums[dstX] / ctx.qCounts[dstX] : 255;
    ctx.qGrayRow[dstX] = static_cast<uint8_t>(avg > 255 ? 255 : avg);
  }
  if (ctx.collect) {
    ctx.bake->collectRow(ctx.qGrayRow.get());
    ++ctx.qFlushedRows;  // qFlushRemainder's loop must still terminate
    return;
  }
  const int outY = ctx.config->y + dstY;
  DirectCacheWriter cw;
  const bool caching = ctx.caching;
  if (caching) {
    if (!ctx.cache.advanceTo(dstY)) {
      ctx.success = false;
      return;
    }
    cw.init(ctx.cache.buffer.get(), ctx.cache.bytesPerRow, ctx.cache.bandRows, ctx.cache.originX);
    cw.beginRow(outY, ctx.config->y + ctx.cache.bandStart);
  }
  DirectPixelWriter pw;
  pw.init(*ctx.renderer);
  // PortraitInverted swaps the axes inside DirectPixelWriter, so its internal
  // band guard tracks the wrong axis for off-panel rows — clip both here.
  const bool paintRow = outY >= 0 && outY < ctx.screenHeight;
  if (paintRow) pw.beginRow(outY);
  // Serpentine row bake: the bake visits the row in its own scan direction
  // and advances the error state (toneRow contract, design §5.2).
  ctx.bake->toneRow(ctx.qGrayRow.get(), ctx.qToneRow.get());
  for (int dstX = 0; dstX < ctx.dstWidth; ++dstX) {
    const uint8_t tone = ctx.qToneRow[dstX];
    const int outX = ctx.config->x + dstX;
    if (paintRow && outX >= 0 && outX < ctx.screenWidth) pw.writePixel(outX, tone);
    if (caching) cw.writePixel(outX, tone);
  }
  if (dstY == ctx.qFlushedRows) ++ctx.qFlushedRows;
}

// Quality-mode core of renderCanvasRow: feed one completed source row.
void qRenderCanvasRow(const uint8_t* grayRow, const int srcY, GifContext& ctx) {
  int firstDstY = (srcY * ctx.dstHeight) / ctx.srcHeight;
  const bool upscale = ctx.dstHeight >= ctx.srcHeight;
  if (upscale) {
    // Each destination row's box covers a single source row: bake it whole.
    int endDstY = firstDstY + 1;
    if (ctx.dstHeight > ctx.srcHeight) endDstY = ((srcY + 1) * ctx.dstHeight) / ctx.srcHeight;
    if (endDstY > ctx.dstHeight) endDstY = ctx.dstHeight;
    for (int dstY = firstDstY; dstY < endDstY; ++dstY) {
      qResetAccum(ctx);
      for (int dstX = 0; dstX < ctx.dstWidth; ++dstX) {
        const int x0 = dstX * ctx.srcWidth / ctx.dstWidth;
        const int x1 = (dstX + 1) * ctx.srcWidth / ctx.dstWidth;
        if (x1 <= x0) {
          // Horizontally enlarged: the box span is empty. Nearest-neighbor
          // the column instead of leaving qCounts at 0, which qFlushRow
          // bakes as white.
          ctx.qSums[dstX] = grayRow[x0 < ctx.srcWidth ? x0 : ctx.srcWidth - 1];
          ctx.qCounts[dstX] = 1;
          continue;
        }
        uint32_t sum = 0;
        for (int x = x0; x < x1; ++x) sum += grayRow[x];
        ctx.qSums[dstX] = sum / (x1 - x0);
        ctx.qCounts[dstX] = 1;
      }
      qFlushRow(dstY, ctx);
      if (!ctx.success) return;
    }
    ctx.qAccumDstY = -1;
    return;
  }

  // Downscale: source rows accumulate into their shared destination row;
  // the row bakes when the source band moves past it. The band mapping is
  // the inverse of the BMP box filter's destination boxes: source row srcY
  // belongs to the destination row whose [dst*srcH/dstH, (dst+1)*srcH/dstH)
  // box contains it — floor(((srcY+1)*dstH - 1)/srcH) — so a 5-to-2 shrink
  // averages rows 0–1 into dst 0, not rows 0–2.
  firstDstY = ((srcY + 1) * ctx.dstHeight - 1) / ctx.srcHeight;
  if (ctx.qAccumDstY != firstDstY) {
    if (ctx.qAccumDstY >= 0) {
      qFlushRow(ctx.qAccumDstY, ctx);
      if (!ctx.success) return;
    }
    ctx.qAccumDstY = firstDstY;
    qResetAccum(ctx);
  }
  for (int dstX = 0; dstX < ctx.dstWidth; ++dstX) {
    const int x0 = dstX * ctx.srcWidth / ctx.dstWidth;
    const int x1 = (dstX + 1) * ctx.srcWidth / ctx.dstWidth;
    if (x1 <= x0) {
      // Mixed-axis resize (vertical shrink, horizontal enlargement): the box
      // span is empty here — nearest-neighbor the column so qCounts stays
      // populated and the bake neither skips nor whitens it.
      ctx.qSums[dstX] += grayRow[x0 < ctx.srcWidth ? x0 : ctx.srcWidth - 1];
      ++ctx.qCounts[dstX];
      continue;
    }
    uint32_t sum = 0;
    for (int x = x0; x < x1; ++x) sum += grayRow[x];
    ctx.qSums[dstX] += sum / (x1 - x0);
    ++ctx.qCounts[dstX];
  }
}

// End of frame: bake the pending row and any destination row no source row
// covered (white). qFlushRow advances qFlushedRows, so everything below the
// pending accumulator is already on disk.
void qFlushRemainder(GifContext& ctx) {
  if (ctx.qAccumDstY >= 0) {
    qFlushRow(ctx.qAccumDstY, ctx);
    ctx.qAccumDstY = -1;
    if (!ctx.success) return;
  }
  while (ctx.qFlushedRows < ctx.dstHeight) {
    qResetAccum(ctx);
    qFlushRow(ctx.qFlushedRows, ctx);
    if (!ctx.success) return;
  }
}

void renderCanvasRow(const uint8_t* grayRow, int srcY, GifContext& ctx) {
  if (ctx.quality) {
    qRenderCanvasRow(grayRow, srcY, ctx);
    return;
  }
  // Map source rows using the exact output-height ratio. During downscaling,
  // multiple source rows may map to the same output row; during upscaling, a
  // source row must be repeated across every destination row in its range.
  int firstDstY = (srcY * ctx.dstHeight) / ctx.srcHeight;
  int endDstY = firstDstY + 1;
  if (ctx.dstHeight > ctx.srcHeight) {
    endDstY = ((srcY + 1) * ctx.dstHeight) / ctx.srcHeight;
  }

  if (firstDstY <= ctx.lastDstY) firstDstY = ctx.lastDstY + 1;
  if (firstDstY >= endDstY || firstDstY >= ctx.dstHeight) return;
  if (endDstY > ctx.dstHeight) endDstY = ctx.dstHeight;

  DirectPixelWriter pw;
  pw.init(*ctx.renderer);

  for (int dstY = firstDstY; dstY < endDstY; ++dstY) {
    ctx.lastDstY = dstY;
    const int outY = ctx.config->y + dstY;
    if (outY < 0 || outY >= ctx.screenHeight) continue;

    pw.beginRow(outY);

    const bool caching = ctx.caching;
    DirectCacheWriter cw;
    if (caching) {
      if (!ctx.cache.advanceTo(dstY)) {
        ctx.success = false;
        return;
      } else {
        // Fork: PixelCache::buffer is a PoolBytes (PSRAM-backed), not a raw pointer.
        cw.init(ctx.cache.buffer.get(), ctx.cache.bytesPerRow, ctx.cache.bandRows, ctx.cache.originX);
        cw.beginRow(outY, ctx.config->y + ctx.cache.bandStart);
      }
    }

    int srcX = 0;
    int error = 0;
    const int outXBase = ctx.config->x;
    const bool useDithering = ctx.config->useDithering;

    for (int dstX = 0; dstX < ctx.dstWidth; ++dstX) {
      const int outX = outXBase + dstX;
      if (outX >= 0 && outX < ctx.screenWidth) {
        const uint8_t gray = grayRow[srcX];
        uint8_t ditheredGray;
        if (useDithering) {
          ditheredGray = applyBayerDither4Level(gray, outX, outY);
        } else {
          ditheredGray = gray / 85;
          if (ditheredGray > 3) ditheredGray = 3;
        }
        pw.writePixel(outX, ditheredGray);
        if (caching) cw.writePixel(outX, ditheredGray);
      }

      error += ctx.srcWidth;
      while (error >= ctx.dstWidth) {
        error -= ctx.dstWidth;
        srcX++;
        if (srcX >= ctx.srcWidth) {
          srcX = ctx.srcWidth - 1;
          break;
        }
      }
    }
  }
}

void processGapRowsUpTo(int canvasY, GifContext& ctx) {
  while (ctx.success && ctx.nextExpectedSrcY < canvasY && ctx.nextExpectedSrcY < ctx.srcHeight) {
    renderCanvasRow(ctx.whiteRow.get(), ctx.nextExpectedSrcY, ctx);
    ctx.nextExpectedSrcY++;
  }
}

void* gifOpenWithHandle(const char* filename, int32_t* size) {
  auto file = makeUniqueNoThrow<HalFile>();
  if (!file) {
    LOG_ERR("GIF", "OOM: file handle for %s", filename);
    return nullptr;
  }
  if (!Storage.openFileForRead("GIF", std::string(filename), *file)) {
    LOG_ERR("GIF", "Failed to open decoder source: %s", filename);
    return nullptr;
  }
  *size = static_cast<int32_t>(file->size());
  return file.release();
}

void gifCloseWithHandle(void* handle) {
  auto* file = reinterpret_cast<HalFile*>(handle);
  if (!file) return;
  file->close();
  delete file;
}

int32_t gifReadWithHandle(GIFFILE* pFile, uint8_t* pBuf, int32_t len) {
  auto* file = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!file) {
    LOG_ERR("GIF", "Read requested without a file handle");
    return 0;
  }
  int32_t bytesRead = file->read(pBuf, len);
  if (bytesRead < 0) {
    LOG_ERR("GIF", "Read failed at offset %d for %d bytes", pFile->iPos, len);
    return 0;
  }
  pFile->iPos += bytesRead;
  return bytesRead;
}

int32_t gifSeekWithHandle(GIFFILE* pFile, int32_t position) {
  auto* file = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!file) {
    LOG_ERR("GIF", "Seek requested without a file handle");
    return -1;
  }
  if (position < 0) position = 0;
  if (position >= pFile->iSize) position = pFile->iSize - 1;
  if (!file->seek(position)) {
    LOG_ERR("GIF", "Seek failed at offset %d", position);
    return -1;
  }
  pFile->iPos = position;
  return position;
}

void gifDrawCallback(GIFDRAW* pDraw) {
  auto* ctx = reinterpret_cast<GifContext*>(pDraw->pUser);
  if (!ctx || !ctx->success || !ctx->grayRow || !ctx->whiteRow) return;

  // Predecode abort (worker decodes only; the foreground leaves the hook
  // null): clearing success makes every remaining row return here
  // immediately and fails the decode, whose existing !ctx.success path drops
  // the partial cache file.
  if (ctx->config->abortPoll != nullptr && ctx->config->abortPoll(ctx->config->abortCtx)) {
    ctx->success = false;
    return;
  }

  ImageToFramebufferDecoder::yieldDuringDecode(ctx->lastYieldMs);

  const int canvasY = pDraw->iY + pDraw->y;
  if (canvasY < 0 || canvasY >= ctx->srcHeight) {
    ctx->success = false;
    return;
  }

  processGapRowsUpTo(canvasY, *ctx);
  if (!ctx->success) return;

  memcpy(ctx->grayRow.get(), ctx->whiteRow.get(), static_cast<size_t>(ctx->srcWidth));
  updateGrayPalette(pDraw, *ctx);

  const uint8_t* pixels = pDraw->pPixels;
  const int startX = pDraw->iX;
  const int endX = startX + pDraw->iWidth;
  const bool hasTransparency = pDraw->ucHasTransparency != 0;
  const uint8_t transparent = pDraw->ucTransparent;

  for (int srcX = startX, i = 0; srcX < endX && srcX < ctx->srcWidth; ++srcX, ++i) {
    if (srcX < 0) continue;
    const uint8_t index = pixels[i];
    if (hasTransparency && index == transparent) continue;
    ctx->grayRow[srcX] = ctx->grayPalette[index];
  }

  renderCanvasRow(ctx->grayRow.get(), canvasY, *ctx);
  ctx->nextExpectedSrcY = canvasY + 1;
}

bool readGifInfoFromPath(const std::string& imagePath, GifBasicInfo& info) {
  HalFile file;
  if (!Storage.openFileForRead("GIF", imagePath, file)) {
    LOG_ERR("GIF", "Failed to open source for header inspection: %s", imagePath.c_str());
    return false;
  }
  return GifCommon::readBasicInfo(file, info);
}

constexpr size_t GIF_DECODER_APPROX_SIZE = sizeof(AnimatedGIF);
constexpr size_t MIN_FREE_HEAP_FOR_GIF = GIF_DECODER_APPROX_SIZE + 16 * 1024;

}  // namespace

bool GifToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  GifBasicInfo info;
  if (!readGifInfoFromPath(imagePath, info)) {
    LOG_ERR("GIF", "Failed to read GIF dimensions: %s", imagePath.c_str());
    return false;
  }
  return validateAndStoreDimensions(info.canvasWidth, info.canvasHeight, out, "GIF");
}

bool GifToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  return decodeGifSource(imagePath.c_str(), nullptr, 0, renderer, config);
}

bool GifToFramebufferConverter::decodeToFramebuffer(uint8_t* data, const size_t size, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  if (data == nullptr) return false;
  return decodeGifSource(nullptr, data, size, renderer, config);
}

bool GifToFramebufferConverter::decodeGifSource(const char* imagePath, uint8_t* mem, const size_t memSize,
                                                GfxRenderer& renderer, const RenderConfig& config) {
  const char* label = mem != nullptr ? "(memory GIF)" : imagePath;

  GifBasicInfo info;
  bool infoParsed = false;
  if (mem != nullptr) {
    HalMemoryFile memFile(mem, memSize);
    infoParsed = GifCommon::readBasicInfoFrom(memFile, info);
  } else {
    infoParsed = readGifInfoFromPath(imagePath, info);
  }
  if (!infoParsed) {
    LOG_ERR("GIF", "Failed to parse GIF header: %s", label);
    return false;
  }
  if (info.interlaced) {
    warnUnsupportedFeature("interlaced GIF", label);
    return false;
  }
  if (info.canvasWidth > MAX_WIDTH) {
    LOG_ERR("GIF",
            "GIF canvas too wide for AnimatedGIF: canvas=%ux%u MAX_WIDTH=%d "
            "renderTarget=%dx%d file=%s",
            info.canvasWidth, info.canvasHeight, MAX_WIDTH, config.maxWidth, config.maxHeight, label);
    return false;
  }
  ImageDimensions sourceDimensions;

  if (!validateAndStoreDimensions(info.canvasWidth, info.canvasHeight, sourceDimensions, "GIF")) return false;

  const size_t freeHeap = HalMemory::getInternalHeap().freeBytes;
  if (freeHeap < MIN_FREE_HEAP_FOR_GIF) {
    LOG_ERR("GIF", "Not enough heap for GIF decoder (%u free, need %u)", freeHeap, MIN_FREE_HEAP_FOR_GIF);
    return false;
  }

  auto gif = makeUniqueNoThrow<AnimatedGIF>();
  if (!gif) {
    LOG_ERR("GIF", "OOM: AnimatedGIF decoder");
    return false;
  }

  auto ctxPtr = makeUniqueNoThrow<GifContext>();
  if (!ctxPtr) {
    LOG_ERR("GIF", "OOM: GIF context");
    return false;
  }
  GifContext& ctx = *ctxPtr;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();
  ctx.srcWidth = sourceDimensions.width;
  ctx.srcHeight = sourceDimensions.height;

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    ctx.dstWidth = config.maxWidth;
    ctx.dstHeight = config.maxHeight;
    ctx.scale = static_cast<float>(ctx.dstWidth) / ctx.srcWidth;
  } else {
    const float scaleX = static_cast<float>(config.maxWidth) / ctx.srcWidth;
    const float scaleY = static_cast<float>(config.maxHeight) / ctx.srcHeight;
    ctx.scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (ctx.scale > 1.0f) ctx.scale = 1.0f;
    ctx.dstWidth = static_cast<int>(ctx.srcWidth * ctx.scale);
    ctx.dstHeight = static_cast<int>(ctx.srcHeight * ctx.scale);
  }
  if (ctx.dstWidth <= 0 || ctx.dstHeight <= 0) {
    LOG_ERR("GIF", "Degenerate output dimensions %dx%d for %s", ctx.dstWidth, ctx.dstHeight, label);
    return false;
  }

  ctx.grayRow = makeUniqueNoThrow<uint8_t[]>(ctx.srcWidth);
  ctx.whiteRow = makeUniqueNoThrow<uint8_t[]>(ctx.srcWidth);
  if (!ctx.grayRow || !ctx.whiteRow) {
    LOG_ERR("GIF", "OOM: GIF row buffers (%d bytes each)", ctx.srcWidth);
    return false;
  }
  memset(ctx.whiteRow.get(), 255, static_cast<size_t>(ctx.srcWidth));

  if (config.qualityTone) {
    // Quality bake state; allocation failure (or a missing injected bake)
    // degrades to the legacy path rather than failing the decode. Partially
    // allocated buffers are released so the fallback's cache band can still
    // allocate under pressure. In collect mode (§2.1) no tone row buffer is
    // needed — the remap pass bakes.
    ctx.qSums = makeUniqueNoThrow<uint32_t[]>(ctx.dstWidth);
    ctx.qCounts = makeUniqueNoThrow<uint16_t[]>(ctx.dstWidth);
    ctx.qGrayRow = makeUniqueNoThrow<uint8_t[]>(ctx.dstWidth);
    if (!config.toneCollect) ctx.qToneRow = makeUniqueNoThrow<uint8_t[]>(ctx.dstWidth);
    if (ctx.qSums && ctx.qCounts && ctx.qGrayRow && (config.toneCollect || ctx.qToneRow) &&
        config.toneBake != nullptr) {
      ctx.bake = config.toneBake;
      ctx.collect = config.toneCollect;
      ctx.quality = true;
    } else {
      LOG_ERR("GIF", "Quality bake unavailable — falling back to legacy dither");
      ctx.qSums.reset();
      ctx.qCounts.reset();
      ctx.qGrayRow.reset();
      ctx.qToneRow.reset();
    }
  }

  gif->begin(GIF_PALETTE_RGB888);
  gif->setDrawType(GIF_DRAW_RAW);
  LOG_DBG("GIF",
          "GIF header: canvas=%ux%u frame=%ux%u frameOffset=%u,%u interlaced=%s "
          "renderTarget=%dx%d MAX_WIDTH=%d freeHeap=%u file=%s",
          info.canvasWidth, info.canvasHeight, info.frameWidth, info.frameHeight, info.frameX, info.frameY,
          info.interlaced ? "yes" : "no", config.maxWidth, config.maxHeight, MAX_WIDTH,
          HalMemory::getInternalHeap().freeBytes, label);
  const int rcOpen = mem != nullptr ? gif->open(mem, static_cast<int>(memSize), gifDrawCallback)
                                    : gif->open(label, gifOpenWithHandle, gifCloseWithHandle, gifReadWithHandle,
                                                gifSeekWithHandle, gifDrawCallback);
  const ScopedCleanup cleanup{[&gif]() { gif->close(); }};
  if (rcOpen != 1) {
    const int err = gif->getLastError();
    LOG_ERR("GIF", "AnimatedGIF open failed: err=%d canvas=%ux%u MAX_WIDTH=%d file=%s", err, info.canvasWidth,
            info.canvasHeight, MAX_WIDTH, label);
    return false;
  }

  ctx.caching = !config.cachePath.empty();
  if (ctx.caching) {
    if (!ctx.cache.begin(config.cachePath, ctx.dstWidth, ctx.dstHeight, config.x, config.y, 1)) {
      // Storage failure on the cache stream is not a decode failure: keep
      // painting the frame and skip the cache (the next pass re-decodes).
      LOG_ERR("GIF", "Cache stream start failed — painting uncached: %s", label);
      ctx.caching = false;
    }
  }

  int delayMs = 0;
  ctx.lastYieldMs = millis();
  const int frameState = gif->playFrame(false, &delayMs, &ctx);
  if (!ctx.success || (gif->getLastError() != GIF_SUCCESS && gif->getLastError() != GIF_EMPTY_FRAME)) {
    LOG_ERR("GIF", "GIF decode failed (state=%d, err=%d)", frameState, gif->getLastError());
    if (ctx.caching) ctx.cache.abort();
    return false;
  }

  processGapRowsUpTo(ctx.srcHeight, ctx);
  if (!ctx.success) {
    LOG_ERR("GIF", "GIF post-process failed: %s", label);
    if (ctx.caching) ctx.cache.abort();
    return false;
  }
  if (ctx.quality) {
    qFlushRemainder(ctx);
    if (!ctx.success) {
      LOG_ERR("GIF", "GIF quality flush failed: %s", label);
      if (ctx.caching) ctx.cache.abort();
      return false;
    }
  }

  if (frameState > 0) {
    // `label` (never the raw imagePath): the memory entry point passes a
    // null imagePath and the warning stringifies its argument.
    warnUnsupportedFeature("animation (using first frame only)", label);
  }

  if (ctx.caching && !ctx.cache.finalize()) {
    LOG_ERR("GIF", "Failed to finalize pixel cache: %s", config.cachePath.c_str());
    return false;
  }

  return true;
}

bool GifToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasGifExtension(extension);
}
