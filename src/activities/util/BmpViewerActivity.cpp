#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <Epub/converters/JpegToFramebufferConverter.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <render/PageRenderer.h>

// fitPlacement (the shared placement derivation) is unguarded engine surface;
// the TTF-only decode/draw pieces below it are not.
#include "activities/reader/TtfImageEngine.h"

#if defined(CROSSPOINT_TTF_READER)
#include <epub/ImageProbe.h>
#include <epub/ZipCatalog.h>

#include "activities/reader/TtfBookRuntime.h"
#include "activities/reader/TtfImageCache.h"
#include "adapters/FrameTargetFactory.h"
#include "adapters/SdCardBookSource.h"
#include "adapters/StandaloneImageCatalog.h"
#else
#include <Epub/converters/PngToFramebufferConverter.h>
#endif

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr char CUSTOM_SLEEP_ROOT_BMP[] = "/sleep.bmp";
constexpr char CUSTOM_SLEEP_TMP_BMP[] = "/sleep.bmp.tmp";
constexpr char JPEG_SLEEP_TMP_PXC[] = "/sleep.bmp.pxc.tmp";
constexpr char TRANSPARENT_SLEEP_ROOT_BMP[] = "/sleep-overlay.bmp";
constexpr char TRANSPARENT_SLEEP_ROOT_PNG[] = "/sleep-overlay.png";
constexpr size_t COPY_BUFFER_SIZE = 2048;
constexpr uint8_t GRAYSCALE_PALETTE[] = {0, 0, 0, 0, 85, 85, 85, 0, 170, 170, 170, 0, 255, 255, 255, 0};

// The centered-fit RenderConfig is an adapter over the engine's ONE integer
// placement derivation (ttfimg::fitPlacement, design amendment §7.2) — the
// same numbers the legacy formula produced, shared with the engine viewer
// path instead of a second rounding derivation.
RenderConfig fitImage(const ImageDimensions& dimensions, const GfxRenderer& renderer) {
  const ttfimg::Placement place =
      ttfimg::fitPlacement(dimensions.width, dimensions.height, renderer.getScreenWidth(), renderer.getScreenHeight());
  RenderConfig config{place.x, place.y, place.w, place.h};
  config.useExactDimensions = true;
  return config;
}

bool overlaySleepMode() {
  const uint8_t mode = SETTINGS.sleepScreen;
  return mode == CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT_CUSTOM ||
         mode == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_TRANSPARENT;
}
}  // namespace

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("BmpViewer", renderer, mappedInput), filePath(std::move(path)) {}

void BmpViewerActivity::loadSiblingImages() {
  siblingImages.clear();
  currentImageIndex = -1;

  if (filePath.empty()) return;

  std::string dirPath = FsHelpers::extractFolderPath(filePath);
  size_t lastSlash = filePath.find_last_of('/');
  std::string fileName = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;

  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  const auto name = makeUniqueNoThrow<char[]>(500);
  if (!name) {
    LOG_ERR("BMP", "OOM: sibling filename buffer");
    return;
  }
  size_t imageCount = 0;
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) continue;
    file.getName(name.get(), 500);
    if (name[0] != '.' && FsHelpers::hasImageExtension(std::string_view{name.get()})) ++imageCount;
  }
  dir.rewindDirectory();
  siblingImages.reserve(imageCount);

  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name.get(), 500);
      if (name[0] != '.') {
        std::string fname(name.get());
        if (FsHelpers::hasImageExtension(fname)) {
          siblingImages.push_back(fname);
        }
      }
    }
    file.close();
  }
  dir.close();

  FsHelpers::sortFileList(siblingImages);

  const auto image = std::find(siblingImages.begin(), siblingImages.end(), fileName);
  if (image != siblingImages.end()) {
    currentImageIndex = static_cast<int>(image - siblingImages.begin());
  }
}

bool BmpViewerActivity::canSetSleepCover() const {
  return FsHelpers::hasBmpExtension(filePath) || FsHelpers::hasJpgExtension(filePath) ||
         (overlaySleepMode() && FsHelpers::hasPngExtension(filePath));
}

