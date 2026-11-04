#pragma once

// Direct TTF downloader for the native-TTF (PSRAM) device class. Sibling of
// FontDownloadActivity, not a subclass: the two share the Wi-Fi → manifest →
// family list → download flow but nothing else — the catalog ships raw sfnt
// files with a path-shaped layout, where the .cpfont flow ships opaque
// binaries keyed by family + point size. Only one of the two is ever compiled:
// FontDownloadActivity for the C3, this one where CROSSPOINT_TTF_READER is set.

#if defined(CROSSPOINT_TTF_READER)

#include <memory>
#include <string>
#include <vector>

#include "FontInstaller.h"
#include "SdCardFont.h"
#include "activities/UiListActivity.h"
#include "util/MemoryClaimGateDevice.h"

// JSON schema version of the TTF catalog manifest. Kept equal to the .cpfont
// manifest's FONTS_MANIFEST_VERSION: both describe the same document shape for
// different payloads, and the tooling bumps them together. Bump this whenever
// the catalog schema changes.
#define FONTS_MANIFEST_VERSION 1

#ifndef TTF_FONTS_MANIFEST_URL
// Catalog manifest served from the public R2 bucket that also hosts the raw
// .ttf files. The .cpfont catalog keeps its own FONT_MANIFEST_URL (GitHub
// release assets), which is why the constant is not shared.
#define TTF_FONTS_MANIFEST_URL "https://pub-794e4fbb87c8444b952f6a2dd026c7b1.r2.dev/manifest/fonts.json"
#endif

