#include "TtfFontDownloadActivity.h"

// The classic device class has no raw-TTF catalog to serve (the header compiles
// to nothing there), so neither does this translation unit.
#if defined(CROSSPOINT_TTF_READER)

#include <ArduinoJson.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace fui = freeink::ui;

namespace {
// Suffix for a face that has been downloaded and verified but not published
// yet. Deliberately not a font extension, so BookFontLoader never registers a
// staged file if the download is interrupted.
constexpr const char* kStageSuffix = ".part";
// Room for "<fonts root>/<slug>/<file>.part": buildFontPath's own buffer plus
// the suffix and its terminator.
constexpr size_t kStagePathSize = 176;
// FAT slack plus the cluster rounding of the staged copies.
constexpr uint64_t kFreeSpaceMargin = 64 * 1024;
// Manifest-fetch cap: the published catalog is ~64 KB today (55 families, 211
// files); the cap leaves headroom for growth while still bounding a hostile
// response well below the SD card's free space. At file scope so the claim
// floors and fetchAndParseManifest share it.
constexpr size_t kMaxTtfManifestBytes = 98304;
// The memory-claim floors (docs/design/2026-11-03-memory-claims.md §3): the
// tlsheap floor PLUS the manifest buffer's own DRAM demand, because the
// POOL allocation below draws DRAM on no-PSRAM boards and poolMakeBytes only
// draws PSRAM on PSRAM boards. This keeps the claim from declaring Proceed
// with budget the manifest buffer would then consume. The POST-allocation
// HttpDownloader::heapAvailableForTransfer() check (:272-275, and per
// transfer at :703-707) stays unchanged and remains the final arbiter
// against what is really left.
#if defined(BOARD_HAS_PSRAM)
constexpr uint32_t kManifestDramBudgetBytes = 0;
#else
constexpr uint32_t kManifestDramBudgetBytes = static_cast<uint32_t>(kMaxTtfManifestBytes);
#endif
constexpr uint32_t kClaimFreeFloor = tlsheap::kMinFreeHeap + kManifestDramBudgetBytes;
constexpr uint32_t kClaimMaxBlockFloor = tlsheap::kMinLargestBlock + kManifestDramBudgetBytes;
}  // namespace

uint8_t TtfFontDownloadActivity::parseStyleToken(const char* token) {
  if (token == nullptr) return 0;
  if (std::strcmp(token, "regular") == 0) return STYLE_REGULAR;
  if (std::strcmp(token, "bold") == 0) return STYLE_BOLD;
  if (std::strcmp(token, "italic") == 0) return STYLE_ITALIC;
  if (std::strcmp(token, "bolditalic") == 0) return STYLE_BOLD_ITALIC;
  return 0;
}

TtfFontDownloadActivity::TtfFontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("TtfFontDownload", renderer, mappedInput),
      fontInstaller_(sdFontSystem.registry()),
      claim_(renderer, "TTFFONT") {}

void TtfFontDownloadActivity::activateIndex(const int index) {
  switch (state_) {
    case FAMILY_LIST:
      nav.selected = index;
      // Activation starts a download or opens the delete prompt; a lingering
      // flash would gray an unrelated row.
      app.clearTapFlash();
      activateSelected();  // ends with requestUpdateAndWait itself
      return;
    case WIFI_SELECTION:
    case LOADING_MANIFEST:
    case DOWNLOADING:
    case COMPLETE:
    case ERROR:
      return;
  }
}

fui::ListNav& TtfFontDownloadActivity::activeNav() { return nav; }

void TtfFontDownloadActivity::onBackButton() { finish(); }

// --- Lifecycle ---

void TtfFontDownloadActivity::onEnter() {
  UiListActivity::onEnter();
  WiFi.mode(WIFI_STA);
  auto wifiPicker = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifiPicker) {
    LOG_ERR("TTFFONT", "OOM: Wi-Fi selection");
    finish();
    return;
  }
  startActivityForResult(std::move(wifiPicker),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void TtfFontDownloadActivity::onExit() {
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    // Reboot rather than rescan in place: BookFontLoader only re-reads the SD
    // font directories at boot, so the new family would otherwise not appear
    // in the reader's font list until the next power cycle.
    silentRestart();
  }
}

void TtfFontDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  // The claim begins at LOADING_MANIFEST entry (§3.2): it records the radio's
  // state and the pre-claim heap, and performs NO blocking call. The fetch
  // and every suspension step run on activity passes that return to the loop
  // (manifestPass(), via handleCustomInput), never inside this callback.
  claim_.begin(kClaimFreeFloor, kClaimMaxBlockFloor);

  {
    RenderLock lock(*this);
    state_ = LOADING_MANIFEST;
    manifestStage_ = ManifestStage::Claim;
  }
  requestUpdateAndWait();
}

