#pragma once

#include <BookFont.h>
#include <FreeInkBook.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Memory.h>
#include <render/TtfFont.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
#include <FtFont.h>
#endif

// Umbrella gate for the TTF-backed UI fallback (TASK 4): needs the native-TTF
// reader AND the FreeType backend (1bpp mono target + hasGlyph coverage).
// HOST_TEST counts as TTF-enabled so the adapter is host-testable (device
// builds only reach it under CROSSPOINT_TTF_READER).
#if (defined(CROSSPOINT_TTF_READER) || defined(HOST_TEST)) && defined(CROSSPOINT_FONT_BACKEND_FT) && \
    CROSSPOINT_FONT_BACKEND_FT
#define CROSSPOINT_TTF_UI_FALLBACK 1
#else
#define CROSSPOINT_TTF_UI_FALLBACK 0
#endif

namespace freeink {
namespace book {

// Active native-TTF backend face (design D2): FreeType under the FT backend
// flag, stb_truetype otherwise. Both satisfy the RasterFont contract the
// FontChain consumes; the stb path stays compilable for rollback.
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
using NativeFace = freeink::font::FtFont;
#else
using NativeFace = TtfFont;
#endif

// ── public value types (discovery + settings) ────────────────────────────────

struct FontFaceInfo {
  // Full SD path including the root. Matches SdCardCacheStorage::kDirMax so
  // long vendor names ("Atkinson Hyperlegible Next/...-Regular.otf") fit.
  static constexpr size_t kFileCap = 160;

  char name[48] = {};        // family display name (manifest or filename stem)
  char file[kFileCap] = {};  // full path under the font root
  uint8_t styleFlags = 0;    // BookFont::StyleFlags this file provides
  // TrueType collection face index (§14.4.2): resolved by refineStyles'
  // inspect scan (first face with a Unicode cmap); 0 for plain .ttf/.otf.
  uint8_t faceIndex = 0;
  uint32_t fileSize = 0;
  uint32_t mtime = 0;  // for fingerprinting
};

struct FamilyInfo {
  char name[48] = {};
  uint8_t faceCount = 0;  // up to 4: REGULAR/BOLD/ITALIC/BOLD_ITALIC
  FontFaceInfo faces[4] = {};
  bool isBuiltinFallback = false;  // the BitmapBookFont chain
};

// The manifest lives in the loader's BSS. Keep the enlarged path storage
// bounded: 32 rows must remain a modest DRAM allocation (~30KB), not a
// runaway buffer.
static_assert(sizeof(FamilyInfo) <= 1024, "FamilyInfo manifest row is too large");

class BookFontLoader {
 public:
  static constexpr uint8_t kMaxDiscoveredFamilies = 32;

  BookFontLoader();
  ~BookFontLoader();

  // Scan /fonts/ for *.ttf/*.otf and build the family manifest.
  void begin();

  // (Re)load the active family if settings changed or registry dirty.
  // MUST be called before getReaderFont() or layoutGenerationHash().
  void ensureLoaded();

  // The live reader chain (never null — falls back to the builtin
  // BitmapBookFont chain when no TTF faces are loaded).
  FontChain* getReaderFont();

  // Content-based fingerprint: FNV-1a over loaded font bytes xor styleCoverage.
  uint32_t fontFingerprint() const;

