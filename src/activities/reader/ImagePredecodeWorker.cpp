#include "ImagePredecodeWorker.h"

#if defined(BOARD_HAS_PSRAM)

#include <Epub.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/converters/ImageToFramebufferDecoder.h>
#include <HalDisplay.h>
#include <HalMemory.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

namespace {  // (helpers live in the class: the reap wait needs the private step constant)
}  // namespace

bool ImagePredecodeWorker::reserve() {
  if (queueStorage_ != nullptr) return true;  // idempotent

  queueStorage_ = static_cast<uint8_t*>(
      heap_caps_aligned_alloc(4, kQueueCapacity * sizeof(Job), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (queueStorage_ == nullptr) {
    LOG_ERR("IPR", "Queue alloc failed (%u B) — predecode disabled",
            static_cast<unsigned>(kQueueCapacity * sizeof(Job)));
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
    heap_caps_free(queueStorage_);
    queueStorage_ = nullptr;
    return false;
  }
  if (!scratchRenderer_.beginWithScratch(scratchFb_, display.getDisplayWidth(), display.getDisplayHeight(),
                                         display.getDisplayWidthBytes())) {
    heap_caps_free(scratchFb_);
    scratchFb_ = nullptr;
    heap_caps_free(queueStorage_);
    queueStorage_ = nullptr;
    return false;
  }
  LOG_INF("IPR", "Reserved: queue=%ux%uB dram scratch=%uB psram", (unsigned)kQueueCapacity, (unsigned)sizeof(Job),
          (unsigned)scratchFbBytes_);
  return true;
}

bool ImagePredecodeWorker::begin(Epub* epub, const uint32_t generation) {
  if (epub == nullptr || queueStorage_ == nullptr || scratchFb_ == nullptr) return false;
  if (sessionValid_.load(std::memory_order_acquire)) {
    if (epub_ == epub) return true;  // already armed for this book
    cancel();                        // re-open of a different book inside one reader session
  }
  epub_ = epub;
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
  sessionValid_.store(true, std::memory_order_release);
  LOG_INF("IPR", "Worker armed: gen=%08x queue=%ux%uB scratch=%uB psram", (unsigned)generation,
          (unsigned)kQueueCapacity, (unsigned)sizeof(Job), (unsigned)scratchFbBytes_);
  // No spawn here: the task starts on the first enqueue that has work.
  return true;
}

bool ImagePredecodeWorker::enqueue(const Job& job) {
  if (queueStorage_ == nullptr || cancel_.load(std::memory_order_acquire)) return false;
  {
    if (xSemaphoreTake(queueMux_, pdMS_TO_TICKS(kJoinTimeoutMs)) != pdTRUE) return false;
    // Duplicate suppression: the same image can appear on several pages (and
    // a rebuild re-fires sightings); one queued decode is enough.
    for (size_t i = 0; i < queueCount_; ++i) {
      const Job& other =
          *reinterpret_cast<const Job*>(queueStorage_ + ((queueHead_ + i) % kQueueCapacity) * sizeof(Job));
      if (strncmp(other.imagePath, job.imagePath, Job::kImagePathCap) == 0) {
        xSemaphoreGive(queueMux_);
        return true;
      }
    }
    if (queueCount_ == kQueueCapacity) {
      // Full: drop the oldest record (design §3.2). The ring holds only the
      // current + next spine, so overflow means a pathologically image-heavy
      // pair of chapters; the dropped images decode on demand as today.
      queueHead_ = (queueHead_ + 1) % kQueueCapacity;
      --queueCount_;
      // The dropped job already left one token on the semaphore; take it back
      // so the token count stays equal to queueCount_.
      xSemaphoreTake(queueSem_, 0);
    }
    pushJob(job);
    // Give inside the locked section: a queued job and its dequeue token are
    // published atomically, so overflow accounting above stays exact.
    xSemaphoreGive(queueMux_);
    xSemaphoreGive(queueSem_);
  }
  // Spawn only when no task is running. Non-blocking: a previous task still
  // exiting (or not yet reaped) leaves the job queued; the next enqueue (the
  // next build tick's sighting) retries the spawn.
  if (!running_.load(std::memory_order_acquire) && !cancel_.load(std::memory_order_acquire) && spawnable()) {
    running_.store(true, std::memory_order_release);
    const SpawnResult spawned = spawnTask();
    if (spawned == SpawnResult::Started) {
      taskCreated_ = true;
    } else {
      running_.store(false, std::memory_order_release);
      if (spawned == SpawnResult::Failed) {
        vSemaphoreDelete(exitedSem_);
        exitedSem_ = nullptr;
        task_ = nullptr;
      }
      // ReapBlocked: spawnTask restored the exit token; retry on the next enqueue.
    }
  }
  return true;
}

void ImagePredecodeWorker::pushJob(const Job& job) {
  const size_t tail = (queueHead_ + queueCount_) % kQueueCapacity;
  *reinterpret_cast<Job*>(queueStorage_ + tail * sizeof(Job)) = job;
  ++queueCount_;
}

bool ImagePredecodeWorker::popJob(Job& out) {
  if (xSemaphoreTake(queueMux_, pdMS_TO_TICKS(kJoinTimeoutMs)) != pdTRUE) return false;
  if (queueCount_ == 0) {
    xSemaphoreGive(queueMux_);
    return false;
  }
  out = *reinterpret_cast<const Job*>(queueStorage_ + queueHead_ * sizeof(Job));
  queueHead_ = (queueHead_ + 1) % kQueueCapacity;
  --queueCount_;
  xSemaphoreGive(queueMux_);
  return true;
}

// Deferred = back of the ring (page order within a spine is preserved for the
// jobs that fit; a deferred job lands behind its successors, which is the
// right fairness under memory pressure).
void ImagePredecodeWorker::deferJob(const Job& job) {
  Job deferred = job;
  deferred.deferrals++;
  if (deferred.deferrals > kMaxDeferrals) {
    LOG_DBG("IPR", "Dropping gate-starved job: %s", job.imagePath);
    return;
  }
  if (xSemaphoreTake(queueMux_, pdMS_TO_TICKS(kJoinTimeoutMs)) != pdTRUE) return;
  if (queueCount_ == kQueueCapacity) {
    xSemaphoreGive(queueMux_);
    return;  // no room: drop rather than evict a fresh job
  }
  pushJob(deferred);
  xSemaphoreGive(queueMux_);
  xSemaphoreGive(queueSem_);
}

bool ImagePredecodeWorker::abortTrampoline(void* ctx) {
  const auto* abort = static_cast<const AbortContext*>(ctx);
  return ImageDecodeGate::workerAbortRequested(abort->hash);
}

void ImagePredecodeWorker::processJob(const Job& job) {
  const uint64_t hash = ImageDecodeGate::pathHash(job.imagePath);

  // The cache can appear between enqueue and dequeue (foreground decoded it,
  // or an earlier sighting's job): re-check before any work (§3.1).
  if (ImageBlock::hasValidCacheFor(job.imagePath, job.width, job.height)) return;

  // Foreground-activity pause (§3.4 rule 5): never checked mid-decode — an
  // in-flight decode only ends via the claim's abort flag (rule 2).
  while (foregroundBusy_.load(std::memory_order_acquire) && !cancel_.load(std::memory_order_acquire)) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (cancel_.load(std::memory_order_acquire) || !sessionValid_.load(std::memory_order_acquire)) return;

  // DRAM + PSRAM gates (§3.4 rule 5): a decode that fails its gate is
  // deferred, not forced.
  const HalMemory::HeapStats internal = HalMemory::getInternalHeap();
  if (internal.freeBytes < kMinFreeHeap || internal.largestBlockBytes < kMinLargestBlock) {
    deferJob(job);
    return;
  }
  if (HalMemory::getPsramHeap().freeBytes < kMinFreePsram) {
    deferJob(job);
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
  size_t imageSize = 0;
  PoolBytes image{epub_->extractItemToPsram(job.srcPath, &imageSize)};
  if (!image) {
    // Oversized (>4 MB cap), OOM, or missing item: the foreground's first
    // view handles it exactly as today (PSRAM fallback → SD staging). A
    // worker failure must never touch the foreground's failure bookkeeping.
    return;
  }
  if (ImageDecodeGate::workerAbortRequested(hash)) {
    // A foreground waiter flagged us during the extract: yield the claim, it
    // re-decodes from scratch.
    return;
  }

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(job.imagePath);
  if (decoder == nullptr) return;

  RenderConfig config;
  config.x = job.x;
  config.y = job.y;
  config.maxWidth = job.width;
  config.maxHeight = job.height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;
  config.cachePath = ImageBlock::cachePathFor(job.imagePath);
  AbortContext abortCtx{hash};
  config.abortPoll = &ImagePredecodeWorker::abortTrampoline;
  config.abortCtx = &abortCtx;

  decoder->decodeToFramebuffer(image.get(), imageSize, scratchRenderer_, config);
  // An aborted or failed decode never leaves a file: the converters drop the
  // partial cache on their existing failure paths, and a decode that "succeeds"
  // after the final band wrote everything is a complete, valid cache the
  // foreground's next pass loads. Never call rememberImageFailure here —
  // that bookkeeping is foreground-render state (§4).

  LOG_DBG("IPR", "Job done spine=%u %s (stack hwm=%u)", job.spine, job.imagePath,
          (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}

void ImagePredecodeWorker::taskTrampoline(void* arg) { static_cast<ImagePredecodeWorker*>(arg)->run(); }

void ImagePredecodeWorker::run() {
  for (;;) {
    if (cancel_.load(std::memory_order_acquire)) break;
    Job job;
    if (xSemaphoreTake(queueSem_, pdMS_TO_TICKS(kIdleTimeoutMs)) != pdTRUE) {
      // Idle window elapsed: self-exit so the session-resident stack is the
      // only permanent cost. A later enqueue respawns (static buffers).
      bool empty = true;
      if (xSemaphoreTake(queueMux_, pdMS_TO_TICKS(kJoinTimeoutMs)) == pdTRUE) {
        empty = queueCount_ == 0;
        xSemaphoreGive(queueMux_);
      }
      if (empty) break;
      continue;
    }
    if (cancel_.load(std::memory_order_acquire)) break;
    if (!popJob(job)) continue;
    if (job.generation != generation_.load(std::memory_order_acquire)) continue;  // stale layout: drop
    processJob(job);
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
  // pattern; the blocking variant lives only in cancel()).
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
    taskStack_ = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(portBYTE_ALIGNMENT, kStackBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (taskStack_ == nullptr) {
      LOG_ERR("IPR", "Stack alloc failed (%u B)", (unsigned)kStackBytes);
      return SpawnResult::Failed;
    }
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
  // two steps can never re-enter epub_ after this returns. Waking the idle
  // wait keeps the join prompt (the task's only long sleep is the queue wait).
  sessionValid_.store(false, std::memory_order_release);
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
  if (scratchFb_ != nullptr) {
    heap_caps_free(scratchFb_);
    scratchFb_ = nullptr;
  }
  if (queueStorage_ != nullptr) {
    heap_caps_free(queueStorage_);
    queueStorage_ = nullptr;
  }
  epub_ = nullptr;
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

#endif  // BOARD_HAS_PSRAM
