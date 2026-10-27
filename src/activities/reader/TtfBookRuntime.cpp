// TtfBookRuntime.cpp — see TtfBookRuntime.h. Whole unit gated by
// CROSSPOINT_TTF_READER (§14.1: nothing links on PSRAM-less boards).

#if defined(CROSSPOINT_TTF_READER)

#include "TtfBookRuntime.h"

#include <BookFontLoader.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <cmath>
#include <cstring>

#include "ReaderLineSpacing.h"
#include "components/UITheme.h"

namespace freeink {
namespace book {

TtfBookRuntime::~TtfBookRuntime() { close(); }

bool TtfBookRuntime::ensureArenas() {
  if (!bookArenaBytes_) {
    bookArenaBytes_ = poolMakeBytes(kBookArenaBytes);
    if (!bookArenaBytes_) {
      LOG_ERR("TTFB", "OOM: %u bytes book arena", static_cast<unsigned>(kBookArenaBytes));
      return false;
    }
    bookArena_ = Arena(bookArenaBytes_.get(), kBookArenaBytes);
  }
  if (!cacheArenaBytes_) {
    cacheArenaBytes_ = poolMakeBytes(kCacheArenaBytes);
    if (!cacheArenaBytes_) {
      LOG_ERR("TTFB", "OOM: %u bytes cache arena", static_cast<unsigned>(kCacheArenaBytes));
      return false;
    }
    cacheArena_ = Arena(cacheArenaBytes_.get(), kCacheArenaBytes);
  }
  if (!scratchBytes_) {
    scratchBytes_ = poolMakeBytes(kScratchBytes);
    if (!scratchBytes_) {
      LOG_ERR("TTFB", "OOM: %u bytes scratch arena", static_cast<unsigned>(kScratchBytes));
      return false;
    }
    scratch_ = Arena(scratchBytes_.get(), kScratchBytes);
  }
  if (!parseBytes_) {
    parseBytes_ = poolMakeBytes(kParseArenaBytes);
    if (!parseBytes_) {
      LOG_ERR("TTFB", "OOM: %u bytes parse arena", static_cast<unsigned>(kParseArenaBytes));
      return false;
    }
    parseArena_ = Arena(parseBytes_.get(), kParseArenaBytes);
  }
  return true;
}

void TtfBookRuntime::resetBuildArenas() {
  // Only while no session holds marks in them (session over).
  scratch_.reset();
  parseArena_.reset();
  prescanBytes_.reset();
  prescanArena_ = Arena{};
}

void TtfBookRuntime::releaseArenas() {
  bookArenaBytes_.reset();
  bookArena_ = Arena{};
  cacheArenaBytes_.reset();
  cacheArena_ = Arena{};
  scratchBytes_.reset();
  scratch_ = Arena{};
  parseBytes_.reset();
  parseArena_ = Arena{};
  prescanBytes_.reset();
  prescanArena_ = Arena{};
  prefetchArenaBytes_.reset();
  prefetchArena_ = Arena{};
}

bool TtfBookRuntime::open(const char* epubPath, const char* cacheDir) {
  close();
  bookSource_ = SdCardBookSource(epubPath);
  cacheStorage_ = SdCardCacheStorage(cacheDir);
  if (!bookSource_.isValid()) {
    LOG_ERR("TTFB", "Cannot open book source: %s", epubPath);
    return false;
  }
  if (!ensureArenas()) return false;

  if (!cacheStorage_.exists(BookCatalog::kCatalogName)) {
    scratch_.reset();
    parseArena_.reset();
    const BookStatus st = BookCatalog::build(bookSource_, cacheStorage_, scratch_, &parseArena_);
    if (st != BookStatus::Ok) {
      LOG_ERR("TTFB", "Catalog build failed: %s", bookStatusName(st));
      LOG_DBG("TTFB", "scratch highWater=%u failedAllocSize=%u", static_cast<unsigned>(scratch_.highWater()),
              static_cast<unsigned>(scratch_.failedAllocSize()));
      scratch_.reset();
      return false;
    }
    scratch_.reset();
  }

  bookArena_ = Arena(bookArenaBytes_.get(), kBookArenaBytes);
  scratch_.reset();
  BookStatus st = catalog_.open(bookSource_, cacheStorage_, bookArena_, scratch_);
  if (st == BookStatus::Stale) {
    LOG_DBG("TTFB", "Catalog stale, rebuilding");
    BookStatus bst = BookCatalog::build(bookSource_, cacheStorage_, scratch_, &parseArena_);
    scratch_.reset();
    if (bst != BookStatus::Ok) {
      LOG_ERR("TTFB", "Catalog rebuild failed: %s", bookStatusName(bst));
      return false;
    }
    bookArena_ = Arena(bookArenaBytes_.get(), kBookArenaBytes);
    scratch_.reset();
    st = catalog_.open(bookSource_, cacheStorage_, bookArena_, scratch_);
  }
  if (st != BookStatus::Ok) {
    LOG_ERR("TTFB", "Catalog open failed: %s", bookStatusName(st));
    scratch_.reset();
    return false;
  }
  scratch_.reset();
  LOG_INF("TTFB", "Catalog open: spines=%u bookArena used=%u", static_cast<unsigned>(catalog_.spineCount()),
          static_cast<unsigned>(bookArena_.used()));
  return true;
}

void TtfBookRuntime::close() {
  abortSession();
  closeChapterCache();
  dropPrefetch();
  catalog_ = BookCatalog{};
  bookSource_ = SdCardBookSource{};
  cacheStorage_ = SdCardCacheStorage{};
  releaseArenas();
}

// ---- chapter cache serving ----

BookStatus TtfBookRuntime::openChapterCache(const uint16_t spineIndex, const uint32_t generation) {
  const unsigned long startMs = millis();
  cacheStorage_.resetReadAtCount();
  const BookStatus st = openChapterCacheInner(spineIndex, generation);
  // Warm-open soak telemetry, unconditional: the storage readAt count is the
  // on-device cost driver (one full SD open/close per call).
  LOG_INF("TTFB", "openChapterCache spine=%u gen=%08x pages=%u ms=%lu readAt=%u status=%s",
          static_cast<unsigned>(spineIndex), generation, st == BookStatus::Ok ? cacheReader_.pageCount() : 0u,
          millis() - startMs, static_cast<unsigned>(cacheStorage_.readAtCount()), bookStatusName(st));
  return st;
}

BookStatus TtfBookRuntime::openChapterCacheInner(const uint16_t spineIndex, const uint32_t generation) {
  closeChapterCache();
  if (!ensureArenas()) return BookStatus::OutOfMemory;
  if (!pageCacheName(spineIndex, generation, cacheName_, sizeof(cacheName_))) {
    LOG_ERR("TTFB", "pageCacheName overflow");
    return BookStatus::NotFound;
  }
  if (!cacheStorage_.exists(cacheName_)) {
    return BookStatus::NotFound;  // cold chapter
  }
  cacheArena_ = Arena(cacheArenaBytes_.get(), kCacheArenaBytes);
  const BookStatus st = cacheReader_.open(cacheStorage_, cacheName_, generation, cacheArena_);
  if (st == BookStatus::Ok) {
    cacheReady_ = true;
    cacheSpine_ = spineIndex;
    cacheGen_ = generation;
  } else {
    cacheArena_ = Arena{};  // release the failed open's allocations
  }
  return st;
}

void TtfBookRuntime::closeChapterCache() {
  cacheReader_ = PageCacheReader{};
  cacheReady_ = false;
  cacheSpine_ = kNoSpine;
  cacheGen_ = 0;
  cacheArena_ = Arena{};
}

uint32_t TtfBookRuntime::availablePageCount(const uint16_t spineIndex) const {
  if (sessionFor(spineIndex)) return writer_.pageCount();
  // The open cache reader answers only its own chapter.
  return (cacheReady_ && cacheSpine_ == spineIndex) ? cacheReader_.pageCount() : 0;
}

uint32_t TtfBookRuntime::pageCharStart(const uint16_t spineIndex, const uint16_t pageIndex) const {
  if (sessionFor(spineIndex)) return writer_.charStart(pageIndex);
  return (cacheReady_ && cacheSpine_ == spineIndex) ? cacheReader_.charStart(pageIndex) : 0;
}

bool TtfBookRuntime::readPage(const uint16_t spineIndex, const uint16_t pageIndex, Page* out) {
  if (sessionFor(spineIndex) && pageIndex < writer_.pageCount()) {
    return writer_.readPage(pageIndex, scratch_, out) == BookStatus::Ok;
  }
  if (cacheReady_ && cacheSpine_ == spineIndex && pageIndex < cacheReader_.pageCount()) {
    return cacheReader_.readPage(pageIndex, scratch_, out) == BookStatus::Ok;
  }
  return false;
}

bool TtfBookRuntime::pageForChar(const uint16_t spineIndex, const uint32_t charOffset, uint32_t* pageOut) const {
  if (sessionFor(spineIndex)) {
    *pageOut = writer_.pageForChar(charOffset);
    return writer_.pageCount() > 0;
  }
  if (!cacheReady_ || cacheSpine_ != spineIndex) return false;
  *pageOut = cacheReader_.pageForChar(charOffset);
  return cacheReader_.pageCount() > 0;
}

bool TtfBookRuntime::charForAnchor(const uint16_t spineIndex, const uint32_t idHash, uint32_t* charOut) const {
  if (sessionFor(spineIndex)) return writer_.charForAnchor(idHash, charOut);
  return cacheReady_ && cacheSpine_ == spineIndex && cacheReader_.charForAnchor(idHash, charOut);
}

// ---- chapter build session ----

BookStatus TtfBookRuntime::beginChapterSession(const uint16_t spineIndex, const LayoutParams& params,
                                               const uint32_t generation) {
  abortSession();
  if (!ensureArenas()) return BookStatus::OutOfMemory;

  ZipEntry entry;
  BookStatus st = catalog_.spineEntry(spineIndex, &entry);
  if (st != BookStatus::Ok) return st;
  st = catalog_.spineHref(spineIndex, spineHref_, sizeof(spineHref_));
  if (st != BookStatus::Ok) return st;

  scratch_.reset();
  parseArena_.reset();
  if (!pageCacheName(spineIndex, generation, cacheName_, sizeof(cacheName_))) {
    LOG_ERR("TTFB", "pageCacheName overflow");
    return BookStatus::NotFound;
  }
  if (!prescanBytes_) {
    prescanBytes_ = poolMakeBytes(kPrescanBytes);
    if (!prescanBytes_) {
      LOG_ERR("TTFB", "OOM: %u bytes prescan arena", static_cast<unsigned>(kPrescanBytes));
      // Proceed without a prescan arena: image pre-probing degrades to the
      // resident parse arena (slower peak, same correctness).
    } else {
      prescanArena_ = Arena(prescanBytes_.get(), kPrescanBytes);
    }
  }
  st = session_.begin(bookSource_, &catalog_.zip(), bookSource_, entry, spineHref_, params, scratch_, writer_,
                      &parseArena_, prescanBytes_ ? &prescanArena_ : nullptr);
  prescanBytes_.reset();  // transient: freed right after begin() returns
  prescanArena_ = Arena{};
  if (st != BookStatus::Ok) {
    LOG_ERR("TTFB", "Session begin failed: %s (%s)", bookStatusName(st), spineHref_);
    session_.abort();
    if (!writer_.finish()) {
      LOG_ERR("TTFB", "Failed to discard writer temp for %s", cacheName_);
    }
    writer_ = PageCacheWriter{};
    scratch_.reset();
    parseArena_.reset();
    return st;
  }
  if (!writer_.begin(cacheStorage_, cacheName_, generation, scratch_)) {
    LOG_ERR("TTFB", "Writer begin failed for %s", cacheName_);
    session_.abort();
    if (!writer_.finish()) {
      LOG_ERR("TTFB", "Failed to close/remove writer temp for %s", cacheName_);
    }
    writer_ = PageCacheWriter{};
    scratch_.reset();
    parseArena_.reset();
    return BookStatus::IoError;
  }
  sessionSpine_ = spineIndex;
  sessionGen_ = generation;
  return BookStatus::Ok;
}

BookStatus TtfBookRuntime::stepBuild(const uint16_t minNewPages) {
  if (sessionSpine_ == kNoSpine) return BookStatus::NotFound;
  if (session_.done()) {
    return finishSession() ? BookStatus::Ok : BookStatus::IoError;
  }
  const BookStatus st = session_.step(minNewPages);
  if (writer_.failed()) {
    LOG_ERR("TTFB", "Writer failed mid-build (%s)", cacheName_);
    abortSession();
    return BookStatus::IoError;
  }
  if (!session_.done() && !session_.active()) {
    LOG_ERR("TTFB", "Session step failed: %s", bookStatusName(st));
    abortSession();
    return st;
  }
  if (session_.done()) return finishSession() ? BookStatus::Ok : BookStatus::IoError;
  return BookStatus::Ok;
}

bool TtfBookRuntime::finishSession() {
  if (sessionSpine_ == kNoSpine) return false;
  // The rename publish below replaces the cache file; release any prefetch
  // reader for this spine before its backing file changes.
  dropPrefetch();
  writer_.setTotalChars(session_.totalChars());
  const bool ok = writer_.finish();
  if (!ok) {
    // The chapter stays uncached: the next open rebuilds it instead of the
    // caller believing a cache file exists.
    LOG_ERR("TTFB", "Writer finish failed for %s", cacheName_);
  }
  LOG_DBG("TTFB", "Session done: %u pages, totalChars=%u, scratch highWater=%u failedAlloc=%u",
          static_cast<unsigned>(writer_.pageCount()), static_cast<unsigned>(session_.totalChars()),
          static_cast<unsigned>(scratch_.highWater()), static_cast<unsigned>(scratch_.failedAllocSize()));
  // Release the engine/sink/parser objects while their arena backing is still
  // intact. resetBuildArenas() only clears marks, so skipping this left
  // session_ holding dangling engine_/parser_ pointers into reset arena
  // memory — the next begin()'s abort() then ran destructors on reused
  // memory (LoadProhibited crash opening the second chapter).
  session_.abort();
  writer_ = PageCacheWriter{};
  sessionSpine_ = kNoSpine;
  sessionGen_ = 0;
  resetBuildArenas();
  return ok;
}

void TtfBookRuntime::abortSession() {
  if (sessionSpine_ == kNoSpine) return;
  if (session_.active()) {
    const uint64_t consumed = session_.bytesConsumed();
    const uint64_t total = session_.bytesTotal();
    LOG_INF("TTFB", "Suspending build spine %u: %u pages (%u/%u bytes)", static_cast<unsigned>(sessionSpine_),
            static_cast<unsigned>(writer_.pageCount()), static_cast<unsigned>(consumed), static_cast<unsigned>(total));
    if (!writer_.suspend(static_cast<uint32_t>(consumed), static_cast<uint32_t>(total))) {
      LOG_ERR("TTFB", "Writer suspend failed — partial build not committed (%u pages lost, spine %u)",
              static_cast<unsigned>(writer_.pageCount()), static_cast<unsigned>(sessionSpine_));
      writer_.finish();  // Close/remove the active .tmp after a failed suspend.
    } else {
      LOG_DBG("TTFB", "Partial build suspended: %u pages (%u/%u bytes)", static_cast<unsigned>(writer_.pageCount()),
              static_cast<unsigned>(consumed), static_cast<unsigned>(total));
    }
  } else if (session_.done()) {
    finishSession();
    return;
  } else {
    // A layout-step failure can deactivate the session while the writer is
    // still open. finish() closes/removes its .tmp handle (or commits nothing
    // after a storage failure), so the storage can accept beginWrite again.
    if (!writer_.finish()) {
      LOG_ERR("TTFB", "Discarding failed build write for %s", cacheName_);
    }
  }
  session_.abort();
  writer_ = PageCacheWriter{};
  sessionSpine_ = kNoSpine;
  sessionGen_ = 0;
  resetBuildArenas();
}

BookStatus TtfBookRuntime::quickLayoutPage(const uint16_t spineIndex, const LayoutParams& params, PageSink& sink) {
  // A live build owns scratch_/parseArena_. The caller must fall back to a
  // full reflow in that rare case rather than corrupting the session.
  if (sessionActive()) return BookStatus::Unsupported;

  ZipEntry entry;
  BookStatus st = catalog_.spineEntry(spineIndex, &entry);
  if (st != BookStatus::Ok) return st;
  st = catalog_.spineHref(spineIndex, spineHref_, sizeof(spineHref_));
  if (st != BookStatus::Ok) return st;

  scratch_.reset();
  parseArena_.reset();
  st = ChapterLayout::layout(bookSource_, catalog_.zip(), entry, spineHref_, params, scratch_, sink, nullptr, nullptr,
                             &parseArena_);
  // The quick pass is transient by contract; release its marks even if the
  // engine returned a failure so the next page read starts from clean arenas.
  scratch_.reset();
  parseArena_.reset();
  return st;
}

bool TtfBookRuntime::openPrefetch(const uint16_t spineIndex, const uint32_t generation) {
  if (prefetchFor(spineIndex) && prefetchGen_ == generation) return true;
  dropPrefetch();
  if (!ensureArenas()) return false;
  if (!prefetchArenaBytes_) {
    prefetchArenaBytes_ = poolMakeBytes(kPrefetchArenaBytes);
    if (!prefetchArenaBytes_) {
      LOG_ERR("TTFB", "OOM: %u bytes prefetch arena", static_cast<unsigned>(kPrefetchArenaBytes));
      return false;
    }
  }
  if (!pageCacheName(spineIndex, generation, prefetchCacheName_, sizeof(prefetchCacheName_))) return false;
  if (!cacheStorage_.exists(prefetchCacheName_)) return false;
  prefetchArena_ = Arena(prefetchArenaBytes_.get(), kPrefetchArenaBytes);
  const BookStatus st = prefetchReader_.open(cacheStorage_, prefetchCacheName_, generation, prefetchArena_);
  if (st != BookStatus::Ok) {
    prefetchArena_ = Arena{};
    return false;
  }
  prefetchSpine_ = spineIndex;
  prefetchGen_ = generation;
  return true;
}

void TtfBookRuntime::dropPrefetch() {
  prefetchReader_ = PageCacheReader{};
  prefetchSpine_ = -1;
  prefetchGen_ = 0;
  prefetchArena_ = Arena{};
}

// ---- layout params ----

void TtfBookRuntime::applyReaderLayoutParams(LayoutParams& params) {
  // Parity with the legacy engine (upstream #4174): the setting is a % of FONT
  // size there, while this engine's lineSpacingPct is a % of the font's natural
  // line height (ChapterLayout lineMetrics height, ~1.2 em). Scale through the
  // 1.2 em anchor: the 1.2x default maps to 100% (natural height), the same
  // visual the fork's old NORMAL produced.
  const uint8_t idx = SETTINGS.lineSpacing < CrossPointSettings::LINE_COMPRESSION_COUNT ? SETTINGS.lineSpacing : 0;
  params.lineSpacingPct = static_cast<uint16_t>(lroundf(LINE_SPACING_MULTIPLIERS[idx] * 100.0f / 1.2f));
  // §2.3 contract: 150 = the half-line extra gap (design line 284); mirrors
  // the legacy preview/reader mapping, not an arbitrary 130.
  params.paragraphSpacingPct = SETTINGS.extraParagraphSpacing != 0 ? 150 : 100;
  switch (SETTINGS.paragraphAlignment % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT) {
    case CrossPointSettings::LEFT_ALIGN:
      params.defaultAlign = TextAlign::Left;
      break;
    case CrossPointSettings::CENTER_ALIGN:
      params.defaultAlign = TextAlign::Center;
      break;
    case CrossPointSettings::RIGHT_ALIGN:
      params.defaultAlign = TextAlign::Right;
      break;
    case CrossPointSettings::JUSTIFIED:
    case CrossPointSettings::BOOK_STYLE:
    default:
      params.defaultAlign = TextAlign::Justify;
      break;
  }
  params.focusReading = SETTINGS.focusReadingEnabled != 0;
  // The reader setting is authoritative: the legacy Section path honors
  // spec.embeddedStyle (CrossPointSettings.cpp), so the TTF path must too.
  // layoutGenerationHash covers the flag, so toggling invalidates caches.
  params.embeddedStyles = SETTINGS.embeddedStyle != 0;
  params.hyphenator = nullptr;  // Phase 2b
  params.baseSizePx = static_cast<uint16_t>(lroundf(static_cast<float>(SETTINGS.ttfFontPointSize) * 150.0f / 72.0f));
  // Reader typography controls, in pixels. Word spacing is stored as a
  // percentage (WORD_SPACING_MIN..MAX in 25% steps) and resolved against the
  // body's own space width, so 100% is the font's natural gap. After
  // baseSizePx: the ratio needs the real body size, not the struct default.
  const int16_t bodySpacePx = static_cast<int16_t>(params.baseSizePx / 4);
  params.wordSpacingPx = static_cast<int16_t>(
      (static_cast<int32_t>(bodySpacePx) * static_cast<int32_t>(SETTINGS.wordSpacing)) / 100 - bodySpacePx);
  params.characterSpacingPx = SETTINGS.getCharacterSpacing();
}

void TtfBookRuntime::makeLayoutParams(GfxRenderer& renderer, LayoutParams& out, const bool autoTurnActive) const {
  out.pageWidth = static_cast<int16_t>(renderer.getScreenWidth());
  out.pageHeight = static_cast<int16_t>(renderer.getScreenHeight());
  int top = 0, right = 0, bottom = 0, left = 0;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const uint8_t screenMargin = SETTINGS.screenMargin;
  top += screenMargin;
  left += screenMargin;
  right += screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (autoTurnActive && (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    bottom += std::max(screenMargin, static_cast<uint8_t>(statusBarHeight +
                                                          UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    bottom += std::max(screenMargin, statusBarHeight);
  }
  out.marginLeft = static_cast<int16_t>(left);
  out.marginRight = static_cast<int16_t>(right);
  out.marginTop = static_cast<int16_t>(top);
  out.marginBottom = static_cast<int16_t>(bottom);

  applyReaderLayoutParams(out);

  const char* lang = catalog_.metadata().language;
  out.language = (lang != nullptr && lang[0] != '\0') ? lang : "en";
  // Phase 3 family selection (design §3.6): the loader loads the family the
  // settings name; empty selection = built-in fallback chain. Bitmap-engine
  // rollback never selects a TTF family.
  fontLoader.selectFamily(
      SETTINGS.readerFontEngine == CrossPointSettings::READER_ENGINE_TTF ? SETTINGS.ttfFontFamilyName : "");
  FontChain* chain = fontLoader.getReaderFont();
  if (chain == nullptr || chain->styleCoverage() == 0) {
    // builtinFallback() can serve an empty chain when its PSRAM backing
    // failed: layout without any face is not viable, so reject here.
    LOG_ERR("TTFB", "No reader font chain available — cannot lay out");
    out.font = nullptr;
    return;
  }
  out.font = chain;
}

}  // namespace book
}  // namespace freeink

#endif  // CROSSPOINT_TTF_READER