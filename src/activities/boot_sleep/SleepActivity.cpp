#include "SleepActivity.h"

#include <BitmapHelpers.h>
#include <Epub.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <PNGdec.h>
#include <Xtc.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "activities/reader/BookCachePath.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120Draw.h"
#include "images/MoonIcon.h"
#include "util/BookProgress.h"

namespace {
// Metalio: B/W sleep images use the full 0xF7 waveform instead of HALF (two
// 0xFC partials), matching the clean pass before absolute gray below.
#if FREEINK_DEVICE_METALIO_EINK4
constexpr auto kSleepClean = HalDisplay::FULL_REFRESH;
#else
constexpr auto kSleepClean = HalDisplay::HALF_REFRESH;
#endif

// The absolute gray paint has no base pass. On Metalio the prior screen shows
// through it after the panel sits unpowered, so clean with 0xF7 first.
void cleanBeforeAbsoluteGray(GfxRenderer& renderer) {
#if FREEINK_DEVICE_METALIO_EINK4
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
#else
  (void)renderer;
#endif
}

HalDisplay::GrayscaleMode sleepGrayscaleMode(const GfxRenderer& renderer) {
  return renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Direct).supported()
             ? HalDisplay::GrayscaleMode::Direct
             : HalDisplay::GrayscaleMode::Absolute;
}

// Kept separate from normal custom sleep sources so alpha-overlay art does not mix with full-screen wallpapers.
constexpr char TRANSPARENT_SLEEP_ROOT_BMP[] = "/sleep-overlay.bmp";
constexpr char TRANSPARENT_SLEEP_ROOT_PNG[] = "/sleep-overlay.png";
constexpr char TRANSPARENT_SLEEP_DIR[] = "/.sleep-overlay";
constexpr char TRANSPARENT_SLEEP_LEGACY_DIR[] = "/sleep-overlay";
constexpr size_t MAX_SLEEP_FILE_NAME_LEN = 256;
constexpr uint8_t MIN_VISIBLE_ALPHA = 8;

struct BitmapPlacement {
  int x = 0;
  int y = 0;
  float cropX = 0.0f;
  float cropY = 0.0f;
};

struct OverlayBmpInfo {
  int width = 0;
  int height = 0;
  bool topDown = false;
  uint32_t dataOffset = 0;
  uint32_t rowBytes = 0;
};

uint16_t readLE16(HalFile& file) {
  const int c0 = file.read();
  const int c1 = file.read();
  const auto b0 = static_cast<uint8_t>(c0 < 0 ? 0 : c0);
  const auto b1 = static_cast<uint8_t>(c1 < 0 ? 0 : c1);
  return static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
}

uint32_t readLE32(HalFile& file) {
  const int c0 = file.read();
  const int c1 = file.read();
  const int c2 = file.read();
  const int c3 = file.read();
  const auto b0 = static_cast<uint8_t>(c0 < 0 ? 0 : c0);
  const auto b1 = static_cast<uint8_t>(c1 < 0 ? 0 : c1);
  const auto b2 = static_cast<uint8_t>(c2 < 0 ? 0 : c2);
  const auto b3 = static_cast<uint8_t>(c3 < 0 ? 0 : c3);
  return static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1) << 8) | (static_cast<uint32_t>(b2) << 16) |
         (static_cast<uint32_t>(b3) << 24);
}

uint32_t readBE32(HalFile& file) {
  const int c0 = file.read();
  const int c1 = file.read();
  const int c2 = file.read();
  const int c3 = file.read();
  if (c0 < 0 || c1 < 0 || c2 < 0 || c3 < 0) return 0;
  return (static_cast<uint32_t>(c0) << 24) | (static_cast<uint32_t>(c1) << 16) | (static_cast<uint32_t>(c2) << 8) |
         static_cast<uint32_t>(c3);
}

bool isValidPngHeader(HalFile& file) {
  static constexpr uint8_t PNG_SIGNATURE[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  static constexpr uint32_t MAX_SOURCE_PIXELS = 2048u * 1536u;
  uint8_t signature[8];
  if (!file.seek(0) || file.read(signature, sizeof(signature)) != static_cast<int>(sizeof(signature)) ||
      !std::equal(std::begin(signature), std::end(signature), std::begin(PNG_SIGNATURE))) {
    return false;
  }

  const uint32_t ihdrLength = readBE32(file);
  char chunkType[4];
  if (file.read(reinterpret_cast<uint8_t*>(chunkType), sizeof(chunkType)) != static_cast<int>(sizeof(chunkType)) ||
      ihdrLength != 13 || !std::equal(std::begin(chunkType), std::end(chunkType), "IHDR")) {
    return false;
  }

  const uint32_t width = readBE32(file);
  const uint32_t height = readBE32(file);
  const int bitDepth = file.read();
  const int colorType = file.read();
  const int compression = file.read();
  const int filter = file.read();
  const int interlace = file.read();

  const bool supportedBitDepth =
      bitDepth == 8 || ((colorType == PNG_PIXEL_GRAYSCALE || colorType == PNG_PIXEL_INDEXED) &&
                        (bitDepth == 1 || bitDepth == 2 || bitDepth == 4));
  const bool supportedColorType = colorType == PNG_PIXEL_GRAYSCALE || colorType == PNG_PIXEL_TRUECOLOR ||
                                  colorType == PNG_PIXEL_INDEXED || colorType == PNG_PIXEL_GRAY_ALPHA ||
                                  colorType == PNG_PIXEL_TRUECOLOR_ALPHA;
  return width > 0 && height > 0 && width <= 2048 && height <= 3072 && width * height <= MAX_SOURCE_PIXELS &&
         supportedBitDepth && supportedColorType && compression == 0 && filter == 0 && interlace == 0;
}

BitmapPlacement calculateBitmapPlacementInBounds(const int bitmapWidth, const int bitmapHeight,
                                                 const GfxRenderer& renderer, const int boundsX, const int boundsY,
                                                 const int boundsW, const int boundsH) {
  BitmapPlacement placement;

  if (bitmapWidth > boundsW || bitmapHeight > boundsH) {
    float ratio = static_cast<float>(bitmapWidth) / static_cast<float>(bitmapHeight);
    const float boundsRatio = static_cast<float>(boundsW) / static_cast<float>(boundsH);

    if (ratio > boundsRatio) {
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        placement.cropX = 1.0f - (boundsRatio / ratio);
        ratio = (1.0f - placement.cropX) * static_cast<float>(bitmapWidth) / static_cast<float>(bitmapHeight);
      }
      placement.x = boundsX;
      placement.y = boundsY + std::round((static_cast<float>(boundsH) - static_cast<float>(boundsW) / ratio) / 2);
    } else {
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        placement.cropY = 1.0f - (ratio / boundsRatio);
        ratio = static_cast<float>(bitmapWidth) / ((1.0f - placement.cropY) * static_cast<float>(bitmapHeight));
      }
      placement.x = boundsX + std::round((static_cast<float>(boundsW) - static_cast<float>(boundsH) * ratio) / 2);
      placement.y = boundsY;
    }
  } else {
    placement.x = boundsX + (boundsW - bitmapWidth) / 2;
    placement.y = boundsY + (boundsH - bitmapHeight) / 2;
  }

  return placement;
}

