#include "Epub.h"

#include <FsHelpers.h>
#include <GifImageLayout.h>
#include <GifLimitedWriter.h>
#include <GifToBmpConverter.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>
#include <Txt.h>
#include <Utf8.h>
#include <ZipFile.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include "Epub/ImageStaging.h"
#include "Epub/parsers/ContainerParser.h"
#include "Epub/parsers/ContentOpfParser.h"
#include "Epub/parsers/TocNavParser.h"
#include "Epub/parsers/TocNcxParser.h"

// Stream chunk size for readItemContentsToStream. On PSRAM boards (excluding X4C,
// whose JPEG/CSS heap thresholds leave no headroom) use 8192 B to reduce inflate
// iterations and SD read count; on all other boards keep 1024 B to preserve heap.
#if defined(BOARD_HAS_PSRAM) && !defined(FREEINK_DEVICE_X4C)
constexpr size_t kStreamChunkSize = 8192;
#else
constexpr size_t kStreamChunkSize = 1024;
#endif

Epub::Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
  cachePath = cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(this->filepath));
}

bool Epub::findContentOpfFile(std::string* contentOpfFile, ZipFile* sharedZip) const {
  const auto containerPath = "META-INF/container.xml";
  size_t containerSize;

  // Get file size without loading it all into heap
  const bool sizeFound = sharedZip ? sharedZip->getInflatedFileSize(containerPath, &containerSize)
                                   : getItemSize(containerPath, &containerSize);
  if (!sizeFound) {
    LOG_ERR("EBP", "Could not find or size META-INF/container.xml");
    return false;
  }

  ContainerParser containerParser(containerSize);

  if (!containerParser.setup()) {
    return false;
  }

  // Stream read (reusing your existing stream logic)
  const bool read = sharedZip ? sharedZip->readFileToStream(containerPath, containerParser, kStreamChunkSize)
                              : readItemContentsToStream(containerPath, containerParser, kStreamChunkSize);
  if (!read) {
    LOG_ERR("EBP", "Could not read META-INF/container.xml");
    return false;
  }

  // Extract the result
  if (containerParser.fullPath.empty()) {
    LOG_ERR("EBP", "Could not find valid rootfile in container.xml");
    return false;
  }

  *contentOpfFile = std::move(containerParser.fullPath);
  return true;
}

bool Epub::parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, const bool writeSpineEntries,
                           const bool metadataOnly, ZipFile* sharedZip) {
  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath, sharedZip)) {
    LOG_ERR("EBP", "Could not find content.opf in zip");
    return false;
  }

  contentBasePath = contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);

  LOG_DBG("EBP", "Parsing content.opf: %s", contentOpfFilePath.c_str());

  size_t contentOpfSize;
  const bool sizeFound = sharedZip ? sharedZip->getInflatedFileSize(contentOpfFilePath.c_str(), &contentOpfSize)
                                   : getItemSize(contentOpfFilePath, &contentOpfSize);
  if (!sizeFound) {
    LOG_ERR("EBP", "Could not get size of content.opf");
    return false;
  }

  ContentOpfParser opfParser(getCachePath(), getBasePath(), contentOpfSize,
                             writeSpineEntries ? bookMetadataCache.get() : nullptr, metadataOnly);
  if (!opfParser.setup()) {
    LOG_ERR("EBP", "Could not setup content.opf parser");
    return false;
  }

  const bool read =
      sharedZip ? sharedZip->readFileToStream(contentOpfFilePath.c_str(), opfParser, kStreamChunkSize, metadataOnly)
                : readItemContentsToStream(contentOpfFilePath, opfParser, kStreamChunkSize, metadataOnly);
  if (!read) {
    LOG_ERR("EBP", "Could not read content.opf");
    return false;
  }

  // Grab data from opfParser into epub. Normalize titles to NFC so NFD (combining
  // mark) text renders correctly — the device fonts have no mark positioning.
  bookMetadata.title = utf8ComposeNfc(opfParser.title);
  bookMetadata.author = utf8ComposeNfc(opfParser.author);
  bookMetadata.language = opfParser.language;

  if (metadataOnly) {
    LOG_DBG("EBP", "Successfully parsed package metadata");
    return true;
  }

  bookMetadata.coverItemHref = opfParser.coverItemHref;

  // Guide-based cover fallback: if no cover found via metadata/properties,
  // try extracting the image reference from the guide's cover page XHTML
  if (bookMetadata.coverItemHref.empty() && !opfParser.guideCoverPageHref.empty()) {
    LOG_DBG("EBP", "No cover from metadata, trying guide cover page: %s", opfParser.guideCoverPageHref.c_str());
    size_t coverPageSize;
    uint8_t* coverPageData = readItemContentsToBytes(opfParser.guideCoverPageHref, &coverPageSize, true);
    if (coverPageData) {
      const std::string coverPageHtml(reinterpret_cast<char*>(coverPageData), coverPageSize);
      poolFree(coverPageData);

      // Determine base path of the cover page for resolving relative image references
      std::string coverPageBase;
      const auto lastSlash = opfParser.guideCoverPageHref.rfind('/');
      if (lastSlash != std::string::npos) {
        coverPageBase = opfParser.guideCoverPageHref.substr(0, lastSlash + 1);
      }

      // Search for image references: xlink:href="..." (SVG) and src="..." (img)
      std::string imageRef;
      for (const char* pattern : {"xlink:href=\"", "src=\""}) {
        auto pos = coverPageHtml.find(pattern);
        while (pos != std::string::npos) {
          pos += strlen(pattern);
          const auto endPos = coverPageHtml.find('"', pos);
          if (endPos != std::string::npos) {
            const auto ref = std::string_view{coverPageHtml}.substr(pos, endPos - pos);
            // Cover BMP generation supports JPG/PNG/GIF. Skip unsupported wrappers so a later
            // supported raster image can still be selected.
            if (FsHelpers::hasPngExtension(ref) || FsHelpers::hasJpgExtension(ref) || FsHelpers::hasGifExtension(ref)) {
              imageRef = ref;
              break;
            }
          }
          pos = coverPageHtml.find(pattern, pos);
        }
        if (!imageRef.empty()) break;
      }

      if (!imageRef.empty()) {
        bookMetadata.coverItemHref = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(coverPageBase + imageRef));
        LOG_DBG("EBP", "Found cover image from guide: %s", bookMetadata.coverItemHref.c_str());
      }
    }
  }

  bookMetadata.textReferenceHref = opfParser.textReferenceHref;

  if (!opfParser.tocNcxPath.empty()) {
    tocNcxItem = opfParser.tocNcxPath;
  }

  if (!opfParser.tocNavPath.empty()) {
    tocNavItem = opfParser.tocNavPath;
  }

  if (!opfParser.cssFiles.empty()) {
    cssFiles = opfParser.cssFiles;
  }

  LOG_DBG("EBP", "Successfully parsed content.opf");
  return true;
}