  // Phase 3 family selection (design §3.6/§14.2): the reader and the settings
  // preview call this from SETTINGS before getReaderFont(). An empty name is
  // an explicit "built-in fallback" selection; a never-selected loader keeps
  // the legacy families_[0] default (debug rig).
  void selectFamily(const char* name);
  // Case-insensitive manifest lookup (§14.4 display name); nullptr when absent.
  const FamilyInfo* findFamily(const char* name) const;
  // Static picker gates: PSRAM present (any face size — oversized faces
  // stream from SD, §14.5) AND every face within the absolute stream cap.
  // Load failures (corrupt fonts) are runtime — they degrade to the fallback
  // chain instead of greying the row.
  static bool isFamilyAvailable(const FamilyInfo& fam);
  // Per-face PSRAM residency guard (CWE-400): faces up to this size load
  // fully resident in PSRAM; larger faces STREAM from SD (§14.5) instead of
  // being skipped.
  static constexpr uint32_t kMaxFaceBytes = 2u * 1024u * 1024u;
  // Absolute cap for streamed faces — SD fonts beyond this are rejected
  // outright (a runaway file must not pin an open handle forever).
  static constexpr uint32_t kMaxStreamFaceBytes = 24u * 1024u * 1024u;
  // Streamed faces cache this much of the file head in PSRAM: an sfnt's
  // per-glyph-fault tables (cmap/loca/hmtx) sit before the multi-MB glyf
  // table, so serving the first MB from RAM collapses each glyph fault's
  // 4-6 scattered SD seeks into one glyf read.
  static constexpr uint32_t kStreamPrefixBytes = 1024u * 1024u;

#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  // Initial pixel size for FreeType faces. Per-run sizes (ruby, preview) adapt
  // at runtime through FtFont::ensureSize — the face is size-agnostic. Mirrors
  // CrossPointSettings::DEFAULT_TTF_FONT_POINT_SIZE without coupling the
  // loader to the settings header.
  static constexpr uint16_t kInitSizePx = 14;

  // Requested reader render options: Light hinting (P2 re-enable). Light
  // grid-fits via the auto-hinter (compiled in per-platform with
  // FREEINK_FONT_ENABLE_AUTOHINT) instead of the TT bytecode interpreter.
  // For CFF outlines Light still routes through the Adobe interpreter, whose
  // stack-resident footprint is the one real risk — gated at runtime by
  // probeHintStackSafety(), which degrades an individual face to unhinted
  // when its measured render depth would not fit the smallest consumer
  // stack (the 32KB FibpPrefetchWorker). Deliberately NOT a user setting:
  // the mode is render-affecting and must stay in lockstep with the FIBP
  // cache identity (renderOptionsFingerprintTag). The requested mode is not a
  // promise that hinting is in effect: a fresh load starts here and only a
  // settled probe verdict (ensureHintProbeSettled) keeps it.
  static constexpr freeink::font::FtFont::RenderOptions kRenderOptions{freeink::font::FtFont::HintingMode::Light};
  // Crisp base for the file-scope effective-options init (C++20 designated
  // init on the aggregate): matches the TEXT_RENDER_CRISP settings default
  // so pre-sync readers agree with the persisted state.
  static constexpr freeink::font::FtFont::RenderOptions kCrispRenderOptions{
      .hinting = freeink::font::FtFont::HintingMode::Light,
      .interpreterVersion = 40,
      .monochrome = true,
  };

  // Render options for the active CrossPointSettings::textRenderMode:
  // Smooth keeps the dual-plane AA coverage; Crisp sets the FT monochrome
  // target (hinted 1-bit glyphs — no coverage, no tone quantization, no
  // plane walk). The mode is applied to live faces via applyRenderMode()
  // (same flush point as the stack-probe degrade) and folded into the
  // fingerprint tag, so a switch invalidates FIBP identity without a reload.
  // Synced from the persisted setting by the reader and the text settings
  // activity (applyRenderMode takes no SETTINGS dependency — host-testable).
  [[nodiscard]] freeink::font::FtFont::RenderOptions currentRenderOptions() const {
    auto options = kRenderOptions;
    options.monochrome = requestedMonochrome_;
    return options;
  }

  // Re-derive every slot's effective options from the requested mode and
  // push them through setRenderOptions() — the P1 glyph-cache flush point —
  // so no stale-quantized bitmap survives the change. No-op on the stb
  // backend. Call with the new mode after SETTINGS.textRenderMode changes;
  // also syncs the fingerprint so the next layoutGenerationHash sees the tag.
  void applyRenderMode(bool crispMode);

  // Per-slot effective options: kRenderOptions unless the P2 stack probe
  // degraded that face to unhinted. Shared with the FIBP prefetch worker so
  // both fingerprint sites fold the SAME effective modes into the identity.
  // Only meaningful for the FT backend; the stb backend has no options.
  static const freeink::font::FtFont::RenderOptions& effectiveRenderOptions(uint8_t faceSlot);

  // The ACTIVE text raster mode after any degrade (slot 0's effective set;
  // support is build-wide so slots never diverge). Paint-path decisions
  // (gray planes vs 1bpp) must follow this, NOT the raw setting — a build
  // without the mono module degrades Crisp to Smooth, and painting Crisp
  // frame formats against AA faces blanks the page.
  static bool effectiveMonochrome();