BitmapPlacement calculateBitmapPlacement(const int bitmapWidth, const int bitmapHeight, const GfxRenderer& renderer) {
  return calculateBitmapPlacementInBounds(bitmapWidth, bitmapHeight, renderer, 0, 0, renderer.getScreenWidth(),
                                          renderer.getScreenHeight());
}

bool parseOverlayBmpHeader(HalFile& file, OverlayBmpInfo& info, const bool logErrors) {
  if (!file) return false;
  if (!file.seek(0)) return false;

  if (readLE16(file) != 0x4D42) {
    if (logErrors) LOG_ERR("SLP", "Transparent overlay is not a BMP");
    return false;
  }

  file.seekCur(8);
  info.dataOffset = readLE32(file);

  const uint32_t dibSize = readLE32(file);
  if (dibSize < 40) {
    if (logErrors) LOG_ERR("SLP", "Unsupported BMP DIB header: %u", static_cast<unsigned>(dibSize));
    return false;
  }

  info.width = static_cast<int32_t>(readLE32(file));
  const auto rawHeight = static_cast<int32_t>(readLE32(file));
  if (rawHeight == std::numeric_limits<int32_t>::min()) {
    if (logErrors) LOG_ERR("SLP", "Bad transparent overlay dimensions: %dx%d", info.width, rawHeight);
    return false;
  }
  info.topDown = rawHeight < 0;
  info.height = info.topDown ? -rawHeight : rawHeight;

  const uint16_t planes = readLE16(file);
  const uint16_t bpp = readLE16(file);
  const uint32_t compression = readLE32(file);

  // Match Bitmap::parseHeaders(): accept BI_RGB (0) and 32bpp BI_BITFIELDS (3), but keep the same
  // byte-layout assumption as custom sleep BMPs. The renderer below treats pixels as BGRA and does not parse masks.
  if (planes != 1 || bpp != 32 || !(compression == 0 || compression == 3)) {
    if (logErrors) {
      LOG_ERR("SLP", "Transparent overlay must be 32-bit BGRA BMP (planes=%u bpp=%u comp=%u)", planes, bpp,
              static_cast<unsigned>(compression));
    }
    return false;
  }

  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;
  if (info.width <= 0 || info.height <= 0 || info.width > MAX_IMAGE_WIDTH || info.height > MAX_IMAGE_HEIGHT) {
    if (logErrors) LOG_ERR("SLP", "Bad transparent overlay dimensions: %dx%d", info.width, info.height);
    return false;
  }

  info.rowBytes = static_cast<uint32_t>(info.width) * 4u;
  if (!file.seek(info.dataOffset)) {
    if (logErrors) LOG_ERR("SLP", "Failed to seek transparent overlay pixel data");
    return false;
  }

  return true;
}

uint8_t bayerThreshold4x4(const int x, const int y) {
  static constexpr uint8_t BAYER_4X4[16] = {0, 128, 32, 160, 192, 64, 224, 96, 48, 176, 16, 144, 240, 112, 208, 80};
  return BAYER_4X4[((y & 0x03) << 2) | (x & 0x03)];
}

enum class TransparentOverlayPass : uint8_t { BW, GrayscaleLsb, GrayscaleMsb };

uint8_t quantizeOverlayLum(const uint8_t lum) {
  // Match Bitmap's native-palette path: 0, 85, 170, 255 map directly to levels 0..3.
  return lum >> 6;
}

bool renderTransparentOverlayPass(HalFile& file, const OverlayBmpInfo& info, const BitmapPlacement& placement,
                                  const GfxRenderer& renderer, uint8_t* row, const TransparentOverlayPass pass) {
  if (!file.seek(info.dataOffset)) {
    LOG_ERR("SLP", "Failed to seek transparent overlay pixel data");
    return false;
  }

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int cropPixX = std::floor(info.width * placement.cropX / 2.0f);
  const int cropPixY = std::floor(info.height * placement.cropY / 2.0f);
  const float croppedWidth = (1.0f - placement.cropX) * static_cast<float>(info.width);
  const float croppedHeight = (1.0f - placement.cropY) * static_cast<float>(info.height);

  float scale = 1.0f;
  if (croppedWidth > 0.0f && croppedHeight > 0.0f) {
    const float widthScale = static_cast<float>(pageWidth) / croppedWidth;
    const float heightScale = static_cast<float>(pageHeight) / croppedHeight;
    scale = std::min(widthScale, heightScale);
    if (scale > 1.0f) scale = 1.0f;
  }
  const bool isScaled = scale < 1.0f;

  for (int bmpY = 0; bmpY < info.height; bmpY++) {
    if (file.read(row, info.rowBytes) != static_cast<int>(info.rowBytes)) {
      LOG_ERR("SLP", "Short read in transparent overlay row %d", bmpY);
      return false;
    }

    int screenY = -cropPixY + (info.topDown ? bmpY : info.height - 1 - bmpY);
    if (isScaled) screenY = std::floor(screenY * scale);
    screenY += placement.y;

    if (screenY >= pageHeight) {
      if (info.topDown) break;
      continue;
    }
    if (screenY < 0) {
      if (!info.topDown) break;
      continue;
    }

    for (int bmpX = cropPixX; bmpX < info.width - cropPixX; bmpX++) {
      int screenX = bmpX - cropPixX;
      if (isScaled) screenX = std::floor(screenX * scale);
      screenX += placement.x;

      if (screenX >= renderer.getScreenWidth()) break;
      if (screenX < 0) continue;

      const uint8_t* pixel = row + (static_cast<size_t>(bmpX) * 4u);
      const uint8_t alpha = pixel[3];
      if (alpha < MIN_VISIBLE_ALPHA || alpha <= bayerThreshold4x4(screenX, screenY)) continue;

      const uint8_t lum = (77u * pixel[2] + 150u * pixel[1] + 29u * pixel[0]) >> 8;
      const uint8_t level = quantizeOverlayLum(lum);

      switch (pass) {
        case TransparentOverlayPass::BW:
          // Same first pass as custom bitmap sleep: all non-white levels are painted black.
          // Transparent overlay's only difference is that opaque white explicitly erases underlying text.
          renderer.drawPixel(screenX, screenY, level < 3);
          break;
        case TransparentOverlayPass::GrayscaleLsb:
        case TransparentOverlayPass::GrayscaleMsb: {
          const auto planePixel =
              grayPlanePixel(level, pass == TransparentOverlayPass::GrayscaleMsb, renderer.grayPlanesAreAbsolute());
          if (planePixel.write) renderer.drawPixel(screenX, screenY, planePixel.black);
          break;
        }
      }
    }
  }

  return true;
}

