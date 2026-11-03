// FibpPrefetchWorker — see FibpPrefetchWorker.h for the threading model.

#if defined(CROSSPOINT_TTF_READER)

#include "FibpPrefetchWorker.h"

#if FIBP_WORKER_ENABLED

#include <Arduino.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "ChapterIndexEngine.h"

// Reap-wait bound (dram-x3 Finding 1): the idle task unlinks a self-deleted
// task on its next pass; on this device that is well under 1 tick. 50 ms is
// orders of magnitude above the steady state yet bounded if the scheduler is
// wedged — the respawn then fails with today's ERR and a later trigger
// retries.
static constexpr uint32_t kReapWaitMaxMs = 50;
static constexpr uint32_t kReapWaitStepMs = 5;

namespace freeink {
namespace book {

// ── faces (main thread only — FT face create/destroy discipline) ────────────

bool FibpPrefetchWorker::buildFaces() {
  // Resolve the same family the loader resolves for the reader chain
  // (§14.4 manifest is boot-stable). An unknown/unavailable family leaves
  // the worker inert: the sync path already serves that case.
  const FamilyInfo* fam = familyName_[0] != '\0' ? fontLoader.findFamily(familyName_) : nullptr;
  if (fam == nullptr || !BookFontLoader::isFamilyAvailable(*fam)) {
    LOG_DBG("PREF", "No worker family '%s' — worker inert", familyName_);
    return false;
  }

  // Mirror tryLoadFace's PSRAM tier: PSRAM-only byte copy per face, the
  // loader's per-face size guard, then FT face init over the borrowed
  // bytes. Slot order and content match the loader's, so the derived
  // fingerprint (and with it the FIBP generation) is identical.
  bool anyLoaded = false;
  uint32_t h = 0x811c9dc5;
  for (uint8_t i = 0; i < 4 && i < fam->faceCount; ++i) {
    const FontFaceInfo& fi = fam->faces[i];
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
    // §14.5 parity: oversized faces stream exactly like tryLoadStreamedFace
    // — own open HalFile + PSRAM prefix, initStream, GPOS off — so the
    // worker's chain covers the same slots as the reader's.
    if (fi.fileSize > BookFontLoader::kMaxFaceBytes) {
      if (fi.fileSize > BookFontLoader::kMaxStreamFaceBytes) continue;
      StreamSource& src = streamSources_[i];
      // Close-before-reopen on the reused member; safe on never-opened slots
      // (HalFile::close() is a no-op with null impl).
      src.file.close();
      src.prefix.reset();
      src.prefixLen = 0;
      if (!Storage.openFileForRead("PREF", fi.file, src.file)) {
        LOG_ERR("PREF", "Streamed font open failed: %s", fi.file);
        continue;
      }
      if (HalMemory::getPsramHeap().largestBlockBytes > BookFontLoader::kStreamPrefixBytes + 256u * 1024u) {
        src.prefix = poolMakeBytes(BookFontLoader::kStreamPrefixBytes);
        if (src.prefix && src.file.seek(0) &&
            src.file.read(src.prefix.get(), BookFontLoader::kStreamPrefixBytes) == BookFontLoader::kStreamPrefixBytes) {
          src.prefixLen = BookFontLoader::kStreamPrefixBytes;
        } else {
          src.prefixLen = 0;
          src.prefix.reset();
        }
      }
      auto face = makeUniqueNoThrow<NativeFace>();
      if (face == nullptr) {
        LOG_ERR("PREF", "OOM: streamed face for %s", fi.file);
        src.file.close();
        src.prefix.reset();
        src.prefixLen = 0;
        continue;
      }
      if (!face->initStream(&streamReadThunk, &src, fi.fileSize, BookFontLoader::kInitSizePx,
                            (fi.styleFlags & StyleBold) ? 700 : 400, (fi.styleFlags & StyleItalic) != 0,
                            fi.faceIndex)) {
        LOG_ERR("PREF", "Streamed face init failed: %s", fi.file);
        src.file.close();
        src.prefix.reset();
        src.prefixLen = 0;
        continue;
      }
      face->setGposByteBudget(0);  // §14.5 trade-off, mirrors the loader
      if (!face->setRenderOptions(BookFontLoader::effectiveRenderOptions(i))) {
        LOG_ERR("PREF", "Render options unsupported for %s — face skipped", fi.file);
        face.reset();
        src.file.close();
        src.prefix.reset();
        src.prefixLen = 0;
        continue;
      }
      if (!chain_.add(face.get(), fi.styleFlags)) {
        LOG_ERR("PREF", "Chain add failed: %s", fi.file);
        face.reset();
        src.file.close();
        src.prefix.reset();
        src.prefixLen = 0;
        continue;
      }
      faceOwners_[i] = std::move(face);
      const uint32_t headHash = BookFontLoader::streamHeadHash(fi.file, fi.fileSize);
      h = BookFontLoader::fontBytesHash(reinterpret_cast<const uint8_t*>(&headHash), sizeof(uint32_t), h);
      const uint32_t sz = fi.fileSize;
      h = BookFontLoader::fontBytesHash(reinterpret_cast<const uint8_t*>(&sz), sizeof(uint32_t), h);
      // Parity with the loader's streamed fold: the mtime distinguishes a
      // same-sized replacement with an identical header region.
      const uint32_t mt = fi.mtime;
      h = BookFontLoader::fontBytesHash(reinterpret_cast<const uint8_t*>(&mt), sizeof(uint32_t), h);
      anyLoaded = true;
      continue;
    }
#endif
    if (fi.fileSize == 0) continue;
    fontBytes_[i] = poolMakeBytes(fi.fileSize);
    if (!fontBytes_[i]) {
      LOG_ERR("PREF", "PSRAM OOM: %u bytes for %s", static_cast<unsigned>(fi.fileSize), fi.file);
      break;  // fewer faces → fingerprint drift → FIBP names miss (harmless)
    }
    auto* bytes = static_cast<uint8_t*>(fontBytes_[i].get());
    HalFile file;
    if (!Storage.openFileForRead("PREF", fi.file, file) || file.read(bytes, fi.fileSize) != fi.fileSize) {
      LOG_ERR("PREF", "Font read failed: %s", fi.file);
      fontBytes_[i].reset();
      continue;
    }
    if (!BookFontLoader::validateSfntBytes(bytes, fi.fileSize, fi.faceIndex)) {
      fontBytes_[i].reset();
      continue;
    }
    auto face = makeUniqueNoThrow<NativeFace>();
    if (face == nullptr) {
      LOG_ERR("PREF", "OOM: face for %s", fi.file);
      fontBytes_[i].reset();
      continue;
    }
    if (!face->init(bytes, fi.fileSize, BookFontLoader::kInitSizePx, (fi.styleFlags & StyleBold) ? 700 : 400,
                    (fi.styleFlags & StyleItalic) != 0, fi.faceIndex)) {
      LOG_ERR("PREF", "Face init failed: %s", fi.file);
      face.reset();
      fontBytes_[i].reset();
      continue;
    }
    // Parity with tryLoadFace: the loader's EFFECTIVE render options (the
    // P2 stack probe may have degraded a slot) — same source of truth, so
    // both fingerprint sites fold identical modes. The loader's degrade
    // funnel guarantees this set is supported by this build; if it ever
    // is not, skip the face rather than rasterize nullptrs into FIBP
    // caches (blank-page regression class).
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
    if (!face->setRenderOptions(BookFontLoader::effectiveRenderOptions(i))) {
      LOG_ERR("PREF", "Render options unsupported for %s — face skipped", fi.file);
      face.reset();
      fontBytes_[i].reset();
      continue;
    }
#endif
    if (!chain_.add(face.get(), fi.styleFlags)) {
      LOG_ERR("PREF", "Chain add failed: %s", fi.file);
      face.reset();
      fontBytes_[i].reset();
      continue;
    }
    faceOwners_[i] = std::move(face);
    h = BookFontLoader::fontBytesHash(bytes, fi.fileSize, h);
    anyLoaded = true;
  }
  if (!anyLoaded) return false;

  // Role-map tag parity (§14.4.1/§14.4.2): after the byte walk, the loader
  // folds each loaded slot's path hash + collection face index — the worker
  // must fold the same values in the same order or the derived fingerprint
  // diverges from computeFingerprint() and every FIBP name misses.
  for (uint8_t i = 0; i < 4 && i < fam->faceCount; ++i) {
    if (faceOwners_[i] == nullptr) continue;
    const FontFaceInfo& fi = fam->faces[i];
    const uint32_t pathHash = BookFontLoader::facePathHash(fi.file);
    h = BookFontLoader::fontBytesHash(reinterpret_cast<const uint8_t*>(&pathHash), sizeof(uint32_t), h);
    const uint8_t faceIdx = fi.faceIndex;
    h = BookFontLoader::fontBytesHash(&faceIdx, sizeof(uint8_t), h);
  }

  // Fingerprint parity: the loader folds the chain coverage it captured
  // BEFORE the fallback tail joined its chain (fingerprintCoverage_) and
  // answers with that same pre-tail identity wherever the hash is evaluated;
  // the worker folds the same pre-tail coverage here, at the same point.
  h ^= static_cast<uint32_t>(chain_.styleCoverage());
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  h ^= 0x46545531u;                                    // backend tag, mirrors computeFingerprint()
  h ^= BookFontLoader::renderOptionsFingerprintTag();  // render options, mirrors computeFingerprint()
#endif
  fingerprint_ = h;
  // The tail registers the missing styles so coverage matches the reader's
  // chain; its faces are device-lifetime singletons whose metrics queries
  // are pure reads — safe to consult from the worker's layout pass.
  BookFontLoader::appendFallbackTail(chain_);
  return chain_.styleCoverage() != 0;
}

// §14.5 streamed-source ReadFn (mirror of BookFontLoader's): serve from the
// PSRAM prefix cache when the range is there, SD for the tail; count 0 is a
// seek probe. ctx is the slot's StreamSource.
unsigned long FibpPrefetchWorker::streamReadThunk(void* ctx, unsigned long offset, unsigned char* buffer,
                                                  unsigned long count) {
  auto* s = static_cast<StreamSource*>(ctx);
  if (s == nullptr || !s->file) return 0;
  const uint32_t cached = s->prefixLen;
  if (offset < cached) {
    if (count == 0) return 0;
    // Subtraction-based: offset + count can wrap in 32-bit unsigned long on
    // hostile offsets; cached - offset is safe because offset < cached.
    const unsigned long fromCache = (count <= cached - offset) ? count : cached - offset;
    std::memcpy(buffer, s->prefix.get() + offset, fromCache);
    if (fromCache == count) return count;
    return fromCache + BookFontLoader::halFileRead(&s->file, offset + fromCache, buffer + fromCache, count - fromCache);
  }
  return BookFontLoader::halFileRead(&s->file, offset, buffer, count);
}

void FibpPrefetchWorker::teardownFaces() {
  chain_ = FontChain{};  // drops its non-owning face pointers first
  for (auto& face : faceOwners_) face.reset();
  for (auto& bytes : fontBytes_) bytes.reset();
  for (auto& src : streamSources_) {
    src.file.close();  // no-op on never-opened slots
    src.prefix.reset();
    src.prefixLen = 0;
  }
  fingerprint_ = 0;
}

// ── params snapshot (scalars only; pointers stay worker-owned) ──────────────

void FibpPrefetchWorker::copyScalarParams(const LayoutParams& from) {
  params_.pageWidth = from.pageWidth;
  params_.pageHeight = from.pageHeight;
  params_.marginLeft = from.marginLeft;
  params_.marginRight = from.marginRight;
  params_.marginTop = from.marginTop;
  params_.marginBottom = from.marginBottom;
  params_.baseSizePx = from.baseSizePx;
  params_.lineSpacingPct = from.lineSpacingPct;
  params_.paragraphSpacingPct = from.paragraphSpacingPct;
  params_.defaultAlign = from.defaultAlign;
  params_.orphanLines = from.orphanLines;
  params_.widowLines = from.widowLines;
  params_.embeddedStyles = from.embeddedStyles;
  params_.focusReading = from.focusReading;
  params_.language = language_;
  params_.font = &chain_;
  params_.stylesheet = nullptr;
  params_.hyphenator = nullptr;
}

// ── lifecycle (main thread) ────────────────────────────────────────────────

bool FibpPrefetchWorker::begin(const BeginContext& ctx) {
  if (running_.load(std::memory_order_acquire)) return true;  // already active
  if (ctx.spineCount == 0) return false;

  cancel_.store(false, std::memory_order_release);
  snprintf(epubPath_, sizeof(epubPath_), "%s", ctx.epubPath);
  snprintf(cacheDir_, sizeof(cacheDir_), "%s", ctx.cacheDir);
  snprintf(familyName_, sizeof(familyName_), "%s", ctx.familyName);
  snprintf(language_, sizeof(language_), "%s", ctx.layout.language != nullptr ? ctx.layout.language : "en");
  spineCount_ = ctx.spineCount;
  gen_.store(ctx.generation, std::memory_order_release);
  notifiedSpine_.store(fibp::kNoChapter, std::memory_order_release);
  copyScalarParams(ctx.layout);
  paramGen_ = ctx.generation;  // lock-free: no worker task exists yet
  sessionGen_ = ctx.generation;

  if (!buildFaces()) {
    teardownFaces();
    LOG_DBG("PREF", "Worker inert (no face set) — sync path unchanged");
    return false;
  }
  params_.font = &chain_;

  queue_.clear();
  queue_.reserve(fibp::kPrefetchLookaheadSpines);
  failed_.assign(spineCount_, 0);
  queueCursor_ = 0;

  // The runtime opens on the worker task (catalog work is heavy SD I/O).
  runtime_ = makeUniqueNoThrow<TtfBookRuntime>();
  if (runtime_ == nullptr) {
    LOG_ERR("PREF", "OOM: TtfBookRuntime");
    teardownFaces();
    return false;
  }

  paramsMux_ = xSemaphoreCreateMutex();
  if (paramsMux_ == nullptr) {
    LOG_ERR("PREF", "OOM: params mutex");
    runtime_.reset();
    teardownFaces();
    return false;
  }
  // No task spawn here: the worker arms itself (state + runtime) but the
  // task starts only on the first threshold-fired notifyChapterProgress —
  // an unconditional start would index from spine 0 before the trigger and
  // could claim the spine the reader just entered (CodeRabbit finding).
  // exitedSem_ stays null ("no task ever existed") so ensureTask() spawns
  // directly on the first trigger.
  LOG_INF("PREF", "Worker armed: %u spines, gen=%08x", static_cast<unsigned>(spineCount_), ctx.generation);
  return true;
}

void FibpPrefetchWorker::notifyChapterProgress(const uint16_t spine, const uint16_t page, const uint16_t pageCount,
                                               const bool lengthFinal) {
  // Position mirror first: the run loop's skip logic (the reader owns the
  // entered chapter) and its change detection see the fresh position even
  // when the trigger has not fired yet.
  notifiedSpine_.store(spine, std::memory_order_release);
  // The worker applies the policy — the reader passes raw progress only.
  if (!fibp::shouldPrefetchNext(page, pageCount, lengthFinal)) return;
  // Dedup the spawn: notifyChapterProgress fires on every reader render once
  // the position is inside the window, and an already-exited worker would
  // otherwise run a full task create/destroy cycle (32 KB stack + TCB churn)
  // per page turn. One spawn per triggered spine window; notifyGeneration
  // re-arms this after a settings change.
  if (spine == lastPrefetchFiredSpine_.load(std::memory_order_acquire)) return;
  // Capped window: the worker self-exits once its one-spine plan is
  // exhausted, so a trigger must be able to respawn it to index the next
  // chapter. A live worker needs no respawn — its run loop re-checks
  // notifiedSpine_ and replans on the change (page turns within the same
  // spine do not change notifiedSpine_, so no queue rebuild per page turn).
  if (ensureTask()) lastPrefetchFiredSpine_.store(spine, std::memory_order_release);
}

bool FibpPrefetchWorker::ensureTask() {
  // Respawn the worker task after a self-exit ("fully indexed") so a later
  // chapter entry or settings change can index again. When a task could
  // still exist, its exit semaphore is consumed (not replaced) first, so
  // the previous task's final give cannot land on a fresh handle
  // (timing-dependent use-after-free). A null semaphore means no task ever
  // existed: spawn directly. Returns true when a task is (or already was)
  // running so callers can dedup their respawn bookkeeping.
  if (running_.load(std::memory_order_acquire) || cancel_.load(std::memory_order_acquire)) {
    return running_.load(
        std::memory_order_acquire);  // a live task keeps running (and replans); a cancelling worker never respawns
  }
  if (exitedSem_ != nullptr && xSemaphoreTake(exitedSem_, pdMS_TO_TICKS(kJoinTimeoutMs)) != pdTRUE) {
    LOG_DBG("PREF", "Previous worker still exiting — respawn skipped");
    return false;
  }
  if (exitedSem_ == nullptr) {
    exitedSem_ = xSemaphoreCreateBinary();
    if (exitedSem_ == nullptr) {
      LOG_ERR("PREF", "OOM: respawn semaphore");
      return false;
    }
  }
  running_.store(true, std::memory_order_release);
  const SpawnResult spawned = spawnTask();
  if (spawned == SpawnResult::Started) return true;
  running_.store(false, std::memory_order_release);
  if (spawned == SpawnResult::Failed) {
    vSemaphoreDelete(exitedSem_);  // restore the invariant: no sem ⇒ no task
    exitedSem_ = nullptr;
    task_ = nullptr;
  }
  // ReapBlocked: spawnTask put the exit token back on the semaphore, so the
  // next trigger joins instantly and re-runs the non-blocking reap probe.
  return false;
}

bool FibpPrefetchWorker::reserveStack() {
  // Idempotent: the one-shot allocation runs once per object lifetime. The
  // reader calls this at reader entry, before the EPUB open loads its ZIP
  // index cache — that cache (entry-count dependent, ~56 KB at 465 entries)
  // can leave no contiguous 32 KB DRAM block for the first spawn (2nd soak,
  // build 0e652a97). Failures are NOT retried by design: the worker stays
  // inert for the session, exactly like today's failed first spawn.
  stackReserveAttempted_ = true;
  if (taskStack_ != nullptr) return true;
  taskStack_ = static_cast<uint8_t*>(
      heap_caps_aligned_alloc(portBYTE_ALIGNMENT, kStackBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (taskStack_ == nullptr) {
    LOG_ERR("PREF", "Stack reserve failed: %u", static_cast<unsigned>(kStackBytes));
    return false;
  }
  return true;
}

// Bounded wait for the idle task's unlink of a self-deleted static task,
// using the same name probe as the spawner's non-blocking check: usable only
// off the render path (cancel/teardown), where a few ms of wait are fine.
static bool reapSettled(uint32_t maxMs) {
  const uint32_t start = millis();
  while (xTaskGetHandle("fibpprefetch") != nullptr) {
    if (millis() - start >= maxMs) return false;
    vTaskDelay(pdMS_TO_TICKS(kReapWaitStepMs));
  }
  return true;
}

FibpPrefetchWorker::SpawnResult FibpPrefetchWorker::spawnTask() {
  // One-shot static buffers: the stack is a single INTERNAL DRAM block (a
  // task stack can never live in PSRAM — the Xtensa windowed ABI faults once
  // the PSRAM cache flips; heap_caps_aligned_alloc satisfies the kernel's
  // 16-byte stack alignment assert) and the TCB is a member of this DRAM-
  // resident object. The buffers persist across self-exit respawns: the
  // required block is never returned to the heap, so respawning cannot fail
  // on fragmentation (the churn that permanently disabled prefetch on
  // image-heavy books). Stack depth is in BYTES on ESP-IDF (vanilla FreeRTOS
  // counts words). On any failure the returned SpawnResult tells ensureTask
  // whether the exit token needs restoring; the buffers survive and the next
  // trigger retries with the same allocation. The reader reserves the stack
  // at entry (reserveStack); this path's own allocation is only a fallback
  // for callers that never reserved — once reserveStack() ran, a spawn-time
  // allocation would just re-fight the fragmentation the reserve avoids.
  if (taskStack_ == nullptr) {
    if (stackReserveAttempted_) {
      // Reader-entry reserve failed this session: prefetch stays disabled,
      // no spawn-time retry (the reserve failure was already logged).
      return SpawnResult::Failed;
    }
    taskStack_ = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(portBYTE_ALIGNMENT, kStackBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (taskStack_ == nullptr) {
      LOG_ERR("PREF", "Stack alloc failed: %u", static_cast<unsigned>(kStackBytes));
      return SpawnResult::Failed;
    }
  }
  // NON-BLOCKING reap probe: a self-deleted static task stays linked on
  // xTasksWaitingTermination until the idle task unlinks it, and reusing the
  // buffers before that would corrupt the new task. Waiting here (the render
  // path) is not allowed, so the probe answers instantly and a still-present
  // name defers the spawn by one trigger instead of stalling 50 ms
  // (CodeRabbit). The name search covers the termination list too, and
  // "fibpprefetch" is unique in the system. The bounded reap wait lives in
  // cancel() only (off the render path).
  if (xTaskGetHandle("fibpprefetch") != nullptr) {
    LOG_DBG("PREF", "Previous task not yet reaped — spawn deferred a trigger");
    // Restore the consumed exit token so the retry path joins this
    // semaphore again instead of dead-waiting on a fresh, empty one.
    xSemaphoreGive(exitedSem_);
    return SpawnResult::ReapBlocked;
  }
  if (xTaskCreateStaticPinnedToCore(taskTrampoline, "fibpprefetch", kStackBytes, this, kPriority,
                                    reinterpret_cast<StackType_t*>(taskStack_), &taskTcb_, kCore) == nullptr) {
    // Same failure visibility as the old dynamic spawn; the buffers persist
    // for the next respawn attempt.
    LOG_ERR("PREF", "Task respawn failed");
    return SpawnResult::Failed;
  }
  return SpawnResult::Started;
}

void FibpPrefetchWorker::notifyGeneration(const uint32_t generation, const LayoutParams& pods) {
  // The generation lands first: the yield hook aborts the in-flight session
  // at the next chunk boundary, then the pass re-seeds under the new value.
  gen_.store(generation, std::memory_order_release);
  // Params + paramGen_ are published atomically under the mutex. Until the
  // swap lands, the worker's publish-wait holds it back (a stale snapshot
  // built under the new generation would commit mismatched layout data).
  if (paramsMux_ != nullptr && xSemaphoreTake(paramsMux_, pdMS_TO_TICKS(kJoinTimeoutMs)) == pdTRUE) {
    copyScalarParams(pods);
    paramGen_ = generation;
    xSemaphoreGive(paramsMux_);
  } else {
    LOG_ERR("PREF", "Params publish timed out — worker idles until republished");
  }
  // Respawn after a "fully indexed" self-exit so later settings changes can
  // re-index under the new generation (see ensureTask()) — but only when a
  // threshold event had actually fired: a generation change alone must not
  // start indexing before the 10%-remaining trigger (CodeRabbit finding).
  // Re-arm the spawn dedup so the same spine can fire again under the new
  // generation.
  const bool fired = lastPrefetchFiredSpine_.load(std::memory_order_acquire) != fibp::kNoChapter;
  // Re-arm the spawn dedup only when no task is running: a live task replans
  // under the new generation and rebuilds the window itself, so clearing here
  // would let the same spine fire a redundant re-index after that task exits.
  if (fired && !running_.load(std::memory_order_acquire)) {
    lastPrefetchFiredSpine_.store(fibp::kNoChapter, std::memory_order_release);
    ensureTask();
  }
}

bool FibpPrefetchWorker::cancel() {
  cancel_.store(true, std::memory_order_release);
  // Join whenever a task could exist (semaphore alive), regardless of the
  // running_ snapshot — the exiting task gives as its final act.
  if (exitedSem_ != nullptr) {
    if (xSemaphoreTake(exitedSem_, pdMS_TO_TICKS(kJoinTimeoutMs)) != pdTRUE) {
      // Pathological: the worker is wedged inside a step. Report failure so
      // the caller RELEASES ownership without destroying: freeing state the
      // task still touches would be a use-after-free (the logged leak is the
      // safer failure mode for firmware).
      LOG_ERR("PREF", "Join timeout — worker state intentionally leaked");
      return false;
    }
  }
  // The join token lands BEFORE vTaskDelete(nullptr): the terminating task
  // still runs its final instructions on its stack, and a self-deleted
  // static task stays linked on xTasksWaitingTermination until the idle task
  // unlinks it — the TCB lives inside THIS object (kody finding). Returning
  // now would free taskStack_ and let the caller destroy the object, then
  // the unlink writes into freed memory. Wait bounded for the reap; on
  // timeout keep the whole object alive: leak beats use-after-free (same
  // contract as the join-timeout path above — the caller releases
  // ownership without destroying).
  if (exitedSem_ != nullptr && taskStack_ != nullptr && !reapSettled(kReapWaitMaxMs)) {
    LOG_ERR("PREF", "Worker task not reaped at cancel — leaking worker + stack");
    return false;
  }
  if (exitedSem_ != nullptr) {
    vSemaphoreDelete(exitedSem_);
    exitedSem_ = nullptr;
  }
  if (paramsMux_ != nullptr) {
    vSemaphoreDelete(paramsMux_);
    paramsMux_ = nullptr;
  }
  task_ = nullptr;
  // Release the session-persistent stack back to the DRAM heap (dram-x3
  // Finding 1). Safe even if the self-deleted task is still on the kernel's
  // termination list: the idle reap of a static task only unlinks the TCB
  // node, never reads or frees the stack. The join-timeout path above returns
  // before this point — a wedged task keeps its state (leak beats UAF) and
  // the stack leaks with the rest of the object.
  if (taskStack_ != nullptr) {
    heap_caps_free(taskStack_);
    taskStack_ = nullptr;
  }
  // A re-begin on this same object (cancel + begin without destroy) must be
  // able to reserve again: the session boundary resets the attempted flag.
  stackReserveAttempted_ = false;
  if (runtime_ != nullptr) runtime_->close();
  runtime_.reset();
  teardownFaces();
  resumeSpine_.store(fibp::kNoChapter, std::memory_order_release);
  queue_.clear();
  queue_.shrink_to_fit();
  failed_.clear();
  failed_.shrink_to_fit();
  sessionGen_ = 0;
  running_.store(false, std::memory_order_release);
  LOG_DBG("PREF", "Worker cancelled");
  return true;
}

// ── worker task ────────────────────────────────────────────────────────────

void FibpPrefetchWorker::taskTrampoline(void* arg) { static_cast<FibpPrefetchWorker*>(arg)->run(); }

void FibpPrefetchWorker::run() {
  if (!runtime_->open(epubPath_, cacheDir_)) {
    LOG_ERR("PREF", "Runtime open failed — worker done");
    running_.store(false, std::memory_order_release);
    task_ = nullptr;
    if (exitedSem_ != nullptr) xSemaphoreGive(exitedSem_);
    vTaskDelete(nullptr);
  }

  uint16_t lastNotifiedSeen = notifiedSpine_.load(std::memory_order_acquire);
  for (;;) {
    if (cancel_.load(std::memory_order_acquire)) break;
    building_.store(fibp::kNoChapter, std::memory_order_release);
    const uint32_t gen = gen_.load(std::memory_order_acquire);
    if (gen != sessionGen_) {
      // Generation bump (settings change): retry everything under the new
      // generation; the old in-flight session was already aborted by the
      // yield hook.
      sessionGen_ = gen;
      failed_.assign(spineCount_, 0);
      queue_.clear();
      queueCursor_ = 0;
    }
    // Reader-requested resume (soak addendum): the opened chapter's partial
    // completion takes priority over the prefetch plan — it is the chapter
    // the user is reading. Claim BEFORE the cache check (same race rule as
    // the plan path); a cache that went COMPLETE since the request drops the
    // claim instead of rewriting it.
    const uint16_t resume = resumeSpine_.load(std::memory_order_acquire);
    if (resume != fibp::kNoChapter && resume < spineCount_ && failed_[resume] == 0) {
      resumeSpine_.store(fibp::kNoChapter, std::memory_order_release);
      // Publish-wait BEFORE the cache check (same window as the plan path):
      // a notifyGeneration landing between gen_ read and the params swap must
      // not burn the claim on a Cancelled build — bailing here re-arms the
      // loop under the new generation. The claim itself is RESTORED: it is
      // pending restore/position state and must survive until the required
      // data is actually available (the reader's re-arm may not re-land it
      // after an arbitrary generation change).
      if (!waitForParamsPublished(gen)) {
        building_.store(fibp::kNoChapter, std::memory_order_release);
        resumeSpine_.store(resume, std::memory_order_release);
        continue;
      }
      building_.store(resume, std::memory_order_release);
      bool complete = false;
      if (runtime_->openChapterCache(resume, gen) == BookStatus::Ok) {
        complete = !runtime_->cachePartial();
        runtime_->closeChapterCache();
      }
      if (complete) {
        // Built by someone else since the request: serve, never rewrite.
        building_.store(fibp::kNoChapter, std::memory_order_release);
      } else {
        const ChapterRun r = buildSpine(resume, gen);
        if (r == ChapterRun::Failed) failed_[resume] = 1;
        building_.store(fibp::kNoChapter, std::memory_order_release);
        vTaskDelay(pdMS_TO_TICKS(kSpineDelayMs));
        continue;
      }
    }
    if (queue_.empty() || queueCursor_ >= queue_.size()) {
      // A fully-drained queue counts as empty: a respawned worker (shared
      // ensureTask) inherits the previous window's exhausted queue, and with
      // the one-spine cap treating it as fresh would leave the new window
      // unplanned — the task would self-exit without indexing anything.
      // The returned snapshot is the spine the plan was built from — using
      // it for lastNotifiedSeen closes the race where a notify lands between
      // the plan's internal read and a second read here: with the one-spine
      // cap that mismatch made the worker exit on an obsolete plan and miss
      // the new chapter's successor entirely.
      lastNotifiedSeen = replan(gen);
    }
    uint16_t spine = fibp::kNoChapter;
    while (queueCursor_ < queue_.size()) {
      const uint16_t candidate = queue_[queueCursor_++];
      if (failed_[candidate]) continue;
      if (candidate == notifiedSpine_.load(std::memory_order_acquire)) {
        // The entered chapter belongs to the reader (its sync rebuild owns
        // the spine while it is on screen) — the worker skips it here and
        // re-plans later if it is still unwritten.
        continue;
      }
      // Claim BEFORE the cache check so the reader's handoff sees a stable
      // claim; the plan is re-verified after claiming to close the race
      // against a chapter entry landing between the two checks.
      building_.store(candidate, std::memory_order_release);
      if (candidate == notifiedSpine_.load(std::memory_order_acquire) || spineHasCache(candidate, gen)) {
        building_.store(fibp::kNoChapter, std::memory_order_release);
        continue;
      }
      spine = candidate;
      break;
    }
    if (spine == fibp::kNoChapter) {
      // Planned spines all exist or failed. A newer chapter entry reorders
      // the plan (the tail spines may now be unwritten) — re-plan NOW: the
      // queue is exhausted, so the empty-check replan above never fires.
      // Otherwise done: fully indexed (attempted) → release the stack (R1).
      const uint16_t notified = notifiedSpine_.load(std::memory_order_acquire);
      if (notified != lastNotifiedSeen) {
        // Same single-snapshot rule: plan from and record the same value.
        lastNotifiedSeen = replan(gen);
        continue;
      }
      break;
    }

    // Heap floor (R1): wait instead of indexing into the reader's DRAM
    // budget; the wait also re-checks cancel/generation.
    while ((ESP.getFreeHeap() < kMinFreeHeap || ESP.getMaxAllocHeap() < kMinMaxAllocHeap) &&
           !cancel_.load(std::memory_order_acquire) && gen_.load(std::memory_order_acquire) == gen) {
      vTaskDelay(pdMS_TO_TICKS(kHeapWaitMs));
    }
    if (cancel_.load(std::memory_order_acquire) || gen_.load(std::memory_order_acquire) != gen) continue;

    if (!waitForParamsPublished(gen)) continue;

    const ChapterRun r = buildSpine(spine, gen);
    if (r == ChapterRun::Failed) failed_[spine] = 1;
    // Cancelled just falls through: the loop head re-seeds or exits.
    vTaskDelay(pdMS_TO_TICKS(kSpineDelayMs));
  }

  running_.store(false, std::memory_order_release);
  task_ = nullptr;
  if (exitedSem_ != nullptr) xSemaphoreGive(exitedSem_);
  vTaskDelete(nullptr);
}

uint16_t FibpPrefetchWorker::replan(const uint32_t generation) {
  // The generation argument documents the plan's context; the queue holds
  // spine indices only (the per-spine work re-reads the atomic). The
  // notified spine is read ONCE here and returned: callers use the returned
  // value for their change detection so plan and comparison share a
  // snapshot.
  (void)generation;
  // Capped window: the next chapter only (kPrefetchLookaheadSpines ahead of
  // the entered one). buildQueue's cap parameter keeps the window testable.
  const uint16_t notified = notifiedSpine_.load(std::memory_order_acquire);
  queue_.resize(fibp::kPrefetchLookaheadSpines);
  const uint16_t n = fibp::buildQueue(spineCount_, notified, queue_.data(), fibp::kPrefetchLookaheadSpines);
  queue_.resize(n);
  queueCursor_ = 0;
  return notified;
}

bool FibpPrefetchWorker::spineHasCache(const uint16_t spine, const uint32_t generation) {
  // Complete AND partial files count: the reader holds/serves existing
  // caches, and renaming over an open reader cache is never attempted.
  const BookStatus st = runtime_->openChapterCache(spine, generation);
  if (st == BookStatus::Ok) {
    runtime_->closeChapterCache();
    return true;
  }
  return false;
}

bool FibpPrefetchWorker::waitForParamsPublished(const uint32_t gen) {
  // gen_ lands before the params swap in notifyGeneration, so hold here until
  // paramGen_ (read under paramsMux_) reaches this pass's generation — a
  // stale snapshot built under the new generation would commit mismatched
  // layout data.
  for (;;) {
    bool published = false;
    if (paramsMux_ != nullptr) xSemaphoreTake(paramsMux_, portMAX_DELAY);
    published = (paramGen_ == gen);
    if (paramsMux_ != nullptr) xSemaphoreGive(paramsMux_);
    if (published) return true;
    if (cancel_.load(std::memory_order_acquire) || gen_.load(std::memory_order_acquire) != gen) return false;
    vTaskDelay(pdMS_TO_TICKS(kPageDelayMs));
  }
}

ChapterRun FibpPrefetchWorker::buildSpine(const uint16_t spine, const uint32_t generation) {
  const TickType_t startTicks = xTaskGetTickCount();
  lastPages_ = 0;
  progressPages_.store(0, std::memory_order_release);
  logSpine_ = spine;
  buildStartMs_ = millis();
  lastHookMs_ = buildStartMs_;
  // Snapshot the params under the mutex: the engine copies them at begin,
  // and a concurrent notifyGeneration must not tear the scalar set. Reject
  // a snapshot whose parameter-generation does not match the requested one
  // (the publish-wait in run() normally closes this window; this is the
  // last-line guard against committing stale layout under a valid gen).
  LayoutParams snapshot;
  bool matches = false;
  if (paramsMux_ != nullptr) xSemaphoreTake(paramsMux_, portMAX_DELAY);
  snapshot = params_;
  matches = (paramGen_ == generation);
  if (paramsMux_ != nullptr) xSemaphoreGive(paramsMux_);
  if (!matches) return ChapterRun::Cancelled;
  const ChapterIndexYield yield{this, &FibpPrefetchWorker::yieldHook};
  const ChapterRun r = runChapterBuild(*runtime_, spine, snapshot, generation, yield, /*chunkPages=*/1);
  const uint32_t ms = (xTaskGetTickCount() - startTicks) * portTICK_PERIOD_MS;

  if (r == ChapterRun::Completed) {
    // Telemetry (R5): one line per indexed spine + the R1 stack probe.
    uint32_t pages = 0;
    if (runtime_->openChapterCache(spine, generation) == BookStatus::Ok) {
      pages = runtime_->availablePageCount(spine);
    }
    runtime_->closeChapterCache();
    char href[64] = {};
    runtime_->catalog().spineHref(spine, href, sizeof(href));
    LOG_INF("PREF", "indexed %s pages=%u ms=%u hwm=%u", href, static_cast<unsigned>(pages), static_cast<unsigned>(ms),
            static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  } else if (r == ChapterRun::Failed) {
    LOG_ERR("PREF", "Build failed s%u gen=%08x ms=%u", spine, generation, static_cast<unsigned>(ms));
  } else {
    LOG_DBG("PREF", "Build cancelled s%u (cancel/gen)", spine);
  }
  return r;
}

bool FibpPrefetchWorker::yieldHook(void* ctx, const uint16_t pagesBuilt) {
  auto* self = static_cast<FibpPrefetchWorker*>(ctx);
  self->progressPages_.store(pagesBuilt, std::memory_order_release);
  const uint32_t now = millis();
  const uint32_t pageMs = now - self->lastHookMs_;
  self->lastHookMs_ = now;
  // Soak addendum: per-page PROF sample (≤ every 10 pages) quantifies the
  // hinting vs layout cost split on device — layout semantics unchanged.
  // advance/kerning time = the font-backend metric path (hinting cost
  // shows up here); the remainder of the page time is engine work. The
  // accumulator is DRAINED on every page (take semantics) so the sampled
  // value is the page this PROF line reports, not a 10-page accumulation.
  const uint64_t advUs = self->chain_.takeMeasureAccumUs();
  if (pagesBuilt == 1 || pagesBuilt % kIndexingProgressLogEveryPages == 0) {
    LOG_DBG("PROF", "phase=ttf_build_page spine=%u page=%u page_ms=%lu adv_us=%llu",
            static_cast<unsigned>(self->logSpine_), static_cast<unsigned>(pagesBuilt),
            static_cast<unsigned long>(pageMs), static_cast<unsigned long long>(advUs));
  }
  // Indexing progress at LOG_INF (soak finding #6): first page and every
  // kIndexingProgressLogEveryPages pages; the completion line covers the
  // last page. A stuck build is diagnosable from any log level.
  if (pagesBuilt == 1 || pagesBuilt % kIndexingProgressLogEveryPages == 0) {
    LOG_INF("TTFB", "Indexing spine=%u page=%u (elapsed %us)", static_cast<unsigned>(self->logSpine_),
            static_cast<unsigned>(pagesBuilt), static_cast<unsigned long>((now - self->buildStartMs_) / 1000));
  }
  // Active build tick: keep the CPU governor out of low power while pages
  // pump (soak finding #6: [PWR] low-power cycling throttled the build).
  powerManager.pokeNormalSpeed();
  if (fibp::shouldStopChunk(self->cancel_.load(std::memory_order_relaxed), self->gen_.load(std::memory_order_acquire),
                            self->sessionGen_)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(kPageDelayMs));  // R4: yield between pages
  return true;
}

}  // namespace book
}  // namespace freeink

#endif  // FIBP_WORKER_ENABLED
#endif  // CROSSPOINT_TTF_READER