// One plan-act pass of the claim recheck loop (the same structure as the
// transfer-progress pump below — input is pumped where the loop returns
// control, never inside a blocking call). Back is polled on every pass and
// cancels the claim (levers stay as pulled; the restore is deferred per §3.3).
void TtfFontDownloadActivity::manifestPass() {
  if (manifestStage_ == ManifestStage::Claim) {
    const memclaim::Phase phase = claim_.pumpOnePass();
    if (phase.verdict == memclaim::Verdict::Fallback) {
      // Existing ERROR screen, STR_MEMORY_ERROR (unchanged UX; the pump has
      // logged the [MEM] claim evidence line).
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = tr(STR_MEMORY_ERROR);
      // Nothing was downloaded, so the error screen offers no retry.
      downloadingFamilyIndex_ = -1;
      return;
    }
    if (phase.verdict == memclaim::Verdict::Proceed) {
      manifestStage_ = ManifestStage::Fetch;
    }
    // Defrag: the next activity pass pulls the next lever (or re-issues the
    // radio stop). No repaint: the frame painted at state entry stays up — the
    // common path (radio already down, caches already released) costs two
    // measurements and no paint (§6 "announced vs silent").
    return;
  }

  // Fetch stage: the claim's levers are already applied; the POST-allocation
  // heapAvailableForTransfer() check inside fetchAndParseManifest() remains
  // the final arbiter (§3).
  if (!fetchAndParseManifest()) {
    // Drop whatever was parsed before the failure: it would otherwise sit in
    // the heap behind the error screen, and leave the retry path pointing at a
    // half-built family table.
    clearManifest();
    RenderLock lock(*this);
    state_ = ERROR;
    // Nothing was downloaded, so the error screen offers no retry.
    downloadingFamilyIndex_ = -1;
    return;
  }
  RenderLock lock(*this);
  rowsDirty_ = true;  // families_ just loaded
  nav.reset();
  state_ = FAMILY_LIST;
}

// --- Manifest fetching ---

void TtfFontDownloadActivity::clearManifest() {
  // Swap rather than clear: clear() keeps the capacity, and this runs to hand
  // the heap back while the error screen is up.
  std::vector<TtfManifestFamily>().swap(families_);
  files_.reset();
  fileEntryCount_ = 0;
  stringArena_.reset();
  arenaUsed_ = 0;
  arenaCapacity_ = 0;
}

bool TtfFontDownloadActivity::internString(const char* text, StrRef& outRef) {
  if (text == nullptr || *text == '\0') {
    outRef = 0;
    return true;
  }
  const size_t length = std::strlen(text) + 1;
  if (arenaUsed_ + length > arenaCapacity_) {
    LOG_ERR("TTFFONT", "Manifest string arena overflow at %u/%u bytes", arenaUsed_, arenaCapacity_);
    return false;
  }
  outRef = arenaUsed_;
  std::memcpy(stringArena_.get() + arenaUsed_, text, length);
  arenaUsed_ = static_cast<uint32_t>(arenaUsed_ + length);
  return true;
}

// Splits "dir/name.ttf" and checks that it is a safe, single-level path whose
// directory matches the family's slug. Anything else (absolute paths, "..",
// backslashes, extra separators, a basename that disagrees with the file's
// name field) is rejected outright rather than sanitised — a manifest we do
// not understand must not be able to steer a write outside the family folder.
bool TtfFontDownloadActivity::splitCatalogPath(const char* path, char* dirBuf, const size_t dirBufSize,
                                               const char*& slug, const char*& baseName) {
  // Bound mirrors the longest published catalog path with room to spare, and
  // keeps every later buffer size derivable from it.
  constexpr size_t kMaxCatalogPathLen = 128;

  if (path == nullptr) return false;
  const size_t length = std::strlen(path);
  if (length == 0 || length > kMaxCatalogPathLen) return false;
  if (path[0] == '/' || std::strchr(path, '\\') != nullptr) return false;

  // Walk the '/'-separated components: each must be a non-empty plain name.
  size_t componentStart = 0;
  const char* lastSlash = nullptr;
  for (size_t i = 0; i <= length; i++) {
    if (i != length && path[i] != '/') continue;
    const size_t componentLen = i - componentStart;
    if (componentLen == 0) return false;
    if (path[componentStart] == '.' && (componentLen == 1 || (componentLen == 2 && path[componentStart + 1] == '.'))) {
      return false;
    }
    lastSlash = (i != length) ? path + i : lastSlash;
    componentStart = i + 1;
  }
  if (lastSlash == nullptr) return false;  // no directory component at all

  baseName = lastSlash + 1;
  if (!FontInstaller::isValidTtfFilename(baseName)) return false;

  // Copy the directory part out so the slug is a plain C string that survives
  // past the path's own lifetime.
  const size_t dirLen = static_cast<size_t>(baseName - path) - 1;
  if (dirLen + 1 > dirBufSize) return false;
  std::memcpy(dirBuf, path, dirLen);
  dirBuf[dirLen] = '\0';

  const char* lastDirSlash = std::strrchr(dirBuf, '/');
  slug = lastDirSlash != nullptr ? lastDirSlash + 1 : dirBuf;
  if (!FontInstaller::isValidFamilyName(slug)) return false;
  return true;
}

const char* TtfFontDownloadActivity::fileBaseName(const StrRef path) const {
  const char* text = str(path);
  const char* slash = std::strrchr(text, '/');
  return slash != nullptr ? slash + 1 : text;
}

void TtfFontDownloadActivity::refreshInstalledState(TtfManifestFamily& family) {
  family.installed = false;
  family.hasUpdate = false;
  if (family.fileCount == 0) return;

  uint32_t foundCount = 0;
  bool sizeMismatch = false;
  for (uint32_t i = 0; i < family.fileCount; i++) {
    const TtfManifestFile& file = files_[family.fileStart + i];
    char path[160];
    FontInstaller::buildFontPath(str(family.dirName), fileBaseName(file.path), path, sizeof(path));
    HalFile f;
    if (!Storage.openFileForRead("FONT", path, f)) continue;
    ++foundCount;
    if (f.fileSize() != file.size) sizeMismatch = true;
    // No explicit close(): the destructor closes local HalFile values
    // (DESTRUCTOR_CLOSES_FILE=1).
  }

  if (foundCount == 0) return;  // nothing on disk yet
  family.hasUpdate = true;
  if (foundCount == family.fileCount && !sizeMismatch) {
    family.installed = true;
    family.hasUpdate = false;
  }
}