  // Fingerprint tag folding the ACTIVE hinting mode into the font
  // fingerprint. Runtime (not constexpr) since the effective per-face mode
  // is a probe outcome: a degrade changes advances and layout, so the tag
  // must change with it and FIBP caches regenerate. RASTER MODE is
  // deliberately EXCLUDED: advances are identical across Smooth/Crisp, so
  // the tag must stay stable across a mode flip (or a firmware update
  // flipping the default) — bitmap identity is governed by the P1 glyph
  // cache's setRenderOptions() flush. Mixed into BOTH fingerprint sites —
  // computeFingerprint() and the FibpPrefetchWorker parity hash — and must
  // be extended whenever kRenderOptions gains a knob that alters ADVANCES.
  static uint32_t renderOptionsFingerprintTag();
#endif

  const FamilyInfo* families() const { return families_.data(); }
  uint8_t familyCount() const { return familyCount_; }

  void markDirty();

  // P2 stack gate, call-site half (no-op on the stb backend, which has no
  // render options to gate). Measures the loaded faces' hinted render depth
  // on a dedicated one-shot task (see probeHintStackSafety). ensureLoaded()
  // settles the probe itself at its tail (the load's shallow-heap window);
  // this call re-runs the settle as the RETRY BACKSTOP: an unmeasurable
  // attempt arms exactly one re-probe, consumed here on the reader's first
  // TTF render — still before the generation hash, the FIBP worker start,
  // and any hinted rasterization, so no consumer runs a hinted glyph the
  // verdict has not cleared.
  void ensureHintProbeSettled();

  // Scrub arenas + unload file bytes when leaving the reader with low heap.
  void releaseResidentCaches();

  // True when a reload is pending (begin()/selectFamily()/markDirty since
  // the last ensureLoaded). Lets the TTF UI fallback release its borrowed
  // faces BEFORE ensureLoaded() frees the bytes they borrow.
  bool isDirty() const { return dirty_.load(std::memory_order_relaxed); }
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  // Borrowed view of the loaded family's RESIDENT face bytes, by style slot
  // (0=regular, 1=bold, 2=italic, 3=bold-italic — the §4.1.1 role order).
  // Null/zero when the slot has no resident bytes: not loaded, or a STREAMED
  // face (streaming keeps no resident bytes, so it cannot serve a UI
  // fallback face — the UI stays bitmap for streamed families).
  // The bytes are owned by the loader and valid until the next ensureLoaded
  // reload or releaseResidentCaches — consumers must re-validate via
  // fontFingerprint() and drop borrowed views when it changes.
  const void* slotFaceBytes(uint8_t slot) const;
  uint32_t slotFaceByteSize(uint8_t slot) const;
  // The collection face index actually used for the slot's face (0 for plain
  // .ttf/.otf). Lets the TtfUiFont adapters init faces against the SAME
  // embedded face the reader selected — index 0 of a TTC can carry different
  // coverage than the discovered Unicode-cmap face.
  uint8_t slotFaceIndex(uint8_t slot) const;
#endif

  // Public fingerprint helper — content-based, never path/mtime.
  uint32_t computeFingerprint() const;

  // Fingerprint with the SD-backed per-face hash cache (P3.1): chained
  // per-slot hashes under /.crosspoint/fonts/ keyed by face path hash, valid
  // only when {fileSize, mtime, incoming chain seed} all match. Pure-memory
  // fallback (computeFingerprint()) runs on any mismatch or absent cache —
  // content semantics are identical either way. Non-const: consults and
  // refreshes the cache.
  uint32_t computeFingerprintCached();

  // FNV-1a over font bytes with a chained seed. The prefetch worker hashes
  // the same face bytes in the same slot order to derive an identical
  // fingerprint (FibpPrefetchWorker).
  static uint32_t fontBytesHash(const uint8_t* data, size_t len, uint32_t seed);
  // sfnt table-directory sanity gate shared by tryLoadFace and the worker's
  // face builder: numTables != 0 and every table's offset/length in-bounds.
  // TrueType collections: faceIndex selects which embedded face's directory
  // is validated (container-absolute table offsets).
  static bool validateSfntBytes(const uint8_t* data, uint32_t size, int faceIndex = 0);

