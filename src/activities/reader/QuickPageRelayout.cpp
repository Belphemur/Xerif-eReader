#include "QuickPageRelayout.h"

#if defined(CROSSPOINT_TTF_READER)

#include <BookTypes.h>
#include <Logging.h>

#include "BookFontLoader.h"
#include "CrossPointSettings.h"
#include "Epub/blocks/ImageDecodeGate.h"
#include "TtfImageEngine.h"
#include "adapters/FrameTargetFactory.h"
#include "adapters/PagePaint.h"
#include "render/PageRenderer.h"
#include "render/TtfFont.h"

namespace {

// Image step of the TTF paint (§5.4): cache hit tone-row blits, else the
// same decode the worker runs (G3 parity — cache populated in the paint
// fallback too), else (policy off) today's SDK-renderImages behavior by
// construction.
void paintTtfPageImages(const freeink::book::Page& page, freeink::book::TtfBookRuntime& ttf, GfxRenderer& renderer) {
  const bool qualityTone = SETTINGS.imageDecodeQuality != 0;
  if (!qualityTone) {
    // Revert switch: the previous image paint path verbatim.
    const freeink::book::BookStatus st = freeink::book::PageRenderer::renderImages(
        page, ttf.source(), ttf.catalog().zip(), ttf.scratch(), makeFrameTarget(renderer));
    if (st != freeink::book::BookStatus::Ok) {
      LOG_DBG("QPR", "TTF image render failed: %s", freeink::book::bookStatusName(st));
    }
    return;
  }

  const char* dir = ttf.cacheDir();
  if (dir == nullptr || dir[0] == '\0') return;
  // An href past ImageSpec::kHrefCap cannot key the tone cache, but the SDK
  // renderer takes the href by pointer with no length cap: paint the whole
  // page through it instead of leaving those images blank. The quality blits
  // below overwrite the SDK's own pixels for the cacheable images.
  for (uint16_t m = 0; m < page.imageCount; ++m) {
    const freeink::book::PageImage& img = page.images[m];
    if (img.href != nullptr && strlen(img.href) >= ttfimg::ImageSpec::kHrefCap) {
      const freeink::book::BookStatus st = freeink::book::PageRenderer::renderImages(
          page, ttf.source(), ttf.catalog().zip(), ttf.scratch(), makeFrameTarget(renderer));
      if (st != freeink::book::BookStatus::Ok) {
        LOG_DBG("QPR", "TTF image render failed: %s", freeink::book::bookStatusName(st));
      }
      break;
    }
  }
  for (uint16_t m = 0; m < page.imageCount; ++m) {
    const freeink::book::PageImage& img = page.images[m];
    if (img.width == 0 || img.height == 0) continue;

    ttfimg::ImageSpec spec{};
    if (img.href != nullptr && strlen(img.href) < ttfimg::ImageSpec::kHrefCap) {
      strncpy(spec.href, img.href, ttfimg::ImageSpec::kHrefCap - 1);
    } else {
      LOG_DBG("QPR", "Image href missing/overlong — skipped");
      continue;
    }
    spec.generation = ttf.cacheGeneration();
    spec.spine = ttf.cacheSpine();
    spec.page = page.pageIndex;
    spec.imgIdx = m;
    spec.x = img.x;
    spec.y = img.y;
    spec.w = static_cast<int16_t>(img.width);
    spec.h = static_cast<int16_t>(img.height);
    spec.orientation = static_cast<uint8_t>(renderer.getOrientation());

    if (ttfimg::hasValidCache(spec, dir, qualityTone) && ttfimg::blitFromCache(renderer, spec, dir, qualityTone)) {
      continue;
    }
    // A cache hit that failed to blit (unreadable file, torn payload) falls
    // through to the foreground decode: the same path a cache miss takes,
    // repainting the placement area fully.

    // First-write-cache (§5.4): the foreground decode populates the same
    // tone cache the worker would have — single-writer gate first (§5.7
    // rule 1); on gate timeout the space stays blank and a later pass
    // retries, typically onto the worker's fresh cache.
    char cachePath[256];
    if (!ttfimg::cacheNameFor(spec, dir, qualityTone, cachePath, sizeof(cachePath))) continue;
    const uint64_t hash = ImageDecodeGate::pathHash(cachePath);
    if (!ImageDecodeGate::acquireForeground(hash)) continue;
    // Decode bus: the worker yields its in-flight decode at the next band
    // boundary (the per-image claim above already marked it); the bounded
    // wait covers the band unwind. On timeout the space stays blank this
    // pass and the next pass retries — never a stall on the loop task.
    if (!ImageDecodeGate::acquireForegroundBus(500)) {
      // The GateGuard below never ran: drop the per-image claim or it stays
      // Owner::Foreground forever and fills the claim table.
      ImageDecodeGate::releaseForeground(hash);
      continue;
    }
    {
      struct GateGuard {
        uint64_t hash;
        ~GateGuard() {
          ImageDecodeGate::releaseForegroundBus();
          ImageDecodeGate::releaseForeground(hash);
        }
      } guard{hash};
      const size_t scratchMark = ttf.scratch().mark();
      const ttfimg::DecodeResult res =
          ttfimg::decodeAndCache(spec, cachePath, ttf.source(), ttf.catalog().zip(), ttf.scratch(), ttf.decodeEpub(),
                                 &renderer, qualityTone, /*caching=*/true, nullptr, nullptr);
      ttf.scratch().release(scratchMark);
      if (res != ttfimg::DecodeResult::Ok) {
        LOG_DBG("QPR", "TTF image decode: kind=%d %s", static_cast<int>(res), spec.href);
      }
    }
  }
}

}  // namespace