class TtfFontDownloadActivity final : public UiListActivity {
 public:
  explicit TtfFontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state_ == LOADING_MANIFEST || state_ == DOWNLOADING ||
           // The download is synchronous and blocks the main loop until it
           // completes, so activityManager.preventAutoSleep() is never polled
           // during downloading.
           state_ == COMPLETE || state_ == ERROR;
  }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_MANIFEST,
    FAMILY_LIST,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  // The LOADING_MANIFEST stage within the manifest pass (docs/design/
  // 2026-11-03-memory-claims.md §3.2): the manifest is a structuring change,
  // not a free reuse — the claim's plan-act passes run BEFORE the fetch, and
  // both run on activity passes that return to the loop (manifestPass(),
  // driven from handleCustomInput), never inside the onWifiSelectionComplete
  // callback, which would busy-wait within one pass with Back locked out.
  enum class ManifestStage : uint8_t {
    Claim,  // one suspension lever per pass; Proceed hands over to Fetch
    Fetch,  // levers applied: the existing fetch/parse path runs this pass
  };
  ManifestStage manifestStage_ = ManifestStage::Claim;

  // Bit per style in a manifest file's "style" token.
  enum StyleFlag : uint8_t {
    STYLE_REGULAR = 1 << 0,
    STYLE_BOLD = 1 << 1,
    STYLE_ITALIC = 1 << 2,
    STYLE_BOLD_ITALIC = 1 << 3,
  };

  // Byte offset into stringArena_; 0 is the empty string.
  using StrRef = uint32_t;

  struct TtfManifestFile {
    // Manifest-relative path ("<slug>/<file>.ttf"): the base-URL suffix to
    // fetch from, and the only string the device derives the SD layout from.
    // A leading directory beyond the slug is tolerated on parse (it never
    // reaches the card) but the published catalog does not emit one. The
    // manifest's own "name" field is deliberately not stored — it is not
    // reliably the object's key
    // ("PT Serif" publishes "PT Serif Regular.ttf" as "PT_Serif_Regular.ttf"),
    // and carrying both invites a write under the wrong name.
    StrRef path = 0;
    uint32_t size = 0;
    uint32_t crc32 = 0;
    uint8_t styleFlags = 0;
  };

  struct TtfManifestFamily {
    StrRef name = 0;
    StrRef description = 0;
    // Last path component of every file; the folder on SD. Kept separate from
    // name because the catalog's display names carry spaces ("Atkinson Hyper
    // Legible") while its slugs do not ("AtkinsonHyperlegible").
    StrRef dirName = 0;
    // Range into files_, which holds every family's files back to back.
    uint32_t fileStart = 0;
    uint32_t fileCount = 0;
    uint32_t totalSize = 0;
    // Union of its files' style bits, for the row subtitle.
    uint8_t styleFlags = 0;
    bool installed = false;
    bool hasUpdate = false;
  };

  State state_ = WIFI_SELECTION;
  FontInstaller fontInstaller_;
  // The memory claim spans the whole activity (§3.3 — never the manifest
  // stage), so every early return — fetch failure, per-transfer TLS refusal,
  // SD-space refusal, Back, goHome — ends it exactly once via RAII. claim.end
  // records state only; the radio restore is deferred to the runtime tick's
  // scene-conditioned re-arm (MemoryClaimGateDevice.h).
  memclaim::DeviceClaim claim_;

  // Manifest data
  std::string baseUrl_;
  // Reused for every file of every family: downloadToFile takes a std::string,
  // so a char buffer would just build a temporary per call.
  std::string downloadUrl_;
  // Manifest strings, null-terminated and packed back to back.
  std::unique_ptr<char[]> stringArena_;
  uint32_t arenaUsed_ = 0;
  uint32_t arenaCapacity_ = 0;
  std::vector<TtfManifestFamily> families_;
  // Every family's files back to back; sized once from the manifest, so it is
  // allocated nothrow like the arena rather than through vector::reserve.
  std::unique_ptr<TtfManifestFile[]> files_;
  uint32_t fileEntryCount_ = 0;

  // Download progress
  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int downloadingFamilyIndex_ = -1;  // -1 = no download in flight, so ERROR offers no retry
  std::string errorMessage_;
  bool cancelRequested_ = false;
  // Set when the cancel came from the home gesture (consumed by the download
  // callback's own input pump); exit to home after the abort unwinds.
  bool goHomeRequested_ = false;

  // Row cache. Rebuilt only when the visible list changes, never for cursor
  // movement or tap flash repaints.
  std::vector<std::string> rowLabels_;
  std::vector<freeink::ui::ListItem> rowItems_;
  bool rowsDirty_ = true;
  void rebuildRowItems();
  void rebuildFamilyRowItems();

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  freeink::ui::ListNav& activeNav() override;
  void onBackButton() override;
  // Non-list states (loading, downloading, complete, error) consume the loop
  // pass here; the family list uses the base list protocol.
  bool handleCustomInput() override;

  void activateSelected();

  // Maps a manifest "style" token to its StyleFlag bits. Unknown tokens yield
  // 0: a catalog that grows a style this firmware does not know still
  // installs, because BookFontLoader infers the style from the filename.
  static uint8_t parseStyleToken(const char* token);
  // Human-readable style list for a family's row subtitle, e.g.
  // "Regular, Bold, Italic".
  static std::string formatStyles(uint8_t styleFlags);
  // Validates a catalog path and yields the two pieces the device needs:
  //   - slug: its last directory component, which becomes the on-SD folder.
  //     BookFontLoader scans exactly one level below the fonts root, so the
  //     catalog's optional leading directory ("ebook_fonts_extra/...") must not
  //     be reproduced on the card.
  //   - baseName: the object's key on the bucket, which is also its name on SD.
  //     Points into `path`, which must outlive the call.
  // The whole path is rejected unless every component is a plain name — no
  // leading '/', no empty, "." or ".." segment, no backslash — so a manifest we
  // do not understand can never steer a write outside the family folder.
  static bool splitCatalogPath(const char* path, char* dirBuf, size_t dirBufSize, const char*& slug,
                               const char*& baseName);
  // Basename of an already-validated file path, without re-validating it.
  const char* fileBaseName(StrRef path) const;
  // Deletes a family's staged (<name>.part) files after a failed or aborted
  // download and re-reads the family's installed flags from the card. The
  // published files are never touched, so a failed update keeps whatever was
  // installed before.
  void discardStagedFiles(TtfManifestFamily& family);
  // True when the card reports enough room for `requiredBytes`. A card whose
  // capacity cannot be read is allowed through: the download itself will
  // surface the real failure.
  bool hasFreeSpaceFor(uint64_t requiredBytes) const;

  void onWifiSelectionComplete(bool success);
  void manifestPass();
  bool fetchAndParseManifest();
  // cppcheck-suppress arithOperationsOnVoidPointer // unique_ptr<char[]>::get() is char*, not void*
  const char* str(StrRef ref) const { return stringArena_ ? stringArena_.get() + ref : ""; }
  // Returns false if the string does not fit the arena reserved for the manifest.
  bool internString(const char* text, StrRef& outRef);
  void clearManifest();
  void downloadFamily(TtfManifestFamily& family);
  void downloadAll();
  void updateAll();
  static bool computeFileCrc32(const char* path, uint32_t& outCrc);
  // Reads a family's files from disk and sets installed/hasUpdate from what is
  // there: every file present at the manifest's size means installed, any file
  // present but short of that means an update is due.
  void refreshInstalledState(TtfManifestFamily& family);
  bool showDownloadAllRow() const;
  bool showUpdateAllRow() const;
  int specialRowCount() const;
  bool isDownloadAllRow(int index) const;
  bool isUpdateAllRow(int index) const;
  bool isSelectedFamilyDeletable() const;
  void promptDeleteSelectedFamily();
  void onDeleteConfirmationResult(const ActivityResult& result);
  int familyIndexFromList(int listIndex) const;
  int listItemCount() const;
  size_t totalDownloadSize() const;
  size_t totalUpdateSize() const;
  static std::string formatSize(size_t bytes);
};

#endif  // CROSSPOINT_TTF_READER