  // The path hash folded into the fingerprint's role-map tag (§14.4.1);
  // exposed so FibpPrefetchWorker can replicate computeFingerprint() exactly.
  static uint32_t facePathHash(const char* file);
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  // Absolute-offset HalFile read for FtFont ReadFn thunks (count 0 is a
  // seek probe). Shared by the loader's and the worker's streamed sources.
  static unsigned long halFileRead(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count);
  // Streamed-slot fingerprint identity: FNV-1a over the file's first
  // kFingerprintHeadBytes read in SD chunks (no full residency). Shared with
  // the prefetch worker so both sites derive identical streamed-slot tags.
  static uint32_t streamHeadHash(const char* file, uint32_t fileSize);
#endif

  // Appends the four Atkinson faces to `chain` as its non-selectable tail:
  // a selected TTF family that lacks a glyph or style degrades to the
  // fallback face instead of a missing glyph (§14.5 chain-tail semantics).
  // Public so the prefetch worker can build an identical tail — the chain's
  // style coverage (and with it the fingerprint / FIBP generation) must
  // match the reader's chain byte for byte.
  static void appendFallbackTail(FontChain& chain);

#if defined(HOST_TEST)
  // Host-test seams: seed the manifest deterministically and read the budget.
  // setFamilyCount drives ensureLoaded()'s familyCount_ > 0 gate so tests can
  // exercise the load/reject paths; editFamily alone never touches the count.
  FamilyInfo& editFamily(uint8_t idx) { return families_[idx]; }
  void setFamilyCountForTest(uint8_t n) { familyCount_ = n; }
  uint32_t dramBudgetForTest() const { return remainingBudget_; }
  // True when the streamed-slot handle is currently open (§14.5 lifecycle
  // assertion: never-opened on a cold loader, open after a streamed load).
  bool streamSourceOpenForTest(uint8_t slot) const {
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
    return slot < 4 && streamSources_[slot].file.isOpen();
#else
    (void)slot;
    return false;
#endif
  }
  // Appends the Atkinson tail to the live chain without a loadable TTF face,
  // so host tests can exercise the tail-append path.
  void forceFallbackTailForTest();
  const FontChain& chainForTest() const { return chain_; }
  // Drive the §14.4 two-root discovery walk against the stub storage.
  static void scanFontsForTest(const char* rootPath, FamilyInfo* families, uint8_t& familyCount) {
    scanFonts(rootPath, families, familyCount);
  }
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  // Drives refineStyles against the stub storage (FT backend only — the
  // metadata pass needs FtFont::inspectStream; the stb backend keeps the
  // filename-derived roles).
  static void refineStylesForTest(FamilyInfo* families, uint8_t familyCount) { refineStyles(families, familyCount); }
#endif
  // Force the P2 stack-probe outcome for a slot as if the probe had
  // degraded it: flips the effective options to unhinted (the device probe
  // itself is FreeRTOS-only and absent on host). Non-const on purpose.
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  void degradeHintForTest(uint8_t faceSlot) { degradeHint(faceSlot, "test seam"); }
  // Record a verdict as the shallow probe would (the FreeRTOS measurement
  // itself is firmware-only), so the reuse and fingerprint re-derivation
  // paths stay exercisable off-device: the degraded slots are applied, then
  // the verdict is cached under loadKey. A loadKey of 0 forces a mismatch on
  // the next load and a fresh probe.
  void recordHintVerdictForTest(uint32_t loadKey, uint8_t degradedMask) {
    for (uint8_t slot = 0; slot < 4; ++slot) {
      if ((degradedMask & static_cast<uint8_t>(1u << slot)) != 0) degradeHint(slot, "test seam");
    }
    recordHintVerdict(loadKey, degradedMask);
  }
  // Inject the body point size the probe would have measured at (host builds
  // compile the FreeRTOS probe out and have no SETTINGS, so the size axis of
  // the verdict key needs a seam). 0 = "unset", the host default.
  static void setHintProbeSizeForTest(uint16_t pointSize);
  // Force the unmeasurable path (device: probe task spawn failure / settle
  // timeout) for the retry state machine's host tests. Sticky file-scope
  // state: hold it with the test file's UnmeasurableOverrideGuard so any
  // exit — including GTEST_SKIP — resets it.
  static void setHintProbeUnmeasurableForTest(bool unmeasurable);
  // Observable probe state for the retry tests (see hintProbeRetryArmed_).
  bool hintProbePendingForTest() const { return hintProbePending_; }
  bool hintProbeRetryArmedForTest() const { return hintProbeRetryArmed_; }
  // Key the cached verdict is stored under (requested-options fingerprint
  // after a successful retry — see hintProbeRetryKey_).
  uint32_t hintVerdictKeyForTest() const { return hintVerdictKey_; }
  // Hinting mode slot 0 carried when the probe last ran, and how many retry
  // pre-probe restores had completed by then (host stub records both). The
  // restore count pins the ORDER: the failed attempt's fail-closed degrade
  // must be rolled back BEFORE the retry probe, so the retry measures the
  // hinted depth the gate exists for. On the host FT variant slot 0 sits at
  // None regardless (the funnel drops autohint), so the mode capture is
  // device-parity documentation; the restore count is the live assertion.
  // -1 = the stub has not run yet.
  static int hintProbeObservedHintingForTest();
  static int hintProbeRestoresAtProbeForTest();
  // Restore every slot to the requested mode — degradeHintForTest is sticky
  // (file-scope static state outlives the test) and the tag participates in
  // other tests' fingerprints.
  void resetHintStateForTest() { resetHintState(); }
#endif
#endif