bool TtfFontDownloadActivity::fetchAndParseManifest() {
  // Fetch the manifest into a PSRAM buffer and parse from memory, avoiding the
  // download-to-SD-then-read-back round-trip that wastes I/O and risks leaving
  // temp files on failure. The published catalog is ~64 KB today (55 families,
  // 211 files); kMaxTtfManifestBytes caps a hostile response well below the SD
  // card's free space.

  // The unconditional pre-allocation release, exactly as today (§3): never a
  // lever-conditional step, so a Proceed can never skip this cleanup. The
  // synchronous no-op release still counts as pulled for the claim's
  // stepsTried (§3.2), in the same pass.
  memclaim::releaseFontCaches(renderer);
  claim_.noteFontCachesPulled();

  // Accumulate into a PoolBytes buffer so the fetch does not depend on guessing
  // the exact manifest size — poolMalloc draws from PSRAM on PSRAM boards.
  auto manifestBuf = poolMakeBytes(kMaxTtfManifestBytes);
  if (!manifestBuf) {
    LOG_ERR("TTFFONT", "OOM: %zu byte manifest buffer", kMaxTtfManifestBytes);
    errorMessage_ = tr(STR_MEMORY_ERROR);
    return false;
  }

  // Heap check after the manifest buffer is allocated so the TLS transfer
  // inside fetchUrl() is validated against what is really left.
  if (!HttpDownloader::heapAvailableForTransfer()) {
    errorMessage_ = tr(STR_MEMORY_ERROR);
    return false;
  }

  size_t manifestLen = 0;
  const auto dataOk = HttpDownloader::fetchUrl(
      TTF_FONTS_MANIFEST_URL,
      [&manifestBuf, &manifestLen](const uint8_t* data, size_t len) {
        if (manifestLen + len > kMaxTtfManifestBytes) {
          LOG_ERR("TTFFONT", "Manifest exceeds %zu bytes; aborting", kMaxTtfManifestBytes);
          return false;
        }
        std::memcpy(manifestBuf.get() + manifestLen, data, len);
        manifestLen += len;
        return true;
      },
      /*username=*/"", /*password=*/"");
  if (!dataOk || manifestLen == 0) {
    LOG_ERR("TTFFONT", "Failed to fetch manifest from %s", TTF_FONTS_MANIFEST_URL);
    errorMessage_ = tr(STR_FETCH_FEED_FAILED);
    return false;
  }

  // HTTP client is now closed — TLS buffers freed. Parse JSON from the
  // in-memory buffer.
  JsonDocument doc;
  DeserializationError err;
  {
    // "styles", "license", "source", "type" and "preview" are not read here:
    // the style list duplicates what each file's own "style" token carries,
    // and the preview URLs serve the web gallery, not the device. Dropping
    // them keeps the DOM smaller while it coexists with the arena below.
    JsonDocument filter;
    filter["version"] = true;
    filter["kind"] = true;
    filter["baseUrl"] = true;
    filter["families"][0]["name"] = true;
    filter["families"][0]["description"] = true;
    filter["families"][0]["files"][0]["path"] = true;
    filter["families"][0]["files"][0]["style"] = true;
    filter["families"][0]["files"][0]["size"] = true;
    filter["families"][0]["files"][0]["crc32"] = true;
    err = deserializeJson(doc, manifestBuf.get(), manifestLen, DeserializationOption::Filter(filter));
  }
  // manifestBuf is a PoolBytes — freed automatically on scope exit

  if (err) {
    LOG_ERR("TTFFONT", "Manifest parse error: %s", err.c_str());
    errorMessage_ = tr(STR_INVALID_FONT_MANIFEST);
    return false;
  }

  const int version = doc["version"] | 0;
  if (version != FONTS_MANIFEST_VERSION) {
    LOG_ERR("TTFFONT", "Unsupported manifest version: %d", version);
    errorMessage_ = tr(STR_UNSUPPORTED_MANIFEST_VERSION);
    return false;
  }

  // Guards against pointing the .cpfont-shaped downloader at the raw-sfnt
  // catalog (or the other way round): both share version 1 but not payload.
  const char* kind = doc["kind"] | "";
  if (std::strcmp(kind, "ttf") != 0) {
    LOG_ERR("TTFFONT", "Manifest is not a TTF catalog (kind=%s)", kind);
    errorMessage_ = tr(STR_INVALID_FONT_MANIFEST);
    return false;
  }

  baseUrl_ = doc["baseUrl"] | "";
  if (baseUrl_.empty()) {
    LOG_ERR("TTFFONT", "Manifest carries no baseUrl");
    errorMessage_ = tr(STR_INVALID_FONT_MANIFEST);
    return false;
  }
  // File paths are relative; a base URL without the trailing slash would
  // concatenate into one long filename.
  if (baseUrl_.back() != '/') baseUrl_.push_back('/');
  downloadUrl_.reserve(baseUrl_.size() + 128);
  clearManifest();

  JsonArray familiesArr = doc["families"].as<JsonArray>();
  if (familiesArr.isNull() || familiesArr.size() == 0) {
    LOG_ERR("TTFFONT", "Manifest carries no families");
    errorMessage_ = tr(STR_INVALID_FONT_MANIFEST);
    return false;
  }

  // Size the arena and the file table in one pass so neither reallocates while
  // the catalog is built: a mid-build growth would both fragment the heap and
  // invalidate arena pointers already handed out below.
  // Mirrors the interning pass below string for string: name, description, one
  // dirName slug per family, and every file path. A slug the sizing pass cannot
  // resolve only means the build pass aborts on that same path a moment later,
  // so the missing bytes are never written against.
  size_t arenaBytes = 1;  // leading terminator makes offset 0 the empty string
  size_t manifestFileCount = 0;
  for (JsonObject fObj : familiesArr) {
    arenaBytes += std::strlen(fObj["name"] | "") + 1;
    arenaBytes += std::strlen(fObj["description"] | "") + 1;
    bool slugCounted = false;
    for (JsonObject fileObj : fObj["files"].as<JsonArray>()) {
      const char* path = fileObj["path"] | "";
      arenaBytes += std::strlen(path) + 1;
      manifestFileCount++;
      if (slugCounted) continue;
      slugCounted = true;
      char fileDir[64];
      const char* fileSlug = nullptr;
      const char* pathBase = nullptr;
      if (splitCatalogPath(path, fileDir, sizeof(fileDir), fileSlug, pathBase)) {
        arenaBytes += std::strlen(fileSlug) + 1;
      }
    }
  }
  stringArena_ = makeUniqueNoThrow<char[]>(arenaBytes);
  if (!stringArena_) {
    LOG_ERR("TTFFONT", "OOM: %zu byte string arena", arenaBytes);
    errorMessage_ = tr(STR_MEMORY_ERROR);
    return false;
  }
  stringArena_[0] = '\0';
  arenaUsed_ = 1;
  arenaCapacity_ = static_cast<uint32_t>(arenaBytes);
  files_ = makeUniqueNoThrow<TtfManifestFile[]>(manifestFileCount);
  if (!files_) {
    LOG_ERR("TTFFONT", "OOM: %zu manifest file entries", manifestFileCount);
    errorMessage_ = tr(STR_MEMORY_ERROR);
    return false;
  }

  families_.reserve(familiesArr.size());

  for (JsonObject fObj : familiesArr) {
    TtfManifestFamily family;
    const char* familyName = fObj["name"] | "";
    if (!FontInstaller::isValidTtfFamilyName(familyName)) {
      LOG_ERR("TTFFONT", "Rejected manifest family name: %s", familyName);
      errorMessage_ = tr(STR_INVALID_FONT_MANIFEST);
      return false;
    }
    JsonArray filesArr = fObj["files"].as<JsonArray>();
    if (filesArr.isNull() || filesArr.size() == 0) {
      LOG_ERR("TTFFONT", "Family has no files: %s", familyName);
      errorMessage_ = tr(STR_INVALID_FONT_MANIFEST);
      return false;
    }

    // Resolve (and validate) the family's folder before interning anything:
    // every file has to land in the same folder because ensureFamilyDir and
    // the delete path both address the family by that single slug.
    char slugBuf[64] = {0};
    bool slugResolved = false;
    for (JsonObject fileObj : filesArr) {
      char fileDir[64];
      const char* fileSlug = nullptr;
      const char* pathBase = nullptr;
      if (!splitCatalogPath(fileObj["path"] | "", fileDir, sizeof(fileDir), fileSlug, pathBase)) {
        LOG_ERR("TTFFONT", "Rejected manifest path in %s: %s", familyName, fileObj["path"] | "");
        errorMessage_ = tr(STR_INVALID_FONT_MANIFEST);
        return false;
      }
      if (!slugResolved) {
        std::snprintf(slugBuf, sizeof(slugBuf), "%s", fileSlug);
        slugResolved = true;
      } else if (std::strcmp(slugBuf, fileSlug) != 0) {
        LOG_ERR("TTFFONT", "Family %s spans more than one folder", familyName);
        errorMessage_ = tr(STR_INVALID_FONT_MANIFEST);
        return false;
      }
    }

    // Two catalog names can share one slug (they differ only in spacing or
    // case), and the card layout is keyed by the slug alone. Installing both
    // would make them overwrite each other's faces in the same folder, so the
    // later entry loses and the first one keeps the folder.
    const TtfManifestFamily* duplicate = nullptr;
    for (const TtfManifestFamily& seen : families_) {
      if (std::strcmp(str(seen.dirName), slugBuf) == 0) {
        duplicate = &seen;
        break;
      }
    }
    if (duplicate != nullptr) {
      LOG_ERR("TTFFONT", "Duplicate folder slug '%s' in family %s; skipping (already served by %s)", slugBuf,
              familyName, str(duplicate->name));
      continue;
    }

    if (!internString(familyName, family.name) || !internString(fObj["description"] | "", family.description) ||
        !internString(slugBuf, family.dirName)) {
      errorMessage_ = tr(STR_INVALID_FONT_MANIFEST);
      return false;
    }

    family.fileStart = fileEntryCount_;
    for (JsonObject fileObj : filesArr) {
      TtfManifestFile file;
      if (!internString(fileObj["path"] | "", file.path)) {
        errorMessage_ = tr(STR_INVALID_FONT_MANIFEST);
        return false;
      }
      // Every path was validated in the slug pre-pass, so the basename is a
      // plain name here.
      const char* pathBase = fileBaseName(file.path);
      file.size = fileObj["size"] | 0u;

      if (!fileObj["crc32"].is<uint32_t>()) {
        LOG_ERR("TTFFONT", "Malformed manifest file entry: missing or invalid crc32 for %s", pathBase);
        errorMessage_ = tr(STR_INVALID_FONT_MANIFEST);
        return false;
      }
      file.crc32 = fileObj["crc32"].as<uint32_t>();

      file.styleFlags = parseStyleToken(fileObj["style"] | "");
      family.styleFlags |= file.styleFlags;

      family.totalSize += file.size;
      files_[fileEntryCount_++] = file;
    }
    family.fileCount = fileEntryCount_ - family.fileStart;

    refreshInstalledState(family);
    families_.push_back(family);
  }

  const size_t rowCapacity = families_.size() + 2;
  rowLabels_.reserve(rowCapacity);
  rowItems_.reserve(rowCapacity);

  LOG_DBG("TTFFONT", "Manifest loaded: %zu families, %u files", families_.size(), fileEntryCount_);
  return true;
}

