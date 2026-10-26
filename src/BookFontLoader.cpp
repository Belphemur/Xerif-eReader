// BookFontLoader.cpp — Phase 1a native TTF font-loader infrastructure.
//
// Scans /fonts/*.ttf|.otf on SD, builds the family manifest, and owns the
// live FontChain plus the builtin BitmapBookFont fallback.
//
// Two-tier allocation: PSRAM tier (S3) via poolMalloc/poolMakeBytes
// (lib/Memory/Memory.h — SPIRAM on BOARD_HAS_PSRAM, DRAM otherwise) and DRAM
// tier (C3 / Sticky, no PSRAM) via makeUniqueNoThrow<uint8_t[]>. Never bare
// new (AGENTS.md §9). kMaxDramFontBytes is DERIVED from measured free heap
// after all arenas are allocated (keeping the 32KB/16KB heap-gate floors) —
// not a hardcoded 256KB. Oversized files: LOG_ERR("BFNT", ...) and stay
// listed but greyed out.
//
// FontChain assembly (<=8 faces, styleCoverage()). fontFingerprint() = FNV-1a
// over the LOADED font bytes xor styleCoverage — content-based, never path/mtime.
//
// Builtin fallback: singleton FontChain over 4 BitmapBookFont instances
// placement-new'ed into PSRAM (each embeds coverage_[64*64]; formerly 16KB
// static BSS).
//
// sfnt VALIDATION BOUNDARY before TtfFont::init: table-directory bounds +
// numTables sanity; on failure LOG_ERR + skip the face. TtfFont::init only
// checks len<12 (TtfFont.cpp:36). Test corpus includes 2 malformed fonts.
//
// Face bytes: stb_truetype BORROWS the source bytes — they must stay resident
// for the face's lifetime (TtfFont.h: "data is borrowed and must outlive the
// font"). They live in fontPsramBytes_/fontDramBytes_ RAII owners, released
// only in ensureLoaded()/releaseResidentCaches() when the face is deleted.
// Glyph rasters live in the per-face arena (owned by BookFontLoader).
//
// Per AGENTS.md: makeUniqueNoThrow, no std::string in hot paths, tr() for UI
// strings, HalStorage only (never SdFat direct).

#include "BookFontLoader.h"

#if defined(ARDUINO)
#include <CrossPointSettings.h>  // probe: configured body size (SETTINGS)
#endif
#include <FreeInkUIBookFont.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>

#include "MemSentinel.h"

#if defined(CROSSPOINT_TTF_READER)
#include <builtinFonts/atkinson_hn_14_bold.h>
#include <builtinFonts/atkinson_hn_14_bolditalic.h>
#include <builtinFonts/atkinson_hn_14_italic.h>
#include <builtinFonts/atkinson_hn_14_regular.h>

#include "adapters/EpdBookFont.h"
#endif

#ifdef HOST_TEST
#include "Arduino.h"  // host-test stub for ESP.getFreeHeap
#if defined(ARDUINO) && defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
#include <freertos/task.h>  // P2 hint stack probe: uxTaskGetStackHighWaterMark
#endif
#endif

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>