 private:
  // P2 stack gate: measure each loaded face's whole hinted render depth on a
  // dedicated one-shot task (32KB stack — the FibpPrefetchWorker size — so
  // attribution is independent of the calling task's lifetime high-water,
  // which any deep settings frame contaminates for the rest of the boot).
  // Slots exceeding kHintProbeStackBudgetBytes are collected into
  // `degradedMask` (bit i = slot i); `unmeasurable` reports that NO
  // measurement could be made (task spawn/settle failure), which degrades
  // fail-closed and must not be cached. No-op off-device.
  void probeHintStackSafety(uint8_t& degradedMask, bool& unmeasurable);
  // Flip a slot's effective options to unhinted through setRenderOptions()
  // (the P1 glyph-cache flush point) and log why.
  void degradeHint(uint8_t faceSlot, const char* reason);
  // Fail-closed half of the gate: degrade every loaded slot when no
  // measurement could be made (probe task spawn/settle failure). Returns the
  // degrade mask so the caller can fold it into the verdict mask and
  // re-derive the fingerprint.
  uint8_t degradeAllLoadedSlots(const char* reason);
  // Production reset shared by ensureLoaded's clear loop and the host test
  // seam: every slot back to the requested mode. Deliberately NOT paired
  // with a verdict: an unprobed load simply serves the requested mode, and
  // the pending probe (or a cached verdict for the same face set) settles
  // it. No live-face propagation here by construction: ensureLoaded deletes
  // the faces before resetting, and the test instance has none.
  void resetHintState();
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  // THE degrade funnel: apply `requested` to the slot's face, falling back
  // to the nearest supported set (drop monochrome → drop hinting) when
  // refused, and record the effective set. Called by tryLoadFace,
  // applyRenderMode, and degradeHint — no call site may setRenderOptions
  // directly and continue on false.
  void applySlotRenderOptions(NativeFace* face, uint8_t faceSlot, const freeink::font::FtFont::RenderOptions& requested,
                              const char* label);
  // Cache the measured verdict for `loadKey` (the content fingerprint the
  // probe ran under) and re-derive the fingerprint when the verdict moved a
  // slot, so layout identity always describes what renders.
  void recordHintVerdict(uint32_t loadKey, uint8_t degradedMask);
  // Re-apply the cached verdict when it was measured for exactly this face
  // set AND this body point size; false means the load owes a fresh probe. A
  // verdict can never outlive the face it describes: the key IS the content
  // identity (bytes + path hash + collection face index + size + mtime), so a
  // replaced or re-pointed face misses it. The size joins it because the
  // probe's depth is a function of the glyph size it rasterized, so a size
  // change must re-probe rather than inherit an old-size verdict.
  bool reuseHintVerdicts(uint32_t loadKey);