// --- Download ---

void TtfFontDownloadActivity::downloadAll() {
  cancelRequested_ = false;
  // One preflight for the whole batch; each family is checked again on its own
  // because the card fills up as the run proceeds.
  if (!hasFreeSpaceFor(totalDownloadSize() + kFreeSpaceMargin)) {
    {
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = tr(STR_NOT_ENOUGH_SPACE);
      // The batch never started, so nothing is retryable: without this the
      // error screen's RETRY would re-run the family of an earlier download.
      downloadingFamilyIndex_ = -1;
    }
    return;
  }
  for (int familyIndex = 0; familyIndex < static_cast<int>(families_.size()); familyIndex++) {
    if (families_[familyIndex].installed) continue;
    downloadFamily(families_[familyIndex]);
    if (state_ == ERROR || cancelRequested_) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void TtfFontDownloadActivity::updateAll() {
  cancelRequested_ = false;
  // An update keeps the installed faces on the card until the new ones are
  // published, so the batch needs room for both copies of the largest face.
  uint32_t largestFile = 0;
  for (const auto& family : families_) {
    if (!family.hasUpdate) continue;
    for (uint32_t i = 0; i < family.fileCount; i++) {
      largestFile = std::max(largestFile, files_[family.fileStart + i].size);
    }
  }
  if (!hasFreeSpaceFor(totalUpdateSize() + largestFile + kFreeSpaceMargin)) {
    {
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = tr(STR_NOT_ENOUGH_SPACE);
      downloadingFamilyIndex_ = -1;
    }
    return;
  }
  for (int familyIndex = 0; familyIndex < static_cast<int>(families_.size()); familyIndex++) {
    if (!families_[familyIndex].hasUpdate) continue;
    downloadFamily(families_[familyIndex]);
    if (state_ == ERROR || cancelRequested_) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

bool TtfFontDownloadActivity::showDownloadAllRow() const {
  for (const auto& family : families_) {
    if (!family.installed) return true;
  }
  return false;
}

bool TtfFontDownloadActivity::showUpdateAllRow() const {
  for (const auto& family : families_) {
    if (family.hasUpdate) return true;
  }
  return false;
}

int TtfFontDownloadActivity::specialRowCount() const {
  return (showDownloadAllRow() ? 1 : 0) + (showUpdateAllRow() ? 1 : 0);
}

bool TtfFontDownloadActivity::isDownloadAllRow(int index) const { return showDownloadAllRow() && index == 0; }

bool TtfFontDownloadActivity::isUpdateAllRow(int index) const {
  return showUpdateAllRow() && index == (showDownloadAllRow() ? 1 : 0);
}

int TtfFontDownloadActivity::listItemCount() const { return static_cast<int>(families_.size()) + specialRowCount(); }

int TtfFontDownloadActivity::listCount() const {
  switch (state_) {
    case FAMILY_LIST:
      return listItemCount();
    case WIFI_SELECTION:
    case LOADING_MANIFEST:
    case DOWNLOADING:
    case COMPLETE:
    case ERROR:
      return 0;
  }
  return 0;
}

int TtfFontDownloadActivity::familyIndexFromList(const int listIndex) const {
  const int filteredIndex = listIndex - specialRowCount();
  if (filteredIndex < 0 || filteredIndex >= static_cast<int>(families_.size())) return -1;
  return filteredIndex;
}

size_t TtfFontDownloadActivity::totalDownloadSize() const {
  size_t total = 0;
  for (const auto& family : families_) {
    if (!family.installed) total += family.totalSize;
  }
  return total;
}

size_t TtfFontDownloadActivity::totalUpdateSize() const {
  size_t total = 0;
  for (const auto& family : families_) {
    if (family.hasUpdate) total += family.totalSize;
  }
  return total;
}

// Standard CRC32 matching zlib/Python zlib.crc32().
bool TtfFontDownloadActivity::computeFileCrc32(const char* path, uint32_t& outCrc) {
  HalFile f;
  if (!Storage.openFileForRead("FONT", path, f)) {
    return false;
  }
  constexpr size_t BUF_SIZE = 128;
  uint8_t buf[BUF_SIZE];
  uint32_t crc = 0;
  while (f.available()) {
    const int n = f.read(buf, BUF_SIZE);
    if (n <= 0) break;
    crc = esp_rom_crc32_le(crc, buf, static_cast<uint32_t>(n));
  }
  outCrc = crc;
  return true;
}

// Removes the family's staged (<name>.part) files and re-reads what the card
// actually holds. Nothing else on the card is touched: an interrupted download
// leaves the previously installed family exactly as it was, which is why the
// flags come back from disk instead of being assumed.
void TtfFontDownloadActivity::discardStagedFiles(TtfManifestFamily& family) {
  char destPath[160];
  char stagePath[kStagePathSize];
  for (uint32_t i = 0; i < family.fileCount; i++) {
    const TtfManifestFile& file = files_[family.fileStart + i];
    FontInstaller::buildFontPath(str(family.dirName), fileBaseName(file.path), destPath, sizeof(destPath));
    snprintf(stagePath, sizeof(stagePath), "%s%s", destPath, kStageSuffix);
    Storage.remove(stagePath);
  }
  refreshInstalledState(family);
}

// A card that cannot hold the pending bytes cannot report a useful download
// error halfway through, so the check runs before any file is fetched.
bool TtfFontDownloadActivity::hasFreeSpaceFor(const uint64_t requiredBytes) const {
  uint64_t freeBytes = 0;
  if (!Storage.freeBytes(&freeBytes)) {
    // Unknown capacity (card not mounted, or the FAT scan could not report a
    // cluster count): let the download discover the problem the hard way rather
    // than refusing work that would have fit. A full card is NOT unknown — it
    // reports a real 0 and is refused below.
    LOG_DBG("TTFFONT", "SD free space unknown; skipping preflight");
    return true;
  }
  if (freeBytes >= requiredBytes) return true;
  LOG_ERR("TTFFONT", "SD needs %llu bytes, %llu free", static_cast<unsigned long long>(requiredBytes),
          static_cast<unsigned long long>(freeBytes));
  return false;
}

void TtfFontDownloadActivity::downloadFamily(TtfManifestFamily& family) {
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingFamilyIndex_ = static_cast<int>(&family - families_.data());
    fileProgress_ = 0;
    fileTotal_ = 0;
    cancelRequested_ = false;
    goHomeRequested_ = false;
  }
  requestUpdateAndWait();

  // Rebuildable SD-font caches (glyph/kern arenas, CJK fallback tables) can
  // hold tens of KB the TLS session needs; release them up front rather than
  // starving the transfer. They repopulate on demand after the download.
  // Shared helper + claim note (§4): the per-family transfers run with the
  // claim already open, so this re-pull is a no-op for the claim's recorder
  // (idempotent step bit) but keeps the counter truthful.
  memclaim::releaseFontCaches(renderer);
  claim_.noteFontCachesPulled();
  LOG_DBG("TTFFONT", "Free heap after SD font cache release: %d bytes", ESP.getFreeHeap());

  // Check before touching the family directory so a failed update leaves the
  // installed family unchanged.
  if (!HttpDownloader::heapAvailableForTransfer()) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_MEMORY_ERROR);
    return;
  }

  // Peak usage is the whole family plus the largest single face, which is
  // still on the card as the installed copy while its staged replacement is
  // being written.
  uint32_t largestFile = 0;
  for (uint32_t i = 0; i < family.fileCount; i++) {
    largestFile = std::max(largestFile, files_[family.fileStart + i].size);
  }
  if (!hasFreeSpaceFor(static_cast<uint64_t>(family.totalSize) + largestFile + kFreeSpaceMargin)) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_NOT_ENOUGH_SPACE);
    return;
  }

  if (!fontInstaller_.ensureFamilyDir(str(family.dirName))) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_FONT_DIR_CREATE_FAILED);
    return;
  }

  for (uint32_t i = 0; i < family.fileCount; i++) {
    const TtfManifestFile& file = files_[family.fileStart + i];

    {
      RenderLock lock(*this);
      fileProgress_ = 0;
      fileTotal_ = file.size;
    }
    requestUpdateAndWait();

    // On-SD layout is <fonts root>/<slug>/<object key> — the catalog path's
    // last two components, not its full path. Reproducing the path verbatim
    // would bury the files one level too deep for BookFontLoader, which reads
    // a family's faces straight out of the folder it finds in the root.
    const char* baseName = fileBaseName(file.path);
    char destPath[160];
    FontInstaller::buildFontPath(str(family.dirName), baseName, destPath, sizeof(destPath));
    // Every face is staged and verified before anything is published, so an
    // update that dies halfway cannot leave the family mixed between old and
    // new bytes — and cannot destroy a working install to get there.
    char stagePath[kStagePathSize];
    snprintf(stagePath, sizeof(stagePath), "%s%s", destPath, kStageSuffix);
    downloadUrl_.assign(baseUrl_).append(str(file.path));

    const auto result = HttpDownloader::downloadToFile(
        downloadUrl_, stagePath,
        [this](size_t downloaded, size_t total) {
          fileProgress_ = downloaded;
          fileTotal_ = total;
          mappedInput.update(true);
          if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
              mappedInput.wasPressed(MappedInputManager::Button::Back)) {
            cancelRequested_ = true;
          }
          // Home remains available now; other configured actions are deferred to
          // the next main-loop pass by the transfer input pump.
          if (mappedInput.wasHomeGesture()) {
            cancelRequested_ = true;
            goHomeRequested_ = true;
          }
          requestUpdate(true);
        },
        // The catalog is served straight off the bucket origin with no redirect
        // chain, so an HTTPS->HTTP redirect can only ever be an attack on the
        // connection: refuse the downgrade rather than follow it. The CRC32 from
        // the TLS-fetched manifest stays the integrity anchor either way.
        &cancelRequested_, "", "", /*headers=*/{}, /*downgradeRedirectsToHttp=*/false);

    if (result == HttpDownloader::ABORTED) {
      discardStagedFiles(family);
      if (goHomeRequested_) {
        onGoHome();
        return;
      }
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
        rowsDirty_ = true;  // installed/hasUpdate just changed above
      }
      return;
    }

    if (result != HttpDownloader::OK) {
      LOG_ERR("TTFFONT", "Download failed: %s (%d)", baseName, result);
      discardStagedFiles(family);
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = std::string("Download failed: ") + baseName;
      return;
    }

    uint32_t actualCrc = 0;
    if (!computeFileCrc32(stagePath, actualCrc)) {
      LOG_ERR("TTFFONT", "Failed to open file for CRC check: %s", stagePath);
      discardStagedFiles(family);
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = std::string("Failed to compute checksum: ") + baseName;
      return;
    }
    if (actualCrc != file.crc32) {
      LOG_ERR("TTFFONT", "CRC32 mismatch for %s: got %08x expected %08x", baseName, actualCrc, file.crc32);
      discardStagedFiles(family);
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = std::string("Checksum mismatch: ") + baseName;
      return;
    }
    LOG_DBG("TTFFONT", "Downloaded %s (size=%u crc32=%08x)", baseName, file.size, actualCrc);

    // CRC only proves the bytes are the ones the catalog published; this proves
    // they are a font.
    if (!fontInstaller_.validateTtfFile(stagePath)) {
      LOG_ERR("TTFFONT", "Invalid sfnt file: %s", stagePath);
      discardStagedFiles(family);
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = std::string("Invalid font file: ") + baseName;
      return;
    }
    currentFileIndex_++;
  }

  // Publish: the whole family is verified, so the folder can now be swapped
  // over face by face. A failure here is the one case that can leave a mixed
  // family behind, so the folder is dropped rather than left half-updated.
  for (uint32_t i = 0; i < family.fileCount; i++) {
    const TtfManifestFile& file = files_[family.fileStart + i];
    const char* baseName = fileBaseName(file.path);
    char destPath[160];
    FontInstaller::buildFontPath(str(family.dirName), baseName, destPath, sizeof(destPath));
    char stagePath[kStagePathSize];
    snprintf(stagePath, sizeof(stagePath), "%s%s", destPath, kStageSuffix);
    if (Storage.replaceFile(stagePath, destPath)) continue;

    LOG_ERR("TTFFONT", "Failed to publish %s", baseName);
    fontInstaller_.deleteTtfFamily(str(family.dirName));
    family.installed = false;
    family.hasUpdate = false;
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = std::string("Failed to install: ") + baseName;
    return;
  }

  family.installed = true;
  family.hasUpdate = false;

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void TtfFontDownloadActivity::promptDeleteSelectedFamily() {
  const int pendingDeleteFamilyIndex = familyIndexFromList(nav.selected);
  if (pendingDeleteFamilyIndex < 0 || pendingDeleteFamilyIndex >= static_cast<int>(families_.size())) {
    return;
  }

  const auto& family = families_[pendingDeleteFamilyIndex];
  auto confirm = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE), str(family.name));
  if (!confirm) {
    LOG_ERR("TTFFONT", "OOM: delete confirmation");
    return;
  }
  startActivityForResult(std::move(confirm),
                         [this](const ActivityResult& result) { onDeleteConfirmationResult(result); });
}