bool Epub::parseTocNcxFile() const {
  // the ncx file should have been specified in the content.opf file
  if (tocNcxItem.empty()) {
    LOG_DBG("EBP", "No ncx file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc ncx file: %s", tocNcxItem.c_str());

  size_t ncxSize;
  if (!getItemSize(tocNcxItem, &ncxSize)) {
    LOG_ERR("EBP", "Could not get size of toc ncx file");
    return false;
  }

  TocNcxParser ncxParser(contentBasePath, ncxSize, bookMetadataCache.get());

  if (!ncxParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc ncx parser");
    return false;
  }

  // Stream the decompressed NCX straight into the parser instead of round-tripping
  // through a temp file on the SD card (decompress -> write -> reopen -> reread -> delete).
  if (!readItemContentsToStream(tocNcxItem, ncxParser, kStreamChunkSize)) {
    LOG_ERR("EBP", "Could not read toc ncx file");
    return false;
  }

  LOG_DBG("EBP", "Parsed TOC items");
  return true;
}

bool Epub::parseTocNavFile() const {
  // the nav file should have been specified in the content.opf file (EPUB 3)
  if (tocNavItem.empty()) {
    LOG_DBG("EBP", "No nav file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc nav file: %s", tocNavItem.c_str());

  size_t navSize;
  if (!getItemSize(tocNavItem, &navSize)) {
    LOG_ERR("EBP", "Could not get size of toc nav file");
    return false;
  }

  // Note: We can't use `contentBasePath` here as the nav file may be in a different folder to the content.opf
  // and the HTMLX nav file will have hrefs relative to itself
  const std::string navContentBasePath = tocNavItem.substr(0, tocNavItem.find_last_of('/') + 1);
  TocNavParser navParser(navContentBasePath, navSize, bookMetadataCache.get());

  if (!navParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc nav parser");
    return false;
  }

  // Stream the decompressed nav document straight into the parser instead of round-tripping
  // through a temp file on the SD card (decompress -> write -> reopen -> reread -> delete).
  if (!readItemContentsToStream(tocNavItem, navParser, kStreamChunkSize)) {
    LOG_ERR("EBP", "Could not read toc nav file");
    return false;
  }

  LOG_DBG("EBP", "Parsed TOC nav items");
  return true;
}

void Epub::discoverCssFilesFromZip() {
  const std::string& opfDir = contentBasePath;
  // Prefer the PSRAM ZIP cache (no SD access, no ZipFile construction). If the
  // cache is unavailable (non-PSRAM board, load() failed, or .zip unreadable)
  // fall back to opening a fresh ZipFile and using its per-instance
  // enumerateFilePaths. isLoaded() guards against the silent-empty-cache failure
  // (CodeRabbit IPV2): a load() that returns early leaves zipCache_ non-null
  // but empty; iterating it would skip CSS files.
#ifdef BOARD_HAS_PSRAM
  if (zipCache_ && zipCache_->isLoaded()) {
    for (const auto& entry : zipCache_->cacheRef()) {
      const std::string_view filePath = entry.first;
      if (!opfDir.empty() && filePath.find(opfDir) != 0) {
        continue;
      }
      if (!FsHelpers::hasCssExtension(filePath)) {
        continue;
      }
      if (std::find(cssFiles.begin(), cssFiles.end(), filePath) != cssFiles.end()) {
        continue;
      }
      LOG_DBG("EBP", "Discovered CSS file via PSRAM cache: %.*s", (int)filePath.size(), filePath.data());
      cssFiles.push_back(std::string{filePath});
    }
    return;
  }
#endif
  ZipFile zf(filepath);
  if (!zf.enumerateFilePaths([&](std::string_view filePath) {
        if (!opfDir.empty() && filePath.find(opfDir) != 0) {
          return;
        }
        if (!FsHelpers::hasCssExtension(filePath)) {
          return;
        }
        if (std::find(cssFiles.begin(), cssFiles.end(), filePath) != cssFiles.end()) {
          return;
        }
        LOG_DBG("EBP", "Discovered CSS file via ZIP enumeration: %.*s", (int)filePath.size(), filePath.data());
        cssFiles.push_back(std::string{filePath});
      })) {
    LOG_ERR("EBP", "Failed to enumerate ZIP file paths for CSS discovery");
  }
}

CssParser::ParseResult Epub::parseCssFiles(const CssParser::CacheStatus existingCacheStatus) const {
  // Maximum CSS file size we'll attempt to parse (uncompressed)
  // Larger files risk memory exhaustion on ESP32
  constexpr size_t MAX_CSS_FILE_SIZE = 128 * 1024;  // 128KB
  // Minimum heap required before attempting CSS parsing
  constexpr size_t MIN_HEAP_FOR_CSS_PARSING = 64 * 1024;  // 64KB

  if (cssFiles.empty()) {
    LOG_DBG("EBP", "No CSS files to parse, but CssParser created for inline styles");
  }

  LOG_DBG("EBP", "CSS files to parse: %zu", cssFiles.size());

  const bool hasPartialCache = existingCacheStatus == CssParser::CacheStatus::Partial;
  cssParser->clear();

  // Some converters emit one byte-identical stylesheet per chapter (100+ .css
  // entries), and each parse costs a zip locate plus an SD extract round-trip.
  // Match each normalized CSS path to its central-directory (CRC32,
  // compressed size) without throwing container allocations, then parse only
  // the first of each identical pair. If scratch allocation fails, parsing all
  // stylesheets is slower but remains correct.
  struct CssDedupEntry {
    uint64_t pathHash = 0;
    uint64_t contentKey = 0;
    size_t cssIndex = 0;
  };
  std::unique_ptr<CssDedupEntry[]> dedupEntries;
  if (cssFiles.size() > 1) {
    dedupEntries = makeUniqueNoThrow<CssDedupEntry[]>(cssFiles.size());
  }
  if (dedupEntries) {
    for (size_t i = 0; i < cssFiles.size(); i++) {
      dedupEntries[i].pathHash = ZipFile::fnvHash64(cssFiles[i].data(), cssFiles[i].size());
      dedupEntries[i].cssIndex = i;
    }
    std::sort(dedupEntries.get(), dedupEntries.get() + cssFiles.size(),
              [](const CssDedupEntry& lhs, const CssDedupEntry& rhs) { return lhs.pathHash < rhs.pathHash; });

    ZipFile(filepath).enumerateFileEntries([&](std::string_view entryPath, uint32_t crc32, uint32_t compressedSize) {
      if (!FsHelpers::hasCssExtension(entryPath)) {
        return;
      }

      const uint64_t pathHash = ZipFile::fnvHash64(entryPath.data(), entryPath.size());
      auto* match = std::lower_bound(
          dedupEntries.get(), dedupEntries.get() + cssFiles.size(), pathHash,
          [](const CssDedupEntry& candidate, const uint64_t hash) { return candidate.pathHash < hash; });
      for (const auto* end = dedupEntries.get() + cssFiles.size(); match != end && match->pathHash == pathHash;
           match++) {
        if (entryPath == cssFiles[match->cssIndex]) {
          match->contentKey = (static_cast<uint64_t>(crc32) << 32) | compressedSize;
          break;
        }
      }
    });
    std::sort(dedupEntries.get(), dedupEntries.get() + cssFiles.size(),
              [](const CssDedupEntry& lhs, const CssDedupEntry& rhs) { return lhs.cssIndex < rhs.cssIndex; });
  } else if (cssFiles.size() > 1) {
    LOG_ERR("EBP", "Insufficient heap for CSS deduplication; parsing every stylesheet");
  }

  size_t skippedDuplicates = 0;
  CssParser::ParseResult parseResult = CssParser::ParseResult::Complete;

  // No cache yet - parse CSS files
  for (size_t cssIndex = 0; cssIndex < cssFiles.size(); cssIndex++) {
    const auto& cssPath = cssFiles[cssIndex];
    const uint64_t dedupKey = dedupEntries ? dedupEntries[cssIndex].contentKey : 0;
    if (dedupKey != 0) {
      const bool seen =
          std::any_of(dedupEntries.get(), dedupEntries.get() + cssIndex,
                      [dedupKey](const CssDedupEntry& candidate) { return candidate.contentKey == dedupKey; });
      if (seen) {
        skippedDuplicates++;
        continue;
      }
    }
    LOG_DBG("EBP", "Parsing CSS file: %s", cssPath.c_str());

    // Check heap before parsing - CSS parsing allocates heavily
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < MIN_HEAP_FOR_CSS_PARSING) {
      LOG_ERR("EBP", "Insufficient heap for CSS parsing (%u bytes free, need %zu), skipping: %s", freeHeap,
              MIN_HEAP_FOR_CSS_PARSING, cssPath.c_str());
      if (parseResult == CssParser::ParseResult::Complete) {
        parseResult = CssParser::ParseResult::Partial;
      }
      continue;
    }

    // Check CSS file size before decompressing - skip files that are too large
    size_t cssFileSize = 0;
    if (getItemSize(cssPath, &cssFileSize)) {
      if (cssFileSize > MAX_CSS_FILE_SIZE) {
        LOG_ERR("EBP", "CSS file too large (%zu bytes > %zu max), skipping: %s", cssFileSize, MAX_CSS_FILE_SIZE,
                cssPath.c_str());
        if (parseResult == CssParser::ParseResult::Complete) {
          parseResult = CssParser::ParseResult::Partial;
        }
        continue;
      }
    }

    CssParser::ParseResult cssResult = CssParser::ParseResult::Error;
    bool cssFromMemory = false;
#ifdef BOARD_HAS_PSRAM
    // Decompress straight into PSRAM and parse from there; no temp file needed.
    {
      size_t cssMemSize = 0;
      PoolBytes cssBuf{readItemContentsToBytes(cssPath, &cssMemSize)};
      if (cssBuf) {
        cssResult = cssParser->loadFromMemory(reinterpret_cast<const char*>(cssBuf.get()), cssMemSize);
        cssFromMemory = true;
      } else {
        LOG_DBG("EBP", "PSRAM buffer unavailable for CSS; streaming to temp file: %s", cssPath.c_str());
      }
    }
#endif
    if (!cssFromMemory) {
      // Extract CSS file to temp location
      const auto tmpCssPath = getCachePath() + "/.tmp.css";
      HalFile tempCssFile;
      if (!Storage.openFileForWrite("EBP", tmpCssPath, tempCssFile)) {
        LOG_ERR("EBP", "Could not create temp CSS file");
        parseResult = CssParser::ParseResult::Error;
        continue;
      }
      if (!readItemContentsToStream(cssPath, tempCssFile, kStreamChunkSize)) {
        LOG_ERR("EBP", "Could not read CSS file: %s", cssPath.c_str());
        // Explicitly close() file before calling Storage.remove()
        tempCssFile.close();
        Storage.remove(tmpCssPath.c_str());
        parseResult = CssParser::ParseResult::Error;
        continue;
      }
      // Explicitly close() file before reopening for reading
      tempCssFile.close();

      // Parse the CSS file
      if (!Storage.openFileForRead("EBP", tmpCssPath, tempCssFile)) {
        LOG_ERR("EBP", "Could not open temp CSS file for reading");
        Storage.remove(tmpCssPath.c_str());
        parseResult = CssParser::ParseResult::Error;
        continue;
      }
      const CssParser::ParseResult streamResult = cssParser->loadFromStream(tempCssFile);
      // Explicitly close() file before calling Storage.remove()
      tempCssFile.close();
      Storage.remove(tmpCssPath.c_str());
      cssResult = streamResult;
    }

    if (cssResult == CssParser::ParseResult::Error) {
      parseResult = CssParser::ParseResult::Error;
    } else if (cssResult == CssParser::ParseResult::Partial && parseResult == CssParser::ParseResult::Complete) {
      parseResult = CssParser::ParseResult::Partial;
    }
  }

  if (parseResult == CssParser::ParseResult::Error) {
    LOG_ERR("EBP", "CSS parse failed; preserving any previous cache for a later retry");
    cssParser->clear();
    return parseResult;
  }

  if (parseResult == CssParser::ParseResult::Partial && cssParser->empty()) {
    LOG_ERR("EBP", "CSS parsing stopped before any usable rules were loaded; cache will not be replaced");
    cssParser->clear();
    return CssParser::ParseResult::Error;
  }

  if (parseResult == CssParser::ParseResult::Partial && hasPartialCache) {
    LOG_DBG("EBP", "CSS retry remained partial; preserving the previous partial cache");
    cssParser->clear();
    return parseResult;
  }

  // A partial cache remains useful for this session, but its header ensures a
  // later EPUB load retries the source stylesheets when more heap is available.
  if (!cssParser->saveToCache(parseResult == CssParser::ParseResult::Complete)) {
    LOG_ERR("EBP", "Failed to save CSS rules to cache");
    cssParser->clear();
    return CssParser::ParseResult::Error;
  }

  LOG_DBG("EBP", "Loaded %zu CSS style rules from %zu files (%zu identical duplicates skipped, %s)",
          cssParser->ruleCount(), cssFiles.size(), skippedDuplicates,
          parseResult == CssParser::ParseResult::Complete ? "complete" : "partial");
  cssParser->clear();
  return parseResult;
}

// load in the meta data for the epub file
// Opens the optional encrypted-entry accessor. A null result without an error
// means normal ZIP reads should be used. A hard error refuses the open with a
// user-presentable reason.
bool Epub::openProtection() {
  std::string err;
  decryptor = freeink::content::openProtectedBook(filepath, err);
  if (!err.empty()) {
    LOG_ERR("EBP", "protected content unavailable: %s", err.c_str());
    protectionError = err;
    return false;
  }
  if (decryptor) {
    LOG_DBG("EBP", "protected content; on-read access path open");
  }
  return true;
}

bool Epub::load(const bool buildIfMissing, const bool skipLoadingCss) {
#ifdef BOOK_PROFILE
  uint32_t load_start_ms = millis();
  uint8_t core = xPortGetCoreID();
  uint32_t psram_free_before = ESP.getFreePsram();
  uint32_t heap_free_before = ESP.getFreeHeap();
#endif
  LOG_DBG("EBP", "Loading ePub: %s", filepath.c_str());

  if (!openProtection()) return false;

  // Initialize spine/TOC cache
  bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
  // Always create CssParser - needed for inline style parsing even without CSS files
  cssParser = makeUniqueNoThrow<CssParser>(cachePath);
  if (!bookMetadataCache || !cssParser) {
    LOG_ERR("EBP", "OOM: metadata cache or CSS parser");
    return false;
  }
#ifdef BOOK_PROFILE
  uint32_t cache_init_ms = millis();
#endif
#ifdef BOARD_HAS_PSRAM
  // Parse the ZIP central directory once and cache entry info in PSRAM.
  void* p = heap_caps_malloc(sizeof(ZipFileCache), MALLOC_CAP_SPIRAM);
  if (p) {
    new (p) ZipFileCache();  // placement-new
    zipCache_ = std::unique_ptr<ZipFileCache, ZipFileCacheDeleter>(static_cast<ZipFileCache*>(p), kZipFileCacheDeleter);
    if (zipCache_) {
      zipCache_->load(filepath);
    }
  } else {
    LOG_ERR("EBP", "Failed to allocate ZipFileCache in PSRAM (%u bytes); continuing without cache",
            static_cast<unsigned>(sizeof(ZipFileCache)));
  }
#endif

  // Try to load existing cache first
  if (bookMetadataCache->load()) {
    if (Txt::isTxtOrMd(filepath)) {
      if (!Txt::validateCache(filepath, cachePath, bookMetadataCache->getCumulativeSize(0))) {
        LOG_DBG("EBP", "TXT/MD cache invalid or outdated: %s", filepath.c_str());
        if (!buildIfMissing) {
          return false;
        }
        bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
        if (!bookMetadataCache) {
          LOG_ERR("EBP", "OOM: TXT/MD metadata cache");
          return false;
        }
        return Txt::buildTxtCache(filepath, cachePath, bookMetadataCache);
      }
      LOG_DBG("EBP", "Loaded TXT/MD from cache: %s", filepath.c_str());
      return true;
    }
#ifdef BOOK_PROFILE
    uint32_t cache_load_ms = millis() - load_start_ms;
    LOG_DBG("EBP", "Loaded existing cache (cache_load_ms=%lu)", (unsigned long)cache_load_ms);
#endif
    if (!skipLoadingCss) {
      const CssParser::CacheStatus cacheStatus = cssParser->inspectCache();
      CssParser::CacheLoadResult cacheLoadResult = CssParser::CacheLoadResult::Invalid;
      if (cacheStatus == CssParser::CacheStatus::Complete) {
        cacheLoadResult = cssParser->loadFromCache();
      }

      if (cacheLoadResult == CssParser::CacheLoadResult::LowMemory) {
        LOG_ERR("EBP", "Insufficient heap to load CSS cache; keeping it for a later retry");
      } else if (cacheLoadResult != CssParser::CacheLoadResult::Complete) {
        LOG_DBG("EBP", "CSS cache missing, partial, or invalid; attempting to parse source stylesheets");
        if (cacheStatus == CssParser::CacheStatus::Invalid ||
            (cacheStatus == CssParser::CacheStatus::Complete &&
             cacheLoadResult == CssParser::CacheLoadResult::Invalid)) {
          cssParser->deleteCache();
        }

        BookMetadataCache::BookMetadata cachedMetadata = bookMetadataCache->coreMetadata;
        CssParser::ParseResult cssParseResult = CssParser::ParseResult::Error;
        if (!parseContentOpf(cachedMetadata, /*writeSpineEntries=*/false)) {
          LOG_ERR("EBP", "Could not parse content.opf from cached bookMetadata for CSS files");
        } else {
          discoverCssFilesFromZip();
          cssParseResult = parseCssFiles(cacheStatus);
        }
        if (cssParseResult != CssParser::ParseResult::Error) {
          // Reload book.bin with the rebuilt CSS rules.
          bookMetadataCache.reset();
          bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
          if (!bookMetadataCache || !bookMetadataCache->load()) {
            LOG_ERR("EBP", "Failed to reload cache after CSS rebuild");
            return false;
          }
          const bool cssCacheChanged =
              cssParseResult == CssParser::ParseResult::Complete ||
              (cssParseResult == CssParser::ParseResult::Partial && cacheStatus != CssParser::CacheStatus::Partial);
          if (cssCacheChanged) {
            // The CSS cache changed, so section caches must use the same rule set.
            Storage.removeDir((cachePath + "/sections").c_str());
          }
        }
      }
    }
    // Release the resolved CSS rule map: it is only needed transiently while building
    // section caches, and createSectionFile reloads it from cache on demand. Holding it
    // resident pins tens of KB for the whole reading session (more on warm resume into
    // an already-cached chapter, where createSectionFile never runs to clear it).
    cssParser->clear();
    LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
    return true;
  }

  // If we didn't load from cache above and we aren't allowed to build, fail now
  if (!buildIfMissing) {
    return false;
  }

  if (Txt::isTxtOrMd(filepath)) {
    return Txt::buildTxtCache(filepath, cachePath, bookMetadataCache);
  }

  // Cache doesn't exist or is invalid, build it
  LOG_DBG("EBP", "Cache not found, building spine/TOC cache");
  setupCacheDir();

  [[maybe_unused]] const uint32_t indexingStart = millis();

  // Begin building cache - stream entries to disk immediately
  if (!bookMetadataCache->beginWrite()) {
    LOG_ERR("EBP", "Could not begin writing cache");
    return false;
  }

  // OPF Pass
  [[maybe_unused]] const uint32_t opfStart = millis();
  BookMetadataCache::BookMetadata bookMetadata;
  if (!bookMetadataCache->beginContentOpfPass()) {
    LOG_ERR("EBP", "Could not begin writing content.opf pass");
    return false;
  }
  if (!parseContentOpf(bookMetadata)) {
    LOG_ERR("EBP", "Could not parse content.opf");
    return false;
  }
  discoverCssFilesFromZip();
  if (!bookMetadataCache->endContentOpfPass()) {
    LOG_ERR("EBP", "Could not end writing content.opf pass");
    return false;
  }
  LOG_DBG("EBP", "OPF pass completed in %lu ms", millis() - opfStart);

  // TOC Pass - try EPUB 3 nav first, fall back to NCX
  [[maybe_unused]] const uint32_t tocStart = millis();
  if (!bookMetadataCache->beginTocPass()) {
    LOG_ERR("EBP", "Could not begin writing toc pass");
    return false;
  }

  bool tocParsed = false;

  // Try EPUB 3 nav document first (preferred)
  if (!tocNavItem.empty()) {
    LOG_DBG("EBP", "Attempting to parse EPUB 3 nav document");
    tocParsed = parseTocNavFile();
  }

  // Fall back to NCX if nav parsing failed or wasn't available
  if (!tocParsed && !tocNcxItem.empty()) {
    LOG_DBG("EBP", "Falling back to NCX TOC");
    tocParsed = parseTocNcxFile();
  }

  if (!tocParsed) {
    LOG_ERR("EBP", "Warning: Could not parse any TOC format");
    // Continue anyway - book will work without TOC
  }

  if (!bookMetadataCache->endTocPass()) {
    LOG_ERR("EBP", "Could not end writing toc pass");
    return false;
  }
  LOG_DBG("EBP", "TOC pass completed in %lu ms", millis() - tocStart);

  // Close the cache files
  if (!bookMetadataCache->endWrite()) {
    LOG_ERR("EBP", "Could not end writing cache");
    return false;
  }

  // Build final book.bin
  [[maybe_unused]] const uint32_t buildStart = millis();
  if (!bookMetadataCache->buildBookBin(filepath, bookMetadata)) {
    LOG_ERR("EBP", "Could not update mappings and sizes");
    return false;
  }
  LOG_DBG("EBP", "buildBookBin completed in %lu ms", millis() - buildStart);
  LOG_DBG("EBP", "Total indexing completed in %lu ms", millis() - indexingStart);

  if (!bookMetadataCache->cleanupTmpFiles()) {
    LOG_DBG("EBP", "Could not cleanup tmp files - ignoring");
  }

  if (!skipLoadingCss) {
    // Parse CSS before reloading book.bin to leave more heap for CSS rule-table growth.
    bookMetadataCache.reset();
    if (parseCssFiles(cssParser->inspectCache()) != CssParser::ParseResult::Error) {
      Storage.removeDir((cachePath + "/sections").c_str());
    }
  }

  // Reload the cache from disk so it's in the correct state
  bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
  if (!bookMetadataCache || !bookMetadataCache->load()) {
    LOG_ERR("EBP", "Failed to reload cache after writing");
    return false;
  }

  LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
  return true;
}

bool Epub::loadMetadata(std::string& title, std::string& author) {
  title.clear();
  author.clear();

  if (Txt::isTxtOrMd(filepath)) {
    title = utf8ComposeNfc(FsHelpers::getFileNameWithoutExtension(filepath));
    return true;
  }

  auto metadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
  if (metadataCache && metadataCache->load()) {
    title = metadataCache->coreMetadata.title;
    author = metadataCache->coreMetadata.author;
    return true;
  }
  if (!metadataCache) {
    LOG_ERR("EBP", "Could not allocate metadata cache reader");
  }
  metadataCache.reset();

  ZipFile zip(filepath);
  if (!zip.open()) {
    LOG_DBG("EBP", "Could not open ePub for package metadata: %s", filepath.c_str());
    return false;
  }

  BookMetadataCache::BookMetadata metadata;
  const bool loaded = parseContentOpf(metadata, /*writeSpineEntries=*/false, /*metadataOnly=*/true, &zip);
  zip.close();
  if (!loaded) return false;

  title = std::move(metadata.title);
  author = std::move(metadata.author);
  return true;
}

bool Epub::loadSyncMetadata(SyncMetadata& metadata) {
  metadata = {};

  ZipFile zip(filepath);
  if (!zip.open()) {
    LOG_DBG("EBP", "Could not open ePub for sync metadata: %s", filepath.c_str());
    return false;
  }

  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath, &zip)) {
    zip.close();
    return false;
  }

  size_t contentOpfSize;
  if (!zip.getInflatedFileSize(contentOpfFilePath.c_str(), &contentOpfSize)) {
    zip.close();
    return false;
  }

  const std::string basePath = contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);
  ContentOpfParser parser(cachePath, basePath, contentOpfSize, nullptr, true);
  if (!parser.setup()) {
    zip.close();
    return false;
  }

  const bool read = zip.readFileToStream(contentOpfFilePath.c_str(), parser, 1024, true);
  zip.close();
  if (!read) return false;

  metadata.isbn = std::move(parser.isbn);
  metadata.asin = std::move(parser.asin);
  metadata.series = std::move(parser.series);
  metadata.seriesIndex = parser.seriesIndex;
  return true;
}