// Starts a gray plane under an overlay. Absolute planes keep the B/W base; other gray planes carry only
// the overlay's gray. Cover + Transparent redraws a gray cover (set only for absolute planes) to keep its gray levels.
// Overlay gray leaves a mask plane in the framebuffer, so a later B/W update of
// the retained sleep frame (the Picco USB charge strip) needs the B/W frame back.
// Absolute planes must keep the gray baseline: restoring the B/W frame there
// makes the next fast refresh drop the grays (Picco SSD1677).
class BwFrameKeeper {
 public:
  BwFrameKeeper(GfxRenderer& renderer, const bool keep)
      : renderer(renderer), keep(keep), stored(!keep || renderer.storeBwBuffer()) {}
  ~BwFrameKeeper() {
    if (keep && stored) renderer.restoreBwBuffer();
  }
  // False when the B/W frame could not be kept; callers must skip the gray passes.
  bool ready() const { return stored; }
  BwFrameKeeper(const BwFrameKeeper&) = delete;
  BwFrameKeeper& operator=(const BwFrameKeeper&) = delete;

 private:
  GfxRenderer& renderer;
  const bool keep;
  const bool stored;
};

bool beginOverlayGrayPlane(GfxRenderer& renderer, const Bitmap* grayCover, const GfxRenderer::RenderMode plane,
                           const bool absolute) {
  if (grayCover) {
    if (grayCover->rewindToData() != BmpReaderError::Ok) return false;
    renderer.clearScreen();
  } else if (!absolute) {
    renderer.clearScreen(0x00);
  }
  renderer.setRenderMode(plane);
  if (!grayCover) return true;
  const auto placement = calculateBitmapPlacement(grayCover->getWidth(), grayCover->getHeight(), renderer);
  return renderer.drawBitmap(*grayCover, placement.x, placement.y, renderer.getScreenWidth(),
                             renderer.getScreenHeight(), placement.cropX, placement.cropY);
}

enum class AlphaOverlayResult : uint8_t { Rendered, NotAlphaOverlay, Error };
enum class AlphaScanResult : uint8_t { Useful, NotUseful, Error };

AlphaScanResult scanForUsefulAlpha(HalFile& file, const OverlayBmpInfo& info, uint8_t* row) {
  if (!file.seek(info.dataOffset)) {
    LOG_ERR("SLP", "Failed to seek transparent overlay pixel data");
    return AlphaScanResult::Error;
  }

  bool hasVisiblePixel = false;
  bool hasNonOpaquePixel = false;
  for (int bmpY = 0; bmpY < info.height; bmpY++) {
    if (file.read(row, info.rowBytes) != static_cast<int>(info.rowBytes)) {
      LOG_ERR("SLP", "Short read while checking transparent overlay row %d", bmpY);
      return AlphaScanResult::Error;
    }

    for (int bmpX = 0; bmpX < info.width; bmpX++) {
      const uint8_t alpha = row[static_cast<size_t>(bmpX) * 4u + 3u];
      hasVisiblePixel |= alpha >= MIN_VISIBLE_ALPHA;
      hasNonOpaquePixel |= alpha < 255;
      if (hasVisiblePixel && hasNonOpaquePixel) return AlphaScanResult::Useful;
    }
  }

  return AlphaScanResult::NotUseful;
}