void TtfFontDownloadActivity::onDeleteConfirmationResult(const ActivityResult& result) {
  if (result.isCancelled) {
    requestUpdate();
    return;
  }

  const int familyIndex = familyIndexFromList(nav.selected);
  if (familyIndex < 0) {
    requestUpdate();
    return;
  }
  auto& family = families_[familyIndex];

  // Deletes by folder slug, not by the catalog's display name: they differ
  // wherever the display name carries spaces.
  if (fontInstaller_.deleteTtfFamily(str(family.dirName)) != FontInstaller::Error::OK) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_FONT_DELETE_FAILED);
    // No download is in flight, so there is nothing for the retry affordance to
    // retry: without this it would restart an unrelated family.
    downloadingFamilyIndex_ = -1;
  } else {
    family.installed = false;
    family.hasUpdate = false;
    // Unlike the other family_ mutations, this one stays in FAMILY_LIST (no
    // state_ transition to hang the rebuild off), so it must set the flag
    // directly.
    rowsDirty_ = true;
  }

  requestUpdate();
}

bool TtfFontDownloadActivity::isSelectedFamilyDeletable() const {
  if (isDownloadAllRow(nav.selected) || isUpdateAllRow(nav.selected)) return false;
  if (nav.selected < specialRowCount() || nav.selected >= listItemCount()) return false;
  return families_[familyIndexFromList(nav.selected)].installed &&
         !families_[familyIndexFromList(nav.selected)].hasUpdate;
}