bool Epub::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("EPB", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("EPB", "Failed to clear cache");
    return false;
  }

  LOG_DBG("EPB", "Cache cleared successfully");
  return true;
}

void Epub::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  Storage.mkdir(cachePath.c_str());
}

const std::string& Epub::getCachePath() const { return cachePath; }

const std::string& Epub::getPath() const { return filepath; }

const std::string& Epub::getTitle() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return blank;
  return bookMetadataCache->coreMetadata.title;
}

const std::string& Epub::getAuthor() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return blank;
  return bookMetadataCache->coreMetadata.author;
}

const std::string& Epub::getLanguage() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return blank;
  return bookMetadataCache->coreMetadata.language;
}

std::string Epub::getCoverBmpPath(bool cropped, bool originalThresholds) const {
  const auto coverFileName =
      std::string("cover") + (originalThresholds ? "_original" : "_legacy_v2") + (cropped ? "_crop" : "");
  return cachePath + "/" + coverFileName + ".bmp";
}

#ifdef BOARD_HAS_PSRAM
bool Epub::generateBmpFromPsram(const std::string& coverImageHref, const std::string& bmpPath, const bool oneBit,
                                const int targetWidth, const int targetHeight, const bool crop,
                                const bool originalThresholds) const {
  const bool isJpg = FsHelpers::hasJpgExtension(coverImageHref);
  if (!isJpg && !FsHelpers::hasPngExtension(coverImageHref)) {
    return false;
  }

  size_t coverSize = 0;
  PoolBytes coverBuf = extractItemToPsram(coverImageHref, &coverSize);
  if (!coverBuf) {
    LOG_DBG("EBP", "Cover PSRAM staging unavailable, falling back to SD");
    return false;
  }

  bool success;
  {
    HalFile bmpFile;
    if (!Storage.openFileForWrite("EBP", bmpPath, bmpFile)) {
      return false;
    }
    if (oneBit) {
      if (isJpg) {
        success = JpegToBmpConverter::jpegMemTo1BitBmpStreamWithSize(coverBuf.get(), coverSize, bmpFile, targetWidth,
                                                                     targetHeight);
      } else {
        success = PngToBmpConverter::pngMemTo1BitBmpStreamWithSize(coverBuf.get(), coverSize, bmpFile, targetWidth,
                                                                   targetHeight);
      }
    } else {
      if (isJpg) {
        success = JpegToBmpConverter::jpegMemToBmpStream(coverBuf.get(), coverSize, bmpFile, crop, originalThresholds);
      } else {
        success = PngToBmpConverter::pngMemToBmpStream(coverBuf.get(), coverSize, bmpFile, crop, originalThresholds);
      }
    }
  }  // bmpFile closes here, before the failure remove below
  if (!success) {
    LOG_ERR("EBP", "Failed to generate BMP from cover image (memory path)");
    Storage.remove(bmpPath.c_str());
    return false;
  }
  LOG_DBG("EBP", "Generated BMP from cover image in memory (%u bytes)", static_cast<unsigned>(coverSize));
  return true;
}
#endif
namespace {
StaticSemaphore_t gifGenerationMutexStorage;
const SemaphoreHandle_t gifGenerationMutex = xSemaphoreCreateMutexStatic(&gifGenerationMutexStorage);

void removeGifTemporaryFile(const std::string& path) {
  if (!Storage.remove(path.c_str()) && Storage.exists(path.c_str())) {
    LOG_ERR("EBP", "Failed to remove GIF temporary file: %s", path.c_str());
  }
}

bool extractGifCover(const Epub& epub, const std::string& href, const std::string& path) {
  HalFile file;
  bool success = false;
  const ScopedCleanup cleanup{[&]() {
    file.close();
    if (!success) removeGifTemporaryFile(path);
  }};
  if (!Storage.openFileForWrite("EBP", path, file)) {
    LOG_ERR("EBP", "Failed to create GIF extraction file: %s", path.c_str());
    return false;
  }
  GifLimitedWriter limited(file, GifCommon::MAX_FILE_BYTES);
  success = epub.readItemContentsToStream(href, limited, 1024);
  const bool closed = file.close();
  success = success && closed;
  if (!success) LOG_ERR("EBP", "GIF extraction failed, exceeded byte limit, or could not close: %s", path.c_str());
  return success;
}

template <typename Convert>
bool generateGifBmp(const Epub& epub, const std::string& href, const std::string& bmpPath, Convert convert) {
  if (!gifGenerationMutex || xSemaphoreTake(gifGenerationMutex, portMAX_DELAY) != pdTRUE) {
    LOG_ERR("EBP", "Failed to lock GIF cache generation");
    return false;
  }
  const ScopedCleanup unlock{[]() { xSemaphoreGive(gifGenerationMutex); }};
  // Another caller may have completed this cache while we waited.
  if (Storage.exists(bmpPath.c_str())) return true;

  const auto gifTempPath = epub.getCachePath() + "/.cover.gif";
  const auto bmpTempPath = bmpPath + ".tmp";
  if (!extractGifCover(epub, href, gifTempPath)) return false;

  HalFile gifFile;
  if (!Storage.openFileForRead("EBP", gifTempPath, gifFile)) {
    LOG_ERR("EBP", "Failed to open extracted GIF: %s", gifTempPath.c_str());
    removeGifTemporaryFile(gifTempPath);
    return false;
  }
  HalFile bmpFile;
  if (!Storage.openFileForWrite("EBP", bmpTempPath, bmpFile)) {
    LOG_ERR("EBP", "Failed to create GIF BMP: %s", bmpPath.c_str());
    gifFile.close();
    removeGifTemporaryFile(gifTempPath);
    removeGifTemporaryFile(bmpTempPath);
    return false;
  }

  bool success = convert(gifFile, bmpFile);
  const bool sourceClosed = gifFile.close();
  const bool outputClosed = bmpFile.close();
  success = success && sourceClosed && outputClosed;
  removeGifTemporaryFile(gifTempPath);
  if (success && !Storage.rename(bmpTempPath.c_str(), bmpPath.c_str())) {
    LOG_ERR("EBP", "Failed to publish GIF BMP: %s", bmpPath.c_str());
    success = false;
  }
  if (!success) removeGifTemporaryFile(bmpTempPath);
  return success;
}
}  // namespace