AlphaOverlayResult tryRenderTransparentOverlayBmp(HalFile& file, GfxRenderer& renderer, const char* pathForLog,
                                                  const Bitmap* grayCover, const int progressPercent) {
  OverlayBmpInfo info;
  if (!parseOverlayBmpHeader(file, info, false)) return AlphaOverlayResult::NotAlphaOverlay;

  const auto placement = calculateBitmapPlacement(info.width, info.height, renderer);
  auto row = makeUniqueNoThrow<uint8_t[]>(info.rowBytes);
  if (!row) {
    LOG_ERR("SLP", "OOM: transparent overlay row (%u bytes)", static_cast<unsigned>(info.rowBytes));
    return AlphaOverlayResult::Error;
  }

  const auto alphaScanResult = scanForUsefulAlpha(file, info, row.get());
  if (alphaScanResult == AlphaScanResult::Error) return AlphaOverlayResult::Error;
  if (alphaScanResult == AlphaScanResult::NotUseful) return AlphaOverlayResult::NotAlphaOverlay;

  LOG_DBG("SLP", "Rendering transparent overlay: %s (%dx%d)", pathForLog, info.width, info.height);

  if (!renderTransparentOverlayPass(file, info, placement, renderer, row.get(), TransparentOverlayPass::BW))
    return AlphaOverlayResult::Error;
  // With progress text in the base framebuffer the absolute planes would erase
  // it (they repaint the whole panel from the plane content alone), so fall
  // back to the OEM fold pipeline, same as renderTransparentOverlayPng.
  const bool absolute = progressPercent < 0 && renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported();
  const BwFrameKeeper bwFrame(renderer, !absolute);
  if (!bwFrame.ready()) {
    renderer.displayBuffer(kSleepClean);
    return AlphaOverlayResult::Rendered;
  }
  if (absolute) {
    cleanBeforeAbsoluteGray(renderer);
    if (!renderer.displayGrayscaleBase(sleepGrayscaleMode(renderer))) return AlphaOverlayResult::Error;
  } else {
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  }

  // Absolute planes retain B/W background bits; each visible overlay pixel is rewritten in both passes.
  if (!beginOverlayGrayPlane(renderer, grayCover, GfxRenderer::GRAYSCALE_LSB, absolute) ||
      !renderTransparentOverlayPass(file, info, placement, renderer, row.get(), TransparentOverlayPass::GrayscaleLsb)) {
    renderer.setRenderMode(GfxRenderer::BW);
    // Keep the current display instead of trying another overlay with a
    // framebuffer that now contains an incomplete gray plane.
    return AlphaOverlayResult::Rendered;
  }
  renderer.copyGrayscaleLsbBuffers();

  if (!beginOverlayGrayPlane(renderer, grayCover, GfxRenderer::GRAYSCALE_MSB, absolute) ||
      !renderTransparentOverlayPass(file, info, placement, renderer, row.get(), TransparentOverlayPass::GrayscaleMsb)) {
    renderer.setRenderMode(GfxRenderer::BW);
    return AlphaOverlayResult::Rendered;
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  return AlphaOverlayResult::Rendered;
}

enum class SleepRecentKind : uint8_t { Standard, Overlay };

bool isRecentSleepIndex(const SleepRecentKind recentKind, const uint16_t idx, const uint8_t window) {
  return recentKind == SleepRecentKind::Overlay ? APP_STATE.isRecentOverlaySleep(idx, window)
                                                : APP_STATE.isRecentSleep(idx, window);
}

void pushRecentSleepIndex(const SleepRecentKind recentKind, const uint16_t idx) {
  if (recentKind == SleepRecentKind::Overlay) {
    APP_STATE.pushRecentOverlaySleep(idx);
  } else {
    APP_STATE.pushRecentSleep(idx);
  }
}

bool findNextValidSleepImage(HalFile& dir, const SleepRecentKind recentKind, char* name) {
  for (auto dirFile = dir.openNextFile(); dirFile; dirFile = dir.openNextFile()) {
    if (dirFile.isDirectory()) continue;

    dirFile.getName(name, MAX_SLEEP_FILE_NAME_LEN);
    if (name[0] == '\0' || name[0] == '.') continue;

    const bool isBmp = FsHelpers::hasBmpExtension(name);
    const bool isPng = recentKind == SleepRecentKind::Overlay && FsHelpers::hasPngExtension(std::string_view{name});
    if (!isBmp && !isPng) {
      LOG_DBG("SLP", "Skipping unsupported sleep image: %s", name);
      continue;
    }

    const bool isValid = isBmp ? [&dirFile]() {
      Bitmap bitmap(dirFile);
      return bitmap.parseHeaders() == BmpReaderError::Ok;
    }()
                               : isValidPngHeader(dirFile);
    if (!isValid) {
      LOG_DBG("SLP", "Skipping invalid sleep image: %s", name);
      continue;
    }
    return true;
  }
  return false;
}

bool selectRandomSleepFile(const char* firstDirPath, const char* secondDirPath, const SleepRecentKind recentKind,
                           std::string& selectedPath, const bool commitRecent = true) {
  auto name = makeUniqueNoThrow<char[]>(MAX_SLEEP_FILE_NAME_LEN);
  if (!name) {
    LOG_ERR("SLP", "OOM: sleep filename buffer");
    return false;
  }

  constexpr size_t DIRECTORY_COUNT = 2;
  const char* dirPaths[DIRECTORY_COUNT] = {firstDirPath, secondDirPath};
  uint16_t directoryCounts[DIRECTORY_COUNT] = {};
  uint16_t fileCount = 0;
  for (size_t i = 0; i < DIRECTORY_COUNT; i++) {
    auto dir = Storage.open(dirPaths[i]);
    if (!dir || !dir.isDirectory()) continue;

    const auto remainingCapacity = static_cast<uint16_t>(UINT16_MAX - fileCount);
    while (directoryCounts[i] < remainingCapacity && findNextValidSleepImage(dir, recentKind, name.get())) {
      directoryCounts[i]++;
    }
    fileCount = static_cast<uint16_t>(fileCount + directoryCounts[i]);
  }

  if (fileCount == 0) return false;

  // Pick a random wallpaper, excluding recently shown ones.
  // Window: up to SLEEP_RECENT_COUNT entries, capped at fileCount-1.
  const uint8_t recentFill =
      recentKind == SleepRecentKind::Overlay ? APP_STATE.recentOverlaySleepFill : APP_STATE.recentSleepFill;
  const uint8_t window = static_cast<uint8_t>(std::min<uint16_t>(recentFill, fileCount - 1));
  auto randomFileIndex = static_cast<uint16_t>(random(fileCount));
  for (uint8_t attempt = 0; attempt < 20 && isRecentSleepIndex(recentKind, randomFileIndex, window); attempt++) {
    randomFileIndex = static_cast<uint16_t>(random(fileCount));
  }

  size_t directoryIndex = 0;
  uint16_t localIndex = randomFileIndex;
  if (randomFileIndex >= directoryCounts[0]) {
    directoryIndex = 1;
    localIndex = static_cast<uint16_t>(randomFileIndex - directoryCounts[0]);
  }

  auto dir = Storage.open(dirPaths[directoryIndex]);
  if (!dir || !dir.isDirectory()) {
    LOG_ERR("SLP", "Failed to open selected sleep image directory: %s", dirPaths[directoryIndex]);
    return false;
  }
  for (uint16_t index = 0; index <= localIndex; ++index) {
    if (!findNextValidSleepImage(dir, recentKind, name.get())) return false;
  }

  const char* selectedDirPath = dirPaths[directoryIndex];
  selectedPath.reserve(strlen(selectedDirPath) + 1 + strlen(name.get()));
  selectedPath = selectedDirPath;
  selectedPath += "/";
  selectedPath += name.get();
  // Shutdown-only picks must neither skew the sleep exclusion window nor pay
  // an SD write on the timer-wake path (which otherwise never persists).
  if (commitRecent) {
    pushRecentSleepIndex(recentKind, randomFileIndex);
    APP_STATE.saveToFile();
  }
  return true;
}

bool drawSleepPopupPreservingFrame(GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int frameThickness = metrics.popupFrameThickness;
  const int popupY = static_cast<int>(renderer.getScreenHeight() * metrics.popupTopOffsetRatio);
  const int popupHeight = renderer.getLineHeight(UI_12_FONT_ID) + metrics.popupMarginY * 2;
  const int bandTop = std::max(0, popupY - frameThickness);
  const int bandBottom = std::min(renderer.getScreenHeight(), popupY + popupHeight + frameThickness);
  const int bandHeight = bandBottom - bandTop;
  const size_t bandBytes = renderer.getRegionByteSize(0, bandTop, renderer.getScreenWidth(), bandHeight);

  auto savedBand = makeUniqueNoThrow<uint8_t[]>(bandBytes);
  if (!savedBand) {
    LOG_ERR("SLP", "OOM: sleep popup background (%u bytes)", static_cast<unsigned>(bandBytes));
    return false;
  }
  if (!renderer.copyRegionToBuffer(0, bandTop, renderer.getScreenWidth(), bandHeight, savedBand.get(), bandBytes)) {
    LOG_ERR("SLP", "Failed to save sleep popup background");
    return false;
  }

  GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  if (!renderer.copyBufferToRegion(0, bandTop, renderer.getScreenWidth(), bandHeight, savedBand.get(), bandBytes)) {
    LOG_ERR("SLP", "Failed to restore sleep popup background");
    return false;
  }
  return true;
}

void releaseSdFontCachesForDecode(const GfxRenderer& renderer) {
  if (auto* fcm = renderer.getFontCacheManager()) {
    LOG_DBG("SLP", "Free heap before SD font cache release: %d bytes", ESP.getFreeHeap());
    fcm->releaseSdFontCaches();
    LOG_DBG("SLP", "Free heap before sleep image decode: %d bytes", ESP.getFreeHeap());
  }
}

// Flush an image that is already drawn into the BW framebuffer (at the given
// placement) to the panel, running the grayscale pipeline when the bitmap
// carries gray levels. The gray plane passes only mark gray pixels, so other
// content in the base framebuffer (frames, captions, overlays) survives: on
// SSD1677 the (0,0) LUT group is a hold waveform, on UC8279 displayStart
// snapshots the base and the plane copies fold it into the absolute planes.
// allowAbsolutePlanes=false opts OUT of that folding (full-screen art only):
// absolute planes repaint the whole panel from the plane content alone, so any
// scene content the gray passes don't redraw (frame border, caption) would be
// erased from the panel. Callers whose base framebuffer carries such scene
// content must pass false. grayCover (a gray cover already drawn into the B/W
// base) gets its gray planes redrawn under overlay passes, like the panel-side
// Cover + Transparent compositing.
void displayImageWithGrayscale(GfxRenderer& renderer, const Bitmap& bitmap, const int x, const int y, const int maxW,
                               const int maxH, const float cropX, const float cropY, const bool hasGreyscale,
                               const bool preserveBackground = false, const bool allowAbsolutePlanes = true,
                               const Bitmap* grayCover = nullptr) {
  if (!hasGreyscale) {
    renderer.displayBuffer(kSleepClean);
    return;
  }

  // Absolute planes retain the B/W base image (no clear needed) and need the
  // absolute-quality base pass; the overlay pipeline repaints into cleared
  // planes over the OEM HALF base.
  const bool absolute = allowAbsolutePlanes && renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported();
  // Keep the B/W frame across the OEM (non-absolute) gray passes: the retained
  // sleep frame (the Picco USB charge strip) needs the B/W frame back. Absolute
  // planes must keep the gray baseline: restoring the B/W frame there makes the
  // next fast refresh drop the grays (Picco SSD1677).
  const BwFrameKeeper bwFrame(renderer, !absolute);
  if (!bwFrame.ready()) {
    renderer.displayBuffer(kSleepClean);
    return;
  }
  if (absolute) {
    cleanBeforeAbsoluteGray(renderer);
    if (!renderer.displayGrayscaleBase(sleepGrayscaleMode(renderer))) return;
  } else {
    // OEM grayscale pipeline base. Must stay HALF: the gray nudge LUT is
    // calibrated against the pixel state the single-pass HALF waveform leaves
    // behind. A FULL (GC) base parks pixels in a different charge state and
    // the differential nudge then lands unevenly (blotchy noise in gray areas).
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  }

  constexpr GfxRenderer::RenderMode grayPasses[2] = {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB};
  bool ready = true;
  for (const auto mode : grayPasses) {
    if (bitmap.rewindToData() != BmpReaderError::Ok) {
      ready = false;
      break;
    }
    if (preserveBackground) {
      if (!beginOverlayGrayPlane(renderer, grayCover, mode, absolute)) {
        ready = false;
        break;
      }
    } else {
      renderer.clearScreen(absolute ? 0xFF : 0x00);
      renderer.setRenderMode(mode);
    }
    if (!renderer.drawBitmap(bitmap, x, y, maxW, maxH, cropX, cropY, preserveBackground)) {
      ready = false;
      break;
    }
    if (mode == GfxRenderer::GRAYSCALE_LSB) {
      renderer.copyGrayscaleLsbBuffers();
    } else {
      renderer.copyGrayscaleMsbBuffers();
    }
  }
  if (ready)
    renderer.displayGrayBuffer();
  else
    LOG_ERR("SLP", "Incomplete grayscale image; keeping the current display");
  renderer.setRenderMode(GfxRenderer::BW);
}

void drawShutdownCaption(const GfxRenderer& renderer, const int captionY) {
  // Two centered lines: the statement, then the instruction. Split at the
  // first sentence boundary (". "); translations without one fall back to a
  // single centered line.
  const char* caption = tr(STR_SHUTDOWN_PRESS_POWER);
  const char* sentenceBreak = std::strstr(caption, ". ");
  if (sentenceBreak) {
    const std::string firstLine(caption, sentenceBreak + 1);  // keep the period
    renderer.drawCenteredText(UI_10_FONT_ID, captionY, firstLine.c_str(), true);
    renderer.drawCenteredText(UI_10_FONT_ID, captionY + renderer.getLineHeight(UI_10_FONT_ID), sentenceBreak + 2, true);
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, captionY, caption, true);
  }
}