void TtfFontDownloadActivity::activateSelected() {
  if (isDownloadAllRow(nav.selected)) {
    currentFileIndex_ = 0;
    currentFileTotal_ = 0;
    for (const auto& family : families_) {
      if (!family.installed) currentFileTotal_ += family.fileCount;
    }
    downloadAll();
  } else if (isUpdateAllRow(nav.selected)) {
    currentFileIndex_ = 0;
    currentFileTotal_ = 0;
    for (const auto& family : families_) {
      if (family.hasUpdate) currentFileTotal_ += family.fileCount;
    }
    updateAll();
  } else {
    // The special rows disappear when a download starts, so a stale selection
    // can map past the family table.
    const int familyIndex = familyIndexFromList(nav.selected);
    if (familyIndex < 0 || familyIndex >= static_cast<int>(families_.size())) return;
    auto& family = families_[familyIndex];
    if (!family.installed || family.hasUpdate) {
      currentFileIndex_ = 0;
      currentFileTotal_ = family.fileCount;
      downloadFamily(family);
    } else {
      promptDeleteSelectedFamily();
      return;
    }
  }
  requestUpdateAndWait();
}

void TtfFontDownloadActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (state_ == FAMILY_LIST && families_.empty()) {
    screen.centeredText(tr(STR_NO_FONTS_AVAILABLE), screen.theme().bodyText);
    return;
  }

  if (rowsDirty_) {
    rebuildRowItems();
    rowsDirty_ = false;
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the status and the row edge
  syncListViewport(screen, props);
  screen.list(props);
}