  // P2 probe state. The verdicts survive a reload (that is the reuse cache);
  // releaseResidentCaches() drops them, and every fresh load re-arms
  // hintProbePending_.
  uint32_t hintVerdictKey_ = 0;     // load key the cached mask was measured under; 0 = none
  uint8_t hintVerdictMask_ = 0;     // bit i: slot i measured too deep for the consumer budget
  uint16_t hintVerdictSizePt_ = 0;  // body point size the cached mask was measured at
  bool hintProbePending_ = true;    // the current face set has no settled verdict
  // One re-probe armed after an unmeasurable attempt (probe task spawn
  // failure / settle timeout), consumed by the next ensureHintProbeSettled()
  // call whatever its outcome: a failed retry never re-arms within the load
  // cycle, and a fresh load cycle re-arms the attempt budget via
  // resetHintState(). Bounds the worst case to probe + one retry.
  bool hintProbeRetryArmed_ = false;
  // Load key (requested-options fingerprint) observed at the attempt that
  // armed the retry. The retry pass re-enters with fingerprint_ under the
  // fail-closed degrade tag, so the successful retry's verdict must be cached
  // under this stored key — the key an unchanged later load computes.
  uint32_t hintProbeRetryKey_ = 0;
#endif

  std::array<FamilyInfo, kMaxDiscoveredFamilies> families_{};
  uint8_t familyCount_ = 0;
  // Selection state (see selectFamily()).
  char selectedFamily_[48] = {};
  bool familySelected_ = false;

  NativeFace* faces_[4] = {};
  FontChain chain_;
  uint32_t fingerprint_ = 0;
  // Style coverage of the FAMILY faces, captured before the fallback tail
  // joins the chain and folded by both fingerprint functions. The tail is a
  // constant that both parity sites register (the loader here, the prefetch
  // worker in FibpPrefetchWorker::buildFaces), so hashing chain_.styleCoverage()
  // directly would make the fingerprint depend on WHERE it is evaluated — a
  // post-tail identity diverges from the worker's parity hash for any family
  // that does not already cover all four styles.
  uint8_t fingerprintCoverage_ = 0;
  bool loaded_ = false;  // a load attempt completed (fingerprint 0 is valid)
  // Requested render mode (task6): Crisp ⇒ FT monochrome target. Synced from
  // the persisted setting by the reader/settings via applyRenderMode(); the
  // default (Crisp) matches the TEXT_RENDER_CRISP settings default.
  bool requestedMonochrome_ = true;
  std::atomic<bool> dirty_{false};

  // Two-tier font-byte storage: each face has its own RAII owner.
  // PSRAM: PoolBytes (poolFree on reset). DRAM: unique_ptr<uint8_t[]> (delete[]).
  PoolBytes fontPsramBytes_[4] = {};
  std::unique_ptr<uint8_t[]> fontDramBytes_[4] = {};
  void* fontBytes_[4] = {};         // non-owning raw pointer for fingerprinting
  uint8_t faceBytesOwner_[4] = {};  // 0=none, 1=PSRAM, 2=DRAM
  uint32_t fontFileSizes_[4] = {};
  // Fingerprint-cache identity per slot, captured in tryLoadFace(): the
  // face's path hash (cache key) and mtime (rehash trigger beside size).
  uint32_t facePathHash_[4] = {};
  uint32_t faceMtime_[4] = {};
  // TrueType collection face index actually loaded per slot — fingerprint
  // identity (§14.4.2): two faces from one container share the file bytes,
  // so the chosen face index must perturb the hash too.
  uint8_t faceIndexUsed_[4] = {};
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  // §14.5 streamed face source per slot: the open HalFile is borrowed by the
  // FT face for the face's lifetime (released in releaseResidentCaches()/
  // ensureLoaded's clear loop); the PSRAM prefix caches the file head.
  struct StreamSource {
    HalFile file;
    PoolBytes prefix;
    uint32_t prefixLen = 0;
  };
  StreamSource streamSources_[4];
  // Fingerprint identity for streamed slots (no resident bytes to walk):
  // FNV-1a over the file's first kFingerprintHeadBytes, chunked off SD.
  uint32_t streamHeadHash_[4] = {};
  // FtFont::ReadFn over a StreamSource (prefix cache + SD tail). ctx is the
  // slot's StreamSource (stable address, loader-lifetime).
  static unsigned long streamReadThunk(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count);
  // §14.5 load path for faces beyond kMaxFaceBytes: open HalFile (kept open
  // for the face's lifetime), PSRAM head-prefix cache, initStream, GPOS off.
  bool tryLoadStreamedFace(uint8_t faceIdx, const FontFaceInfo& fi, FontChain& chain);
  // Release a slot's streamed source (close-before-reopen discipline).
  void releaseStreamSource(uint8_t faceIdx);
#endif