bool BmpViewerActivity::renderImage() {
  const bool hasPrevious = siblingImages.size() > 1 && currentImageIndex > 0;
  const bool hasNext = siblingImages.size() > 1 && currentImageIndex != -1 &&
                       currentImageIndex < static_cast<int>(siblingImages.size()) - 1;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), canSetSleepCover() ? tr(STR_SET_SLEEP_COVER) : "",
                                            hasPrevious ? "<" : "", hasNext ? ">" : "");
#if defined(CROSSPOINT_TTF_READER)
  // Engine boards: JPEG/PNG go through the production TTF image pipeline
  // (design amendment §7). BMP files never reach renderImage — onEnter routes
  // them to the legacy Bitmap path on every board.
  return renderImageEngine(labels);
#else
  // Small boards keep the legacy converter display verbatim.
  const bool jpeg = FsHelpers::hasJpgExtension(filePath);
  JpegToFramebufferConverter jpegDecoder;
  PngToFramebufferConverter pngDecoder;
  ImageToFramebufferDecoder& decoder =
      jpeg ? static_cast<ImageToFramebufferDecoder&>(jpegDecoder) : static_cast<ImageToFramebufferDecoder&>(pngDecoder);
  ImageDimensions dimensions;
  if (!decoder.getDimensions(filePath, dimensions)) return false;
  if (dimensions.width <= 0 || dimensions.height <= 0) return false;

  const auto config = fitImage(dimensions, renderer);
  const auto drawImage = [&]() {
    if (!decoder.decodeToFramebuffer(filePath, renderer, config)) return false;
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return true;
  };

  if (!drawImage()) return false;
  if (!jpeg) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return true;
  }

  const bool absolute = renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported();
  if (absolute && !renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Absolute)) return false;
  if (!absolute) renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  bool ready = true;
  // ponytail: decode each plane; add a pixel cache if large-image latency needs it.
  for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
    renderer.clearScreen(absolute ? 0xFF : 0x00);
    renderer.setRenderMode(mode);
    if (!drawImage()) {
      ready = false;
      break;
    }
    if (mode == GfxRenderer::GRAYSCALE_LSB) {
      renderer.copyGrayscaleLsbBuffers();
    } else {
      renderer.copyGrayscaleMsbBuffers();
    }
  }
  if (ready) renderer.displayGrayBuffer();

  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen();
  if (!drawImage()) ready = false;
  renderer.cleanupGrayscaleWithFrameBuffer();
  return ready;
#endif
}

#if defined(CROSSPOINT_TTF_READER)

namespace {
constexpr char VIEWER_FICACHE_DIR[] = "/.crosspoint/viewer/ficache";

// Dither scope: the base pass carries the image as a Bayer dither (the
// reader's baseDither, QuickPageRelayout.cpp:231); the flag must not leak
// into later painting on this renderer.
struct DitherScope {
  GfxRenderer& renderer;
  ~DitherScope() { renderer.setDitherImagesInBw(false); }
};
}  // namespace