bool Epub::generateCoverBmp(bool cropped, bool originalThresholds) const {
  // Already generated, return true
  if (Storage.exists(getCoverBmpPath(cropped, originalThresholds).c_str())) {
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate cover BMP, cache not loaded");
    return false;
  }

  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;

  // TXT/MD: the companion cover can appear (or vanish) after the cache was
  // built; re-probe instead of trusting the cached href.
  if (Txt::isTxtOrMd(filepath)) {
    std::string companion = coverImageHref;
    if (companion.empty() || !Storage.exists(companion.c_str())) {
      companion = Txt::findCompanionCoverImage(filepath);
    }
    if (companion.empty()) {
      LOG_ERR("EBP", "No known cover image");
      return false;
    }
    return Txt::convertCoverImageToBmp(companion, getCoverBmpPath(cropped, originalThresholds), 0, cropped,
                                       originalThresholds);
  }

  if (coverImageHref.empty()) {
    LOG_ERR("EBP", "No known cover image");
    return false;
  }

#ifdef BOARD_HAS_PSRAM
  // PSRAM boards decode the cover from a pool buffer: no .cover.jpg/.cover.png
  // temp ever reaches SD. Oversized (>4 MB) or OOM falls back to the SD path.
  if (generateBmpFromPsram(coverImageHref, getCoverBmpPath(cropped, originalThresholds), /*oneBit=*/false, 0, 0,
                           cropped, originalThresholds)) {
    return true;
  }
  LOG_DBG("EBP", "Falling back to SD staging for cover");
#endif

  if (FsHelpers::hasJpgExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating BMP from JPG cover image (%s mode%s)", cropped ? "cropped" : "fit",
            originalThresholds ? ", original thresholds" : "");
    const auto coverJpgTempPath = getCachePath() + "/.cover.jpg";
    {
      HalFile coverJpg;
      if (!Storage.openFileForWrite("EBP", coverJpgTempPath, coverJpg)) {
        return false;
      }
      readItemContentsToStream(coverImageHref, coverJpg, kStreamChunkSize);
    }  // write handle closes before the read reopen below

    bool success;
    {
      HalFile coverJpg;
      if (!Storage.openFileForRead("EBP", coverJpgTempPath, coverJpg)) {
        return false;
      }

      HalFile coverBmp;
      if (!Storage.openFileForWrite("EBP", getCoverBmpPath(cropped, originalThresholds), coverBmp)) {
        return false;
      }
      success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp, cropped, originalThresholds);
    }  // both handles close before the temp remove below
    Storage.remove(coverJpgTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate BMP from cover image");
      Storage.remove(getCoverBmpPath(cropped, originalThresholds).c_str());
    }
    LOG_DBG("EBP", "Generated BMP from JPG cover image, success: %s", success ? "yes" : "no");
    return success;
  }

  if (FsHelpers::hasPngExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating BMP from PNG cover image (%s mode%s)", cropped ? "cropped" : "fit",
            originalThresholds ? ", original thresholds" : "");
    const auto coverPngTempPath = getCachePath() + "/.cover.png";
    {
      HalFile coverPng;
      if (!Storage.openFileForWrite("EBP", coverPngTempPath, coverPng)) {
        return false;
      }
      readItemContentsToStream(coverImageHref, coverPng, 1024);
    }  // write handle closes before the read reopen below

    bool success;
    {
      HalFile coverPng;
      if (!Storage.openFileForRead("EBP", coverPngTempPath, coverPng)) {
        return false;
      }

      HalFile coverBmp;
      if (!Storage.openFileForWrite("EBP", getCoverBmpPath(cropped, originalThresholds), coverBmp)) {
        return false;
      }
      success = PngToBmpConverter::pngFileToBmpStream(coverPng, coverBmp, cropped, originalThresholds);
    }  // both handles close before the temp remove below
    Storage.remove(coverPngTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate BMP from PNG cover image");
      Storage.remove(getCoverBmpPath(cropped, originalThresholds).c_str());
    }
    LOG_DBG("EBP", "Generated BMP from PNG cover image, success: %s", success ? "yes" : "no");
    return success;
  }

  if (FsHelpers::hasGifExtension(coverImageHref)) {
    return generateGifBmp(*this, coverImageHref, getCoverBmpPath(cropped, originalThresholds),
                          [cropped, originalThresholds](HalFile& gif, Print& bmp) {
                            return GifToBmpConverter::gifFileToBmpStream(gif, bmp, cropped, originalThresholds);
                          });
  }

  LOG_ERR("EBP", "Cover image is not a supported format, skipping");
  return false;
}