  // Per-face glyph arenas — each has its own persistent backing buffer.
  // Size must fit TtfFont's profile-scaled slot tables before any glyph
  // bitmap: SMALL 4.6KB / STANDARD 9.2KB / LARGE 36.9KB (see TtfFont.h).
  // Backing storage is sized by profile too, so C3 (SMALL) does not burn
  // static BSS on STANDARD/LARGE-sized tables it cannot use. Compare the
  // resolved profile VALUES (BookProfile.h defines inactive selectors to 0,
  // so defined() alone would always be true).
  Arena arenas_[4];

  // PSRAM-only directive (zero native-TTF DRAM BSS): arena backing is
  // allocated lazily from the pool per face slot (poolMakeBytes), never
  // statically. poolMakeBytes lands in PSRAM on PSRAM boards and falls to
  // DRAM malloc otherwise — per design §14.1 no TTF face is ever loaded on
  // PSRAM-less boards, so it never allocates there in practice.
  PoolBytes glyphBacking_[4] = {};

  // Native-TTF PSRAM budget (all well within the 8MB pool):
  //   glyph arenas: 4 x kGlyphArenaBytes (12/32/48KB by profile) = 48–192KB
  //   builtin BitmapBookFont fallback: 4 x sizeof(BitmapBookFont) ≈ 16.4KB
  //   font file bytes: up to kMaxPsramFontBytes (2MB) per face
#include "BookProfile.h"
#if FREEINK_BOOK_PROFILE == FREEINK_BOOK_PROFILE_SMALL
  static constexpr size_t kGlyphArenaBytes = 12 * 1024;
#elif FREEINK_BOOK_PROFILE == FREEINK_BOOK_PROFILE_LARGE
  static constexpr size_t kGlyphArenaBytes = 48 * 1024;
#else
  static constexpr size_t kGlyphArenaBytes = 32 * 1024;
#endif

  // Aggregate DRAM budget: derived from free heap with floor guards.
  uint32_t remainingBudget_ = 0;

  // Two-level family walk (design §14.4): one subfolder per family under
  // `rootPath`, hidden root scanned first so it wins on name collisions.
  // Appends into the caller's manifest (capped at kMaxDiscoveredFamilies).
  static void scanFonts(const char* rootPath, FamilyInfo* families, uint8_t& familyCount);
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  // Face-metadata style resolution (design §14.4.1, ported from upstream
  // #3646): reads each face's real OS/2 weight + italic flag via
  // FtFont::inspectStream, then re-assigns the four style roles
  // deterministically (upright nearest 400 = regular, nearest 700 = bold,
  // same for the italic pair; all-italic promotion; ties break to lower
  // weight then lexicographic path — never SD enumeration order). A face
  // that cannot be inspected keeps its filename-inferred estimate as the
  // pick input (the filename heuristics remain the fallback).
  static void refineStyles(FamilyInfo* families, uint8_t familyCount);
#endif
  static FontChain* builtinFallback();
  // One of the four baked Atkinson fallback faces (§14.3), owned by the
  // builtin singleton; appended to the active chain as its tail.
  static RenderFont* builtinFace(uint8_t idx);

  // Load a single face into the live chain (member so it can access private
  // state: faces_, arenas_, fontBytes_, fontPsramBytes_, fontDramBytes_).
  bool tryLoadFace(uint8_t faceIdx, const FontFaceInfo& fi, FontChain& chain);

  // Initialize the aggregate DRAM budget from current free heap.
  void initBudget();

  // Max faces to load from one family (4: regular/bold/italic/bold-italic).
  static constexpr uint8_t kMaxFacesPerFamily = 4;
};

// Defined in main.cpp beside sdFontSystem (design §3.2).
extern BookFontLoader fontLoader;

}  // namespace book
}  // namespace freeink