// Render-time fallback for state saved before the cache triple existed: a cold
// time-left cache with a live book path reads the book's stats record (135 B
// fixed record, no Epub allocation) to surface the persisted estimate. Pages
// left has no pre-upgrade source, so it stays unknown. Callers gate this on
// APP_STATE.openBookCacheLegacy so post-upgrade sessions that leave the
// estimate unknown never surface a stale record value, and the SD reads stay
// confined to the first boot after the upgrade.
static uint32_t loadTimeLeftFallback(const std::string& bookPath) {
  // Shared helper: empty for non-book paths, and the correct epub_/xtc_ prefix
  // per extension (a wrong prefix would only cost failed opens).
  const std::string cachePath = cachePathForBookPath(bookPath);
  if (cachePath.empty()) return 0;
  const BookReadingStats stats = BookReadingStats::load(cachePath);
  return stats.estimatedTimeLeftSeconds;
}

// Draw the current book's enriched reading-status line ("42% • ~2h 15m • 134
// pages left"), centered near the bottom of the screen (or at an explicit y
// for the framed shutdown layout). Negative progress draws nothing.
void drawReadingProgressLine(const GfxRenderer& renderer, const int progressPercent, const uint32_t timeLeftSeconds,
                             const int pagesLeft, const int y = -1) {
  if (progressPercent < 0) return;
  char text[48];
  formatSleepProgressLine(progressPercent, timeLeftSeconds, pagesLeft, text, sizeof(text));
  renderer.drawCenteredText(UI_10_FONT_ID, y < 0 ? renderer.getScreenHeight() - 32 : y, text, true);
}

// Resolve the custom sleep art for the shutdown screen: same source order as
// the custom sleep screen, but WITHOUT committing to the recent-sleep window
// or persisting state (an SD write here would delay the timer-wake rail cut).
// Returns true when outPath holds a usable image.
bool selectShutdownCustomImage(std::string& outPath) {
  HalFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      outPath = "/sleep.bmp";
      return true;
    }
  }

  selectRandomSleepFile("/.sleep", "/sleep", SleepRecentKind::Standard, outPath, /*commitRecent=*/false);
  if (!outPath.empty()) delay(100);  // Same settle window as the sleep random pick.
  return !outPath.empty();
}