void TtfFontDownloadActivity::rebuildRowItems() {
  switch (state_) {
    case FAMILY_LIST:
      rebuildFamilyRowItems();
      return;
    case WIFI_SELECTION:
    case LOADING_MANIFEST:
    case DOWNLOADING:
    case COMPLETE:
    case ERROR:
      rowLabels_.clear();
      rowItems_.clear();
      return;
  }
}

void TtfFontDownloadActivity::rebuildFamilyRowItems() {
  const int listSize = listItemCount();
  rowLabels_.assign(listSize, std::string());
  rowItems_.clear();
  rowItems_.reserve(listSize);
  for (int i = 0; i < listSize; i++) {
    fui::ListItem item;
    if (isDownloadAllRow(i)) {
      rowLabels_[i] = std::string(tr(STR_DOWNLOAD_ALL)) + " (" + formatSize(totalDownloadSize()) + ")";
      item.label = rowLabels_[i].c_str();
    } else if (isUpdateAllRow(i)) {
      rowLabels_[i] = std::string(tr(STR_UPDATE_ALL)) + " (" + formatSize(totalUpdateSize()) + ")";
      item.label = rowLabels_[i].c_str();
    } else {
      const auto& family = families_[familyIndexFromList(i)];
      item.label = str(family.name);
      // Subtitle is the catalog's own copy (description, else the styles the
      // family ships) — same contract as the .cpfont list's description.
      if (family.description != 0) {
        rowLabels_[i] = str(family.description);
      } else if (family.styleFlags != 0) {
        rowLabels_[i] = formatStyles(family.styleFlags);
      }
      if (!rowLabels_[i].empty()) item.subtitle = rowLabels_[i].c_str();
      if (family.hasUpdate) {
        item.value = tr(STR_UPDATE_AVAILABLE);
      } else if (family.installed) {
        item.value = tr(STR_INSTALLED);
        // Dimmed but still tappable (opens the delete prompt): visual-only
        // disabled state, the row stays enabled for hit registration.
        item.state = fui::StateDisabled;
      }
    }
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }
}