std::string Epub::getThumbBmpPath() const { return cachePath + "/thumb_[HEIGHT].bmp"; }
std::string Epub::getThumbBmpPath(int height) const { return cachePath + "/thumb_" + std::to_string(height) + ".bmp"; }

bool Epub::generateThumbBmp(int height) const {
  // Already generated, return true
  if (Storage.exists(getThumbBmpPath(height).c_str())) {
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate thumb BMP, cache not loaded");
    return false;
  }

  return generateThumbBmpForCover(height, bookMetadataCache->coreMetadata.coverItemHref);
}

bool Epub::generateThumbBmpFromSource(int height) {
  if (Storage.exists(getThumbBmpPath(height).c_str())) return true;
  if (Txt::isTxtOrMd(filepath)) {
    std::string companionCover = Txt::findCompanionCoverImage(filepath);
    if (companionCover.empty()) return false;
    setupCacheDir();
    return generateThumbBmpForCover(height, companionCover);
  }
  // Parser input and metadata outlive parsing but exceed the small task stack budget.
  auto metadata = makeUniqueNoThrow<BookMetadataCache::BookMetadata>();
  auto zip = makeUniqueNoThrow<ZipFile>(filepath);
  if (!metadata || !zip) {
    LOG_ERR("EBP", "OOM: cover metadata");
    return false;
  }
  if (!zip->open()) {
    LOG_ERR("EBP", "Could not open EPUB for cover metadata");
    return false;
  }
  if (!parseContentOpf(*metadata, /*writeSpineEntries=*/false, /*metadataOnly=*/false, zip.get())) return false;
  zip.reset();
  // The cover of a protected book is encrypted like everything else.
  if (!decryptor && !openProtection()) return false;
  setupCacheDir();
  return generateThumbBmpForCover(height, metadata->coverItemHref);
}