// Draw one image framed for the shutdown screen: centered border (~3/4 panel,
// inset 6), caption below, cover filter applied like the sleep screen does.
// Flushes the panel itself (the bitmap must stay open through the grayscale
// passes). Returns false when the BMP is unusable and the caller must fall
// back.
bool renderShutdownImageFramed(GfxRenderer& renderer, HalFile& file, const bool dithering, const char* pathForLog,
                               const int progressPercent = -1, const uint32_t timeLeftSeconds = 0,
                               const int pagesLeft = -1) {
  Bitmap bitmap(file, dithering);
  const auto parseResult = bitmap.parseHeaders();
  if (parseResult != BmpReaderError::Ok) {
    LOG_ERR("SLP", "Invalid shutdown image BMP %s: %s", pathForLog, Bitmap::errorToString(parseResult));
    return false;
  }

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  constexpr float FRAME_FRACTION = 0.76f;
  constexpr int FRAME_INSET = 6;
  const int frameW = static_cast<int>(static_cast<float>(pageWidth) * FRAME_FRACTION);
  const int frameH = static_cast<int>(static_cast<float>(pageHeight) * FRAME_FRACTION);
  const int frameX = (pageWidth - frameW) / 2;
  const int frameY = (pageHeight - frameH) / 2;

  const int innerX = frameX + FRAME_INSET;
  const int innerY = frameY + FRAME_INSET;
  const int innerW = frameW - 2 * FRAME_INSET;
  const int innerH = frameH - 2 * FRAME_INSET;
  const auto placement =
      calculateBitmapPlacementInBounds(bitmap.getWidth(), bitmap.getHeight(), renderer, innerX, innerY, innerW, innerH);
  // Draw against the INSET bounds, not the full panel: drawBitmap fits the
  // (cropped) bitmap into maxWidth/maxHeight, so passing the panel size lets
  // a large image scale past the border. (CodeRabbit finding.)
  renderer.drawBitmap(bitmap, placement.x, placement.y, innerW, innerH, placement.cropX, placement.cropY);
  renderer.drawRect(frameX, frameY, frameW, frameH);
  // Caption BEFORE any inversion: invertScreen flips the whole framebuffer, so
  // drawing the caption after it would hide black ink on the flipped panel.
  drawShutdownCaption(renderer, frameY + frameH + 24);
  // Progress sits between the frame and the caption, before any inversion so
  // it lands in the same B/W layer as the caption.
  drawReadingProgressLine(renderer, progressPercent, timeLeftSeconds, pagesLeft, frameY + frameH + 8);

  const bool hasGreyscale = bitmap.hasGreyscale() &&
                            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;
  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    // Same filter semantics as the sleep screen; flips frame, image, caption.
    renderer.invertScreen();
  }

  // The shutdown screen is a framed scene: border and caption live in the base
  // framebuffer but outside the image, so the absolute-plane pass (which
  // repaints the whole panel) would erase them. Force the OEM fold pipeline.
  displayImageWithGrayscale(renderer, bitmap, placement.x, placement.y, innerW, innerH, placement.cropX,
                            placement.cropY, hasGreyscale, /*preserveBackground=*/false,
                            /*allowAbsolutePlanes=*/false);
  return true;
}

}  // namespace

void SleepActivity::onEnter() {
  Activity::onEnter();

  const bool renderQuickResume =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);

  if (renderQuickResume) {
    // Quick Resume keeps the current frame as-is, so the driver's inversion
    // state stays too: a night-mode page sleeps in night polarity, and the
    // moon icon inverts with it at transfer like any other draw.
    return renderLastScreenSleepScreen();
  }

  const bool frameWasInverted = display.isInverted();

  // The remaining sleep screens draw fresh content in normal polarity. This
  // activity draws directly from onEnter (outside ActivityManager's
  // per-render polarity resolution), so clear any inversion left over from a
  // night-mode reader render.
  display.setInverted(false);

  const uint8_t sleepScreen = SETTINGS.sleepScreen;
  if (sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT_CUSTOM ||
      sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_TRANSPARENT) {
    // Transparent mode retains the current framebuffer. Materialize any
    // output-level inversion first so the retained content keeps its visible
    // polarity after the display driver returns to normal.
    if (frameWasInverted) renderer.invertScreen();
    if (APP_STATE.lastSleepFromReader) {
      ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    }
    drawSleepPopupPreservingFrame(renderer);
    if (APP_STATE.lastSleepFromReader) {
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
    }
    releaseSdFontCachesForDecode(renderer);
    if (sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_TRANSPARENT) {
      return renderCoverSleepScreen();
    }
    return renderTransparentCustomSleepScreen();
  }

  // Show popup with reader orientation only when going to sleep from reader
  if (APP_STATE.lastSleepFromReader) {
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  } else {
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  }

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
      return renderCoverSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      if (APP_STATE.lastSleepFromReader) {
        return renderCoverSleepScreen();
      } else {
        return renderCustomSleepScreen();
      }
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderCustomSleepScreen() const {
  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  // This takes priority over both custom sleep directories.
  HalFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true,
                  renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported() &&
                      display.getController() == HalDisplay::Controller::SSD1677 &&
                      SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: /sleep.bmp");
      renderBitmapSleepScreen(bitmap);
      file.close();
      return;
    }
    file.close();
  }

  std::string selectedPath;
  selectRandomSleepFile("/.sleep", "/sleep", SleepRecentKind::Standard, selectedPath);

  if (!selectedPath.empty()) {
    HalFile randFile;
    if (Storage.openFileForRead("SLP", selectedPath, randFile)) {
      LOG_DBG("SLP", "Randomly loading: %s", selectedPath.c_str());
      delay(100);
      Bitmap bitmap(randFile, true,
                    renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported() &&
                        display.getController() == HalDisplay::Controller::SSD1677 &&
                        SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        renderBitmapSleepScreen(bitmap);
        randFile.close();
        return;
      }
      randFile.close();
    }
  }

  renderDefaultSleepScreen();
}

// Sleep screens paint with a single HALF refresh (stock parity): the OEM X4
// firmware's only clean refresh in normal operation is the single-pass 0xD7
// sequence, used once for the sleep image. It never runs the multi-flash GC
// waveform (0xF7) that FULL_REFRESH selects (#2471's blinking complaint).
void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  // The logo must be XOR-inverted before the transparent blit, or its black
  // backdrop paints as ink (see drawLogo120Inverted). Keeps the natural
  // polarity of the framebuffer, relevant when setInverted() / dark sleep
  // clear paths are involved.
  drawLogo120Inverted(renderer, (pageWidth - 120) / 2, (pageHeight - 120) / 2);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));

  // Make sleep screen dark unless light is selected in settings
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(kSleepClean);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap, const bool preserveBackground,
                                            const int progressPercent, const uint32_t timeLeftSeconds,
                                            const int pagesLeft) const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto placement = calculateBitmapPlacement(bitmap.getWidth(), bitmap.getHeight(), renderer);
  const int x = placement.x;
  const int y = placement.y;
  const float cropX = placement.cropX;
  const float cropY = placement.cropY;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  LOG_DBG("SLP", "drawing to %d x %d", x, y);
  if (!preserveBackground) renderer.clearScreen();

  const bool hasGreyscale = (bitmap.hasGreyscale() || (preserveBackground && overlayGrayCover)) &&
                            (preserveBackground || SETTINGS.sleepScreenCoverFilter ==
                                                       CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER);

  if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY, preserveBackground)) {
    renderer.displayBuffer(kSleepClean);
    return;
  }

  // Draw BEFORE the inversion filter and before the transparent-overlay
  // branch and the grayscale passes. After the inversion the text would stay
  // black ink over the already-flipped cover and become unreadable; the gray
  // passes only repaint gray planes over the preserved B/W background, so the
  // text placed here survives.
  drawReadingProgressLine(renderer, progressPercent, timeLeftSeconds, pagesLeft);

  if (!preserveBackground &&
      SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  // Cover + Transparent: the B/W cover is the overlay's background; the overlay renderers redraw its gray planes.
  if (overlayCover == &bitmap) {
    if (hasGreyscale && renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported()) {
      overlayGrayCover = &bitmap;
    }
    // The cover call already drew the progress; forward it so the plain-cover
    // fallback (no valid overlay) redraws it after its own clearScreen.
    renderTransparentCustomSleepScreen(progressPercent, timeLeftSeconds, pagesLeft);
    overlayGrayCover = nullptr;
    return;
  }

  // Absolute planes repaint the whole panel from the plane content alone, so
  // they would erase the progress text living in the base framebuffer. While
  // text is shown, force the OEM fold pipeline, which keeps base content
  // (same choice renderShutdownImageFramed makes for its frame + caption).
  displayImageWithGrayscale(renderer, bitmap, x, y, pageWidth, pageHeight, cropX, cropY, hasGreyscale,
                            preserveBackground, /*allowAbsolutePlanes=*/progressPercent < 0, overlayGrayCover);
}