// Engine-board JPEG/PNG display (design amendment §7.3): the production
// pipeline + the reader's absolute-arm plane sequence, not a variant.
bool BmpViewerActivity::renderImageEngine(const MappedInputManager::Labels& labels) {
  // Standalone file → production dispatch (§7.1): the open file is presented
  // as a headerless raw stored entry, so decodeAndCache runs unmodified. Both
  // objects are call-scoped — nothing outlives the synchronous decode.
  freeink::book::SdCardBookSource source(filePath.c_str());
  ttfimg::StandaloneImageCatalog catalog;
  if (!source.isValid() || !catalog.open(source.size())) {
    LOG_ERR("BMP", "Standalone image open failed: %s", filePath.c_str());
    return false;
  }
  const freeink::book::ZipEntry* entry = catalog.find(filePath.c_str());
  if (entry == nullptr) return false;

  // The reader's scratch budget (TtfBookRuntime::kScratchBytes): identical
  // SDK degrade thresholds (progressive store, DC fallback, JPEG pre-scale
  // step-down) to a reader decode. Under pool fragmentation a smaller arena
  // still decodes — the engine's own ladder degrades (pre-scale step-down,
  // DC fallback) rather than failing — so the allocation walks down before
  // the render is refused (review 2026-11-11); a refused render is then a
  // real decode-capability failure, not an allocation artifact.
  PoolBytes scratchBlock;
  size_t scratchBytes = 0;
  for (const size_t cap :
       {freeink::book::TtfBookRuntime::kScratchBytes, freeink::book::TtfBookRuntime::kScratchBytes / 2,
        freeink::book::TtfBookRuntime::kScratchBytes / 4}) {
    scratchBlock = poolMakeBytes(cap);
    if (scratchBlock) {
      scratchBytes = cap;
      break;
    }
  }
  if (!scratchBlock) {
    LOG_ERR("BMP", "OOM: image decode scratch");
    return false;
  }
  freeink::book::Arena scratch(scratchBlock.get(), scratchBytes);

  // Placement comes from the engine's own probe — the one source of truth
  // (decodeAndCache re-probes internally; the header scan is cheap next to
  // the decode).
  freeink::book::ImageInfo info;
  if (freeink::book::probeImage(source, *entry, scratch, &info) != freeink::book::BookStatus::Ok ||
      (info.kind != freeink::book::ImageInfo::Kind::Jpeg && info.kind != freeink::book::ImageInfo::Kind::Png)) {
    LOG_ERR("BMP", "Image probe failed: %s", filePath.c_str());
    return false;
  }
  const ttfimg::Placement place =
      ttfimg::fitPlacement(info.width, info.height, renderer.getScreenWidth(), renderer.getScreenHeight());

  ttfimg::ImageSpec spec{};
  if (filePath.size() >= ttfimg::ImageSpec::kHrefCap) {
    // Same overlong-href rule as the reader's paint (QuickPageRelayout.cpp:57):
    // the identity and the decode take the pointer; the decode is
    // synchronous, so the spec's outliving requirement holds.
    spec.hrefExt = filePath.c_str();
  } else {
    strncpy(spec.href, filePath.c_str(), ttfimg::ImageSpec::kHrefCap - 1);
  }
  spec.generation = ttfimg::viewerGeneration(source.modificationTime(), source.size());
  spec.x = place.x;
  spec.y = place.y;
  spec.w = place.w;
  spec.h = place.h;
  spec.orientation = static_cast<uint8_t>(renderer.getOrientation());

  // The reader's settings semantics verbatim (QuickPageRelayout.cpp:29), and
  // the reader's imageGrayscale definition (EpubReaderActivity.cpp:5832):
  // only the explicit B/W choice drops tone — Placeholder/Suppress are layout
  // choices for book pages, not tone choices for a photo viewer.
  const bool qualityTone = SETTINGS.imageDecodeQuality != 0;
  const bool clahe = qualityTone && SETTINGS.pictureDetail != 0;
  const bool imageGrayscale = SETTINGS.imageRendering != CrossPointSettings::IMAGES_DISPLAY_BW;

  if (!Storage.exists(VIEWER_FICACHE_DIR) && !Storage.mkdir(VIEWER_FICACHE_DIR)) {
    // Storage-failure doctrine: the decode degrades to painting uncached,
    // so the viewer keeps going rather than failing the image here.
    LOG_ERR("BMP", "Viewer cache dir unavailable: %s", VIEWER_FICACHE_DIR);
  }

  // One paint step, shared by every pass: cache blit first, else the same
  // decode the reader's foreground runs (first-write cache, §5.4 G3 parity).
  const auto paintEngineImage = [&]() -> bool {
    if (ttfimg::hasValidCache(spec, VIEWER_FICACHE_DIR, qualityTone, clahe) &&
        ttfimg::blitFromCache(renderer, spec, VIEWER_FICACHE_DIR, qualityTone, clahe)) {
      return true;
    }
    char cachePath[256];
    if (!ttfimg::cacheNameFor(spec, VIEWER_FICACHE_DIR, qualityTone, clahe, cachePath, sizeof(cachePath))) return false;
    const size_t mark = scratch.mark();
    const ttfimg::DecodeResult res = ttfimg::decodeAndCache(
        spec, cachePath, source, catalog, scratch, /*epub=*/nullptr, &renderer, qualityTone, /*caching=*/true,
        /*abortPoll=*/nullptr, /*abortCtx=*/nullptr, /*paintToTarget=*/true, clahe);
    scratch.release(mark);
    if (res != ttfimg::DecodeResult::Ok) {
      LOG_DBG("BMP", "Image decode kind=%d: %s", static_cast<int>(res), filePath.c_str());
      return false;
    }
    return true;
  };

  const auto drawHints = [&]() { GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4); };

  // Revert arm (imageDecodeQuality == 0): the reader's §5.6 semantics — SDK
  // error-diffusion paint over a one-image synthetic page, no tone cache.
  if (!qualityTone) {
    freeink::book::PageImage img{};
    img.href = (spec.hrefExt != nullptr) ? spec.hrefExt : spec.href;
    img.x = place.x;
    img.y = place.y;
    img.width = static_cast<uint16_t>(place.w);
    img.height = static_cast<uint16_t>(place.h);
    freeink::book::Page page{};
    page.images = &img;
    page.imageCount = 1;
    const freeink::book::BookStatus st =
        freeink::book::PageRenderer::renderImages(page, source, catalog, scratch, makeFrameTarget(renderer));
    if (st != freeink::book::BookStatus::Ok) {
      LOG_DBG("BMP", "Revert image render failed: %s", filePath.c_str());
      return false;
    }
    drawHints();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return true;
  }

  // The reader's grayscale decision table, not a viewer variant (§7.3).
  const bool route =
      imageGrayscale && ttfimg::absoluteGrayImageRoute(
                            /*pageHasImages=*/true, qualityTone, gpio.deviceIsX3(),
                            display.getController() == HalDisplay::Controller::UC8279,
                            renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported());

  // Quality arms. Gray transport = the reader's absolute route
  // (ttfAbsoluteImageGray — UC8279-class), or any differential-gray panel,
  // where the pre-PR viewer plane-danced every JPEG and the reader's own
  // overlay transports carry image tone as well (review 2026-11-11: no
  // grayscale regression on papermono-class boards). Without a gray
  // transport the baked tones dither into the B/W framebuffer.
  const bool absolute = route;
  if (imageGrayscale && (absolute || renderer.grayscaleCapabilities().supported())) {
    // Plane dance — the renderTtfImageGrayAbsolute order
    // (EpubReaderActivity.cpp:5050), stored-BW discipline included.
    renderer.clearScreen();
    renderer.setRenderMode(GfxRenderer::BW);
    if (absolute) {
      // The absolute base snapshot carries the image as a Bayer dither
      // (the reader's baseDither, QuickPageRelayout.cpp:231).
      DitherScope scope{renderer};
      renderer.setDitherImagesInBw(true);
      if (!paintEngineImage()) return false;  // nothing displayed yet: fail clean
    } else {
      // Overlay planes expect every gray pixel black in the base
      // (setDitherImagesInBw's contract) — no dither here.
      if (!paintEngineImage()) return false;
    }
    drawHints();

    if (absolute) {
      if (!renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Absolute)) {
        LOG_ERR("BMP", "Could not start absolute image display; keeping B/W base");
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        return true;
      }
    } else {
      // The legacy viewer's differential base waveform.
      renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
    }
    if (!renderer.storeBwBuffer()) {
      // The plane passes overwrite the framebuffer; without the stored base
      // they cannot be undone — keep the displayed base (reader semantics).
      LOG_ERR("BMP", "Failed to store BW buffer; keeping B/W base");
      renderer.setRenderMode(GfxRenderer::BW);
      return true;
    }

    bool planesOk = true;
    for (const auto msbPass : {false, true}) {
      // Absolute planes seed white (the walk clears dark bits); differential
      // planes seed black (the walk sets dark bits) — the pre-PR viewer's
      // exact seeds.
      renderer.clearScreen(absolute ? 0xFF : 0x00);
      renderer.setRenderMode(msbPass ? GfxRenderer::GRAYSCALE_MSB : GfxRenderer::GRAYSCALE_LSB);
      if (!paintEngineImage()) {
        planesOk = false;
        break;
      }
      drawHints();  // the plane upload replaces the whole frame — hints join the planes
      if (msbPass) {
        renderer.copyGrayscaleMsbBuffers();
      } else {
        renderer.copyGrayscaleLsbBuffers();
      }
    }

    // displayGrayBuffer BEFORE setRenderMode(BW) — the arm's exact order:
    // setRenderMode(BW) while absoluteGrayPlanes is set cancels the pass.
    if (planesOk) renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.restoreBwBuffer();
    renderer.cleanupGrayscaleWithFrameBuffer();
    if (!planesOk) {
      // A plane pass failed after the base was DISPLAYED: the restored base
      // is a usable image — keep it and report success (review 2026-11-11;
      // the error screen must not delete a shown image).
      LOG_ERR("BMP", "Plane pass failed; keeping displayed base");
    }
    return true;
  }

  // No gray transport (or imageRendering B/W): 1-bit dither, single pass.
  renderer.clearScreen();
  renderer.setRenderMode(GfxRenderer::BW);
  bool painted = true;
  {
    DitherScope scope{renderer};
    renderer.setDitherImagesInBw(true);
    painted = paintEngineImage();
  }
  if (painted) drawHints();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  return painted;
}