bool Epub::generateThumbBmpForCover(int height, const std::string& coverImageHref) const {
  if (coverImageHref.empty()) {
    LOG_DBG("EBP", "No known cover image for thumbnail");
  } else {
    if (Txt::isTxtOrMd(filepath)) {
      // Companion can appear/vanish after the cache was built; re-probe.
      std::string companion = coverImageHref;
      if (!Storage.exists(companion.c_str())) {
        companion = Txt::findCompanionCoverImage(filepath);
      }
      if (companion.empty()) {
        return false;
      }
      return Txt::convertCoverImageToBmp(companion, getThumbBmpPath(height), height);
    }

#ifdef BOARD_HAS_PSRAM
    // PSRAM boards decode the thumbnail from a pool buffer: no .cover.jpg/
    // .cover.png temp ever reaches SD. Falls back to the SD path below on
    // oversized (>4 MB), OOM, or decode failure.
    if (FsHelpers::hasJpgExtension(coverImageHref) || FsHelpers::hasPngExtension(coverImageHref)) {
      const int thumbTargetWidth = static_cast<int>(height * 0.6);
      if (generateBmpFromPsram(coverImageHref, getThumbBmpPath(height), /*oneBit=*/true, thumbTargetWidth, height,
                               /*crop=*/true, /*originalThresholds=*/false)) {
        return true;
      }
      LOG_DBG("EBP", "Falling back to SD staging for thumb");
    }
#endif
    if (FsHelpers::hasJpgExtension(coverImageHref)) {
      LOG_DBG("EBP", "Generating thumb BMP from JPG cover image");
      const auto coverJpgTempPath = getCachePath() + "/.cover.jpg";

      {
        HalFile coverJpg;
        if (!Storage.openFileForWrite("EBP", coverJpgTempPath, coverJpg)) {
          return false;
        }
        readItemContentsToStream(coverImageHref, coverJpg, 1024);
      }  // write handle closes before the read reopen below

      bool success;
      {
        HalFile coverJpg;
        if (!Storage.openFileForRead("EBP", coverJpgTempPath, coverJpg)) {
          return false;
        }

        HalFile thumbBmp;
        if (!Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp)) {
          return false;
        }
        // Use smaller target size for Continue Reading card (half of screen: 240x400)
        // Generate 1-bit BMP for fast home screen rendering (no gray passes needed)
        const int thumbTargetWidth = static_cast<int>(height * 0.6);
        success = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(coverJpg, thumbBmp, thumbTargetWidth, height);
      }  // both handles close before the temp remove below
      Storage.remove(coverJpgTempPath.c_str());

      if (!success) {
        LOG_ERR("EBP", "Failed to generate thumb BMP from JPG cover image");
        Storage.remove(getThumbBmpPath(height).c_str());
      }
      LOG_DBG("EBP", "Generated thumb BMP from JPG cover image, success: %s", success ? "yes" : "no");
      return success;
    } else if (FsHelpers::hasPngExtension(coverImageHref)) {
      LOG_DBG("EBP", "Generating thumb BMP from PNG cover image");
      const auto coverPngTempPath = getCachePath() + "/.cover.png";

      {
        HalFile coverPng;
        if (!Storage.openFileForWrite("EBP", coverPngTempPath, coverPng)) {
          return false;
        }
        readItemContentsToStream(coverImageHref, coverPng, kStreamChunkSize);
      }  // write handle closes before the read reopen below

      bool success;
      {
        HalFile coverPng;
        if (!Storage.openFileForRead("EBP", coverPngTempPath, coverPng)) {
          return false;
        }

        HalFile thumbBmp;
        if (!Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp)) {
          return false;
        }
        const int thumbTargetWidth = static_cast<int>(height * 0.6);
        success = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(coverPng, thumbBmp, thumbTargetWidth, height);
      }  // both handles close before the temp remove below
      Storage.remove(coverPngTempPath.c_str());

      if (!success) {
        LOG_ERR("EBP", "Failed to generate thumb BMP from PNG cover image");
        Storage.remove(getThumbBmpPath(height).c_str());
      }
      LOG_DBG("EBP", "Generated thumb BMP from PNG cover image, success: %s", success ? "yes" : "no");
      return success;
    } else if (FsHelpers::hasGifExtension(coverImageHref)) {
      return generateGifBmp(*this, coverImageHref, getThumbBmpPath(height), [height](HalFile& gif, Print& bmp) {
        return GifToBmpConverter::gifFileTo1BitBmpStreamWithSize(gif, bmp, height * 0.6, height);
      });
    } else {
      LOG_ERR("EBP", "Cover image is not a supported format, skipping thumbnail");
    }
  }

  // Write an empty bmp file to avoid generation attempts in the future
  HalFile thumbBmp;
  Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp);
  return false;
}