bool SleepActivity::renderSleepOverlayFile(HalFile& file, const char* pathForLog, const int progressPercent,
                                           const uint32_t timeLeftSeconds, const int pagesLeft) const {
  const auto alphaResult =
      tryRenderTransparentOverlayBmp(file, renderer, pathForLog, overlayGrayCover, progressPercent);
  if (alphaResult == AlphaOverlayResult::Rendered) return true;
  if (alphaResult == AlphaOverlayResult::Error) return false;

  Bitmap bitmap(file);
  const auto parseResult = bitmap.parseHeaders();
  if (parseResult != BmpReaderError::Ok) {
    LOG_ERR("SLP", "Invalid sleep overlay BMP %s: %s", pathForLog, Bitmap::errorToString(parseResult));
    return false;
  }

  LOG_DBG("SLP", "Rendering regular BMP sleep overlay: %s (%dx%d)", pathForLog, bitmap.getWidth(), bitmap.getHeight());
  // drawBitmap leaves white pixels untouched; skipping the initial clear makes
  // them transparent while retaining the existing grayscale pipeline. The
  // forwarded percent keeps this flush off the absolute planes so it does not
  // erase the progress text the cover pass already drew.
  renderBitmapSleepScreen(bitmap, true, progressPercent, timeLeftSeconds, pagesLeft);
  return true;
}

bool SleepActivity::renderTransparentOverlayPng(const std::string& path, const int progressPercent,
                                                const uint32_t timeLeftSeconds, const int pagesLeft) const {
  ImageDimensions dimensions;
  if (!PngToFramebufferConverter::getDimensionsStatic(path, dimensions)) return false;

  const auto placement = calculateBitmapPlacement(dimensions.width, dimensions.height, renderer);
  RenderConfig config;
  config.x = placement.x;
  config.y = placement.y;
  config.maxWidth = renderer.getScreenWidth();
  config.maxHeight = renderer.getScreenHeight();
  config.useDithering = false;
  config.sourceCropX = placement.cropX;
  config.sourceCropY = placement.cropY;
  config.useExactDimensions = placement.cropX > 0.0f || placement.cropY > 0.0f;
  config.preserveAlpha = true;

  PngToFramebufferConverter converter;
  LOG_DBG("SLP", "Rendering transparent PNG overlay: %s (%dx%d)", path.c_str(), dimensions.width, dimensions.height);

  if (!converter.decodeToFramebuffer(path, renderer, config)) return false;
  // Same scene-content rule as displayImageWithGrayscale: with progress text
  // in the base framebuffer, the absolute planes would erase it, so fall back
  // to the OEM fold pipeline.
  const bool absolute = progressPercent < 0 && renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported();
  const BwFrameKeeper bwFrame(renderer, !absolute);
  if (!bwFrame.ready()) {
    renderer.displayBuffer(kSleepClean);
    return true;
  }
  if (absolute) {
    // The cover replaces the previous screen, so clean it like the BMP paths do.
    if (overlayCover) cleanBeforeAbsoluteGray(renderer);
    if (!renderer.displayGrayscaleBase(sleepGrayscaleMode(renderer))) return false;
  } else {
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  }

  // Absolute planes retain B/W background bits; each visible overlay pixel is rewritten in both passes.
  if (!beginOverlayGrayPlane(renderer, overlayGrayCover, GfxRenderer::GRAYSCALE_LSB, absolute) ||
      !converter.decodeToFramebuffer(path, renderer, config)) {
    renderer.setRenderMode(GfxRenderer::BW);
    return true;
  }
  renderer.copyGrayscaleLsbBuffers();

  if (!beginOverlayGrayPlane(renderer, overlayGrayCover, GfxRenderer::GRAYSCALE_MSB, absolute) ||
      !converter.decodeToFramebuffer(path, renderer, config)) {
    renderer.setRenderMode(GfxRenderer::BW);
    return true;
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  return true;
}

bool SleepActivity::renderSleepOverlayPath(const std::string& path, const int progressPercent,
                                           const uint32_t timeLeftSeconds, const int pagesLeft) const {
  if (FsHelpers::hasPngExtension(path)) {
    return Storage.exists(path.c_str()) &&
           renderTransparentOverlayPng(path, progressPercent, timeLeftSeconds, pagesLeft);
  }

  HalFile file;
  return Storage.openFileForRead("SLP", path, file) &&
         renderSleepOverlayFile(file, path.c_str(), progressPercent, timeLeftSeconds, pagesLeft);
}

void SleepActivity::renderTransparentCustomSleepScreen(const int progressPercent, const uint32_t timeLeftSeconds,
                                                       const int pagesLeft) const {
  if (renderSleepOverlayPath(TRANSPARENT_SLEEP_ROOT_BMP, progressPercent, timeLeftSeconds, pagesLeft)) return;
  if (renderSleepOverlayPath(TRANSPARENT_SLEEP_ROOT_PNG, progressPercent, timeLeftSeconds, pagesLeft)) return;

  std::string selectedPath;
  selectRandomSleepFile(TRANSPARENT_SLEEP_DIR, TRANSPARENT_SLEEP_LEGACY_DIR, SleepRecentKind::Overlay, selectedPath);

  if (!selectedPath.empty() && renderSleepOverlayPath(selectedPath, progressPercent, timeLeftSeconds, pagesLeft))
    return;

  LOG_ERR("SLP", "No valid transparent sleep overlay found");
  if (overlayCover) {
    // Show the plain cover.
    const Bitmap& cover = *overlayCover;
    overlayCover = nullptr;
    if (cover.rewindToData() == BmpReaderError::Ok)
      return renderBitmapSleepScreen(cover, /*preserveBackground=*/false, progressPercent, timeLeftSeconds, pagesLeft);
  }
  renderDefaultSleepScreen();
}

void SleepActivity::renderCoverSleepScreen() const {
  void (SleepActivity::*renderNoCoverSleepScreen)() const = &SleepActivity::renderDefaultSleepScreen;
  bool withOverlay = false;
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      renderNoCoverSleepScreen = &SleepActivity::renderCustomSleepScreen;
      break;
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_TRANSPARENT):
      withOverlay = true;
      break;
    default:
      break;
  }

  std::string coverBmpPath;
  if (APP_STATE.openEpubPath.empty() || !resolveCoverBmpPath(renderer, APP_STATE.openEpubPath, coverBmpPath)) {
    if (withOverlay) return renderTransparentCustomSleepScreen();
    return (this->*renderNoCoverSleepScreen)();
  }

  HalFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      if (withOverlay) overlayCover = &bitmap;
      int progressPercent = -1;
      uint32_t timeLeftSeconds = 0;
      int pagesLeft = -1;
      if (SETTINGS.sleepScreenCoverShowProgress && !APP_STATE.openEpubPath.empty()) {
        // Seeded by the reader at exit; the SD reload only serves state from
        // before the cache existed (no reader exit since the upgrade).
        progressPercent = APP_STATE.openBookProgressPercent;
        if (progressPercent < 0) progressPercent = loadBookProgress(APP_STATE.openEpubPath);
        timeLeftSeconds = APP_STATE.openBookTimeLeftSeconds;
        pagesLeft = APP_STATE.openBookPagesLeft;
        // Fallback for pre-upgrade state only: a cold time-left cache in a
        // state file that predates the cache triple reads the book's stats
        // record directly. A post-upgrade session that leaves the estimate
        // unknown (tracking off, no pace yet) stays unknown instead of
        // surfacing a stale record value.
        if (timeLeftSeconds == 0 && APP_STATE.openBookCacheLegacy) {
          timeLeftSeconds = loadTimeLeftFallback(APP_STATE.openEpubPath);
        }
      }
      renderBitmapSleepScreen(bitmap, /*preserveBackground=*/false, progressPercent, timeLeftSeconds, pagesLeft);
      overlayCover = nullptr;
      return;
    }
  }

  if (withOverlay) return renderTransparentCustomSleepScreen();
  return (this->*renderNoCoverSleepScreen)();
}

