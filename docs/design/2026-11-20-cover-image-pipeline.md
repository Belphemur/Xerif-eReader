# Cover Image Pipeline — Blue-Noise Bake for Covers, Thumbs, and Sleep Art

**Status:** superseded in part by 2026-11-25-image-pipeline-adr.md (implemented, merged as develop `d05825bc`, PR #337, 18 commits).
**Supersedes:** the Amendment A engine-facade draft (parked during the design
discussion); this document records the owner-settled design as built.

> **Superseded (in part) by the as-built ADR: [2026-11-25-image-pipeline-adr.md](2026-11-25-image-pipeline-adr.md) — the current image pipeline (blue-noise bake, DPID, JPEGDEC, shared bake core).**
**Date:** 2026-11-20/21. **Author:** Antoine Aflalo (Belphemur).

## 1. Problem

Three cover surfaces still baked through legacy quantizers while the reader's
page images had moved to the blue-noise pipeline (commit `94caae1a`,
JPEGDEC + void-and-cluster blue noise in linear light):

| Surface | Legacy path | Defect |
| --- | --- | --- |
| Sleep cover (EPUB) | `Epub::generateCoverBmp` → `JpegToBmpConverter`, `USE_ATKINSON=true`, 2-bit BMP | coarse Atkinson dither; cover art decoded twice |
| Home "Continue Reading" thumb | `Epub::generateThumbBmpForCover` → 1-bit BMP, PSRAM pool or SD `.cover.jpg` staging | same double decode; legacy threshold dither |
| Details/OPDS cover | `DetailsActivity` → `ImageDecoderFactory` → `JpegToFramebufferConverter` (Bayer) | nearest-neighbor + Bayer, visibly worse than the reader |

Both the cover pass and the thumb pass decoded the same JPEG independently.
The cache formats had drifted into two divergent pixel-cache implementations
(TtfImageCache's ToneCacheWriter vs the converters' PixelCache band writer)
with two different revision schemes.

## 2. Owner-settled decisions (Amendment B)

1. **Gating:** `BOARD_HAS_PSRAM` alone selects the quality engine at build
   time (`imagebake::kQualitySink`, lib/ImageBake/ImageBakeSink.h). No
   panel-class term, no runtime capability predicates — the five runtime
   `renderer.grayscaleCapabilities().supported()` paint gates this replaces
   were exactly the duplication the switch removes. No-PSRAM boards (X3/C3
   floor) keep the legacy Bayer/Atkinson output byte-identical to the
   pre-quality baseline.
2. **Cache revisioning:** an in-file identity header — magic `'C','P'` +
   bake revision letter + u32 identity hash32 + u16 width/height
   (`kCacheHeaderBytes = 11`) — is the ONLY revision channel. The `.pxc2`
   extension token was proposed and reverted (owner directive: "use magic
   number instead of new file extension, same design as the epub image
   cache"). Stale or headerless legacy files fail the magic check at open
   and regenerate naturally.
3. **Combined cover cache:** one decode of the cover art produces ONE file,
   `cover_c<cropChar>_<thumbHeight>.pxc`, holding two seekable sections: a
   1-bit thumb BMP (home stays B/W-fast — no gray passes on the home
   screen) and a 2-bit cover BMP. The thumb height (BaseTheme 400, Lyra
   226) participates in the identity hash and filename, so a theme switch
   regenerates once.
4. **BMP writers:** the writers survive as the no-PSRAM legacy fallback and
   for GIF-covered books (GIF keeps the legacy cover/thumb pair on all
   builds — decision 3). `BmpStreamWriter::begin(..., BmpBake bake = Legacy)`
   with a `kUseBlueNoiseBake` revert switch (BmpStreamWriter.cpp:22) keeps
   the one-line converter wiring; `BmpToBmpConverter` (sleep re-encode) and
   GIF output stay byte-identical.
5. **PNG accepted directly for custom sleep screens on PSRAM builds**
   (`SleepActivity::renderEngineSleepPng`, SleepActivity.cpp:1149) — no
   re-encode file. The BMP-only filter stays on no-PSRAM builds; the
   overlay path already rendered PNG (`renderTransparentOverlayPng`).

## 3. As-built architecture

### 3.1 `lib/ImageBake/` — the shared core

- **BlueNoise64 / DpidScaler** — moved from `src/activities/reader/`; one
  32-byte-aligned `.rodata` copy of `kBlueNoise64[64][64]` (toroidal,
  values 0..255, each used 16× per tile) linked by both lanes.
- **ImageBakeQuantizer.h** — `quantizeGray2bit` bit-exact vs the reader's
  ToneBake4 (pinned by host test), `quantizeGray1bit` as the documented
  ordered-threshold choice.
- **ImageBakeCache.{h,cpp}** — the consolidated pixel-cache core:
  `CacheIdentity` (letter + hash32), the 11-byte identity header,
  `ToneCacheWriter` (ex TtfImageCache), the band-buffered writer (ex
  PixelCache.h, deleted), shared `readValidCacheHeader`, `pathIdentity`.
- **CoverCache.{h,cpp}** — the combined file: 11-byte identity header, a
  2-entry section directory (offset/length/w/h per section), then the two
  self-contained BMP payloads. `assembleCoverCache` publishes atomically
  (`Storage.replaceFile`); `openCoverBitmap` is kind-agnostic — a legacy
  standalone BMP (no `'C','P'` magic) opens whole-file, so callers carry
  no fallback branches.
- **ImageDecodeSource.h** — the DataSource seam: a per-open decode state
  object (HalFile + DataSource), killing the per-open heap `HalFile` and
  the module-static shared JPEG state (the PNG converter's per-open
  `pngOpenWithHandle` pattern, adopted for JPEG).
- **ImageBakeSink.h** — `kQualitySink` from `BOARD_HAS_PSRAM`; cache
  letters: framebuffer image pages `'n'` (blue-noise) / `'l'` (legacy),
  combined cover cache `'c'`.

### 3.2 Consumers

- **Sleep cover + shutdown (SleepActivity.cpp)** — opens sections via
  `imagebake::openCoverBitmap`; `renderEngineSleepPng` renders PNG custom
  sleep art directly on PSRAM builds; screen-sized `/sleep.bmp` and the
  BMP re-encode path unchanged.
- **Home thumb (HomeActivity.cpp:150-227)** — thumb from the 1-bit section
  of the same combined file (theme templates resolve the
  `cover_c<crop>_<height>` name).
- **Details/OPDS (DetailsActivity.cpp)** — cover paint through
  `ImageDecoderFactory` with `imagebake::sinkQuantize2bit` dispatching to
  the quality or legacy quantizer.
- **Epub (Epub.cpp)** — `generateCoverBmp`/`generateThumbBmpForCover`
  fold into the combined-cache generation; the section temps live until
  `assembleCoverCache` consumes them (Epub.cpp:1166-1211 — the round-6
  review fix: the ScopedCleanup used to delete them at inner-scope exit
  before assembly opened them).

### 3.3 HalStorage sub-file view

`HalStorage::openFileSubview` hands each section to the unchanged `Bitmap`
reader: extent captured before the handle move (FsBaseFile::move nulls the
source's internal handles — a post-move `fileSize()` reads 0), backing
cursor seeked to the window start (a fresh open sits at 0; an immediate
read would serve header bytes), and `Bitmap::parseHeaders`'s absolute
`seek(0)` translated through the window base.

## 4. Deliberate deviations (accepted, documented)

1. **Reader JPEG/PNG quality path stays on the SDK `ImageRenderer`** —
   JPEGDEC's progressive fallback is DC-only 1/8, a real quality trade.
2. **BMP custom sleep art keeps the `BmpToBmpConverter` re-encode** on
   PSRAM — no non-TTF BMP framebuffer front end exists.
3. **Sleep-cover zip decode rides `extractItemToPsram` staging** — the
   firmware `ZipFile` exposes stream/whole-memory reads only, no seekable
   entry.

## 5. Host tests

`test/image_bake/` — ImageBakeQuantizerTest (7 tests: palette anchors,
per-pixel parity vs ToneBake4, row fast-path across tiles, position
independence, 1-bit extremes) + CoverCacheTest (sub-file-view round-trip,
seek-to-0 translation, clamped reads, view-coherent size, identity
rejection, thumb-height keying, failure atomicity). The ex-PixelCache
suite lives on as `test/gif_support/BandCacheWriterTest`. Host evidence at
merge time: full suite green on the final head `4a86cef4`; CI Host unit
tests pass on `d05825bc`'s pre-squash head and the squash tree is
content-identical.

## 6. Owner verification recipe (X4 Pro)

Sleep from a book → cover tone from the 2-bit section; home card thumb →
1-bit section (Lyra regenerates once at 226); Details cover → blue-noise
sink; SD shows one `cover_c<cropChar>_<height>.pxc` per book and no new
`cover_*_v3`/`thumb_` files (old ones orphan and the cache sweep removes
them); drop a PNG into `/.sleep` → renders directly with no
`sleep_<hash>.bmp` for it; screen-sized `/sleep.bmp` unchanged; X3/C3
byte-identical to the pre-quality baseline. Quality regression on device =
immediate revert via `kUseBlueNoiseBake = false`.