// --- Input handling ---

bool TtfFontDownloadActivity::handleCustomInput() {
  if (state_ == FAMILY_LIST) {
    // The base list protocol (Back/Confirm, touch routing, swipe scroll,
    // button navigation) handles the family list.
    return false;
  }

  if (state_ == LOADING_MANIFEST) {
    // Back is polled every pass here, never locked out (§5): the claim pumps
    // one suspension lever per pass and the loop returns control in between.
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
      // Levers stay as pulled; the claim ends with the activity (RAII member)
      // and the radio restore is deferred to the runtime re-arm (§3.3).
      finish();
      return true;
    }
    manifestPass();
    return true;
  }

  if (state_ == COMPLETE) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
        rowsDirty_ = true;  // the completed download changed installed/hasUpdate
      }
      requestUpdate();
    }
  } else if (state_ == ERROR) {
    int x = 0;
    int y = 0;
    const bool retryRequested =
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y);
    const bool canRetry = downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(families_.size());
    if (retryRequested && canRetry) {
      downloadFamily(families_[downloadingFamilyIndex_]);
      requestUpdateAndWait();
      return true;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) || retryRequested) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
        rowsDirty_ = true;  // the failed download reset installed/hasUpdate
      }
      requestUpdate();
    }
  }

  return true;
}

// --- Rendering ---

std::string TtfFontDownloadActivity::formatSize(size_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%zu B", bytes);
  }
  return buf;
}

std::string TtfFontDownloadActivity::formatStyles(uint8_t styleFlags) {
  std::string out;
  const auto append = [&out](const char* name) {
    if (!out.empty()) out += ", ";
    out += name;
  };
  if (styleFlags & STYLE_REGULAR) append("Regular");
  if (styleFlags & STYLE_BOLD) append("Bold");
  if (styleFlags & STYLE_ITALIC) append("Italic");
  if (styleFlags & STYLE_BOLD_ITALIC) append("Bold Italic");
  return out;
}

void TtfFontDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_BROWSER), nullptr);

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_MANIFEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
  } else if (state_ == FAMILY_LIST) {
    renderUi();

    const bool hasVisibleFamilies = !families_.empty();
    const char* confirmLabel = !hasVisibleFamilies            ? ""
                               : isSelectedFamilyDeletable()  ? tr(STR_DELETE)
                               : isUpdateAllRow(nav.selected) ? tr(STR_UPDATE)
                                                              : tr(STR_DOWNLOAD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, hasVisibleFamilies ? tr(STR_DIR_UP) : "",
                                              hasVisibleFamilies ? tr(STR_DIR_DOWN) : "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == DOWNLOADING) {
    const auto& family = families_[downloadingFamilyIndex_];

    std::string statusText = std::string(tr(STR_DOWNLOADING)) + " " + str(family.name) + " (" +
                             std::to_string(currentFileIndex_ + 1) + "/" + std::to_string(currentFileTotal_) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, statusText.c_str());

    float progress = 0;
    if (fileTotal_ > 0) {
      progress = static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_);
    }

    const int barY = centerY + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(progress * 100), 100);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_FONT_INSTALLED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_FONT_INSTALL_FAILED), true,
                              EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

#endif  // CROSSPOINT_TTF_READER