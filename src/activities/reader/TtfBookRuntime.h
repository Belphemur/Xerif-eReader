#pragma once

// TtfBookRuntime — the native-TTF chapter pipeline owned by EpubReaderActivity
// (design §3.5, Phase 2a). Bridges the FreeInkBook engine (BookCatalog,
// ChapterLayoutSession, PageCacheReader/Writer) to CrossPoint's SD-backed
// adapters and SETTINGS, so the reader keeps its activity/chrome code and only
// swaps the page source.
//
// Arena model (all PSRAM-only via poolMakeBytes; no static BSS):
//   bookArena_    catalog resident tables (reset only on book switch)
//   cacheArena_   current chapter's cache-reader index (reset per open)
//   scratch_      layout working set + writer index chunks + page decode
//   parseArena_   resident session parse state (inflate window for deflated
//                 chapters, ~64KB — no extract-to-stored optimization in 2a)
//   prescan_      transient image pre-scan arena (~64KB), freed after begin()
//   prefetchArena_ next chapter's cache-reader index (lazily opened)
//
// The class performs no UI drawing and never touches SETTINGS directly except
// through the LayoutParams builder (kept free of renderer calls beyond
// geometry so host tests can exercise the pure helpers).

#if defined(CROSSPOINT_TTF_READER)

#include <BookCatalog.h>
#include <BookStorage.h>
#include <CrossPointSettings.h>
#include <Memory.h>
#include <cache/PageCache.h>
#include <layout/ChapterLayout.h>

#include "ChapterIndexEngine.h"
#include "adapters/SdCardBookSource.h"
#include "adapters/SdCardCacheStorage.h"

class GfxRenderer;
class Epub;

namespace freeink {
namespace book {

class TtfBookRuntime : public ChapterIndexTarget {
 public:
  // Arena sizing (PSRAM-only, design §3.3). Scratch covers the STANDARD
  // profile ChapterLayout peak (~152KB measured) plus writer index chunks
  // and page decode. Parse arena covers a deflated chapter's resident
  // inflate state (~46KB window + decompressor + XML headroom).
  static constexpr size_t kBookArenaBytes = 128 * 1024;
  static constexpr size_t kCacheArenaBytes = 48 * 1024;
  static constexpr size_t kScratchBytes = 192 * 1024;
  static constexpr size_t kParseArenaBytes = 64 * 1024;
  static constexpr size_t kPrescanBytes = 64 * 1024;
  static constexpr size_t kPrefetchArenaBytes = 48 * 1024;

  TtfBookRuntime() = default;
  ~TtfBookRuntime();

  TtfBookRuntime(const TtfBookRuntime&) = delete;
  TtfBookRuntime& operator=(const TtfBookRuntime&) = delete;

  // Opens the EPUB as a BookSource and the book catalog (build once, Stale →
  // rebuild). cacheDir is "<book cache path>/ficache".
  bool open(const char* epubPath, const char* cacheDir);
  void close();
  bool isOpen() const { return catalog_.isOpen(); }
  const BookCatalog& catalog() const { return catalog_; }
  // The open book container (image decoding reaches the whole zip).
  SdCardBookSource& source() { return bookSource_; }

  // ---- serving: current chapter pages (writer while building, reader after)

  // Opens the cache reader for spine+generation. NotFound keeps nothing open
  // (cold); Stale likewise (the file is rebuilt by the next session).
  BookStatus openChapterCache(uint16_t spineIndex, uint32_t generation);
  void closeChapterCache();
  bool cacheReady() const { return cacheReady_; }
  uint16_t cacheSpine() const { return cacheSpine_; }
  bool cachePartial() const { return cacheReady_ && cacheReader_.isPartial(); }
  uint32_t cacheTotalChars() const { return cacheReady_ ? cacheReader_.totalChars() : 0; }
  // Partial-cache input-side progress (PageCacheReader): the basis for the
  // estimated total page count while the rest of the chapter builds in the
  // background (the old Section engine's extrapolation, mirrored).
  uint32_t cacheBuildBytesConsumed() const { return cacheReady_ ? cacheReader_.buildBytesConsumed() : 0; }
  uint32_t cacheBuildBytesTotal() const { return cacheReady_ ? cacheReader_.buildBytesTotal() : 0; }
  uint32_t cacheGeneration() const { return cacheReady_ ? cacheGen_ : 0; }

  // Chapter build session for one spine. Begins the writer + layout session;
  // the first step() emits the first page. Suspends (partial commit) on
  // abort; finish() (full commit) when the session completes.
  BookStatus beginChapterSession(uint16_t spineIndex, const LayoutParams& params, uint32_t generation) override;
  BookStatus stepBuild(uint16_t minNewPages) override;
  bool finishSession();          // session done: commit final cache file (false = SD failure)
  void abortSession() override;  // suspend partial + tear down (arena reset included)
  bool sessionFor(uint16_t spineIndex) const override { return sessionSpine_ == spineIndex; }
  bool sessionActive() const override { return sessionSpine_ != kNoSpine && session_.active(); }
  bool sessionDone() const override { return sessionSpine_ != kNoSpine && session_.done(); }
  bool sessionMatchesGeneration(uint32_t generation) const {
    return sessionSpine_ != kNoSpine && sessionGen_ == generation;
  }
  uint32_t sessionTotalChars() const { return sessionSpine_ == kNoSpine ? 0 : session_.totalChars(); }
  uint16_t sessionSpine() const { return sessionSpine_; }
  uint64_t sessionBytesConsumed() const { return session_.bytesConsumed(); }
  uint64_t sessionBytesTotal() const { return session_.bytesTotal(); }