uint8_t* Epub::readItemContentsToBytes(const std::string& itemHref, size_t* size, const bool trailingNullByte) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return nullptr;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);

  // Decode encrypted entries on demand in memory.
  if (decryptor && decryptor->isEncrypted(path)) {
    const size_t plainSize = decryptor->decryptedSize(path);
    if (plainSize > SIZE_MAX - (trailingNullByte ? 1 : 0)) return nullptr;
    const size_t total = plainSize + (trailingNullByte ? 1 : 0);
    uint8_t* content = static_cast<uint8_t*>(malloc(total > 0 ? total : 1));
    if (!content) {
      LOG_ERR("EBP", "insufficient memory for %s (%u bytes)", path.c_str(), static_cast<unsigned>(total));
      return nullptr;
    }
    struct BufferSink {
      uint8_t* data;
      size_t capacity;
      size_t written;
    } state{content, plainSize, 0};
    auto append = [](void* context, const uint8_t* data, size_t size) {
      auto* target = static_cast<BufferSink*>(context);
      if (size > target->capacity - target->written) return false;
      memcpy(target->data + target->written, data, size);
      target->written += size;
      return true;
    };
    if (!decryptor->decryptToSink(path, append, &state) || state.written != plainSize) {
      free(content);
      LOG_ERR("EBP", "content read failed for %s", path.c_str());
      return nullptr;
    }
    if (trailingNullByte) content[plainSize] = 0;
    if (size) *size = plainSize;
    return content;
  }

  const auto content = ZipFile(filepath).readFileToMemory(path.c_str(), size, trailingNullByte);
  if (!content) {
    LOG_DBG("EBP", "Failed to read item %s", path.c_str());
    return nullptr;
  }

  return content;
}

bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize,
                                    const bool allowEarlyStop) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return false;
  }

  if (Txt::isTxtOrMd(filepath)) {
    if (FsHelpers::normalisePath(itemHref) != Txt::CONTENT_HREF) {
      LOG_DBG("EBP", "TXT/MD has no item %s", itemHref.c_str());
      return false;
    }
    return Txt::streamTxtToHtml(filepath, out, allowEarlyStop);
  }

  const std::string path = FsHelpers::normalisePath(itemHref);
  if (decryptor && decryptor->isEncrypted(path)) {
    auto append = [](void* context, const uint8_t* data, size_t size) {
      return static_cast<Print*>(context)->write(data, size) == size;
    };
    if (!decryptor->decryptToSink(path, append, &out)) {
      LOG_ERR("EBP", "content read failed for %s", path.c_str());
      return false;
    }
    return true;
  }

#ifdef BOARD_HAS_PSRAM
  // Fast path: use the PSRAM ZIP cache to skip the central-directory scan.
  // We open the ZIP once, then call the FileStatSlim overload of readFileToStream
  // which avoids the second CD scan that loadFileStatSlim would otherwise do.
  if (zipCache_) {
    ZipFile::FileStatSlim stat;  // by value: the worker task never maps a pointer across a loop-task erase
    if (zipCache_->statFor(path, stat)) {
      ZipFile zf(filepath);
      if (zf.open()) {
        if (zf.readFileToStream(stat, out, chunkSize, allowEarlyStop)) {
          return true;
        }
        // Cached read failed (rare: shifted localHeaderOffset, mid-stream inflate
        // error, output write error). Do NOT fall through to a non-cached
        // re-read on the same `out` — if bytes were already written, appending
        // again would corrupt the output. Mark the cached entry stale (so a
        // future read re-scans) and return false. The caller treats this as
        // a read failure, the same as pre-cache behaviour.
        LOG_ERR("EBP", "Cached ZIP read failed for %s; invalidating cache entry", path.c_str());
        zipCache_->invalidate(path);
        return false;
      }
    }
  }
#endif
  return ZipFile(filepath).readFileToStream(path.c_str(), out, chunkSize, allowEarlyStop);
}

PoolBytes Epub::extractItemToPsram(const std::string& itemHref, size_t* size) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return nullptr;
  }

  size_t itemSize = 0;
  if (!getItemSize(itemHref, &itemSize)) {
    LOG_ERR("EBP", "Could not size image item: %s", itemHref.c_str());
    return nullptr;
  }

  if (!isImageSizeWithinPsramLimit(itemSize)) {
    LOG_DBG("EBP", "Image too large for PSRAM staging: %s (%zu bytes)", itemHref.c_str(), itemSize);
    return nullptr;
  }

  PoolBytes buf{readItemContentsToBytes(itemHref, size)};
  if (!buf) {
    LOG_ERR("EBP", "OOM reading image item: %s", itemHref.c_str());
  }
  return buf;
}

bool Epub::extractItemToFile(const std::string& itemHref, const std::string& destPath) const {
  HalFile out;
  if (!Storage.openFileForWrite("EBP", destPath, out)) {
    return false;
  }
  // Large images dominate lazy extraction. Match the section streamer size to
  // halve SD read/write calls while adding only 8 KB of transient ZIP buffers.
  const bool ok = readItemContentsToStream(itemHref, out, 8192);
  out.flush();
  out.close();
  if (!ok) {
    Storage.remove(destPath.c_str());
  }
  return ok;
}

bool Epub::getItemSize(const std::string& itemHref, size_t* size) const {
  if (Txt::isTxtOrMd(filepath)) {
    // Only the single content spine item exists; other hrefs have no size.
    if (FsHelpers::normalisePath(itemHref) != Txt::CONTENT_HREF) {
      return false;
    }
    HalFile f;
    if (Storage.openFileForRead("EBP", filepath, f)) {
      if (size) *size = f.size();
      return true;
    }
    return false;
  }
  const std::string path = FsHelpers::normalisePath(itemHref);
#ifdef BOARD_HAS_PSRAM
  if (zipCache_) {
    ZipFile::FileStatSlim stat;
    if (zipCache_->statFor(path, stat)) {
      *size = stat.uncompressedSize;
      return true;
    }
  }
#endif
  return ZipFile(filepath).getInflatedFileSize(path.c_str(), size);
}

int Epub::getSpineItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }
  return bookMetadataCache->getSpineCount();
}

size_t Epub::getCumulativeSpineItemSize(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }
  return bookMetadataCache->getCumulativeSize(spineIndex);
}

BookMetadataCache::SpineEntry Epub::getSpineItem(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineItem called but cache not loaded");
    return {};
  }

  if (spineIndex < 0 || spineIndex >= bookMetadataCache->getSpineCount()) {
    LOG_ERR("EBP", "getSpineItem index:%d is out of range", spineIndex);
    return bookMetadataCache->getSpineEntry(0);
  }

  return bookMetadataCache->getSpineEntry(spineIndex);
}