#endif  // CROSSPOINT_TTF_READER

void BmpViewerActivity::onEnter() {
  Activity::onEnter();

  if (siblingImages.empty() && !filePath.empty()) {
    loadSiblingImages();
  }

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  GUI.fillPopupProgress(renderer, popupRect, 20);  // Initial 20% progress
  if (FsHelpers::hasPngExtension(filePath) || FsHelpers::hasJpgExtension(filePath)) {
    renderer.clearScreen();
    if (!renderImage()) {
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, "", "", "");
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    return;
  }

  HalFile file;
  // 1. Open the BMP file
  if (Storage.openFileForRead("BMP", filePath, file)) {
    Bitmap bitmap(file, true,
                  renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported() &&
                      display.getController() == HalDisplay::Controller::SSD1677);

    // 2. Parse headers to get dimensions
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      int x, y;

      if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
        float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
        const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

        if (ratio > screenRatio) {
          // Wider than screen
          x = 0;
          y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
        } else {
          // Taller than screen
          x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
          y = 0;
        }
      } else {
        // Center small images
        x = (pageWidth - bitmap.getWidth()) / 2;
        y = (pageHeight - bitmap.getHeight()) / 2;
      }

      // 4. Prepare Rendering
      bool hasPrevious = (siblingImages.size() > 1 && currentImageIndex > 0);
      bool hasNext = (siblingImages.size() > 1 && currentImageIndex != -1 &&
                      currentImageIndex < static_cast<int>(siblingImages.size()) - 1);

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), canSetSleepCover() ? tr(STR_SET_SLEEP_COVER) : "",
                                                (hasPrevious ? "<" : ""), (hasNext ? ">" : ""));

      GUI.fillPopupProgress(renderer, popupRect, 50);

      renderer.clearScreen();
      if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
        renderer.clearScreen();
        renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        return;
      }

      // Draw UI hints on the base layer
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      if (bitmap.hasGreyscale()) {
        const bool absolute = renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported();
        if (absolute && !renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Absolute)) return;
        if (!absolute) renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
        bool planesReady = true;
        for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
          if (bitmap.rewindToData() != BmpReaderError::Ok) {
            LOG_ERR("BMP", "Failed to rewind bitmap for grayscale rendering");
            planesReady = false;
            break;
          }
          renderer.clearScreen(absolute ? 0xFF : 0x00);
          renderer.setRenderMode(mode);
          if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
            planesReady = false;
            break;
          }
          GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
          if (mode == GfxRenderer::GRAYSCALE_LSB) {
            renderer.copyGrayscaleLsbBuffers();
          } else {
            renderer.copyGrayscaleMsbBuffers();
          }
        }
        if (planesReady) renderer.displayGrayBuffer();

        // Rebuild the BW framebuffer for popups and subsequent differential updates.
        renderer.setRenderMode(GfxRenderer::BW);
        renderer.clearScreen();
        if (bitmap.rewindToData() != BmpReaderError::Ok ||
            !renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
          LOG_ERR("BMP", "Failed to rewind bitmap to restore the BW framebuffer");
          renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
          planesReady = false;
        }
        GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
        renderer.cleanupGrayscaleWithFrameBuffer();
        if (!planesReady) renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      } else {
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      }

    } else {
      // Handle file parsing error
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_INVALID_BMP_FILE));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }

    file.close();
  } else {
    // Handle file open error
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
}