namespace freeink {
namespace book {

#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
// Style → FreeType axis coordinate (design §4.1): bold = wght 700, else 400.
// Italic is passed separately; FT synthesizes oblique/embolden when an axis
// is absent.
constexpr int styleToWeight(uint8_t styleFlags) { return (styleFlags & StyleBold) ? 700 : 400; }

// P2 effective render options, one slot per family face (REGULAR, BOLD,
// ITALIC, BOLD_ITALIC). Starts at the requested mode; the stack probe
// (ensureHintProbeSettled) may degrade a slot to unhinted. File-scope so
// renderOptionsFingerprintTag() stays a cheap register-fold, and so the
// prefetch worker (via the static accessor) folds the identical state into
// its parity hash. Written only on the loopTask inside ensureLoaded() and
// ensureHintProbeSettled(); the worker reads it between loader generations,
// so the sequencing keeps the accesses non-overlapping.
freeink::font::FtFont::RenderOptions effectiveRenderOptions_[4] = {
    BookFontLoader::kCrispRenderOptions, BookFontLoader::kCrispRenderOptions, BookFontLoader::kCrispRenderOptions,
    BookFontLoader::kCrispRenderOptions};

// Build-wide mono support as OBSERVED through the degrade funnel: false once
// any loaded face's mono request was refused. effectiveMonochrome() consults
// it so paint-path decisions never select Crisp/1bpp against AA rasters when
// slot 0's own face never got to observe the refusal (load failure).
bool monoAcceptable_ = true;

// The smallest stack a hinted render must fit: the Arduino loop task
// (SET_LOOP_TASK_STACK_SIZE(49152), src/main.cpp), the only consumer that
// RASTERIZES with these faces. The FibpPrefetchWorker (32KB) only measures
// metrics — FtFont::advance() reads FT_LOAD_NO_HINTING|FAST_ONLY and the
// worker never calls rasterize() — so it never pays the hinted depth (bench:
// test/hint_mem_bench, results in
// docs/design/2026-09-29-hint-stack-bench-results.md).
constexpr uint32_t kHintConsumerStackBytes = 48 * 1024;

// Headroom a measured face must keep on that consumer. The stress set below
// rasterizes from a face's resident bytes, so it never reaches the SD tail a
// real consumer stacks UNDER a render: streamReadThunk -> halFileInspectRead
// -> HalFile::read -> the HalStorage mutex -> SdFat, entered from inside the
// glyph fault. That leg is unreachable from the probe, so it is reserved here
// rather than measured.
constexpr uint32_t kHintProbeStackMarginBytes = 8 * 1024;

// Additional stack depth a face's hinted render may consume before the probe
// degrades it. Derived from the consumer it protects, not picked: the render
// path's caller frames and the streamed-face SD tail are not visible to the
// probe (it measures resident bytes), so they are reserved as margin.
constexpr uint32_t kHintProbeStackBudgetBytes = kHintConsumerStackBytes - kHintProbeStackMarginBytes;

// Stress set for the probe: hinted outlines with distinctive contours, at
// sizes spanning the reader's runtime range (body, ruby, large) so the
// deepest autohint/Adobe paths are exercised.
constexpr uint32_t kHintProbeCodepoints[] = {'A', 'g', 'M', '@', 0x00C6u, 0x2019u};

// The body point size the probe measures at, and the SIZE axis of the verdict
// cache: the Adobe CFF depth is a function of the glyph size the probe
// rasterized, so a verdict measured at one size must not be reused at another.
#if defined(HOST_TEST)
static uint16_t g_hintProbeSizeOverride = 0;  // 0 = unset: the host probe is compiled out
// Retry-state tests: force the unmeasurable path (device: probe task spawn
// failure / settle timeout) through the host stub of probeHintStackSafety.
static bool g_hintProbeUnmeasurableOverride = false;
// Host stub observability: slot 0's hinting mode at the last probe entry, and
// how many retry pre-probe restores had completed by then (the retry must
// restore BEFORE the probe runs, so the snapshot must be 1 at the retry). -1
// = the stub has not run yet.
static int g_hintProbeObservedHinting = -1;
static int g_hintProbeRestoresAtProbe = -1;
// Incremented by the retry's pre-probe restore pass (host builds only; the
// device has no host stub to snapshot it).
static int g_hintProbeRetryRestoreCount = 0;
#endif
static uint16_t currentHintProbeSizePt() {
#if defined(HOST_TEST)
  if (g_hintProbeSizeOverride != 0) return g_hintProbeSizeOverride;
#endif
#if defined(ARDUINO)
  return static_cast<uint16_t>(SETTINGS.ttfFontPointSize);
#else
  return 0;
#endif
}
#if defined(HOST_TEST)
void BookFontLoader::setHintProbeSizeForTest(uint16_t pointSize) { g_hintProbeSizeOverride = pointSize; }
void BookFontLoader::setHintProbeUnmeasurableForTest(bool unmeasurable) {
  g_hintProbeUnmeasurableOverride = unmeasurable;
}
int BookFontLoader::hintProbeObservedHintingForTest() { return g_hintProbeObservedHinting; }
int BookFontLoader::hintProbeRestoresAtProbeForTest() { return g_hintProbeRestoresAtProbe; }
#endif

void BookFontLoader::degradeHint(uint8_t faceSlot, const char* reason) {
  freeink::font::FtFont::RenderOptions degraded = effectiveRenderOptions_[faceSlot];
  degraded.hinting = freeink::font::FtFont::HintingMode::None;
  // Route through setRenderOptions(): the P1 glyph-cache flush point, so no
  // stale hinted bitmap survives the mode change (review contract).
  applySlotRenderOptions(faces_[faceSlot], faceSlot, degraded, reason);
  // A designed fallback to unhinted rendering, not a fault: the glyphs stay
  // correct, only the grid-fit is dropped.
  LOG_INF("BFNT", "Hinting degraded to None for face slot %u (%s)", faceSlot, reason);
}

// Fail-closed half of the gate: an unmeasurable probe (spawn/settle
// failure) must degrade hinting, never leave the requested mode in effect.
// Returns the degrade mask so the caller can fold it into the verdict mask
// and re-derive the fingerprint — the identity must follow the degrades even
// when they were forced by a failed measurement.
uint8_t BookFontLoader::degradeAllLoadedSlots(const char* reason) {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < 4; ++i) {
    if (faces_[i] == nullptr) continue;
    degradeHint(i, reason);
    mask |= static_cast<uint8_t>(1u << i);
  }
  return mask;
}

void BookFontLoader::applySlotRenderOptions(NativeFace* face, uint8_t faceSlot,
                                            const freeink::font::FtFont::RenderOptions& requested, const char* label) {
  freeink::font::FtFont::RenderOptions opts = requested;
  if (face != nullptr && !face->setRenderOptions(opts)) {
    // Degrade to the nearest SUPPORTED set — never keep unsupported options:
    // a mono request without the compiled-in mono module rasterizes EVERY
    // glyph to nullptr (blank page, the device regression this guards).
    opts.monochrome = false;  // drop mono first: AA Smooth is the nearest render
    if (!face->setRenderOptions(opts)) {
      opts.hinting = freeink::font::FtFont::HintingMode::None;
      if (!face->setRenderOptions(opts)) {
        LOG_ERR("BFNT", "Render options unusable (slot %u, %s)", faceSlot, label);
      }
    } else {
      LOG_ERR("BFNT", "Render options degraded to AA (slot %u, %s)", faceSlot, label);
    }
    // Mono support is a build-wide constant, so one refusal settles it for
    // every slot — including slots whose face never loaded (and thus never
    // observed a refusal themselves).
    monoAcceptable_ = false;
  } else if (face != nullptr && opts.monochrome) {
    monoAcceptable_ = true;
  }
  // The effective set drives the fingerprint tag and every paint-path mode
  // decision (effectiveMonochrome), so FIBP identity always matches what
  // actually renders.
  effectiveRenderOptions_[faceSlot] = opts;
}

const freeink::font::FtFont::RenderOptions& BookFontLoader::effectiveRenderOptions(uint8_t faceSlot) {
  return effectiveRenderOptions_[faceSlot];
}

bool BookFontLoader::effectiveMonochrome() { return monoAcceptable_ && effectiveRenderOptions_[0].monochrome; }

void BookFontLoader::applyRenderMode(bool crispMode) {
  // Crisp/Smooth switch without a face reload: the mode changes glyph
  // RASTERIZATION only (advances are unchanged — Light hinting is on in
  // both modes), so the faces stay resident. setRenderOptions() is the P1
  // glyph-cache flush point; the fingerprint tag folds the new mode, so
  // FIBP cache identity regenerates on the next layoutGenerationHash. No
  // stack re-probe: the Adobe interpreter footprint is the same in both
  // modes (only the rasterizer differs), so the load-time probe verdict
  // stays valid.
  requestedMonochrome_ = crispMode;
  for (uint8_t i = 0; i < 4; ++i) {
    freeink::font::FtFont::RenderOptions requested = currentRenderOptions();
    // A stack-probe degrade must survive the mode switch: the requested set
    // carries Light, but the slot's PROBE VERDICT is None — re-enabling
    // hinted CFF here would put the Adobe interpreter back onto the 32KB
    // worker stack it was probed off of (review r5).
    requested.hinting = effectiveRenderOptions_[i].hinting;
    applySlotRenderOptions(faces_[i], i, requested, "applyRenderMode");
  }
  // Re-derive the cached fingerprint so the next generation hash sees the
  // new tag (cheap post-P3.1; 0-consistent for unloaded/fallback states).
  if (loaded_) fingerprint_ = computeFingerprintCached();
}

uint32_t BookFontLoader::renderOptionsFingerprintTag() {
  // 3 bits per slot for HintingMode (values fit 0..4). ONLY hinting folds
  // here: hinting changes ADVANCES, so it is part of layout identity (FIBP
  // gen). The raster mode (monochrome vs AA) alters glyph BITMAPS only —
  // advances are byte-identical across modes — so folding it would
  // re-index every book on a firmware update that merely flips the Crisp
  // default (the soak observed exactly that); bitmap identity is governed
  // by the P1 glyph cache's setRenderOptions() flush instead.
  uint32_t tag = 0;
  for (uint8_t i = 0; i < 4; ++i) {
    tag |= static_cast<uint32_t>(effectiveRenderOptions_[i].hinting) << (3 * i);
  }
  return tag;
}

#if defined(ARDUINO) && defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
namespace {
// One-shot probe of a hinted render's TRUE depth, run on a dedicated task so
// the measurement is independent of the calling task's history.
// uxTaskGetStackHighWaterMark is a LIFETIME minimum: read from the loop task,
// one deep settings/preview frame lowers it for the rest of the boot and
// every later probe misattributes that history to the font. The probe task
// starts virgin at a known stack size, so its single reading IS the face's
// whole render depth.
struct HintProbeRequest {
  NativeFace* face;
  uint8_t slot;
  uint16_t bodyPx;
  uint32_t consumed;
  SemaphoreHandle_t done;
};

// The probe must be able to complete a render DEEPER than any consumer it
// judges — a measuring device the same size as its subject overflows instead
// of reporting. At 32KB (what it used to be) a hinted CFF face (~28KB
// measured on device) left under 4KB of slack, which a real consumer's SD
// tail (streamReadThunk -> HalStorage -> SdFat, entered from inside the glyph
// fault) exhausts — the probe task took the LoadProhibited on the way to
// printing its own verdict. Sized consumer + margin, so a face anywhere up to
// the full consumer budget is measured and degraded rather than crashing the
// measurer. The reading itself is unaffected by the stack size (the
// high-water is measured from the task's own top), so the extra bytes cost
// accuracy nothing; transiently +16KB over the consumer at book open, one
// task at a time, reaped before the next.
constexpr size_t kHintProbeTaskStackBytes = kHintConsumerStackBytes + kHintProbeStackMarginBytes;
constexpr UBaseType_t kHintProbeTaskPriority = 1;
// Worker-task convention (FibpPrefetchWorker.h kCore): 0 is valid on both
// targets; the loop task runs on core 1 where it exists.
constexpr BaseType_t kHintProbeTaskCore = 0;
// Bounded settle: a probe that takes longer than this is treated as failed.
constexpr TickType_t kHintProbeSettleTimeoutTicks = pdMS_TO_TICKS(5000);

// Task body for ONE face: rasterize the stress set and record the depth the
// render reached (fresh task: the high-water starts at the stack size, so
// consumed = the drop from the entry reading is the face's own peak — no
// caller frames and no earlier face's depth in it). FT faces are created and
// closed on the main thread; the prefetch worker already rasterizes
// main-created FT faces from its own task, and this one-shot probe runs
// under the same discipline (the main thread blocks on `done` for the
// probe's whole lifetime). Uses req->face, never the loader's inline state.
void hintProbeTaskBody(void* param) {
  auto* req = static_cast<HintProbeRequest*>(param);
  const UBaseType_t before = uxTaskGetStackHighWaterMark(nullptr);  // bytes on ESP-IDF
  const uint16_t probeSizes[3] = {BookFontLoader::kInitSizePx, req->bodyPx,
                                  static_cast<uint16_t>(req->bodyPx * 2 > 240 ? 240 : req->bodyPx * 2)};
  for (const uint32_t cp : kHintProbeCodepoints) {
    for (const uint16_t size : probeSizes) {
      req->face->rasterize(cp, size);
    }
  }
  const UBaseType_t after = uxTaskGetStackHighWaterMark(nullptr);
  req->consumed = before > after ? static_cast<uint32_t>(before - after) : 0;
  LOG_DBG("BFNT", "Hint probe slot %u consumed %u B", req->slot, static_cast<unsigned>(req->consumed));
  // The give is the join point: every write to req-> is above it, and nothing
  // below it touches the request again. Park here so the waiter can reap this
  // task deterministically (see the vTaskSuspend/vTaskDelete pair in
  // probeHintStackSafety).
  xSemaphoreGive(req->done);
  for (;;) vTaskSuspend(nullptr);
}
}  // namespace

void BookFontLoader::probeHintStackSafety(uint8_t& degradedMask, bool& unmeasurable) {
  degradedMask = 0;
  unmeasurable = false;
  if (kRenderOptions.hinting == freeink::font::FtFont::HintingMode::None) return;
  // One FRESH task per face: the high-water is a task-lifetime minimum, so a
  // shared task would only report each later face's excess over the deepest
  // preceding one (a later over-budget CFF face could read ~0 and stay
  // hinted). A virgin stack per slot makes each reading the face's own full
  // depth, directly comparable to kHintProbeStackBudgetBytes.
  HintProbeRequest req{};
  req.bodyPx = static_cast<uint16_t>(lroundf(SETTINGS.ttfFontPointSize * 150.0f / 72.0f));
  req.done = xSemaphoreCreateBinary();
  if (req.done == nullptr) {
    LOG_ERR("BFNT", "Hint probe semaphore alloc failed");
    degradedMask |= degradeAllLoadedSlots("stack probe: unmeasurable");
    unmeasurable = true;
    return;
  }
  for (uint8_t i = 0; i < 4; ++i) {
    if (faces_[i] == nullptr) continue;
    req.face = faces_[i];
    req.slot = i;
    req.consumed = 0;
    TaskHandle_t probeTask = nullptr;
    if (xTaskCreatePinnedToCore(hintProbeTaskBody, "hintprobe", kHintProbeTaskStackBytes, &req, kHintProbeTaskPriority,
                                &probeTask, kHintProbeTaskCore) != pdPASS) {
      LOG_ERR("BFNT", "Hint probe task spawn failed");
      vSemaphoreDelete(req.done);
      degradedMask |= degradeAllLoadedSlots("stack probe: unmeasurable");
      unmeasurable = true;
      return;
    }
    bool overBudget = false;
    if (xSemaphoreTake(req.done, kHintProbeSettleTimeoutTicks) != pdTRUE) {
      // The probe outlived its budget: the readings are void. Keep blocking
      // until it gives — req lives on this frame and the task can still write
      // through it — so the frame is never abandoned while the task is live.
      // The extra wait bounds cleanup, not the verdict: this slot has already
      // failed closed.
      LOG_ERR("BFNT", "Hint probe slot %u timed out", i);
      xSemaphoreTake(req.done, portMAX_DELAY);
      unmeasurable = true;
    } else {
      overBudget = req.consumed > kHintProbeStackBudgetBytes;
    }
    // Strict join, on EVERY path and before any face work: suspend the probe
    // from here rather than trusting it to have parked itself. Whether it
    // already suspended or has not reached its own suspend yet, it is parked
    // by the time this returns, and reaping a suspended task frees its TCB and
    // stack in THIS context — so the next slot's spawn cannot lose the
    // allocation to an in-flight reclaim and fail closed the whole family.
    vTaskSuspend(probeTask);
    vTaskDelete(probeTask);
    // Degrades run only after the join: setRenderOptions() is the P1
    // glyph-cache flush point, so it must not race a live rasterization of
    // the very face it is reconfiguring.
    if (unmeasurable) {
      degradedMask |= degradeAllLoadedSlots("stack probe: unmeasurable");
      vSemaphoreDelete(req.done);
      return;
    }
    if (overBudget) {
      degradeHint(i, "stack probe: over budget");
      degradedMask |= static_cast<uint8_t>(1u << i);
    }
  }
  vSemaphoreDelete(req.done);
}
#else
void BookFontLoader::probeHintStackSafety(uint8_t& degradedMask, bool& unmeasurable) {
#if defined(HOST_TEST)
  // Record slot 0's mode at probe entry: pins that the retry measures the
  // REQUESTED mode (the pre-probe restore), not the failed attempt's
  // fail-closed degrade.
  g_hintProbeObservedHinting = static_cast<int>(effectiveRenderOptions_[0].hinting);
  g_hintProbeRestoresAtProbe = g_hintProbeRetryRestoreCount;
  // Mirror the device contract: every unmeasurable device path degrades all
  // loaded slots INSIDE the probe, so the fingerprint follows the degrade
  // exactly as it does on the target.
  if (g_hintProbeUnmeasurableOverride) {
    degradedMask |= degradeAllLoadedSlots("stack probe: unmeasurable");
    unmeasurable = true;
  }
#endif
}
#endif  // ARDUINO

void BookFontLoader::resetHintState() {
  for (uint8_t i = 0; i < 4; ++i) effectiveRenderOptions_[i] = currentRenderOptions();
  monoAcceptable_ = true;  // fresh load: support is re-observed by the funnel
  // New load cycle: the retry budget re-arms with the fresh probe.
  hintProbeRetryArmed_ = false;
  hintProbeRetryKey_ = 0;
#if defined(HOST_TEST)
  g_hintProbeObservedHinting = -1;
  g_hintProbeRestoresAtProbe = -1;
  g_hintProbeRetryRestoreCount = 0;
#endif
}
#endif  // CROSSPOINT_FONT_BACKEND_FT

#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
void BookFontLoader::recordHintVerdict(uint32_t loadKey, const uint8_t degradedMask) {
  hintVerdictKey_ = loadKey;
  hintVerdictMask_ = degradedMask;
  hintVerdictSizePt_ = currentHintProbeSizePt();
  hintProbePending_ = false;
  // Hinting changes advances, so the tag folded into layout identity must
  // follow the verdict: a fingerprint derived before the degrade would
  // describe a hinted face that now renders unhinted.
  if (degradedMask != 0 && loaded_) fingerprint_ = computeFingerprintCached();
}

bool BookFontLoader::reuseHintVerdicts(const uint32_t loadKey) {
  // Same principle as applyRenderMode's: the Adobe footprint is a property
  // of the face bytes and the interpreter build, not of the raster mode or
  // of when the load happened — so an unchanged face set reuses its
  // measurement instead of re-probing. The body point size is the exception:
  // the footprint scales with the glyph size the probe rasterized, so a size
  // change re-probes rather than inheriting an old-size verdict.
  if (hintVerdictKey_ == 0 || hintVerdictKey_ != loadKey || hintVerdictSizePt_ != currentHintProbeSizePt()) {
    return false;
  }
  for (uint8_t i = 0; i < 4; ++i) {
    if ((hintVerdictMask_ & static_cast<uint8_t>(1u << i)) != 0) degradeHint(i, "cached probe verdict");
  }
  hintProbePending_ = false;
  return true;
}

void BookFontLoader::ensureHintProbeSettled() {
  if (!hintProbePending_ || !loaded_) return;
  // Load key: fingerprint_ under the requested tag. No verdict has been
  // applied for this face set, or the pending flag would already be clear.
  const uint32_t loadKey = fingerprint_;
  const bool wasRetry = hintProbeRetryArmed_;
  if (wasRetry) {
#if defined(HOST_TEST)
    ++g_hintProbeRetryRestoreCount;  // the stub snapshots this at probe entry
#endif
    // The failed attempt degraded every loaded slot fail-closed. Restore the
    // requested mode BEFORE the re-measurement so the probe measures the
    // hinted depth it gates: a reading taken at HintingMode::None would
    // clear a hinted render the verdict never tested. The probe task is not
    // spawned yet, so the P1 flush cannot race a rasterization; if the retry
    // is unmeasurable too, probeHintStackSafety's fail-closed path re-degrades
    // every slot and the branch below keeps the session degraded.
    for (uint8_t i = 0; i < 4; ++i) {
      if (faces_[i] == nullptr) continue;
      applySlotRenderOptions(faces_[i], i, currentRenderOptions(), "hint probe retry");
    }
    fingerprint_ = computeFingerprintCached();
  }
  uint8_t degradedMask = 0;
  bool unmeasurable = false;
  probeHintStackSafety(degradedMask, unmeasurable);
  if (unmeasurable) {
    // No measurement could be made (probe task spawn/settle failure): it
    // describes nothing about the faces. Degrade for the rest of the session
    // (fail closed) but do not cache it: the next load re-measures instead of
    // freezing a failed probe as a font property.
    hintVerdictKey_ = 0;
    hintVerdictMask_ = 0;
    hintVerdictSizePt_ = 0;
    if (degradedMask != 0 && loaded_) fingerprint_ = computeFingerprintCached();
    if (!hintProbeRetryArmed_) {
      // First unmeasurable attempt this load cycle: arm exactly one re-probe,
      // run by the next ensureHintProbeSettled() call — the reader's
      // first-render site is the backstop call site. Bounds the damage when
      // the shallow window cannot spawn the 56KB probe task (heap
      // fragmentation): hinting is lost only when BOTH windows fail.
      hintProbeRetryArmed_ = true;
      // Remember the key observed HERE — the requested-options fingerprint,
      // captured before the fail-closed degrade moved the tag. The retry pass
      // re-enters with the degraded tag, so caching its verdict under the
      // entry key would never match what a later unchanged load computes.
      hintProbeRetryKey_ = loadKey;
      LOG_INF("BFNT", "Hint probe retry armed after unmeasurable attempt");
    } else {
      // The retry also failed: disarm and stop probing for this load cycle.
      hintProbeRetryArmed_ = false;
      hintProbePending_ = false;
    }
    return;
  }
  hintProbeRetryArmed_ = false;
  // The restored slots carry the requested mode again (pre-probe restore);
  // slots the fresh measurement found over budget keep the degrade
  // probeHintStackSafety just applied. Cache the verdict under the
  // REQUESTED-options key — on the retry pass the entry fingerprint_ described
  // the fail-closed degrade, which no later unchanged load computes.
  recordHintVerdict(wasRetry && hintProbeRetryKey_ != 0 ? hintProbeRetryKey_ : loadKey, degradedMask);
}
#else
// No members to touch: kept non-static so the call site reads identically
// on both backends (and cppcheck's functionStatic can't see that).
// cppcheck-suppress functionStatic
void BookFontLoader::ensureHintProbeSettled() {}  // stb backend: no render options to gate
#endif  // CROSSPOINT_FONT_BACKEND_FT

// Hard bounds for the DRAM-tier font file size gate. Design §3.3: the value is
// derived from ESP.getFreeHeap()/getMaxAllocHeap() after all arenas are
// allocated; this constant is the current placeholder (128KB floor).
static constexpr uint32_t kMaxDramFontBytes = 128 * 1024;

// Device-lifetime fallback faces (owned by builtinFallback()'s singleton pool
// block). builtinFace() hands these out so appendFallbackTail() can register
// them as an active chain's tail without transferring ownership.
RenderFont* g_builtinFaces[4] = {};

// SFNT minimum: 12-byte header + numTables * 16-byte entries.
static constexpr uint32_t kMinSfntLen(uint16_t numTables) { return 12u + static_cast<uint32_t>(numTables) * 16u; }

// HalFile-based absolute-offset read used by the inspect thunk and the
// §14.5 stream tail reads (defined later, in the scanFonts region).
static unsigned long halFileInspectRead(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count);

// FNV-1a hash over font data, mixed with style coverage.
static uint32_t fontFNV1a(const uint8_t* data, size_t len, uint32_t seed = 0x811c9dc5) {
  uint32_t h = seed;
  for (size_t i = 0; i < len; ++i) {
    h ^= static_cast<uint32_t>(data[i]);
    h += (h << 1) + (h << 4) + (h << 7) + (h << 8) + (h << 24);
  }
  return h;
}

// ── SD fingerprint cache (P3.1) ───────────────────────────────────────────
// One 28-byte record per face under /.crosspoint/fonts/, keyed by the face's
// path hash: {magic, version, inSeed, hash, headHash, fileSize, mtime}.
// `hash` is the CHAINED FNV-1a over the face bytes with incoming seed
// `inSeed`, so it is only served when the recorded seed matches the running
// chain — a face's entry can never poison the fingerprint after a
// predecessor changed. mtime is only a rehash trigger, never a hash input
// (design P3.1). `headHash` is the FNV-1a over the first kFingerprintHeadBytes
// bytes: verifying it on a hit (~4 KB of the resident bytes, ~2% of a full
// walk) closes the same-size/same-mtime rewrite hole from review — a font
// replacement changes the header (checkSumAdjustment/head timestamps) with
// near-certainty. Residual window: a rewrite keeping size, mtime AND the
// whole first 4 KB identical — accepted and documented (design P3.1).
struct FingerprintCacheRecord {
  uint32_t magic;    // 'BFP2'
  uint32_t version;  // 2
  uint32_t inSeed;
  uint32_t hash;
  uint32_t headHash;
  uint32_t fileSize;
  uint32_t mtime;
};
static_assert(sizeof(FingerprintCacheRecord) == 28, "fixed-size SD record");
static_assert(alignof(FingerprintCacheRecord) <= alignof(uint32_t), "no alignment surprises");
constexpr uint32_t kFingerprintCacheMagic = 0x42465032u;  // 'BFP2'
constexpr uint32_t kFingerprintCacheVersion = 2;
constexpr size_t kFingerprintCacheBytes = sizeof(FingerprintCacheRecord);
constexpr size_t kFingerprintHeadBytes = 4096;

// Cache file path for a face's path hash. Fixed cap: 8 hex digits + dir.
void fingerprintCachePath(uint32_t pathHash, char (&out)[64]) {
  snprintf(out, sizeof(out), "/.crosspoint/fonts/fp_%08x.bin", static_cast<unsigned>(pathHash));
}

// Unaligned-buffer-safe record decode (RISC-V alignment rule): fields are
// uint32 at 4-byte stride, so memcpy each instead of casting the buffer.
bool decodeFingerprintRecord(const uint8_t* buf, size_t len, FingerprintCacheRecord& rec) {
  if (len != kFingerprintCacheBytes) return false;
  memcpy(&rec, buf, kFingerprintCacheBytes);  // 24-byte POD at a heap-aligned buffer start
  return rec.magic == kFingerprintCacheMagic && rec.version == kFingerprintCacheVersion;
}

// Returns true + fills `hash` when a valid cache record matches the face's
// current {fileSize, mtime, headHash} and the chain's incoming seed. mtime 0
// (missing SD timestamp) disables the cache for that face: size alone cannot
// tell a rewritten file apart, so fail closed and recompute.
bool BookFontLoader_readFingerprintCache(uint32_t pathHash, uint32_t fileSize, uint32_t mtime, uint32_t inSeed,
                                         uint32_t headHash, uint32_t& hash) {
  if (mtime == 0) return false;
  char path[64];
  fingerprintCachePath(pathHash, path);
  HalFile file;
  if (!Storage.openFileForRead("BFNT", path, file)) return false;
  if (file.fileSize() != kFingerprintCacheBytes) return false;  // corrupt/oversize → recompute
  uint8_t buf[kFingerprintCacheBytes];
  if (file.read(buf, kFingerprintCacheBytes) != static_cast<int>(kFingerprintCacheBytes)) return false;
  FingerprintCacheRecord rec{};
  if (!decodeFingerprintRecord(buf, kFingerprintCacheBytes, rec)) return false;
  if (rec.fileSize != fileSize || rec.mtime != mtime || rec.inSeed != inSeed || rec.headHash != headHash) {
    return false;
  }
  hash = rec.hash;
  return true;
}

// Writes the record atomically: stage to <path>.tmp, close, then rename over
// the final path. A partial write can never leave a torn final record (the
// rename publishes only complete 28-byte stages), and every failure path
// removes the temp file. Best effort — a failed write only costs the next
// open one byte-walk rehash.
void BookFontLoader_writeFingerprintCache(uint32_t pathHash, uint32_t fileSize, uint32_t mtime, uint32_t inSeed,
                                          uint32_t headHash, uint32_t hash) {
  if (mtime == 0) return;  // cache disabled without a usable rehash trigger
  char path[64];
  char tmpPath[70];
  fingerprintCachePath(pathHash, path);
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
  Storage.ensureDirectoryExists("/.crosspoint/fonts");
  bool staged = false;
  {
    // Scope: the handle must be closed before the rename/remove below
    // (DESTRUCTOR_CLOSES_FILE=1 — close happens at the block exit).
    HalFile file;
    const FingerprintCacheRecord rec{
        kFingerprintCacheMagic, kFingerprintCacheVersion, inSeed, hash, headHash, fileSize, mtime};
    staged = Storage.openFileForWrite("BFNT", tmpPath, file) &&
             file.write(&rec, kFingerprintCacheBytes) == kFingerprintCacheBytes;
  }
  if (!staged) {
    LOG_DBG("BFNT", "fp-cache stage failed for %s", tmpPath);
    if (!Storage.remove(tmpPath)) LOG_ERR("BFNT", "fp-cache: stale temp %s", tmpPath);
    return;
  }
  // Publish the staged record in one replace (remove + rename under the
  // storage mutex; SdFat rename refuses an existing destination).
  if (!Storage.replaceFile(tmpPath, path)) {
    LOG_DBG("BFNT", "fp-cache publish failed for %s", path);
    if (!Storage.remove(tmpPath)) LOG_ERR("BFNT", "fp-cache: stale temp %s", tmpPath);
  }
}

// Read a font file's bytes via HalStorage into a caller-provided buffer.
// Returns bytes read, or 0 on failure.
static uint32_t readFontFile(const char* path, uint8_t* buf, uint32_t bufSz) {
  HalFile file;
  if (!Storage.openFileForRead("BFNT", path, file)) {
    LOG_ERR("BFNT", "Cannot open font %s", path);
    return 0;
  }
  uint32_t sz = file.fileSize();
  if (sz == 0 || sz > bufSz) {
    LOG_ERR("BFNT", "Font %s too large for buffer (%u > %u)", path, sz, bufSz);
    return 0;
  }
  size_t got = file.read(buf, sz);
  if (got != sz) {
    LOG_ERR("BFNT", "Font %s read error: %zu != %u", path, got, sz);
    return 0;
  }
  return sz;
}

// ── scanFonts — per-family TTF discovery (design §14.4) ──────────────────

#if defined(CROSSPOINT_TTF_READER) || defined(HOST_TEST)
namespace {
// Font roots. The hidden root is scanned first so it wins on family-name
// collisions, matching the SdCardFontRegistry sleep-folder pattern.
constexpr const char* kFontsRootHidden = "/.fonts";
constexpr const char* kFontsRootVisible = "/fonts";

// Case-insensitive ends-with on a null-terminated string.
bool endsWithIgnoreCase(const char* s, const char* suffix) {
  const size_t sLen = strlen(s);
  const size_t sufLen = strlen(suffix);
  if (sLen < sufLen) return false;
  for (size_t i = 0; i < sufLen; ++i) {
    if (tolower(static_cast<unsigned char>(s[sLen - sufLen + i])) != tolower(static_cast<unsigned char>(suffix[i]))) {
      return false;
    }
  }
  return true;
}

// Word-boundary case-insensitive substring test: the token must start after
// a non-alphanumeric (or string start) and end before one, so "SemiBold"
// does not match "bold".
bool hasWord(const char* hay, const char* token) {
  const size_t tLen = strlen(token);
  for (size_t i = 0; hay[i] != '\0'; ++i) {
    if (i > 0 && isalnum(static_cast<unsigned char>(hay[i - 1]))) continue;
    size_t j = 0;
    while (token[j] != '\0' && hay[i + j] != '\0' && tolower(static_cast<unsigned char>(hay[i + j])) == token[j]) {
      ++j;
    }
    if (token[j] != '\0') continue;
    const char after = hay[i + tLen];
    if (after == '\0' || !isalnum(static_cast<unsigned char>(after))) return true;
  }
  return false;
}

// Style inference per design §14.4. Bold and italic are detected
// independently (so "Font-Bold-Italic.ttf" gets both flags); the fused
// "bolditalic"/"boldoblique" forms are matched explicitly because their
// halves never sit on word boundaries ("BoldOblique": "bold" ends inside
// the word, "oblique" starts inside it). "SemiBold" never matches "bold" —
// the weight heuristics own those names.
// `lower` is the lowercased filename stem.
bool inferStyleFlags(const char* lower, uint8_t& styleOut) {
  if (hasWord(lower, "bolditalic") || hasWord(lower, "boldoblique")) {
    styleOut = StyleBold | StyleItalic;
    return true;
  }
  const bool italic = hasWord(lower, "italic") || hasWord(lower, "oblique") || hasWord(lower, "ital");
  const bool bold = hasWord(lower, "bold");
  if (italic && bold) {
    styleOut = StyleBold | StyleItalic;
    return true;
  }
  if (italic) {
    styleOut = StyleItalic;
    return true;
  }
  if (bold) {
    styleOut = StyleBold;
    return true;
  }
  if (hasWord(lower, "regular") || hasWord(lower, "normal") || hasWord(lower, "book") || hasWord(lower, "roman") ||
      hasWord(lower, "text")) {
    styleOut = StyleNone;
    return true;
  }
  if (hasWord(lower, "semibold") || hasWord(lower, "demibold") || hasWord(lower, "medium") || hasWord(lower, "black") ||
      hasWord(lower, "heavy") || hasWord(lower, "extrabold")) {
    styleOut = StyleBold;
    return true;
  }
  if (hasWord(lower, "light") || hasWord(lower, "thin")) {
    styleOut = StyleNone;
    return true;
  }
  return false;
}

// Case-insensitive comparison for family dedupe and same-style duplicate
// resolution (lexicographically-first filename wins).
int ciCompare(const char* a, const char* b) {
  while (*a != '\0' && *b != '\0') {
    const int ca = tolower(static_cast<unsigned char>(*a));
    const int cb = tolower(static_cast<unsigned char>(*b));
    if (ca != cb) return ca - cb;
    ++a;
    ++b;
  }
  return tolower(static_cast<unsigned char>(*a)) - tolower(static_cast<unsigned char>(*b));
}
}  // namespace
#endif

// ── BookFontLoader implementation ────────────────────────────────────────────

BookFontLoader::BookFontLoader() = default;

BookFontLoader::~BookFontLoader() {
  // Delete loaded faces and release their byte/arena owners (glyphBacking_
  // releases its pool blocks via reset() in releaseResidentCaches()).
  releaseResidentCaches();
}

void BookFontLoader::begin() {
  // Release any live resident state FIRST (deleting loaded faces before
  // nulling their pointers), then reset the manifest and counters.
  releaseResidentCaches();
  familyCount_ = 0;
  families_ = {};
  dirty_.store(false, std::memory_order_relaxed);
#if defined(CROSSPOINT_TTF_READER)
  // Hidden root first so it wins on family-name collisions (§14.4).
  scanFonts(kFontsRootHidden, families_.data(), familyCount_);
  scanFonts(kFontsRootVisible, families_.data(), familyCount_);
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  // Face-metadata style resolution (design §14.4.1): overwrites the
  // filename-inferred roles with the deterministic weight-based assignment.
  refineStyles(families_.data(), familyCount_);
#endif
#endif
  remainingBudget_ = 0;
  initBudget();
}

void BookFontLoader::ensureLoaded() {
  // loaded_ distinguishes "a load attempt completed" from "never attempted":
  // fingerprint 0 is a legitimate outcome (all faces rejected), so it cannot
  // be the loaded-state flag or every getReaderFont() re-runs the SD load.
  if (!dirty_.load(std::memory_order_relaxed) && loaded_) return;

  // Clear previous state. The RAII owners (fontPsramBytes_/fontDramBytes_)
  // release the byte buffers; never poolFree the raw pointers manually —
  // fontPsramBytes_[i].reset() already calls poolFree (double-free).
  for (uint8_t i = 0; i < 4; ++i) {
    if (faces_[i]) {
      delete faces_[i];
      faces_[i] = nullptr;
    }
    fontBytes_[i] = nullptr;
    fontPsramBytes_[i].reset();
    fontDramBytes_[i].reset();
    faceBytesOwner_[i] = 0;
    fontFileSizes_[i] = 0;
    facePathHash_[i] = 0;
    faceMtime_[i] = 0;
    faceIndexUsed_[i] = 0;
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
    // Fresh load: every slot back to the requested mode, so the verdict
    // (cached below, or a fresh probe) is the only thing that degrades.
    resetHintState();
    // Streamed source: close-before-reopen (a reload reopens this slot's
    // HalFile member).
    releaseStreamSource(i);
#endif
    arenas_[i] = Arena{};
    glyphBacking_[i].reset();
  }
  chain_ = FontChain{};
  fingerprintCoverage_ = 0;
  if (familyCount_ == 0) {
    // No manifest: no real fingerprint exists. Zero it so a cache generation
    // derived from the stale value can't collide with a previously loaded
    // family's caches; dirty stays clear until the next begin()/selectFamily.
    fingerprint_ = 0;
    dirty_.store(false, std::memory_order_relaxed);
    loaded_ = true;  // a load attempt completed — the short-circuit must hold
    return;
  }

  // Recompute the DRAM budget: the release loop above freed the previous
  // faces' bytes, so a reload must not inherit the previously spent budget.
  initBudget();

  // Phase 3 family selection (design §3.6): the SETTINGS-driven reader and
  // preview call selectFamily() first; an empty selection means the built-in
  // fallback chain. An unselected loader keeps the legacy families_[0]
  // default so the debug rig (and any pre-settings consumer) still works.
  const FamilyInfo* famPtr = nullptr;
  if (!familySelected_) {
    famPtr = &families_[0];
  } else if (selectedFamily_[0] != '\0') {
    famPtr = findFamily(selectedFamily_);
    if (famPtr == nullptr) {
      LOG_DBG("BFNT", "Selected family '%s' not found — built-in fallback", selectedFamily_);
    }
  }  // explicit fallback selection: famPtr stays null
  if (famPtr == nullptr) {
    // Fallback chain (explicit or renamed family): fingerprint_ would
    // otherwise keep the previous family's value and derive a stale FIBP
    // generation from it.
    fingerprint_ = 0;
    dirty_.store(false, std::memory_order_relaxed);
    loaded_ = true;  // a load attempt completed — the short-circuit must hold
    return;          // chain stays empty; getReaderFont() serves the fallback
  }

  for (uint8_t i = 0; i < famPtr->faceCount && i < 4; ++i) {
    if (!tryLoadFace(i, famPtr->faces[i], chain_)) {
      // Face skipped (too large, invalid sfnt, OOM); continue with fewer faces.
    }
  }
  // Identity is the PRE-fallback-tail one: the tail is a constant both parity
  // sites register, so folding chain_.styleCoverage() here (after the tail)
  // would make the fingerprint depend on WHEN it is evaluated and diverge
  // from the worker's parity hash for any family without all four styles.
  // Fingerprint both functions from the coverage captured before the tail.
  fingerprintCoverage_ = chain_.styleCoverage();
  fingerprint_ = computeFingerprintCached();
  memSentinelCheck("font ensureLoaded");
  appendFallbackTail(chain_);
  loaded_ = true;
  dirty_.store(false, std::memory_order_relaxed);
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  // Load key: the fingerprint under the REQUESTED render options — no verdict
  // has been applied for this face set yet, or the probe would be pending on
  // an already-settled one. A verdict already measured for this exact face
  // set (and body size) is re-applied here, so a plain reload of unchanged
  // faces reuses it.
  const bool reusedVerdict = reuseHintVerdicts(fingerprint_);
  if (reusedVerdict && hintVerdictMask_ != 0) {
    // The re-applied degrade moved the hinting tag: re-derive so the
    // fingerprint describes what actually renders.
    fingerprint_ = computeFingerprintCached();
  } else if (!reusedVerdict) {
    // This face set owes a probe — settle it HERE, at the load's shallow-heap
    // window, instead of deferring to the reader's first render where book-open
    // allocations have already fragmented the largest block below the probe
    // task's 56KB stack (x4pro telemetry: 93KB free, ~47KB largest block →
    // spawn failed → every slot degraded, every session). The caller blocks
    // on the probe's completion semaphore exactly as the old first-render
    // site did — the same bounded cost, moved to an earlier window. An
    // unmeasurable attempt here arms the one-retry backstop (D3), which the
    // reader's first-render ensureHintProbeSettled() call consumes.
    hintProbePending_ = true;
    ensureHintProbeSettled();
  }
#endif
}

FontChain* BookFontLoader::getReaderFont() {
  ensureLoaded();
  return (chain_.styleCoverage() != 0) ? &chain_ : builtinFallback();
}

uint32_t BookFontLoader::fontFingerprint() const { return fingerprint_; }

void BookFontLoader::markDirty() { dirty_.store(true, std::memory_order_relaxed); }

void BookFontLoader::selectFamily(const char* name) {
  const char* clean = name != nullptr ? name : "";
  if (familySelected_ && strncmp(selectedFamily_, clean, sizeof(selectedFamily_)) == 0) return;
  // An unselected loader loads families_[0] (see ensureLoaded's Phase 3
  // comment) without recording it. Selecting that same family is therefore
  // not a change: record it WITHOUT dirtying, or the first reader render
  // after boot spuriously reloads — freeing the resident bytes the
  // UI-fallback faces borrowed at boot — and the next draw walks a freed
  // cmap (issue #168).
  if (!familySelected_ && loaded_ && familyCount_ > 0 &&
      strncmp(families_[0].name, clean, sizeof(selectedFamily_)) == 0) {
    strncpy(selectedFamily_, clean, sizeof(selectedFamily_) - 1);
    selectedFamily_[sizeof(selectedFamily_) - 1] = '\0';
    familySelected_ = true;
    return;
  }
  strncpy(selectedFamily_, clean, sizeof(selectedFamily_) - 1);
  selectedFamily_[sizeof(selectedFamily_) - 1] = '\0';
  familySelected_ = true;
  markDirty();
}

const FamilyInfo* BookFontLoader::findFamily(const char* name) const {
  if (name == nullptr || name[0] == '\0') return nullptr;
  // Case-insensitive per the loader contract (§14.4 display names): settings
  // can round-trip through the web UI/JSON with different casing, and a
  // renamed family on SD still degrades to the fallback.
  const auto match = [name](const FamilyInfo& fam) { return strcasecmp(fam.name, name) == 0; };
  const auto it = std::find_if(families_.begin(), families_.begin() + familyCount_, match);
  return it != families_.begin() + familyCount_ ? &*it : nullptr;
}

bool BookFontLoader::isFamilyAvailable(const FamilyInfo& fam) {
  if (HalMemory::getPsramHeap().totalBytes == 0) return false;
  for (uint8_t i = 0; i < fam.faceCount && i < 4; ++i) {
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
    // Oversized faces no longer grey the row: they stream from SD (§14.5).
    // Only the absolute stream cap still disqualifies a face.
    if (fam.faces[i].fileSize > kMaxStreamFaceBytes) return false;
#else
    // stb backend has no streaming: the residency guard still applies, so
    // oversized rows are greyed instead of offered and silently degrading.
    if (fam.faces[i].fileSize > kMaxFaceBytes) return false;
#endif
  }
  return true;
}

#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
const void* BookFontLoader::slotFaceBytes(uint8_t slot) const {
  // Owner 3 = streamed: no resident bytes to lend (see header contract).
  if (slot >= 4 || faceBytesOwner_[slot] == 0 || faceBytesOwner_[slot] == 3) return nullptr;
  return fontBytes_[slot];
}

uint32_t BookFontLoader::slotFaceByteSize(uint8_t slot) const {
  if (slot >= 4 || faceBytesOwner_[slot] == 0 || faceBytesOwner_[slot] == 3) return 0;
  return fontFileSizes_[slot];
}

uint8_t BookFontLoader::slotFaceIndex(uint8_t slot) const {
  if (slot >= 4 || faceBytesOwner_[slot] == 0 || faceBytesOwner_[slot] == 3) return 0;
  return faceIndexUsed_[slot];
}
#endif  // CROSSPOINT_FONT_BACKEND_FT

void BookFontLoader::releaseResidentCaches() {
  // Same release discipline as ensureLoaded(): RAII owners own the bytes.
  for (uint8_t i = 0; i < 4; ++i) {
    if (faces_[i]) {
      delete faces_[i];
      faces_[i] = nullptr;
    }
    fontBytes_[i] = nullptr;
    fontPsramBytes_[i].reset();
    fontDramBytes_[i].reset();
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
    // Streamed source: close the borrowed HalFile + free the prefix (§14.5).
    releaseStreamSource(i);
#endif
    faceBytesOwner_[i] = 0;
    fontFileSizes_[i] = 0;
    facePathHash_[i] = 0;
    faceMtime_[i] = 0;
    faceIndexUsed_[i] = 0;
    arenas_[i] = Arena{};
    glyphBacking_[i].reset();
  }
  chain_ = FontChain{};
  fingerprint_ = 0;
  loaded_ = false;  // next getReaderFont() must re-attempt the load
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  // Nothing resident: drop the cached verdicts so the next load measures
  // its own faces instead of trusting a set that is no longer in memory.
  hintVerdictKey_ = 0;
  hintVerdictMask_ = 0;
  hintVerdictSizePt_ = 0;
  hintProbePending_ = true;
  hintProbeRetryArmed_ = false;
#endif
}

uint32_t BookFontLoader::fontBytesHash(const uint8_t* data, const size_t len, const uint32_t seed) {
  return fontFNV1a(data, len, seed);
}

uint32_t BookFontLoader::facePathHash(const char* file) {
  return fontFNV1a(reinterpret_cast<const uint8_t*>(file), strlen(file));
}

#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
unsigned long BookFontLoader::halFileRead(void* ctx, const unsigned long offset, unsigned char* buffer,
                                          const unsigned long count) {
  return halFileInspectRead(ctx, offset, buffer, count);
}

uint32_t BookFontLoader::streamHeadHash(const char* file, const uint32_t fileSize) {
  // Chunked SD walk over the file head — no full residency, so this works
  // for multi-MB streamed faces. 512B stack chunks stay in budget.
  HalFile f;
  if (!Storage.openFileForRead("BFNT", file, f)) return 0;
  const size_t headLen = fileSize < kFingerprintHeadBytes ? fileSize : kFingerprintHeadBytes;
  uint8_t chunk[512];
  uint32_t h = 0x811c9dc5;
  size_t got = 0;
  while (got < headLen) {
    const size_t want = headLen - got < sizeof(chunk) ? headLen - got : sizeof(chunk);
    const size_t n = f.read(chunk, want);
    if (n == 0) break;
    h = fontFNV1a(chunk, n, h);
    got += n;
  }
  if (got != headLen) {
    LOG_ERR("BFNT", "Stream head read short for %s (%zu/%zu)", file, got, headLen);
    return 0;
  }
  return h;
}

// Streamed-source ReadFn: serve from the PSRAM prefix cache when the range
// is there, hit SD only for the tail (glyf outlines). A read straddling the
// boundary splits across both; count 0 is a seek probe.
unsigned long BookFontLoader::streamReadThunk(void* ctx, unsigned long offset, unsigned char* buffer,
                                              unsigned long count) {
  auto* s = static_cast<StreamSource*>(ctx);
  if (s == nullptr || !s->file) return 0;
  const uint32_t cached = s->prefixLen;
  if (offset < cached) {
    if (count == 0) return 0;  // seek probe
    // Subtraction-based: offset + count can wrap in 32-bit unsigned long on
    // hostile offsets; cached - offset is safe because offset < cached.
    const unsigned long fromCache = (count <= cached - offset) ? count : cached - offset;
    std::memcpy(buffer, s->prefix.get() + offset, fromCache);
    if (fromCache == count) return count;
    return fromCache + halFileInspectRead(&s->file, offset + fromCache, buffer + fromCache, count - fromCache);
  }
  return halFileInspectRead(&s->file, offset, buffer, count);
}
#endif  // CROSSPOINT_FONT_BACKEND_FT

bool BookFontLoader::validateSfntBytes(const uint8_t* data, uint32_t size, const int faceIndex) {
  if (data == nullptr) return false;
  if (size < 12) {
    LOG_ERR("BFNT", "Font too small for sfnt header (%u bytes)", size);
    return false;
  }
  // TrueType collection: validate the faceIndex-th embedded sfnt directory
  // (table offsets are container-absolute per the TTC spec).
  uint32_t base = 0;
  if (data[0] == 't' && data[1] == 't' && data[2] == 'c' && data[3] == 'f') {
    if (size < 16) {
      LOG_ERR("BFNT", "Font too small for TTC header (%u bytes)", size);
      return false;
    }
    const uint32_t numFonts = static_cast<uint32_t>(data[8]) << 24 | static_cast<uint32_t>(data[9]) << 16 |
                              static_cast<uint32_t>(data[10]) << 8 | static_cast<uint32_t>(data[11]);
    if (faceIndex < 0 || static_cast<uint32_t>(faceIndex) >= numFonts) {
      LOG_ERR("BFNT", "TTC face %u out of range (%u faces)", faceIndex, numFonts);
      return false;
    }
    // Subtraction-based bounds: the whole 4-byte offset-array entry must fit
    // before it is read (untrusted numFonts/index must not drive an OOB read).
    const size_t offField = 12 + static_cast<size_t>(faceIndex) * 4;
    if (offField > size || size - offField < 4) {
      LOG_ERR("BFNT", "TTC face %u offset entry beyond size %u", faceIndex, size);
      return false;
    }
    base = static_cast<uint32_t>(data[offField]) << 24 | static_cast<uint32_t>(data[offField + 1]) << 16 |
           static_cast<uint32_t>(data[offField + 2]) << 8 | static_cast<uint32_t>(data[offField + 3]);
    if (base > size || size - base < 12) {
      LOG_ERR("BFNT", "TTC face %u base %u beyond size %u", faceIndex, base, size);
      return false;
    }
    data += base;
    size -= base;  // face-relative size for the directory bounds checks
  }
  // Table offsets in a TTC directory are container-absolute, so the per-
  // table bounds checks below run against the FULL container size.
  const uint32_t fullSize = size + base;
  const uint16_t numTables = static_cast<uint16_t>((data[4] << 8) | data[5]);
  if (numTables == 0) {
    LOG_ERR("BFNT", "Font invalid numTables 0");
    return false;
  }
  const uint32_t minSz = kMinSfntLen(numTables);
  if (size < minSz) {
    LOG_ERR("BFNT", "Font too small for table directory (%u < %u)", size, minSz);
    return false;
  }
  for (uint16_t i = 0; i < numTables; ++i) {
    const uint8_t* entry = data + 12 + static_cast<size_t>(i) * 16;
    // Guard against overflow in offset+length (uint32_t wraparound).
    const uint32_t offset = static_cast<uint32_t>(entry[8]) << 24 | static_cast<uint32_t>(entry[9]) << 16 |
                            static_cast<uint32_t>(entry[10]) << 8 | static_cast<uint32_t>(entry[11]);
    const uint32_t length = static_cast<uint32_t>(entry[12]) << 24 | static_cast<uint32_t>(entry[13]) << 16 |
                            static_cast<uint32_t>(entry[14]) << 8 | static_cast<uint32_t>(entry[15]);
    if (length > fullSize || offset > fullSize || offset + length < offset || offset + length > fullSize) {
      LOG_ERR("BFNT", "Font table %u O/L %u/%u exceeds size %u", i, offset, length, size);
      return false;
    }
  }
  return true;
}

uint32_t BookFontLoader::computeFingerprint() const {
  // FNV-1a over loaded face bytes (only valid ones) xor styleCoverage.
  // Never uses path or mtime — content-based (design §3.4). Returns 0 when
  // nothing loaded (sentinel distinct from any real FNV-1a result) so the
  // layout fingerprint doesn't depend on a failed load.
  bool anyLoaded = false;
  uint32_t h = 0x811c9dc5;
  for (uint8_t i = 0; i < 4; ++i) {
    if (!fontFileSizes_[i]) continue;
    anyLoaded = true;
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
    if (faceBytesOwner_[i] == 3) {
      // §14.5 streamed slot: no resident bytes — fold the SD head hash, the
      // size and the mtime instead (same values the prefetch worker folds).
      // The mtime distinguishes a same-sized replacement whose header region
      // is identical but whose later metrics/outlines differ.
      h = fontFNV1a(reinterpret_cast<const uint8_t*>(&streamHeadHash_[i]), sizeof(uint32_t), h);
      const uint32_t sz = fontFileSizes_[i];
      h = fontFNV1a(reinterpret_cast<const uint8_t*>(&sz), sizeof(uint32_t), h);
      h = fontFNV1a(reinterpret_cast<const uint8_t*>(&faceMtime_[i]), sizeof(uint32_t), h);
      continue;
    }
#endif
    h = fontFNV1a(static_cast<const uint8_t*>(fontBytes_[i]), fontFileSizes_[i], h);
  }
  if (!anyLoaded) return 0;
  // Role-map tag: which file plays which style slot. A metadata-driven role
  // re-assignment (§14.4.1) can swap files between slots without changing
  // the loaded byte SET's sequential FNV order, so the per-slot path hashes
  // must participate — otherwise a stale section cache renders the new role
  // map over the old layout. The collection face index joins the tag (two
  // faces of one .ttc share the file bytes, §14.4.2).
  for (uint8_t i = 0; i < 4; ++i) {
    if (fontFileSizes_[i] > 0 && faceBytesOwner_[i] != 0) {
      h = fontFNV1a(reinterpret_cast<const uint8_t*>(&facePathHash_[i]), sizeof(uint32_t), h);
      h = fontFNV1a(&faceIndexUsed_[i], sizeof(uint8_t), h);
    }
  }
  h ^= static_cast<uint32_t>(fingerprintCoverage_);
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  // D4: backend tag ("FTU1"). FreeType's advances/kerning differ from stb's
  // (different hinting), so a stale stb-layout section cache must invalidate.
  // Folding the tag into the byte-hash touches only TTF-family caches; the
  // format is unchanged → no SECTION_FILE_VERSION bump.
  h ^= 0x46545531u;
  // Render-affecting options (hinting) participate in cache identity —
  // see kRenderOptions / renderOptionsFingerprintTag().
  h ^= renderOptionsFingerprintTag();
#endif
  return h;
}

uint32_t BookFontLoader::computeFingerprintCached() {
  // Same content semantics as computeFingerprint(), but each slot's chained
  // byte-walk is served from (and refreshed into) the SD cache keyed by the
  // face's path hash. The incoming chain seed is part of every cache record,
  // so a slot can never be served for a different predecessor chain (e.g.
  // after slot 0's file was replaced). Faces without an SD mtime bypass the
  // cache entirely — readFingerprintCache fails closed for them.
  bool anyLoaded = false;
  uint32_t h = 0x811c9dc5;
  for (uint8_t i = 0; i < 4; ++i) {
    if (!fontFileSizes_[i]) continue;
    anyLoaded = true;
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
    if (faceBytesOwner_[i] == 3) {
      // §14.5 streamed slot: bypass the SD fp-cache (its record would need a
      // full residency walk) — fold the SD head hash + size directly, same
      // values as computeFingerprint().
      h = fontFNV1a(reinterpret_cast<const uint8_t*>(&streamHeadHash_[i]), sizeof(uint32_t), h);
      const uint32_t sz = fontFileSizes_[i];
      h = fontFNV1a(reinterpret_cast<const uint8_t*>(&sz), sizeof(uint32_t), h);
      h = fontFNV1a(reinterpret_cast<const uint8_t*>(&faceMtime_[i]), sizeof(uint32_t), h);
      continue;
    }
#endif
    const uint32_t inSeed = h;
    const auto* bytes = static_cast<const uint8_t*>(fontBytes_[i]);
    // Head hash: the cheap identity check that runs on every hit (~4 KB,
    // ~2% of a full walk). FNV-1a folds sequentially, so the miss path
    // chains the head walk straight into the full hash — no re-walk.
    const size_t headLen = fontFileSizes_[i] < kFingerprintHeadBytes ? fontFileSizes_[i] : kFingerprintHeadBytes;
    const uint32_t headHash = fontFNV1a(bytes, headLen, inSeed);
    uint32_t slotHash = 0;
    if (!BookFontLoader_readFingerprintCache(facePathHash_[i], fontFileSizes_[i], faceMtime_[i], inSeed, headHash,
                                             slotHash)) {
      slotHash = fontFNV1a(bytes + headLen, fontFileSizes_[i] - headLen, headHash);
      BookFontLoader_writeFingerprintCache(facePathHash_[i], fontFileSizes_[i], faceMtime_[i], inSeed, headHash,
                                           slotHash);
    }
    h = slotHash;
  }
  if (!anyLoaded) return 0;
  // Role-map tag: same rationale as computeFingerprint() — a role
  // re-assignment changes which file plays which slot; the .ttc face index
  // joins it (§14.4.2).
  for (uint8_t i = 0; i < 4; ++i) {
    if (fontFileSizes_[i] > 0 && faceBytesOwner_[i] != 0) {
      h = fontFNV1a(reinterpret_cast<const uint8_t*>(&facePathHash_[i]), sizeof(uint32_t), h);
      h = fontFNV1a(&faceIndexUsed_[i], sizeof(uint8_t), h);
    }
  }
  h ^= static_cast<uint32_t>(fingerprintCoverage_);
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  h ^= 0x46545531u;  // backend tag, mirrors computeFingerprint()
  h ^= renderOptionsFingerprintTag();
#endif
  return h;
}

FontChain* BookFontLoader::builtinFallback() {
  // Singleton FontChain over 4 fallback faces (4 styles), placement-new'ed
  // into a pool block so their payloads live in PSRAM instead of static BSS
  // (PSRAM-only directive). The faces are intentional device-lifetime
  // singletons: destructors are never run so FontChain entries remain valid
  // after this function returns. The face pointers stay in a static table so
  // appendFallbackTail() can register the same faces as an active chain's
  // tail without owning them.
#if defined(CROSSPOINT_TTF_READER)
  // Reader chain (§14.5): four RenderFont adapters over the baked Atkinson
  // fonts so the whole chain speaks the same rasterize protocol.
  using FaceType = EpdBookFont;
#else
  using FaceType = freeink::ui::BitmapBookFont;
#endif
  static FontChain fallback;
  static PoolBytes backing;  // PoolBytes object itself is only a pointer of BSS
  static bool init = false;
  if (!init) {
    static constexpr size_t kFallbackBytes = 4 * sizeof(FaceType);
    backing = poolMakeBytes(kFallbackBytes);
    if (!backing) {
      LOG_ERR("BFNT", "OOM: %u bytes for builtin fallback fonts", static_cast<unsigned>(kFallbackBytes));
      return &fallback;  // empty chain (coverage 0); caller falls back further
    }
    // Slot addresses as byte offsets from the pool block: placement-new takes
    // void*, so do the byte arithmetic on char* (defined; void* arithmetic is
    // not — cppcheck portability gate) and let it implicitly convert to void*.
    // No typed pointer variable (cppcheck constVariablePointer), no destructor
    // call (see singleton note).
    // cppcheck-suppress constVariablePointer ; placement-new writes through these addresses
    auto* slots = reinterpret_cast<char*>(backing.get());
    constexpr auto faceSize = sizeof(FaceType);
#if defined(CROSSPOINT_TTF_READER)
    auto* r = new (slots + 0 * faceSize) FaceType(&atkinson_hn_14_regular);
    auto* b = new (slots + 1 * faceSize) FaceType(&atkinson_hn_14_bold);
    auto* i = new (slots + 2 * faceSize) FaceType(&atkinson_hn_14_italic);
    auto* bi = new (slots + 3 * faceSize) FaceType(&atkinson_hn_14_bolditalic);
#else
    auto* r = new (slots + 0 * faceSize) FaceType(freeink::ui::kNotoSansFont);
    auto* b = new (slots + 1 * faceSize) FaceType(freeink::ui::kNotoSansFont);
    auto* i = new (slots + 2 * faceSize) FaceType(freeink::ui::kNotoSansFont);
    auto* bi = new (slots + 3 * faceSize) FaceType(freeink::ui::kNotoSansFont);
#endif
    fallback.add(r, StyleNone);
    fallback.add(b, StyleBold);
    fallback.add(i, StyleItalic);
    fallback.add(bi, StyleBold | StyleItalic);
    g_builtinFaces[0] = r;
    g_builtinFaces[1] = b;
    g_builtinFaces[2] = i;
    g_builtinFaces[3] = bi;
    // Mark built only after full construction: a transient PSRAM failure
    // above must leave init false so the next call retries, instead of
    // permanently serving the empty chain.
    init = true;
  }
  return &fallback;
}

RenderFont* BookFontLoader::builtinFace(const uint8_t idx) {
  // Ensures the singleton is constructed, then hands out the device-lifetime
  // face pointer (null only when the pool backing failed; FontChain::add
  // treats a null font as a safe no-op).
  builtinFallback();
  return idx < 4 ? g_builtinFaces[idx] : nullptr;
}

void BookFontLoader::appendFallbackTail(FontChain& chain) {
  chain.add(builtinFace(0), StyleNone);
  chain.add(builtinFace(1), StyleBold);
  chain.add(builtinFace(2), StyleItalic);
  chain.add(builtinFace(3), StyleBold | StyleItalic);
}

#if defined(HOST_TEST)
void BookFontLoader::forceFallbackTailForTest() { appendFallbackTail(chain_); }
#endif
#if defined(CROSSPOINT_TTF_READER) || defined(HOST_TEST)
void BookFontLoader::scanFonts(const char* rootPath, FamilyInfo* families, uint8_t& familyCount) {
  HalFile root = Storage.open(rootPath);
  const int familiesBefore = familyCount;
  if (!root || !root.isDirectory()) {
    // Missing roots are normal (a card may use only one of the two roots),
    // but the owner-visible boot log must distinguish this from a family cap.
    LOG_INF("BFNT", "Font root unavailable: %s", rootPath);
    return;
  }

  // The walk frame would need over 1KB of stack locals (over the 256B stack
  // budget, and scanFonts runs from boot wiring) — one heap scratch instead.
  struct ScanScratch {
    char dirName[64];                            // FamilyInfo::name cap; longer folder names are skipped
    char fileName[FontFaceInfo::kFileCap];       // long vendor filenames fit untruncated
    char lower[64];                              // lowercased stem
    char subPath[160];                           // SdCardCacheStorage::kDirMax
    char newFile[FontFaceInfo::kFileCap];        // full-path validation scratch
    char soloFile[FontFaceInfo::kFileCap];       // §14.4 rule 7: the lone candidate's name
    char soloLower[64];                          // the lone candidate's stem — `lower` is stale by promotion time
    char tokenlessFile[FontFaceInfo::kFileCap];  // first no-token candidate
    char tokenlessLower[64];                     // its stem
    FamilyInfo fam;                              // ~940B manifest row — heap, reset per family
    uint32_t soloSize = 0;
    uint32_t tokenlessSize = 0;
    uint32_t soloMtime = 0;
    uint32_t tokenlessMtime = 0;
  };
  // sizeof() on the decayed pointers would measure the pointer, not the
  // buffer — the walk uses the struct's member sizes everywhere.
  // >1 KB of transient scan state: try PSRAM first, with an explicit DRAM
  // fallback for exhausted/no-PSRAM hosts.
  PoolBytes scratchPool = poolMakeBytes(sizeof(ScanScratch));
  std::unique_ptr<ScanScratch> scratchDram;
  if (!scratchPool) scratchDram = makeUniqueNoThrow<ScanScratch>();
  if (!scratchPool && !scratchDram) {
    LOG_ERR("BFNT", "OOM: scan scratch");
    return;
  }
  ScanScratch* scratch = scratchPool ? new (scratchPool.get()) ScanScratch : scratchDram.get();
  char* dirName = scratch->dirName;
  char* fileName = scratch->fileName;
  char* lower = scratch->lower;
  char* soloLower = scratch->soloLower;
  char* subPath = scratch->subPath;
  constexpr size_t kDirNameCap = sizeof(ScanScratch::dirName);
  constexpr size_t kFileNameCap = sizeof(ScanScratch::fileName);
  constexpr size_t kLowerCap = sizeof(ScanScratch::lower);
  constexpr size_t kSubPathCap = sizeof(ScanScratch::subPath);
  while (true) {
    HalFile dir = root.openNextFile();
    if (!dir) break;
    if (!dir.isDirectory()) continue;
    const size_t nameLen = dir.getName(dirName, kDirNameCap);

    // Skip hidden/system folders (macOS ._*, .Trashes, _folders).
    if (dirName[0] == '.' || dirName[0] == '_') continue;
    // Hidden-root family with the same name: MERGE this root's faces into it
    // (§14.4). A new face only fills an unused style slot; for a style both
    // roots provide, the already-stored hidden-root face wins outright.
    uint8_t existingIndex = kMaxDiscoveredFamilies;
    for (uint8_t i = 0; i < familyCount; ++i) {
      if (ciCompare(families[i].name, dirName) == 0) {
        existingIndex = i;
        break;
      }
    }
    const bool mergingExisting = existingIndex < kMaxDiscoveredFamilies;
    if (!mergingExisting && familyCount >= kMaxDiscoveredFamilies) {
      LOG_INF("BFNT", "Family cap reached (%u): skipping %s", static_cast<unsigned>(kMaxDiscoveredFamilies), dirName);
      continue;
    }
    if (nameLen >= kDirNameCap - 1 || nameLen >= sizeof(FamilyInfo::name)) {
      LOG_DBG("BFNT", "Family name too long: %s", dirName);
      continue;
    }

    FamilyInfo& fam = mergingExisting ? families[existingIndex] : scratch->fam;
    if (!mergingExisting) {
      fam = {};
      strncpy(fam.name, dirName, sizeof(fam.name) - 1);
    }

    const int subLen = snprintf(subPath, kSubPathCap, "%s/%s", rootPath, dirName);
    if (subLen < 0 || static_cast<size_t>(subLen) >= kSubPathCap) continue;

    HalFile subdir = Storage.open(subPath);
    if (!subdir || !subdir.isDirectory()) continue;

    // Extension-accepted candidates (§14.4 rule 7: a family folder with
    // exactly one .ttf/.otf registers it as Regular even without style
    // tokens in the name).
    char* const soloFile = scratch->soloFile;
    char* const tokenlessFile = scratch->tokenlessFile;
    char* const tokenlessLower = scratch->tokenlessLower;
    uint32_t& soloSize = scratch->soloSize;
    uint32_t& tokenlessSize = scratch->tokenlessSize;
    uint32_t& soloMtime = scratch->soloMtime;
    uint32_t& tokenlessMtime = scratch->tokenlessMtime;
    // These candidates persist in the shared scratch across the outer family
    // loop; reset them so one family's Regular candidates cannot leak into
    // the next family's post-loop resolution.
    soloFile[0] = '\0';
    soloLower[0] = '\0';
    soloSize = 0;
    soloMtime = 0;
    tokenlessFile[0] = '\0';
    tokenlessLower[0] = '\0';
    tokenlessSize = 0;
    tokenlessMtime = 0;
    uint8_t candidateCount = 0;

    while (true) {
      HalFile entry = subdir.openNextFile();
      if (!entry) break;
      if (entry.isDirectory()) continue;
      entry.getName(fileName, kFileNameCap);

      // Skip macOS resource forks, hidden files, editor backups.
      if (fileName[0] == '.' || fileName[0] == '_') continue;
      const size_t nameLen = strlen(fileName);
      if (nameLen > 0 && fileName[nameLen - 1] == '~') continue;
      const bool isTtf = endsWithIgnoreCase(fileName, ".ttf");
      const bool isOtf = !isTtf && endsWithIgnoreCase(fileName, ".otf");
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
      const bool isTtc = !isTtf && !isOtf && endsWithIgnoreCase(fileName, ".ttc");
#else
      // stb_truetype cannot parse TTC containers — skip them on the rollback
      // backend rather than offering a family whose faces fail to load.
      const bool isTtc = false;
      if (endsWithIgnoreCase(fileName, ".ttc")) LOG_DBG("BFNT", "Skipping %s: .ttc needs the FT backend", fileName);
#endif
      if (!isTtf && !isOtf && !isTtc) continue;
      if (candidateCount < UINT8_MAX) ++candidateCount;
      if (candidateCount == 1) {
        snprintf(soloFile, kFileNameCap, "%s", fileName);
        soloSize = entry.fileSize();
        soloMtime = entry.modificationTime();
        soloLower[0] = '\0';
      }

      // Stem for style inference (extension stripped, lowercased).
      const size_t stemLen = nameLen - 4;
      if (stemLen == 0 || stemLen >= kLowerCap) continue;
      for (size_t i = 0; i < stemLen; ++i) {
        lower[i] = static_cast<char>(tolower(static_cast<unsigned char>(fileName[i])));
      }
      lower[stemLen] = '\0';
      if (candidateCount == 1) snprintf(soloLower, kLowerCap, "%s", lower);

      uint8_t style = 0;
      if (!inferStyleFlags(lower, style)) {
        // Do not drop it: remember the lexicographically-first no-token file
        // as the family's Regular candidate (fixes "Bookerly Display" and
        // tokenless Amazon Ember files being skipped, which previously made
        // Bold the promoted Regular).
        if (tokenlessLower[0] == '\0' || ciCompare(lower, tokenlessLower) < 0) {
          snprintf(tokenlessFile, kFileNameCap, "%s", fileName);
          snprintf(tokenlessLower, kLowerCap, "%s", lower);
          tokenlessSize = entry.fileSize();
          tokenlessMtime = entry.modificationTime();
        }
        continue;
      }

      // Same-style duplicate: within one root the lexicographically-first
      // filename wins; when merging from the second root the stored
      // (hidden-root) face always wins.
      uint8_t slot = kMaxFacesPerFamily;
      for (uint8_t i = 0; i < fam.faceCount; ++i) {
        if (fam.faces[i].styleFlags == style) {
          slot = i;
          break;
        }
      }
      const bool replacingExisting = slot < kMaxFacesPerFamily;
      if (replacingExisting) {
        if (mergingExisting || ciCompare(lower, fam.faces[slot].name) >= 0) continue;
      } else {
        if (fam.faceCount >= kMaxFacesPerFamily) continue;
      }
      // Validate the full path BEFORE touching the slot: a too-long path must
      // not clobber an existing face (or shrink the count of one).
      char* const newFile = scratch->newFile;
      if (snprintf(newFile, kFileNameCap, "%s/%s", subPath, fileName) >= static_cast<int>(FontFaceInfo::kFileCap)) {
        LOG_INF("BFNT", "Path too long for %s/%s", fam.name, fileName);
        continue;
      }
      if (!replacingExisting) slot = fam.faceCount++;

      FontFaceInfo& face = fam.faces[slot];
      face = {};
      snprintf(face.name, sizeof(face.name), "%s", lower);
      snprintf(face.file, sizeof(face.file), "%s", newFile);
      face.styleFlags = style;
      face.fileSize = entry.fileSize();
      face.mtime = entry.modificationTime();
    }

    if (fam.faceCount == 0) {
      if (candidateCount == 1) {
        // Single-file family: register the lone face as Regular (§14.4).
        FontFaceInfo& face = fam.faces[0];
        fam.faceCount = 1;
        face = {};
        snprintf(face.name, sizeof(face.name), "%s", soloLower);
        if (snprintf(face.file, sizeof(face.file), "%s/%s", subPath, soloFile) >= static_cast<int>(sizeof(face.file))) {
          fam.faceCount = 0;
          continue;
        }
        face.fileSize = soloSize;
        face.mtime = soloMtime;
        face.styleFlags = StyleNone;
      } else if (tokenlessLower[0] != '\0') {
        // Multiple no-token candidates: lexicographically-first becomes Regular.
        FontFaceInfo& face = fam.faces[0];
        fam.faceCount = 1;
        face = {};
        snprintf(face.name, sizeof(face.name), "%s", tokenlessLower);
        if (snprintf(face.file, sizeof(face.file), "%s/%s", subPath, tokenlessFile) >=
            static_cast<int>(sizeof(face.file))) {
          fam.faceCount = 0;
          continue;
        }
        face.fileSize = tokenlessSize;
        face.mtime = tokenlessMtime;
        face.styleFlags = StyleNone;
      } else {
        continue;  // empty / unparseable family
      }
    } else {
      bool hasRegular = false;
      for (uint8_t i = 0; i < fam.faceCount; ++i) {
        if (fam.faces[i].styleFlags == StyleNone) {
          hasRegular = true;
          break;
        }
      }
      if (!hasRegular && tokenlessLower[0] != '\0') {
        // Explicit Regular wins if present. Otherwise add the remembered
        // no-token candidate instead of promoting Bold to Regular.
        uint8_t slot = kMaxFacesPerFamily;
        for (uint8_t i = 0; i < fam.faceCount; ++i) {
          if (fam.faces[i].styleFlags == StyleNone) {
            slot = i;
            break;
          }
        }
        if (slot == kMaxFacesPerFamily && fam.faceCount < kMaxFacesPerFamily) slot = fam.faceCount++;
        if (slot < kMaxFacesPerFamily) {
          FontFaceInfo& face = fam.faces[slot];
          face = {};
          snprintf(face.name, sizeof(face.name), "%s", tokenlessLower);
          if (snprintf(face.file, sizeof(face.file), "%s/%s", subPath, tokenlessFile) >=
              static_cast<int>(sizeof(face.file))) {
            if (slot == fam.faceCount - 1) --fam.faceCount;
            continue;
          }
          face.fileSize = tokenlessSize;
          face.mtime = tokenlessMtime;
          face.styleFlags = StyleNone;
        }
      } else if (!hasRegular) {
        // No tokenless candidate either: preserve the legacy promotion of the
        // lexicographically-first face (case-insensitive) to Regular.
        int first = 0;
        for (uint8_t i = 1; i < fam.faceCount; ++i) {
          if (ciCompare(fam.faces[i].name, fam.faces[first].name) < 0) first = i;
        }
        fam.faces[first].styleFlags = StyleNone;
      }
    }

    if (mergingExisting) {
      LOG_DBG("BFNT", "Family %s: %u faces after merge from %s", fam.name, fam.faceCount, rootPath);
    } else {
      families[familyCount++] = fam;
      LOG_DBG("BFNT", "Family %s: %u faces from %s", fam.name, fam.faceCount, rootPath);
    }
  }

  LOG_INF("BFNT", "Font root %s: %d new families (total %d/%u)", rootPath, familyCount - familiesBefore, familyCount,
          static_cast<unsigned>(kMaxDiscoveredFamilies));
}

#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
// FtFont::ReadFn over a HalFile (absolute-offset reads; count 0 is a seek
// probe). Used only by refineStyles' inspectStream calls.
static unsigned long halFileInspectRead(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count) {
  auto* f = static_cast<HalFile*>(ctx);
  if (f == nullptr || !*f) return 0;
  if (!f->seek(static_cast<size_t>(offset))) return 0;
  if (count == 0) return 0;
  const int n = f->read(buffer, count);
  return n < 0 ? 0 : static_cast<unsigned long>(n);
}

void BookFontLoader::refineStyles(FamilyInfo* families, uint8_t familyCount) {
  using freeink::font::FtFont;
  for (uint8_t famIdx = 0; famIdx < familyCount; ++famIdx) {
    FamilyInfo& fam = families[famIdx];
    if (fam.faceCount == 0) continue;

    // Candidate weight/italic per face: filename-derived estimate, refined
    // by the face's real OS/2 weight + italic flag (inspectStream reads only
    // the sfnt header tables — no face is retained). An unreadable face
    // keeps the estimate, so the filename heuristics stay the fallback.
    uint16_t weights[kMaxFacesPerFamily];
    bool italics[kMaxFacesPerFamily];
    for (uint8_t s = 0; s < fam.faceCount; ++s) {
      const FontFaceInfo& fi = fam.faces[s];
      weights[s] = static_cast<uint16_t>(styleToWeight(fi.styleFlags));
      italics[s] = (fi.styleFlags & StyleItalic) != 0;
      HalFile f = Storage.open(fi.file);
      if (f && !f.isDirectory()) {
        FtFont::FaceInfo info;
        // faceIndex -1 scans the collection (§14.4.2) and reports the first
        // face with a Unicode cmap — for plain .ttf/.otf that is face 0.
        if (FtFont::inspectStream(&halFileInspectRead, &f, static_cast<unsigned long>(f.fileSize()), info, nullptr, 0,
                                  -1) == FtFont::InspectResult::Ok) {
          weights[s] = info.weight;
          italics[s] = info.italic;
          fam.faces[s].faceIndex = static_cast<uint8_t>(info.faceIndex);
        }
      }
    }

    // Nearest target weight within the upright/italic bucket; ties break to
    // the lower weight, then the lexicographically smaller path — never SD
    // enumeration order. `exclude` keeps bold/boldItalic from re-picking an
    // already-assigned face (up to two exclusions, -1 = none).
    const auto pick = [&](const bool wantItalic, const int target, const int ex1, const int ex2) -> int {
      int best = -1;
      for (uint8_t s = 0; s < fam.faceCount; ++s) {
        if (italics[s] != wantItalic || s == ex1 || s == ex2) continue;
        if (best < 0) {
          best = s;
          continue;
        }
        const int dc = std::abs(static_cast<int>(weights[s]) - target);
        const int db = std::abs(static_cast<int>(weights[best]) - target);
        if (dc < db ||
            (dc == db && (weights[s] < weights[best] ||
                          (weights[s] == weights[best] && ciCompare(fam.faces[s].file, fam.faces[best].file) < 0)))) {
          best = s;
        }
      }
      return best;
    };

    int regular = pick(false, 400, -1, -1);
    if (regular < 0) {
      // All faces italic: the italic nearest 400 anchors the family as
      // regular (the chain synthesizes the other styles from it).
      regular = pick(true, 400, -1, -1);
      if (regular >= 0) LOG_DBG("BFNT", "No upright face in %s — promoting", fam.name);
      if (regular < 0) continue;  // no usable faces at all
    }
    // Bold must be genuinely heavier than the regular pick; otherwise the
    // engine synthesizes it (a same-or-lighter file would look identical).
    int bold = pick(false, 700, regular, -1);
    if (bold >= 0 && weights[bold] <= weights[regular]) bold = -1;
    const bool regularIsItalic = italics[regular];
    int italic = regularIsItalic ? -1 : pick(true, 400, -1, -1);
    int boldItalic = pick(true, 700, italic >= 0 ? italic : regular, -1);
    // Genuinely heavier than the italic ANCHOR — in an all-italic family the
    // regular pick IS that anchor (the italic role stays synthesized).
    const int italicAnchor = italic >= 0 ? italic : regular;
    if (boldItalic >= 0 && weights[boldItalic] <= weights[italicAnchor]) boldItalic = -1;
    if (boldItalic >= 0 && !italics[boldItalic]) boldItalic = -1;

    // Rewrite the face array in role order [regular, bold, italic,
    // boldItalic]; unselected candidates are dropped. The permutation needs
    // a 4-slot copy (FontFaceInfo is ~220B — over the stack budget), so one
    // transient scratch allocation, released before the next family.
    const int8_t roleSrc[4] = {static_cast<int8_t>(regular), static_cast<int8_t>(bold), static_cast<int8_t>(italic),
                               static_cast<int8_t>(boldItalic)};
    const uint8_t roleFlags[4] = {StyleNone, StyleBold, StyleItalic, StyleBold | StyleItalic};
    PoolBytes scratchPool = poolMakeBytes(kMaxFacesPerFamily * sizeof(FontFaceInfo));
    std::unique_ptr<FontFaceInfo[]> scratchDram;
    if (!scratchPool) scratchDram = makeUniqueNoThrow<FontFaceInfo[]>(kMaxFacesPerFamily);
    FontFaceInfo* picked = scratchPool ? new (scratchPool.get()) FontFaceInfo[kMaxFacesPerFamily] : scratchDram.get();
    if (picked == nullptr) {
      LOG_ERR("BFNT", "OOM: refine scratch");
      continue;  // keep the filename-derived roles for this family
    }
    uint8_t pickedCount = 0;
    for (uint8_t r = 0; r < 4; ++r) {
      if (roleSrc[r] < 0) continue;
      picked[pickedCount] = fam.faces[roleSrc[r]];
      picked[pickedCount].styleFlags = roleFlags[r];
      ++pickedCount;
    }
    if (pickedCount > 0) {
      for (uint8_t s = 0; s < pickedCount; ++s) fam.faces[s] = picked[s];
      fam.faceCount = pickedCount;
      LOG_DBG("BFNT", "Family %s: %u faces by metadata weight", fam.name, static_cast<unsigned>(pickedCount));
    }
  }
}
#endif  // CROSSPOINT_FONT_BACKEND_FT
#endif  // CROSSPOINT_TTF_READER || HOST_TEST (scanFonts region)

// ── tryLoadFace — single face into the live chain ────────────────────────────
// Member of BookFontLoader so it can access private members (faces_,
// fontPsramBytes_, fontDramBytes_, fontBytes_, arenas_, remainingBudget_).

bool BookFontLoader::tryLoadFace(uint8_t faceIdx, const FontFaceInfo& fi, FontChain& chain) {
  // Fingerprint-cache identity (P3.1): path hash is the SD cache key, mtime
  // the rehash trigger beside size. Captured up front; a later failure in
  // this slot leaves the identity set but harmless (no fontBytes_ = the slot
  // is skipped by the fingerprint walk).
  facePathHash_[faceIdx] = facePathHash(fi.file);
  faceMtime_[faceIdx] = fi.mtime;
  faceIndexUsed_[faceIdx] = fi.faceIndex;

  // DRAM-tier size gate: skip oversized files (design §3.3). PSRAM-backed
  // boards bypass this DRAM budget; the PSRAM tier has its own guard below.
  if (HalMemory::getPsramHeap().totalBytes == 0) {
    if (fi.fileSize > kMaxDramFontBytes) {
      LOG_ERR("BFNT", "Font %s too large for DRAM tier (%u > %u)", fi.file, fi.fileSize, kMaxDramFontBytes);
      return false;
    }
    // Budget of zero = exhausted; reject every non-empty font.
    if (fi.fileSize > remainingBudget_) {
      LOG_ERR("BFNT", "Font %s exceeds remaining DRAM budget (%u > %u)", fi.file, fi.fileSize, remainingBudget_);
      return false;
    }
  } else if (fi.fileSize > kMaxFaceBytes) {
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
    // §14.5: faces beyond the PSRAM residency guard STREAM from SD instead
    // of being skipped (owner amendment: SD-streaming approved).
    return tryLoadStreamedFace(faceIdx, fi, chain);
#else
    LOG_ERR("BFNT", "Font %s too large for PSRAM tier (%u > %u)", fi.file, fi.fileSize,
            static_cast<unsigned>(kMaxFaceBytes));
    return false;
#endif
  }

  // Allocate a transient buffer for the font file bytes. PSRAM path first on
  // boards that have it; DRAM fallback otherwise. The owning handle is stored
  // in the member arrays immediately; fontBytes points at the member-owned
  // storage for the rest of the function.
  void* fontBytes = nullptr;
  bool isPsram = false;

  if (HalMemory::getPsramHeap().totalBytes > 0) {
    fontPsramBytes_[faceIdx] = poolMakeBytes(fi.fileSize);
    if (fontPsramBytes_[faceIdx]) {
      fontBytes = fontPsramBytes_[faceIdx].get();
      isPsram = true;
    }
  }

  std::unique_ptr<uint8_t[]> localDram;
  if (!fontBytes) {
    // DRAM fallback (no PSRAM, or PSRAM pool exhausted): the DRAM gates apply
    // to the actual allocation tier, not the detected board capability.
    // NOTE: poolMalloc cannot serve this fallback — on PSRAM builds it is
    // PSRAM-only (heap_caps_malloc MALLOC_CAP_SPIRAM, no runtime DRAM
    // fallback), so an exhausted PSRAM pool needs an explicit plain-DRAM
    // allocation, which is why the tier stays tracked in faceBytesOwner_.
    if (fi.fileSize > kMaxDramFontBytes) {
      LOG_ERR("BFNT", "Font %s too large for DRAM tier (%u > %u)", fi.file, fi.fileSize, kMaxDramFontBytes);
      fontPsramBytes_[faceIdx].reset();
      return false;
    }
    if (fi.fileSize > remainingBudget_) {
      LOG_ERR("BFNT", "Font %s exceeds remaining DRAM budget (%u > %u)", fi.file, fi.fileSize, remainingBudget_);
      fontPsramBytes_[faceIdx].reset();
      return false;
    }
    localDram = makeUniqueNoThrow<uint8_t[]>(fi.fileSize);
    if (!localDram) {
      LOG_ERR("BFNT", "Font buffer OOM for %u bytes", fi.fileSize);
      fontPsramBytes_[faceIdx].reset();
      return false;
    }
    fontBytes = localDram.get();
    isPsram = false;
  }

  if (readFontFile(fi.file, static_cast<uint8_t*>(fontBytes), fi.fileSize) != fi.fileSize) {
    // Cleanup on failure: release whatever we allocated.
    if (isPsram) {
      fontPsramBytes_[faceIdx].reset();
    } else {
      localDram.reset();
    }
    return false;
  }

  // sfnt validation boundary: numTables sanity + table-directory O/L checks.
  // Shared with the prefetch worker's face builder (one gate, one behavior).
  if (!validateSfntBytes(static_cast<const uint8_t*>(fontBytes), fi.fileSize, fi.faceIndex)) {
    if (isPsram) {
      fontPsramBytes_[faceIdx].reset();
    } else {
      localDram.reset();
    }
    return false;
  }

  // Per-face glyph arena — each face gets its OWN persistent backing buffer
  // (not the shared glyphBuf from the previous version). This prevents
  // overwriting glyph data when loading multiple faces (PRRT_kwDOUDrzps6g4-n7).
  // Size by SDK profile (kGlyphArenaBytes in the header): TtfFont's slot
  // tables alone need 4.6KB (SMALL), 9.2KB (STANDARD), 36.9KB (LARGE)
  // before any glyph bitmap — 8KB fails STANDARD at TtfFont::init.
  // Maximal alignment: Arena::allocArray aligns the OFFSET from base_, and
  // TtfFont allocates GlyphSlot (uint64_t key) through it. Pool blocks come
  // from heap_caps_malloc (≥4-byte aligned), which covers GlyphSlot's
  // uint64_t key on ESP32 (its natural alignment is 4 on this 32-bit ABI).
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  // D6: FreeType owns the glyph slot (FontAlloc routes all FT heap to PSRAM
  // when present), so the caller-side glyph arena and its backing pool are
  // skipped entirely on this backend.
  NativeFace* face = new (std::nothrow) NativeFace();
  if (!face) {
    LOG_ERR("BFNT", "FtFont OOM for %s", fi.file);
    if (isPsram) {
      fontPsramBytes_[faceIdx].reset();
    } else {
      localDram.reset();
    }
    return false;
  }
  // Borrowed bytes: the file bytes outlive the face (same lifetime rules as
  // the stb path — released only in ensureLoaded()/releaseResidentCaches()).
  const bool initOk = face->init(static_cast<const uint8_t*>(fontBytes), fi.fileSize, kInitSizePx,
                                 styleToWeight(fi.styleFlags), (fi.styleFlags & StyleItalic) != 0, fi.faceIndex);
  if (!initOk) {
    LOG_ERR("BFNT", "FtFont::init failed for %s", fi.file);
    delete face;
    if (isPsram) {
      fontPsramBytes_[faceIdx].reset();
    } else {
      localDram.reset();
    }
    return false;
  }
  // Reader-wide render options (hinting + text render mode) through the
  // single supported/degrade funnel; the effective set lands in
  // effectiveRenderOptions_ either way.
  applySlotRenderOptions(face, faceIdx, effectiveRenderOptions(faceIdx), fi.file);
#else
  if (!glyphBacking_[faceIdx]) {
    glyphBacking_[faceIdx] = poolMakeBytes(kGlyphArenaBytes);
    if (!glyphBacking_[faceIdx]) {
      LOG_ERR("BFNT", "Glyph arena OOM for %s (%u bytes)", fi.file, static_cast<unsigned>(kGlyphArenaBytes));
      if (isPsram) {
        fontPsramBytes_[faceIdx].reset();
      } else {
        localDram.reset();
      }
      return false;
    }
  }
  arenas_[faceIdx] = Arena(glyphBacking_[faceIdx].get(), kGlyphArenaBytes);

  NativeFace* face = new (std::nothrow) NativeFace();
  if (!face) {
    LOG_ERR("BFNT", "TtfFont OOM for %s", fi.file);
    if (isPsram) {
      fontPsramBytes_[faceIdx].reset();
    } else {
      localDram.reset();
    }
    glyphBacking_[faceIdx].reset();
    arenas_[faceIdx] = Arena{};
    return false;
  }
  if (!face->init(static_cast<const uint8_t*>(fontBytes), fi.fileSize, arenas_[faceIdx])) {
    LOG_ERR("BFNT", "TtfFont::init failed for %s", fi.file);
    delete face;
    if (isPsram) {
      fontPsramBytes_[faceIdx].reset();
    } else {
      localDram.reset();
    }
    glyphBacking_[faceIdx].reset();
    arenas_[faceIdx] = Arena{};
    return false;
  }
#endif

  if (!chain.add(face, fi.styleFlags)) {
    LOG_ERR("BFNT", "FontChain::add failed to register %s (duplicate style?)", fi.file);
    delete face;
    if (isPsram) {
      fontPsramBytes_[faceIdx].reset();
    } else {
      localDram.reset();
    }
    glyphBacking_[faceIdx].reset();
    arenas_[faceIdx] = Arena{};
    return false;
  }
  // Transfer ownership of the font bytes to the loader.
  // PSRAM: fontPsramBytes_ holds the RAII owner (heap_caps_free on reset).
  // DRAM:   fontDramBytes_ holds the unique_ptr<uint8_t[]> (delete[] on reset).
  // fontBytes_ is the non-owning raw pointer used for fingerprinting.
  fontBytes_[faceIdx] = fontBytes;
  faceBytesOwner_[faceIdx] = static_cast<uint8_t>(isPsram ? 1 : 2);
  fontFileSizes_[faceIdx] = fi.fileSize;
  faces_[faceIdx] = face;

  // Steal the RAII owners so they persist beyond this function.
  if (isPsram) {
    // fontPsramBytes_[faceIdx] already moved from psram above.
  } else {
    fontDramBytes_[faceIdx] = std::move(localDram);
  }

  // Decrement the aggregate DRAM budget (only for DRAM-tier allocations).
  if (!isPsram && fi.fileSize <= remainingBudget_) {
    remainingBudget_ -= fi.fileSize;
  }

  return true;
}

void BookFontLoader::initBudget() {
  // Two-tier allocation budget (design §3.3): derive the DRAM budget from the
  // current free heap, keeping 32KB/16KB heap-gate floors for the hot render
  // path and stack respectively. On PSRAM boards (S3) fonts bypass the DRAM
  // budget entirely, so this only governs the C3/Sticky (no-PSRAM) tier.
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t maxAlloc = ESP.getMaxAllocHeap();
  uint32_t usable = std::min(freeHeap, maxAlloc);
  // Reserve 32KB for other allocator needs, 16KB for stack safety.
  uint32_t floor = 32 * 1024 + 16 * 1024;
  if (usable > floor) {
    remainingBudget_ = usable - floor;
  } else {
    // Heap at/below the reserve: nothing safe to spend.
    remainingBudget_ = 0;
  }
  // Cap at the compile-time max to avoid surprises.
  if (remainingBudget_ > kMaxDramFontBytes) {
    remainingBudget_ = kMaxDramFontBytes;
  }
}

#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
void BookFontLoader::releaseStreamSource(const uint8_t faceIdx) {
  // Close-before-reopen discipline: a slot reload reopens the same HalFile
  // member, so the stale handle must be closed first (DESTRUCTOR_CLOSES_FILE
  // only covers scope-exit, not member reuse). Safe on never-opened slots:
  // HalFile::close() is a no-op when the handle was never assigned an Impl.
  streamSources_[faceIdx].file.close();
  streamSources_[faceIdx].prefix.reset();
  streamSources_[faceIdx].prefixLen = 0;
  streamHeadHash_[faceIdx] = 0;
}

bool BookFontLoader::tryLoadStreamedFace(const uint8_t faceIdx, const FontFaceInfo& fi, FontChain& chain) {
  if (fi.fileSize > kMaxStreamFaceBytes) {
    LOG_ERR("BFNT", "Font %s too large to stream (%u > %u)", fi.file, fi.fileSize,
            static_cast<unsigned>(kMaxStreamFaceBytes));
    return false;
  }
  StreamSource& src = streamSources_[faceIdx];
  releaseStreamSource(faceIdx);
  if (!Storage.openFileForRead("BFNT", fi.file, src.file)) {
    LOG_ERR("BFNT", "Cannot open streamed font %s", fi.file);
    return false;
  }
  // PSRAM prefix cache over the file head: an sfnt's per-glyph-fault tables
  // (cmap/loca/hmtx) sit before the multi-MB glyf table, so serving the
  // first MB from RAM collapses each glyph fault's scattered SD seeks into
  // one glyf read. Gated per source so a small-PSRAM board takes what fits;
  // a failed head read falls back to pure streaming.
  if (HalMemory::getPsramHeap().largestBlockBytes > kStreamPrefixBytes + 256u * 1024u) {
    src.prefix = poolMakeBytes(kStreamPrefixBytes);
    if (src.prefix) {
      if (src.file.seek(0) && src.file.read(src.prefix.get(), kStreamPrefixBytes) == kStreamPrefixBytes) {
        src.prefixLen = kStreamPrefixBytes;
      } else {
        LOG_DBG("BFNT", "Prefix read failed for %s — pure streaming", fi.file);
        src.prefixLen = 0;
        src.prefix.reset();
      }
    }
  }
  LOG_INF("BFNT", "Streaming %s (%u KB%s)", fi.file, static_cast<unsigned>(fi.fileSize / 1024),
          src.prefixLen ? ", 1MB prefix cached" : ", no prefix");

  NativeFace* face = new (std::nothrow) NativeFace();
  if (face == nullptr) {
    LOG_ERR("BFNT", "FtFont OOM for %s", fi.file);
    releaseStreamSource(faceIdx);
    return false;
  }
  // Borrowed source: the open HalFile + prefix outlive the face (released in
  // releaseResidentCaches()/ensureLoaded's clear loop — same lifetime rules
  // as the resident borrowed-bytes contract).
  if (!face->initStream(&streamReadThunk, &src, fi.fileSize, kInitSizePx, styleToWeight(fi.styleFlags),
                        (fi.styleFlags & StyleItalic) != 0, fi.faceIndex)) {
    LOG_ERR("BFNT", "FtFont::initStream failed for %s", fi.file);
    delete face;
    releaseStreamSource(faceIdx);
    return false;
  }
  // Streamed faces give up GPOS kerning: the lazily-copied table would pull
  // scattered multi-MB SD reads into the render path. Documented trade-off
  // (§14.5 / PR body); advances stay identical, kern pairs collapse.
  face->setGposByteBudget(0);
  applySlotRenderOptions(face, faceIdx, effectiveRenderOptions(faceIdx), fi.file);
  if (!chain.add(face, fi.styleFlags)) {
    LOG_ERR("BFNT", "FontChain::add failed to register %s", fi.file);
    delete face;
    releaseStreamSource(faceIdx);
    return false;
  }
  // Slot identity: owner 3 = streamed (no resident byte walk — the
  // fingerprint folds the SD head hash + size instead).
  faceBytesOwner_[faceIdx] = 3;
  fontFileSizes_[faceIdx] = fi.fileSize;
  facePathHash_[faceIdx] = facePathHash(fi.file);
  faceMtime_[faceIdx] = fi.mtime;
  faceIndexUsed_[faceIdx] = fi.faceIndex;
  streamHeadHash_[faceIdx] = streamHeadHash(fi.file, fi.fileSize);
  faces_[faceIdx] = face;
  return true;
}
#endif  // CROSSPOINT_FONT_BACKEND_FT

}  // namespace book
}  // namespace freeink