BookMetadataCache::TocEntry Epub::getTocItem(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_DBG("EBP", "getTocItem called but cache not loaded");
    return {};
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_DBG("EBP", "getTocItem index:%d is out of range", tocIndex);
    return {};
  }

  return bookMetadataCache->getTocEntry(tocIndex);
}

int Epub::getTocItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }

  return bookMetadataCache->getTocCount();
}

// work out the section index for a toc index
int Epub::getSpineIndexForTocIndex(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex called but cache not loaded");
    return 0;
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex: tocIndex %d out of range", tocIndex);
    return 0;
  }

  const int spineIndex = bookMetadataCache->getTocEntry(tocIndex).spineIndex;
  if (spineIndex < 0) {
    LOG_DBG("EBP", "Section not found for TOC index %d", tocIndex);
    return 0;
  }

  return spineIndex;
}

int Epub::getTocIndexForSpineIndex(const int spineIndex) const { return getSpineItem(spineIndex).tocIndex; }

int Epub::getSpineIndexForSize(const size_t bytes) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded() || static_cast<uint64_t>(bytes) > UINT32_MAX) return -1;
  return bookMetadataCache->getSpineIndexForSize(static_cast<uint32_t>(bytes));
}

size_t Epub::getBookSize() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded() || bookMetadataCache->getSpineCount() == 0) {
    return 0;
  }
  return getCumulativeSpineItemSize(getSpineItemsCount() - 1);
}

int Epub::getSpineIndexForTextReference() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTextReference called but cache not loaded");
    return 0;
  }
  LOG_DBG("EBP", "Core Metadata: cover(%d)=%s, textReference(%d)=%s",
          bookMetadataCache->coreMetadata.coverItemHref.size(), bookMetadataCache->coreMetadata.coverItemHref.c_str(),
          bookMetadataCache->coreMetadata.textReferenceHref.size(),
          bookMetadataCache->coreMetadata.textReferenceHref.c_str());

  if (bookMetadataCache->coreMetadata.textReferenceHref.empty()) {
    // there was no textReference in epub, so we return 0 (the first chapter)
    return 0;
  }

  // loop through spine items to get the correct index matching the text href
  for (size_t i = 0; i < static_cast<size_t>(getSpineItemsCount()); i++) {
    if (getSpineItem(i).href == bookMetadataCache->coreMetadata.textReferenceHref) {
      LOG_DBG("EBP", "Text reference %s found at index %d", bookMetadataCache->coreMetadata.textReferenceHref.c_str(),
              i);
      return i;
    }
  }
  // This should not happen, as we checked for empty textReferenceHref earlier
  LOG_DBG("EBP", "Section not found for text reference");
  return 0;
}

// Calculate progress in book (returns 0.0-1.0)
float Epub::calculateProgress(const int currentSpineIndex, const float currentSpineRead) const {
  const size_t bookSize = getBookSize();
  if (bookSize == 0) {
    return 0.0f;
  }
  const size_t prevChapterSize = (currentSpineIndex >= 1) ? getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t curChapterSize = getCumulativeSpineItemSize(currentSpineIndex) - prevChapterSize;
  const float sectionProgSize = currentSpineRead * static_cast<float>(curChapterSize);
  const float totalProgress = static_cast<float>(prevChapterSize) + sectionProgSize;
  return totalProgress / static_cast<float>(bookSize);
}

int Epub::resolveHrefToSpineIndex(const std::string& href) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return -1;

  // Split before decoding so escaped '#' characters in filenames stay part of the path.
  const size_t hashPos = href.find('#');
  const std::string rawTarget = hashPos != std::string::npos ? href.substr(0, hashPos) : href;
  const std::string target = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(rawTarget));

  // Same-file reference (anchor-only)
  if (target.empty()) return -1;

  // Extract just the filename for comparison
  size_t targetSlash = target.find_last_of('/');
  std::string targetFilename = (targetSlash != std::string::npos) ? target.substr(targetSlash + 1) : target;

  for (int i = 0; i < getSpineItemsCount(); i++) {
    const auto& spineHref = getSpineItem(i).href;
    // Try exact match first
    if (spineHref == target) return i;
    // Then filename-only match
    size_t spineSlash = spineHref.find_last_of('/');
    std::string spineFilename = (spineSlash != std::string::npos) ? spineHref.substr(spineSlash + 1) : spineHref;
    if (spineFilename == targetFilename) return i;
  }
  return -1;
}

#ifdef BOARD_HAS_PSRAM
void Epub::ZipFileCache::load(const std::string& epubPath) {
  // ZipFile stores its filePath as a const std::string& member. Pass an lvalue
  // reference (from the caller's stored std::string) so the reference stays
  // valid for the lifetime of the ZipFile. A const char* here would create a
  // temporary std::string that expires at the end of this full-expression,
  // leaving ZipFile::filePath as a dangling reference (CodeRabbit IPV5).
  ZipFile zf(epubPath);
  if (!zf.open()) return;
  if (!zf.loadAllFileStatSlims()) return;
  // Move the cache out of zf to avoid the ~2x transient copy the const& getter
  // would force. The map lives in DRAM and is entry-count dependent
  // (~115 B/entry incl. heap nodes; ~56 KB at 465 entries — measured in the
  // 2026-11-03 soak), big enough to make the copy worth avoiding.
  cache_ = zf.takeFileStatSlimCache();
  loaded_ = true;
  LOG_DBG("ZIP", "ZIP cache loaded: %zu entries", static_cast<size_t>(cache_.size()));
}

const ZipFile::FileStatSlim* Epub::ZipFileCache::get(std::string_view itemPath) const {
  if (!loaded_) return nullptr;
  // Allocate the lookup key once. Caller passes std::string_view to avoid
  // the per-call FsHelpers::normalisePath() + string copy the old API did
  // (CodeRabbit IPV8; AGENTS.md rule 4); the std::string here is unavoidable
  // without C++20 heterogeneous lookup (transparent hash + comparator on the
  // map). The cost is one short-string allocation per cache lookup, which is
  // still far less than the previous path (which allocated a path AND
  // normalised it AND compared to a std::string key).
  std::lock_guard<std::mutex> lock(cacheMutex_);
  if (auto it = cache_.find(std::string{itemPath}); it != cache_.end()) {
    return &it->second;
  }
  return nullptr;
}

bool Epub::ZipFileCache::statFor(std::string_view itemPath, ZipFile::FileStatSlim& out) const {
  if (!loaded_) return false;
  std::lock_guard<std::mutex> lock(cacheMutex_);
  if (auto it = cache_.find(std::string{itemPath}); it != cache_.end()) {
    out = it->second;
    return true;
  }
  return false;
}

void Epub::ZipFileCache::invalidate(std::string_view itemPath) {
  // Erase one cached entry so the next read re-scans the ZIP central
  // directory for this item. No-op if the entry is not present.
  // Guarded: the predecode worker's lookups run beside this loop-task erase
  // (qodo round-3 #4).
  std::lock_guard<std::mutex> lock(cacheMutex_);
  cache_.erase(std::string{itemPath});
}

#ifdef BOARD_HAS_PSRAM
// PSRAM deleter for the ZipFileCache object itself: placement-new'd in load()
// via heap_caps_malloc(MALLOC_CAP_SPIRAM). The matching free MUST be
// heap_caps_free to release the cap-matched allocation; default_delete's
// `delete` would call operator delete, which on ESP-IDF routes to
// heap_caps_free (works in practice) but is not cap-safe in general.
// CodeRabbit IPVX: make the deleter a function pointer to avoid the
// unique_ptr<ChapterHtmlSlimParser> static_assert issue (this is a leaf
// type so the static_assert passes, but the function-pointer form is the
// project convention).
void Epub::ZipFileCache::deleteSelfPsram(ZipFileCache* p) {
  if (p) {
    p->~ZipFileCache();
    heap_caps_free(p);
  }
}
#endif
#endif