bool SleepActivity::resolveCoverBmpPath(const GfxRenderer& renderer, const std::string& bookPath,
                                        std::string& outPath) {
  const bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;
  // SSD absolute images use the new thresholds; other panels retain legacy tuning.
  const bool originalThresholds =
      renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported() &&
      display.getController() == HalDisplay::Controller::SSD1677 &&
      SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  // Check if the current book is XTC, TXT, or EPUB
  if (FsHelpers::hasXtcExtension(bookPath)) {
    // Handle XTC file
    Xtc lastXtc(bookPath, "/.crosspoint");
    if (!lastXtc.load()) {
      LOG_ERR("SLP", "Failed to load last XTC");
      return false;
    }

    if (!lastXtc.generateCoverBmp()) {
      LOG_ERR("SLP", "Failed to generate XTC cover bmp");
      return false;
    }

    outPath = lastXtc.getCoverBmpPath();
  } else if (FsHelpers::hasReflowableBookExtension(bookPath)) {
    // Handle EPUB, TXT, or Markdown file
    Epub lastEpub(bookPath, "/.crosspoint");
    // Skip loading css since we only need metadata here
    if (!lastEpub.load(true, true)) {
      LOG_ERR("SLP", "Failed to load last epub");
      return false;
    }

    if (!lastEpub.generateCoverBmp(cropped, originalThresholds)) {
      LOG_ERR("SLP", "Failed to generate cover bmp");
      return false;
    }

    outPath = lastEpub.getCoverBmpPath(cropped, originalThresholds);
  } else {
    return false;
  }

  return !outPath.empty();
}

void SleepActivity::renderShutdownScreen(GfxRenderer& renderer) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Manual power off renders outside ActivityManager's per-render polarity
  // resolution, so normalize output polarity first: under night mode the
  // blank/logo fallback would otherwise paint an unreadable inverted panel.
  // The timer-wake path boots cold and is already normal.
  display.setInverted(false);

  renderer.clearScreen();

  const auto sleepMode = static_cast<CrossPointSettings::SLEEP_SCREEN_MODE>(SETTINGS.sleepScreen);
  // COVER_CUSTOM mirrors the sleep screen: cover art when sleeping from the
  // reader, custom art when sleeping from home (persisted for the timer-wake
  // path; set fresh in enterPowerOff for the manual one).
  const bool useCover =
      sleepMode == CrossPointSettings::SLEEP_SCREEN_MODE::COVER ||
      (sleepMode == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM && APP_STATE.lastSleepFromReader);

  bool haveImage = false;
  int progressPercent = -1;
  uint32_t timeLeftSeconds = 0;
  int pagesLeft = -1;
  if (useCover) {
    if (SETTINGS.sleepScreenCoverShowProgress && !APP_STATE.openEpubPath.empty()) {
      // Seeded by the reader at exit (and persisted at sleep entry); the SD
      // reload only serves state from before the cache existed.
      progressPercent = APP_STATE.openBookProgressPercent;
      if (progressPercent < 0) progressPercent = loadBookProgress(APP_STATE.openEpubPath);
      timeLeftSeconds = APP_STATE.openBookTimeLeftSeconds;
      pagesLeft = APP_STATE.openBookPagesLeft;
      // Pre-upgrade fallback, same guard as the sleep path: only runs when
      // the cached estimate is cold in a pre-cache state file.
      if (timeLeftSeconds == 0 && APP_STATE.openBookCacheLegacy) {
        timeLeftSeconds = loadTimeLeftFallback(APP_STATE.openEpubPath);
      }
    }
    const std::string& coverPath = APP_STATE.autoPowerOffCoverBmpPath;
    HalFile file;
    if (!coverPath.empty() && Storage.openFileForRead("SLP", coverPath, file)) {
      haveImage = renderShutdownImageFramed(renderer, file, /*dithering=*/false, coverPath.c_str(), progressPercent,
                                            timeLeftSeconds, pagesLeft);
    }
  }

  // CUSTOM art also backs COVER_CUSTOM when its cover is missing or unusable.
  if (!haveImage && (sleepMode == CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM ||
                     sleepMode == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM)) {
    std::string customPath;
    if (selectShutdownCustomImage(customPath)) {
      HalFile file;
      if (Storage.openFileForRead("SLP", customPath, file)) {
        haveImage = renderShutdownImageFramed(renderer, file, /*dithering=*/true, customPath.c_str());
      }
    }
  }

  if (!haveImage) {
    if (sleepMode != CrossPointSettings::SLEEP_SCREEN_MODE::BLANK) {
      // Same inverted-logo treatment as renderDefaultSleepScreen.
      drawLogo120Inverted(renderer, (pageWidth - 120) / 2, (pageHeight - 120) / 2);
    }
    // BLANK stays faithful to the setting: blank panel, caption only.
    drawShutdownCaption(
        renderer, sleepMode == CrossPointSettings::SLEEP_SCREEN_MODE::BLANK ? pageHeight / 2 : pageHeight / 2 + 95);
    // Cover mode that lost its image still shows progress below the caption.
    drawReadingProgressLine(renderer, progressPercent, timeLeftSeconds, pagesLeft);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  APP_STATE.autoPowerOffCoverBmpPath.clear();
}

void SleepActivity::renderLastScreenSleepScreen() const {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawImage(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
  // Only the moon differs from the displayed frame, so a differential FAST
  // update adds it without the flashing clean pass (which sweeps the panel
  // through the inverse — a full white flash on a night-mode page).
  if (gpio.deviceIsX3()) {
    renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(kSleepClean);
}
