#include "ImagePredecodeWorker.h"

#if defined(BOARD_HAS_PSRAM) && defined(CROSSPOINT_TTF_READER)

#include <BookArena.h>
#include <BookTypes.h>
#include <CrossPointSettings.h>
#include <Epub/blocks/ImageDecodeGate.h>
#include <HalMemory.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "TtfImageEngine.h"

bool ImagePredecodeWorker::reserve() {
  if (queueStorage_ != nullptr) return true;  // idempotent

  queueStorage_ = static_cast<uint8_t*>(
      heap_caps_aligned_alloc(4, kQueueCapacity * sizeof(ttfimg::ImageSpec), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  // Fresh ring, always: a re-reserve after cancel() must not inherit the
  // previous book's cursor (the storage itself is new, uninitialized memory).
  queueHead_ = 0;
  queueCount_ = 0;
  if (queueStorage_ == nullptr) {
    LOG_ERR("IPR", "Queue alloc failed (%u B) — predecode disabled",
            static_cast<unsigned>(kQueueCapacity * sizeof(ttfimg::ImageSpec)));
    return false;
  }

  // Task stack in the same reserve window: a lazy first-spawn allocation
  // would meet a fragmented heap and silently disable the worker mid-session
  // (#292 review round 3 / FibpPrefetchWorker precedent).
  taskStack_ = static_cast<uint8_t*>(
      heap_caps_aligned_alloc(portBYTE_ALIGNMENT, kStackBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (taskStack_ == nullptr) {
    LOG_ERR("IPR", "Task stack alloc failed (%u B) — predecode disabled", static_cast<unsigned>(kStackBytes));
    heap_caps_free(queueStorage_);
    queueStorage_ = nullptr;
    return false;
  }

  // Scratch framebuffer: PSRAM-only (a 48 KB internal-DRAM scratch would be
  // the exact pressure this worker exists to avoid). HalMemory::allocatePsram
  // never falls back to internal RAM.
  scratchFbBytes_ = display.getBufferSize();
  scratchFb_ = HalMemory::allocatePsram(scratchFbBytes_).release();
  if (scratchFb_ == nullptr) {
    LOG_ERR("IPR", "Scratch framebuffer alloc failed (%u B) — predecode disabled",
            static_cast<unsigned>(scratchFbBytes_));
    heap_caps_free(taskStack_);
    taskStack_ = nullptr;
    heap_caps_free(queueStorage_);
    queueStorage_ = nullptr;
    return false;
  }
  if (!scratchRenderer_.beginWithScratch(scratchFb_, display.getDisplayWidth(), display.getDisplayHeight(),
                                         display.getDisplayWidthBytes())) {
    heap_caps_free(scratchFb_);
    scratchFb_ = nullptr;
    heap_caps_free(taskStack_);
    taskStack_ = nullptr;
    heap_caps_free(queueStorage_);
    queueStorage_ = nullptr;
    return false;
  }
  scratchBegan_ = true;

  // SDK decode arena: probe + row-decode scratch (JPEGDEC band, RowScaler
  // accumulators). PSRAM. The control object lives BEFORE the allocatable
  // region: alloc() bumps from the buffer base, so a control object placed
  // at the base would be overwritten by the first decode scratch allocation.
  static constexpr size_t kArenaCtrlBytes =
      (sizeof(freeink::book::Arena) + alignof(freeink::book::Arena) - 1) & ~(alignof(freeink::book::Arena) - 1);
  arenaBytes_ = HalMemory::allocatePsram(kArenaCtrlBytes + kDecodeArenaBytes).release();
  if (arenaBytes_ == nullptr) {
    LOG_ERR("IPR", "Decode arena alloc failed (%u B) — predecode disabled",
            static_cast<unsigned>(kArenaCtrlBytes + kDecodeArenaBytes));
    heap_caps_free(scratchFb_);
    scratchFb_ = nullptr;
    scratchBegan_ = false;
    heap_caps_free(taskStack_);
    taskStack_ = nullptr;
    heap_caps_free(queueStorage_);
    queueStorage_ = nullptr;
    return false;
  }
  decodeArena_ = new (arenaBytes_) freeink::book::Arena(arenaBytes_ + kArenaCtrlBytes, kDecodeArenaBytes);

  LOG_INF("IPR", "Reserved: queue=%ux%uB scratch=%uB psram stack=%uB arena=%uB psram", (unsigned)kQueueCapacity,
          (unsigned)sizeof(ttfimg::ImageSpec), (unsigned)scratchFbBytes_, (unsigned)kStackBytes,
          (unsigned)kDecodeArenaBytes);
  return true;
}

bool ImagePredecodeWorker::begin(Epub* epub, freeink::book::BookSource* source, const freeink::book::ZipCatalog* zip,
                                 const char* cacheDir, const uint32_t generation) {
  if (epub == nullptr || source == nullptr || zip == nullptr || cacheDir == nullptr || queueStorage_ == nullptr ||
      !scratchBegan_ || decodeArena_ == nullptr) {
    return false;
  }
  if (sessionValid_.load(std::memory_order_acquire)) {
    if (epub_ == epub) return true;  // already armed for this book
    // Re-open of a different book inside one reader session: cancel() frees
    // the queue and scratch, so re-reserve before re-arming.
    cancel();
    if (!reserve()) return false;
  }
  epub_ = epub;
  source_ = source;
  zip_ = zip;
  snprintf(cacheDir_, sizeof(cacheDir_), "%s", cacheDir);
  generation_.store(generation, std::memory_order_release);
  cancel_.store(false, std::memory_order_release);

  exitedSem_ = xSemaphoreCreateBinary();
  queueMux_ = xSemaphoreCreateMutex();
  queueSem_ = xSemaphoreCreateCounting(kQueueCapacity, 0);
  if (exitedSem_ == nullptr || queueMux_ == nullptr || queueSem_ == nullptr) {
    LOG_ERR("IPR", "OOM: worker semaphores — predecode disabled");
    if (exitedSem_ != nullptr) vSemaphoreDelete(exitedSem_);
    if (queueMux_ != nullptr) vSemaphoreDelete(queueMux_);
    if (queueSem_ != nullptr) vSemaphoreDelete(queueSem_);
    exitedSem_ = queueMux_ = queueSem_ = nullptr;
    // Buffers stay reserved (reserve() stays armed); the session gate below
    // keeps the worker disabled, so the scratch renderer is never driven.
    epub_ = nullptr;
    return false;
  }
  // The initial token means "no live worker task": an empty binary semaphore
  // would permanently fail the first spawnable(), predecode would never run,
  // and every cancel() would burn its join on a token no task could give.
  xSemaphoreGive(exitedSem_);
  sessionValid_.store(true, std::memory_order_release);
  LOG_INF("IPR", "Worker armed: gen=%08x queue=%ux%uB", (unsigned)generation, (unsigned)kQueueCapacity,
          (unsigned)sizeof(ttfimg::ImageSpec));
  // No spawn here: the task starts on the first enqueue that has work.
  return true;
}

bool ImagePredecodeWorker::enqueue(const ttfimg::ImageSpec& spec) {
  if (queueStorage_ == nullptr || !sessionValid_.load(std::memory_order_acquire) ||
      cancel_.load(std::memory_order_acquire)) {
    return false;
  }
  if (xSemaphoreTake(queueMux_, pdMS_TO_TICKS(kJoinTimeoutMs)) != pdTRUE) return false;
  // Duplicate suppression: the same image can appear on several pages (and a
  // rebuild re-fires sightings); one queued decode is enough. When the queued
  // record was captured under an older generation (layout changed while
  // queued), refresh it in place with the authoritative sighting — dropping
  // the new sighting would leave a record the dequeue discards as stale.
  bool refreshed = false;
  for (size_t i = 0; i < queueCount_; ++i) {
    const size_t slot = (queueHead_ + i) % kQueueCapacity;
    ttfimg::ImageSpec& other = *reinterpret_cast<ttfimg::ImageSpec*>(queueStorage_ + slot * sizeof(ttfimg::ImageSpec));
    if (strncmp(other.href, spec.href, ttfimg::ImageSpec::kHrefCap) == 0 && other.x == spec.x && other.y == spec.y &&
        other.w == spec.w && other.h == spec.h) {
      // Refresh in place with the authoritative sighting (fresh generation —
      // dropping the new sighting would leave a record the dequeue discards
      // as stale), but keep the HIGHER deferral count: the sighting's fresh
      // record would reset a deferred job's retry budget on every re-sight,
      // letting an image under steady memory pressure defer forever
      // (review 2026-11-11).
      ttfimg::ImageSpec updated = spec;
      if (other.deferrals > updated.deferrals) updated.deferrals = other.deferrals;
      other = updated;
      refreshed = true;
      break;
    }
  }
  if (!refreshed && queueCount_ == kQueueCapacity) {
    // Full: drop the oldest record (§5.2 reading order rolls forward). The
    // ring holds only the current + next spine, so overflow means a
    // pathologically image-heavy pair of chapters; the dropped images decode
    // on demand as today.
    queueHead_ = (queueHead_ + 1) % kQueueCapacity;
    --queueCount_;
    // The dropped job already left one token on the semaphore; take it back
    // so the token count stays equal to queueCount_.
    xSemaphoreTake(queueSem_, 0);
  }
  if (!refreshed) {
    pushJob(spec);
    // Give inside the locked section: a queued job and its dequeue token are
    // published atomically, so overflow accounting above stays exact.
    xSemaphoreGive(queueSem_);
  }
  xSemaphoreGive(queueMux_);
  // Spawn attempt runs for BOTH paths (fresh push and refresh-in-place):
  // a repeat sighting must retry a spawn a ReapBlocked deferral skipped,
  // or a queued job could wait behind a worker that exited unnoticed.
  // Spawn only when no task is running. Non-blocking: a previous task still
  // exiting (or not yet reaped) leaves the job queued; the next enqueue (the
  // next build tick's sighting) retries the spawn.
  if (!running_.load(std::memory_order_acquire) && !cancel_.load(std::memory_order_acquire) && spawnable()) {
    running_.store(true, std::memory_order_release);
    const SpawnResult spawned = spawnTask();
    if (spawned == SpawnResult::Started) {
      taskCreated_ = true;
      return true;
    }
    running_.store(false, std::memory_order_release);
    if (spawned == SpawnResult::Failed) {
      vSemaphoreDelete(exitedSem_);
      exitedSem_ = nullptr;
      task_ = nullptr;
    }
    // ReapBlocked: spawnable() restored the exit token; retry on the next enqueue.
  }
  return true;
}

void ImagePredecodeWorker::pushJob(const ttfimg::ImageSpec& spec) {
  const size_t tail = (queueHead_ + queueCount_) % kQueueCapacity;
  *reinterpret_cast<ttfimg::ImageSpec*>(queueStorage_ + tail * sizeof(ttfimg::ImageSpec)) = spec;
  ++queueCount_;
}

bool ImagePredecodeWorker::popJob(ttfimg::ImageSpec& out) {
  if (xSemaphoreTake(queueMux_, pdMS_TO_TICKS(kJoinTimeoutMs)) != pdTRUE) return false;
  if (queueCount_ == 0) {
    xSemaphoreGive(queueMux_);
    return false;
  }
  out = *reinterpret_cast<const ttfimg::ImageSpec*>(queueStorage_ + queueHead_ * sizeof(ttfimg::ImageSpec));
  queueHead_ = (queueHead_ + 1) % kQueueCapacity;
  --queueCount_;
  xSemaphoreGive(queueMux_);
  return true;
}

// Deferred = back of the ring (reading order within the spine is preserved
// for the jobs that fit; a deferred job lands behind its successors, which is
// the right fairness under memory pressure).
void ImagePredecodeWorker::deferJob(const ttfimg::ImageSpec& spec) {
  ttfimg::ImageSpec deferred = spec;
  deferred.deferrals++;
  if (deferred.deferrals > kMaxDeferrals) {
    LOG_DBG("IPR", "Dropping gate-starved job: %s", spec.href);
    return;
  }
  if (xSemaphoreTake(queueMux_, pdMS_TO_TICKS(kJoinTimeoutMs)) != pdTRUE) return;
  if (queueCount_ == kQueueCapacity) {
    xSemaphoreGive(queueMux_);
    return;  // no room: drop rather than evict a fresh job
  }
  pushJob(deferred);
  // The token spent on the popped job is handed back: the ring occupies one
  // token per queued job at all times (enqueue/defer both pair give with
  // push, the popped job's token is replaced here) — the dequeue or idle
  // path then never stalls a queued job behind a missing token.
  xSemaphoreGive(queueMux_);
  xSemaphoreGive(queueSem_);
}

bool ImagePredecodeWorker::abortTrampoline(void* ctx) {
  const auto* abort = static_cast<const AbortContext*>(ctx);
  // Two yield conditions, both checked at band boundaries only: the claim's
  // own abort (foreground took the same image / teardown), and the decode
  // bus request — the foreground needs the shared book surfaces (source
  // file handle, Epub ZIP extraction cache) and wins over a background job.
  return ImageDecodeGate::workerAbortRequested(abort->hash) || ImageDecodeGate::foregroundBusPending();
}

void ImagePredecodeWorker::processJob(const ttfimg::ImageSpec& spec) {
  const bool qualityTone = SETTINGS.imageDecodeQuality != 0;  // §5.6 revert switch
  // Picture detail (DPID design §Contract 4): CLAHE gate rides with the
  // quality tone policy — only meaningful when the quality bake runs.
  const bool clahe = qualityTone && SETTINGS.pictureDetail != 0;
  char cachePath[256];
  if (!ttfimg::cacheNameFor(spec, cacheDir_, qualityTone, clahe, cachePath, sizeof(cachePath))) {
    LOG_DBG("IPR", "Cache path overflow: %s", spec.href);
    return;
  }
  const uint64_t hash = ImageDecodeGate::pathHash(cachePath);

  // The cache can appear between enqueue and dequeue (foreground decoded it,
  // or an earlier sighting's job): re-check before any work (§5.4).
  if (ttfimg::hasValidCache(spec, cacheDir_, qualityTone, clahe)) return;

  // Foreground-activity pause: never checked mid-decode — an in-flight
  // decode only ends via the claim's abort flag (§5.7 rule 1).
  while (foregroundBusy_.load(std::memory_order_acquire) && !cancel_.load(std::memory_order_acquire)) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (cancel_.load(std::memory_order_acquire) || !sessionValid_.load(std::memory_order_acquire)) return;

  // DRAM + PSRAM gates (§5.7 rule 1): a decode that fails its gate is
  // deferred, not forced.
  if (!ttfimg::decodeGatesOk()) {
    // INFO on the first deferral only (the flash test reads the budget
    // headroom directly from serial — PSRAM budget 2026-11-09); retries stay
    // silent so a memory-stuck backlog cannot spam serial.
    if (spec.deferrals == 0) {
      LOG_INF("IPR", "Decode deferred (DRAM/PSRAM gate): %s psram free %u KiB", spec.href,
              static_cast<unsigned>(HalMemory::getPsramHeap().freeBytes / 1024));
    }
    deferJob(spec);
    return;
  }

  if (!ImageDecodeGate::tryAcquireWorker(hash)) {
    // Foreground owns this image right now and will produce the cache itself.
    return;
  }
  struct ClaimRelease {
    uint64_t hash;
    ~ClaimRelease() { ImageDecodeGate::releaseWorker(hash); }
  } claimRelease{hash};

  if (!sessionValid_.load(std::memory_order_acquire)) return;
  // Re-check AFTER acquiring the claim: the foreground can publish the cache
  // while this job waited for the claim; decoding now would truncate it.
  if (ttfimg::hasValidCache(spec, cacheDir_, qualityTone, clahe)) return;

  // Decode under the capture-time orientation: the converters and the scratch
  // paint clamp against an orientation-aware screen width (a portrait default
  // would zero-fill the right side of landscape placements).
  scratchRenderer_.setOrientation(static_cast<GfxRenderer::Orientation>(spec.orientation));

  // Decode-bus (§5.7 rule 4): the whole decode reads the SHARED book
  // surfaces — the SdCardBookSource's seek+read window and, for GIF/BMP, the
  // reader's Epub ZIP extraction cache. Per-path claims cannot serialize
  // those across jobs, so the worker holds the bus for the whole decode and
  // yields it at band boundaries when the foreground requests it.
  while (!ImageDecodeGate::tryAcquireDecodeBus()) {
    if (cancel_.load(std::memory_order_acquire)) return;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  // Arena mark/release: probe + row-decode scratch is per-job scratch, like
  // every other engine caller — without the mark the bump arena fills after
  // a handful of jobs and every later decode fails into the foreground.
  const size_t arenaMark = decodeArena_->mark();
  AbortContext abortCtx{hash};
  const ttfimg::DecodeResult res =
      ttfimg::decodeAndCache(spec, cachePath, *source_, *zip_, *decodeArena_, epub_, &scratchRenderer_, qualityTone,
                             /*caching=*/true, &ImagePredecodeWorker::abortTrampoline, &abortCtx,
                             /*paintToTarget=*/false, clahe);
  decodeArena_->release(arenaMark);
  ImageDecodeGate::releaseDecodeBus();
  // An aborted or failed decode never leaves a file (the engine drops the
  // partial cache on any failure path); "Ok" is a complete, valid cache the
  // foreground's next pass blits. A worker Failed IS a no-cache outcome — the
  // job produced nothing and a dropped decode means the cache stays cold for
  // its whole sighting window: retry bounded like the gate deferrals instead
  // of logging done (review 2026-11-10).
  if (res == ttfimg::DecodeResult::Failed) {
    deferJob(spec);
    return;
  }
  if (res != ttfimg::DecodeResult::Ok) {
    LOG_DBG("IPR", "Job skipped (kind %d): spine=%u %s", static_cast<int>(res), spec.spine, spec.href);
    return;
  }
  LOG_DBG("IPR", "Job done spine=%u page=%u %s (stack hwm=%u)", spec.spine, spec.page, spec.href,
          (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}

void ImagePredecodeWorker::taskTrampoline(void* arg) { static_cast<ImagePredecodeWorker*>(arg)->run(); }

void ImagePredecodeWorker::run() {
  for (;;) {
    if (cancel_.load(std::memory_order_acquire)) break;
    ttfimg::ImageSpec spec;
    if (xSemaphoreTake(queueSem_, pdMS_TO_TICKS(kIdleTimeoutMs)) != pdTRUE) {
      // Idle window elapsed: self-exit so the session-resident stack is the
      // only permanent cost. A later enqueue respawns (static buffers). The
      // exit decision runs under the ring mutex: an enqueue racing the exit
      // either defers its spawn to its next sighting or spawns itself.
      bool empty = false;
      if (xSemaphoreTake(queueMux_, pdMS_TO_TICKS(kJoinTimeoutMs)) == pdTRUE) {
        empty = queueCount_ == 0;
        if (empty) running_.store(false, std::memory_order_release);
        xSemaphoreGive(queueMux_);
      }
      if (empty) {
        // An enqueue can land after the empty decision: it publishes the job
        // under the ring mutex, then reads running_ (now false) and tries to
        // spawn — a spawn spawnable() refuses while THIS task has not yet
        // given exitedSem_. Re-peek the token after a settle delay so such a
        // late push is consumed here instead of waiting for the next
        // sighting (a refresh sighting carries no token, so it would not
        // wake this task otherwise).
        vTaskDelay(pdMS_TO_TICKS(2));
        if (uxSemaphoreGetCount(queueSem_) > 0) continue;
        break;
      }
      continue;
    }
    if (cancel_.load(std::memory_order_acquire)) break;
    if (!popJob(spec)) continue;
    if (spec.generation != generation_.load(std::memory_order_acquire)) continue;  // stale layout: drop
    processJob(spec);
    vTaskDelay(pdMS_TO_TICKS(kJobDelayMs));
  }
  running_.store(false, std::memory_order_release);
  task_ = nullptr;
  if (exitedSem_ != nullptr) xSemaphoreGive(exitedSem_);
  vTaskDelete(nullptr);
}

bool ImagePredecodeWorker::spawnable() {
  // Non-blocking exit-join: when a previous task could still exist, consume
  // its exit token (or bail) first so the final give cannot land on a fresh
  // handle. Then the reap probe: a self-deleted static task stays linked on
  // xTasksWaitingTermination until the idle task unlinks it — reusing the
  // buffers before that would corrupt the new task (FibpPrefetchWorker
  // pattern; the blocking variant lives in cancel()).
  if (exitedSem_ == nullptr) {
    exitedSem_ = xSemaphoreCreateBinary();
    if (exitedSem_ == nullptr) return false;
  } else if (xSemaphoreTake(exitedSem_, 0) != pdTRUE) {
    return false;  // previous task still exiting
  }
  if (xTaskGetHandle("imgpredecode") != nullptr) {
    // Still linked on the termination list: restore the token so the next
    // attempt joins instantly, and defer the spawn by one enqueue.
    xSemaphoreGive(exitedSem_);
    return false;
  }
  return true;
}

ImagePredecodeWorker::SpawnResult ImagePredecodeWorker::spawnTask() {
  if (taskStack_ == nullptr) {
    LOG_ERR("IPR", "spawnTask without a reserved stack");
    return SpawnResult::Failed;
  }
  if (xTaskCreateStaticPinnedToCore(taskTrampoline, "imgpredecode", kStackBytes, this, kPriority,
                                    reinterpret_cast<StackType_t*>(taskStack_), &taskTcb_, kCore) == nullptr) {
    LOG_ERR("IPR", "Task spawn failed");
    return SpawnResult::Failed;
  }
  return SpawnResult::Started;
}

bool ImagePredecodeWorker::cancel() {
  cancel_.store(true, std::memory_order_release);
  // Order matters: the session gate closes before the join, so a task between
  // two steps can never re-enter the source refs after this returns. A decode
  // still in flight is aborted at its next band boundary (otherwise a
  // multi-second large-image decode would outlive the bounded join and leak
  // the worker); waking the idle wait keeps the join prompt.
  sessionValid_.store(false, std::memory_order_release);
  ImageDecodeGate::abortAllWorkerClaims();
  if (queueSem_ != nullptr) xSemaphoreGive(queueSem_);
  if (exitedSem_ != nullptr) {
    if (xSemaphoreTake(exitedSem_, pdMS_TO_TICKS(kJoinTimeoutMs)) != pdTRUE) {
      LOG_ERR("IPR", "Join timeout — worker state intentionally leaked");
      return false;
    }
  }
  if (taskCreated_ && exitedSem_ != nullptr && taskStack_ != nullptr && !reapSettled(kReapWaitMaxMs)) {
    LOG_ERR("IPR", "Worker task not reaped at cancel — leaking worker + stack");
    return false;
  }
  if (exitedSem_ != nullptr) {
    vSemaphoreDelete(exitedSem_);
    exitedSem_ = nullptr;
  }
  if (queueMux_ != nullptr) {
    vSemaphoreDelete(queueMux_);
    queueMux_ = nullptr;
  }
  if (queueSem_ != nullptr) {
    vSemaphoreDelete(queueSem_);
    queueSem_ = nullptr;
  }
  if (taskStack_ != nullptr) {
    heap_caps_free(taskStack_);
    taskStack_ = nullptr;
  }
  taskCreated_ = false;
  // The ring cursors die with the storage they indexed (re-reserve gets a
  // fresh, uninitialized block — stale indices would read garbage specs).
  queueHead_ = 0;
  queueCount_ = 0;
  if (scratchFb_ != nullptr) {
    heap_caps_free(scratchFb_);
    scratchFb_ = nullptr;
    scratchBegan_ = false;
  }
  if (arenaBytes_ != nullptr) {
    if (decodeArena_ != nullptr) {
      decodeArena_->~Arena();
      decodeArena_ = nullptr;
    }
    heap_caps_free(arenaBytes_);
    arenaBytes_ = nullptr;
  }
  if (queueStorage_ != nullptr) {
    heap_caps_free(queueStorage_);
    queueStorage_ = nullptr;
  }
  epub_ = nullptr;
  source_ = nullptr;
  zip_ = nullptr;
  return true;
}

bool ImagePredecodeWorker::reapSettled(const uint32_t maxMs) {
  const uint32_t start = millis();
  while (xTaskGetHandle("imgpredecode") != nullptr) {
    if (millis() - start >= maxMs) return false;
    vTaskDelay(pdMS_TO_TICKS(kReapWaitStepMs));
  }
  return true;
}

#endif  // BOARD_HAS_PSRAM && CROSSPOINT_TTF_READER