  // Transient page-only relayout for the quick font preview. Calls
  // sink.onPage() for laid pages and stops when the sink returns false. With
  // params.windowStartChar set, the engine lays only the page containing that
  // char offset (window mode, ADR 2026-10-14). Uses the runtime's
  // scratch/parse arenas; valid only when no build session is active. Does
  // not touch the committed page cache.
  BookStatus quickLayoutPage(uint16_t spineIndex, const LayoutParams& params, PageSink& sink);

  // Unified page access over writer (live build) and cache reader. All
  // accessors are scoped to `spineIndex`: a session building a DIFFERENT
  // spine (the next-chapter prefetch) never serves its writer data here —
  // the spine's own cache reader (or nothing) answers instead.
  uint32_t availablePageCount(uint16_t spineIndex) const override;
  uint32_t pageCharStart(uint16_t spineIndex, uint16_t pageIndex) const;
  bool readPage(uint16_t spineIndex, uint16_t pageIndex, Page* out);  // scratch mark held by caller
  bool pageForChar(uint16_t spineIndex, uint32_t charOffset, uint32_t* pageOut) const;
  bool charForAnchor(uint16_t spineIndex, uint32_t idHash, uint32_t* charOut) const;

  // Scratch arena for readPage()'s decode allocations: the caller takes a
  // mark before readPage() and releases it after rendering (Page text/runs
  // point into it).
  Arena& scratch() { return scratch_; }

  // Image-engine hooks (design 2026-11-07-ttf-image-engine.md §5.4): the
  // reader hands the owning Epub in (GIF/BMP decode extractions) and the
  // paint path reads the directory the .pxc tone caches live in.
  void setDecodeEpub(Epub* epub) { decodeEpub_ = epub; }
  Epub* decodeEpub() const { return decodeEpub_; }
  const char* cacheDir() const { return cacheDirBuf_; }

  // Next-spine prefetch: only the cache reader index, lazily opened behind
  // the same heap gates as the legacy Section prefetch.
  bool openPrefetch(uint16_t spineIndex, uint32_t generation);
  void dropPrefetch();
  bool prefetchFor(uint16_t spineIndex) const {
    return prefetchSpine_ >= 0 && static_cast<uint16_t>(prefetchSpine_) == spineIndex;
  }
  uint32_t prefetchPageCount() const { return prefetchSpine_ >= 0 ? prefetchReader_.pageCount() : 0; }
  // True when the prefetch reader holds a suspended partial prefix (the
  // next chapter still needs a build to reach its end).
  bool prefetchPartial() const { return prefetchSpine_ >= 0 && prefetchReader_.isPartial(); }

  // ---- pure helpers (host-testable) ----

  // Fills geometry-independent LayoutParams fields the reader controls.
  // lineSpacingPct mirrors getReaderLineCompression()'s Bookerly table,
  // paragraphSpacingPct the extraParagraphSpacing toggle (documented §3.6).
  static void applyReaderLayoutParams(LayoutParams& params);
  // Aligns LayoutParams with the current renderer geometry + chrome margins
  // (mirrors renderBook's viewport computation; autoTurnActive widens the
  // bottom margin the same way the legacy path does for the auto-turn bar).
  void makeLayoutParams(GfxRenderer& renderer, LayoutParams& out, bool autoTurnActive) const;

 private:
  static constexpr uint16_t kNoSpine = 0xFFFF;

  // Ensures arena backings exist; false on PSRAM OOM (LOG_ERR with sizes).
  bool ensureArenas();
  // openChapterCache without the duration telemetry line.
  BookStatus openChapterCacheInner(uint16_t spineIndex, uint32_t generation);
  void resetBuildArenas();  // scratch + parse + prescan reset (session over)
  void releaseArenas();

  SdCardBookSource bookSource_;
  SdCardCacheStorage cacheStorage_;
  BookCatalog catalog_;

  PoolBytes bookArenaBytes_;
  Arena bookArena_;
  PoolBytes cacheArenaBytes_;
  Arena cacheArena_;
  PoolBytes scratchBytes_;
  Arena scratch_;
  PoolBytes parseBytes_;
  Arena parseArena_;
  PoolBytes prescanBytes_;
  Arena prescanArena_;
  PoolBytes prefetchArenaBytes_;
  Arena prefetchArena_;

  PageCacheReader cacheReader_;
  bool cacheReady_ = false;
  uint16_t cacheSpine_ = kNoSpine;
  uint32_t cacheGen_ = 0;

  PageCacheWriter writer_;  // also the session's PageSink
  ChapterLayoutSession session_;
  uint16_t sessionSpine_ = kNoSpine;
  uint32_t sessionGen_ = 0;

  PageCacheReader prefetchReader_;
  int prefetchSpine_ = -1;
  uint32_t prefetchGen_ = 0;

  char cacheName_[64] = {};
  char prefetchCacheName_[64] = {};
  char spineHref_[192] = {};
  char cacheDirBuf_[160] = {};  // set by open() — the .pxc tone-cache directory
  Epub* decodeEpub_ = nullptr;  // image decode extractions (reader-owned)
};

}  // namespace book
}  // namespace freeink

#endif  // CROSSPOINT_TTF_READER