void BmpViewerActivity::onExit() {
  Activity::onExit();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

bool BmpViewerActivity::saveJpegSleepCover() {
  if (Storage.exists(JPEG_SLEEP_TMP_PXC) && !Storage.remove(JPEG_SLEEP_TMP_PXC)) {
    LOG_ERR("BMP", "Failed to remove previous sleep image pixels");
    return false;
  }
  bool keepBmpTemp = false;
  const ScopedCleanup cleanup{[&keepBmpTemp]() {
    Storage.remove(JPEG_SLEEP_TMP_PXC);
    if (!keepBmpTemp) Storage.remove(CUSTOM_SLEEP_TMP_BMP);
  }};

  JpegToFramebufferConverter decoder;
  ImageDimensions dimensions;
  if (!decoder.getDimensions(filePath, dimensions)) return false;
  auto config = fitImage(dimensions, renderer);
  if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP &&
      (dimensions.width > renderer.getScreenWidth() || dimensions.height > renderer.getScreenHeight())) {
    const float ratio = static_cast<float>(dimensions.width) / dimensions.height;
    const float screenRatio = static_cast<float>(renderer.getScreenWidth()) / renderer.getScreenHeight();
    config.x = config.y = 0;
    config.maxWidth = renderer.getScreenWidth();
    config.maxHeight = renderer.getScreenHeight();
    if (ratio > screenRatio) {
      config.sourceCropX = 1.0f - screenRatio / ratio;
    } else {
      config.sourceCropY = 1.0f - ratio / screenRatio;
    }
  }
  config.cachePath = JPEG_SLEEP_TMP_PXC;
  renderer.clearScreen();
  bool cacheWritten;
  if (!decoder.decodeToFramebuffer(filePath, renderer, config, cacheWritten) || !cacheWritten) {
    LOG_ERR("BMP", "Failed to decode sleep image pixels");
    return false;
  }

  HalFile input, output;
  if (!Storage.openFileForRead("BMP", JPEG_SLEEP_TMP_PXC, input)) return false;
  uint16_t width, height;
  if (input.read(&width, sizeof(width)) != sizeof(width) || input.read(&height, sizeof(height)) != sizeof(height) ||
      width != config.maxWidth || height != config.maxHeight) {
    LOG_ERR("BMP", "Invalid sleep image pixels");
    return false;
  }
  const size_t pixelRowBytes = (width + 3) / 4;
  const size_t bmpRowBytes = (width + 15) / 16 * 4;
  if (input.size() != 4 + pixelRowBytes * height) {
    LOG_ERR("BMP", "Incomplete sleep image pixels");
    return false;
  }
  auto row = makeUniqueNoThrow<uint8_t[]>(bmpRowBytes);
  if (!row) {
    LOG_ERR("BMP", "OOM: sleep image row");
    return false;
  }
  if (!Storage.openFileForWrite("BMP", CUSTOM_SLEEP_TMP_BMP, output)) return false;
  BmpHeader header;
  createBmpHeader(&header, width, height, BmpRowOrder::TopDown);
  header.infoHeader.biBitCount = 2;
  header.infoHeader.biClrUsed = header.infoHeader.biClrImportant = 4;
  header.infoHeader.biSizeImage = bmpRowBytes * height;
  header.fileHeader.bfOffBits = sizeof(header) - sizeof(header.colors) + sizeof(GRAYSCALE_PALETTE);
  header.fileHeader.bfSize = header.fileHeader.bfOffBits + header.infoHeader.biSizeImage;
  if (output.write(&header, sizeof(header) - sizeof(header.colors)) != sizeof(header) - sizeof(header.colors) ||
      output.write(GRAYSCALE_PALETTE, sizeof(GRAYSCALE_PALETTE)) != sizeof(GRAYSCALE_PALETTE)) {
    LOG_ERR("BMP", "Failed to write sleep image header");
    return false;
  }
  for (int y = 0; y < height; ++y) {
    if (input.read(row.get(), pixelRowBytes) != pixelRowBytes || output.write(row.get(), bmpRowBytes) != bmpRowBytes) {
      LOG_ERR("BMP", "Failed to write sleep image row %d", y);
      return false;
    }
  }
  if (!output.close()) {
    LOG_ERR("BMP", "Failed to close sleep image");
    return false;
  }
  if (!Storage.replaceFile(CUSTOM_SLEEP_TMP_BMP, CUSTOM_SLEEP_ROOT_BMP)) {
    // ponytail: FAT replacement is not atomic; keep the complete temp for recovery.
    keepBmpTemp = true;
    LOG_ERR("BMP", "Failed to publish sleep image");
    return false;
  }
  return true;
}