QuickRelayoutResult quickRelayoutPage(freeink::book::TtfBookRuntime& ttf, GfxRenderer& renderer,
                                      const uint16_t spineIndex, const uint32_t anchorChar,
                                      const uint32_t windowEndChar, const bool autoPageTurn, QuickPageCapture& capture,
                                      PoolBytes& sessionBuffer) {
  QuickRelayoutResult out;

  // A failed layout must not leave the previous settings' capture ready: the
  // caller would otherwise paint it through a null/stale font chain.
  capture.reset();

  freeink::book::LayoutParams params;
  ttf.makeLayoutParams(renderer, params, autoPageTurn);
  if (params.font == nullptr) {
    LOG_ERR("QPR", "Quick relayout: no font chain");
    return out;
  }
  out.font = params.font;
  out.characterSpacingPx = params.characterSpacingPx;
  out.wordSpacingPx = params.wordSpacingPx;

  // One backing buffer per surface session: allocated on the first relayout,
  // freed when the owning surface closes, so changes never churn the PSRAM
  // pool. Without it the sink would fall back to painting every scanned page.
  if (!capture.attached()) {
    if (!sessionBuffer) sessionBuffer = poolMakeBytes(QuickPageCapture::kBufferBytes);
    if (sessionBuffer) {
      capture.attach(sessionBuffer.get(), QuickPageCapture::kBufferBytes);
    } else {
      LOG_ERR("QPR", "OOM: quick relayout buffer");
      return out;
    }
  }

  // Page-local window (ADR 2026-10-14): the engine lays only the page
  // containing the anchor. At anchor 0 the window is disabled (a full pass
  // would lay the whole chapter) — the sink stops it after page 0. The sink
  // bounds the capture to the entry page's span so a smaller font cannot
  // pull later pages' text into the preview.
  params.windowStartChar = anchorChar;
  QuickWindowSink sink(capture, anchorChar, windowEndChar);

  const auto st = ttf.quickLayoutPage(spineIndex, params, sink);
  out.captured = capture.ready();
  // reachedAnchor: the sink captured a page whose text reaches past the
  // anchor (a pass without one — layout failure, anchor past the chapter's
  // last char — is unconfirmed).
  out.reachedAnchor = sink.reachedAnchor();
  if (!out.reachedAnchor || st != freeink::book::BookStatus::Ok) {
    LOG_DBG("QPR", "quick relayout: window page %s (status %s)", out.reachedAnchor ? "captured" : "missed",
            freeink::book::bookStatusName(st));
  }
  return out;
}

void paintCapturedPage(const freeink::book::Page& page, void* font, GfxRenderer& renderer,
                       freeink::book::TtfBookRuntime& ttf, const int16_t characterSpacingPx,
                       const int16_t wordSpacingPx) {
  // §11 Q7 construction (a): with text AA engaged the paint goes via PagePaint
  // (the tone-1 boundary of the shared uniform quantizer) and a dual plane
  // walk supplies the two gray tones through the panel's AA waveform — the
  // same 4-level pipeline the bitmap reader uses. Images keep the 1bpp
  // engine path (no plane bits for image pixels).
  const bool pageHasImages = page.imageCount > 0 && SETTINGS.imageRendering == CrossPointSettings::IMAGES_DISPLAY;
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  const bool smoothText = !freeink::book::fontLoader.effectiveMonochrome();
#else
  const bool smoothText = SETTINGS.textRenderMode == CrossPointSettings::TEXT_RENDER_SMOOTH;
#endif
  const bool grayParity = smoothText && !pageHasImages && renderer.grayscaleCapabilities().supported();
  auto* chain = static_cast<freeink::book::FontChain*>(font);
  if (grayParity) {
    freeink::book::PagePaint::paintText(page, *chain, renderer, characterSpacingPx, wordSpacingPx);
  } else {
    const freeink::book::FrameTarget frameTarget = makeFrameTarget(renderer);
    freeink::book::PageRenderer::renderText(page, *chain, frameTarget, nullptr, characterSpacingPx, wordSpacingPx);
    // Ruby annotations are engine records — the same pass the engine's own
    // render() runs; no CrossPoint layout involvement.
    if (page.rubyCount > 0) {
      freeink::book::PageRenderer::renderRubies(page, *chain, frameTarget);
    }
  }
  freeink::book::PageRenderer::renderRules(page, makeFrameTarget(renderer));
  if (pageHasImages) {
    paintTtfPageImages(page, ttf, renderer);
  } else if (SETTINGS.imageRendering == CrossPointSettings::IMAGES_PLACEHOLDER) {
    // §3.5 item 11: image policy is CrossPoint-side; placeholder mode draws
    // the engine's reserved geometry as an outline instead of decoding.
    for (uint16_t m = 0; m < page.imageCount; ++m) {
      const auto& image = page.images[m];
      if (image.width <= 0 || image.height <= 0) continue;
      renderer.drawRect(image.x, image.y, image.width, image.height);
    }
  }
}

#endif  // CROSSPOINT_TTF_READER