void BmpViewerActivity::doSetSleepCover() {
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  const bool jpeg = FsHelpers::hasJpgExtension(filePath);
  const bool transparentMode = !jpeg && overlaySleepMode();
  if (!canSetSleepCover()) return;

  const char* destination =
      transparentMode ? (FsHelpers::hasPngExtension(filePath) ? TRANSPARENT_SLEEP_ROOT_PNG : TRANSPARENT_SLEEP_ROOT_BMP)
                      : CUSTOM_SLEEP_ROOT_BMP;
  bool success = filePath == destination;

  if (jpeg) {
    success = saveJpegSleepCover();
  } else if (!success) {
    auto buffer = makeUniqueNoThrow<uint8_t[]>(COPY_BUFFER_SIZE);
    if (!buffer) {
      LOG_ERR("BMP", "OOM: sleep cover copy buffer");
    } else {
      // Copy beside the target and swap in only a complete image, so a failed
      // copy keeps the previous sleep cover instead of a truncated one.
      const std::string tmp = std::string(destination) + ".tmp";
      HalFile inFile, outFile;
      if (Storage.openFileForRead("BMP", filePath, inFile) && Storage.openFileForWrite("BMP", tmp, outFile)) {
        int bytesRead;
        success = true;
        while ((bytesRead = inFile.read(buffer.get(), COPY_BUFFER_SIZE)) > 0) {
          if (outFile.write(buffer.get(), static_cast<size_t>(bytesRead)) != static_cast<size_t>(bytesRead)) {
            success = false;
            break;
          }
        }
        if (bytesRead < 0) success = false;
        outFile.close();
        success = success && Storage.replaceFile(tmp.c_str(), destination);
        if (!success) Storage.remove(tmp.c_str());
      }
    }
  }

  if (success) {
    if (!transparentMode) SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
    SETTINGS.saveToFile();
    GUI.drawPopup(renderer, tr(STR_DONE));
  } else {
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
  }

  delay(1000);
  onEnter();
}

void BmpViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  auto openSibling = [this](const int delta) {
    if (currentImageIndex < 0) {
      return false;
    }
    const int nextIndex = currentImageIndex + delta;
    if (siblingImages.size() <= 1 || nextIndex < 0 || nextIndex >= static_cast<int>(siblingImages.size())) {
      return false;
    }
    currentImageIndex = nextIndex;
    std::string dirPath = FsHelpers::extractFolderPath(filePath);
    if (dirPath.back() != '/') dirPath += "/";
    filePath = dirPath + siblingImages[currentImageIndex];
    onEnter();
    return true;
  };

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToFileBrowser(filePath);
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left) {
    openSibling(1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right) {
    openSibling(-1);
    return;
  }

  // Touch boards draw no button hints; long-press stands in for Confirm, as for file browser actions.
  int touchX = 0;
  int touchY = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenLongPress(touchX, touchY)) {
    if (!canSetSleepCover()) return;

    auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_SET_SLEEP_COVER_CONFIRM),
                                                                filePath.substr(filePath.find_last_of('/') + 1));
    if (!confirmation) {
      LOG_ERR("BMP", "OOM: sleep cover confirmation (%zu bytes)", sizeof(ConfirmationActivity));
      return;
    }
    startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
      if (result.isCancelled) {
        onEnter();
        return;
      }
      doSetSleepCover();
    });
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    openSibling(-1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    openSibling(1);
    return;
  }
}
