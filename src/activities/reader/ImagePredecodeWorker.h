#pragma once

// ImagePredecodeWorker — background .pxc producer for the legacy EPUB reader
// path. After each section build tick the reader enqueues image sightings
// (images whose .pxc cache is absent); this worker claims each path in the
// ImageDecodeGate, extracts the compressed image into the PSRAM pool, decodes
// it through the same converters the foreground uses — into a worker-owned
// GfxRenderer over a PSRAM scratch framebuffer (GfxRenderer::beginWithScratch)
// with the .pxc as the kept output — and publishes the finished cache on SD.
// A first page-turn onto an image page then costs a cache load, not a decode.
//
// Protocol and lifecycle: docs/design/2026-11-06-image-predecode.md §3.3-§3.4.
// Task shape mirrors FibpPrefetchWorker: static DRAM stack + static TCB
// (dram-x3 findings — exitedSem_ join plus the bounded idle-unlink wait
// before any buffer reuse), core 0, lowest priority, self-exit when the queue
// stays empty, respawn on the next enqueue. Inert stub on PSRAM-less builds —
// the C3 floor keeps today's on-demand decode exactly (design §3.7).

#include <cstdint>
#include <memory>

class Epub;

#if defined(BOARD_HAS_PSRAM)

#include <Epub/blocks/ImageBlock.h>
#include <Epub/blocks/ImageDecodeGate.h>
#include <FreeRTOS.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <esp_heap_caps.h>
#include <semphr.h>
#include <task.h>

#include <atomic>
#include <new>

#if defined(__has_include)
#if __has_include(<sdkconfig.h>)
#include <sdkconfig.h>
#endif
#endif

class ImagePredecodeWorker {
 public:
  // POD job record: fixed path caps keep the ring allocation-free. Paths are
  // "<cacheDir>/epub_<hash>/img_<spine>_<n>.<ext>" (~50 B) and the zip item
  // href; longer sightings are refused at enqueue rather than truncated (a
  // truncated path would claim/decode a file nobody reads).
  struct Job {
    static constexpr size_t kImagePathCap = 96;
    static constexpr size_t kSrcPathCap = 112;
    char imagePath[kImagePathCap] = {};
    char srcPath[kSrcPathCap] = {};
    uint16_t spine = 0;
    int16_t width = 0;
    int16_t height = 0;
    int16_t x = 0;
    int16_t y = 0;
    uint32_t generation = 0;
    uint8_t deferrals = 0;  // heap/PSRAM gate retries before the job drops
  };
  static constexpr size_t kQueueCapacity = 64;

  // Main thread (reader entry, BEFORE the EPUB open — the queue's DRAM block
  // must land while the heap is still whole, the same window and rationale as
  // the FibpPrefetchWorker stack reserve). Allocates the queue ring (DRAM)
  // and the scratch framebuffer (PSRAM) and begins the scratch renderer.
  // Idempotent. Returns false when an allocation fails: the worker stays
  // disabled for the session and the foreground decode path is unchanged.
  bool reserve();
  // Main thread, after the EPUB object exists. Binds the extraction source,
  // creates the sync primitives, and arms the session (the task itself starts
  // on the first enqueue). Requires a successful reserve().
  bool begin(Epub* epub, uint32_t generation);
  bool begun() const { return sessionValid_.load(std::memory_order_acquire); }
  // Main thread. Settings/geometry change: later jobs not matching the new
  // generation are dropped at dequeue (their layout, and with it the .pxc
  // geometry, is stale).
  void notifyGeneration(uint32_t generation) { generation_.store(generation, std::memory_order_release); }
  // Loop task (section sighting hook). Pushes the job in page order and
  // spawns the task when none is running. Never blocks: a spawn deferred by
  // the reap probe leaves the job queued for the next enqueue.
  bool enqueue(const Job& job);
  // Loop task. True while the reader is inside the page render+display path;
  // the worker pauses between jobs (never mid-decode) so it competes for no
  // SD/pool bandwidth while the user is waiting on pixels (§3.4 rule 5).
  void setForegroundBusy(bool busy) { foregroundBusy_.store(busy, std::memory_order_release); }
  // Main thread. Stops the task (bounded join), releases any claim it still
  // holds, and frees the queue + scratch. Returns false when the join timed
  // out: the caller must then RELEASE ownership without destroying the object
  // (leak beats use-after-free, the FibpPrefetchWorker contract).
  bool cancel();
  bool active() const { return running_.load(std::memory_order_acquire); }

 private:
  static void taskTrampoline(void* arg);
  void run();  // worker task context (self-deletes)
  void processJob(const Job& job);
  // Band-boundary abort hook handed to the converters via RenderConfig.
  struct AbortContext {
    uint64_t hash;
  };
  static bool abortTrampoline(void* ctx);
  // Bounded wait for the idle task's unlink of a self-deleted static task
  // (FibpPrefetchWorker reapSettled pattern): correctness-mandatory after a
  // live join before the stack/TCB are reused or freed.
  static bool reapSettled(uint32_t maxMs);
  enum class SpawnResult : uint8_t { Started, Failed, ReapBlocked };
  SpawnResult spawnTask();
  bool spawnable();  // non-blocking exit-join + reap probe for enqueue
  void deferJob(const Job& job);
  bool popJob(Job& out);
  void pushJob(const Job& job);

  // Session constants (§3.6 budget + ImageBlock's heap-gate precedents).
  static constexpr size_t kStackBytes = 16 * 1024;
  static constexpr size_t kMinFreeHeap = 24 * 1024;     // PXC_HEAP_RESERVE precedent
  static constexpr size_t kMinLargestBlock = 8 * 1024;  // PXC_MAX_ALLOC_RESERVE precedent
  static constexpr size_t kMinFreePsram = 256 * 1024;   // headroom under the pool's existing ~4 MB cap
  static constexpr uint8_t kMaxDeferrals = 4;           // gate-failed retries before a job drops
  static constexpr uint32_t kIdleTimeoutMs = 4000;      // empty-queue self-exit window
  static constexpr uint32_t kJobDelayMs = 4;            // pace between jobs
  static constexpr uint32_t kJoinTimeoutMs = 3000;
  static constexpr uint32_t kReapWaitMaxMs = 50;
  static constexpr uint32_t kReapWaitStepMs = 2;
  static constexpr UBaseType_t kPriority = 1;
  static constexpr BaseType_t kCore = 0;  // the loop task (all rendering) runs on core 1

  std::atomic<bool> cancel_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> foregroundBusy_{false};
  std::atomic<bool> sessionValid_{false};  // epub_ dereference gate (see cancel())
  std::atomic<uint32_t> generation_{0};
  Epub* epub_ = nullptr;  // reader-owned; cleared by cancel() before epub teardown

  SemaphoreHandle_t exitedSem_ = nullptr;  // worker gives before self-delete
  SemaphoreHandle_t queueMux_ = nullptr;   // guards the ring
  SemaphoreHandle_t queueSem_ = nullptr;   // counting: one token per queued job
  TaskHandle_t task_ = nullptr;

  // Fixed-capacity ring of POD jobs (design §3.2). Deviation from §3.6's
  // "<2 KB" queue line: inline path storage dominates (~224 B/record), so the
  // ring is one session-lifetime DRAM heap block (~14 KB) allocated at
  // reader-entry time, not .bss — same trade the FibpPrefetchWorker's 32 KB
  // stack reserve already makes on PSRAM boards.
  uint8_t* queueStorage_ = nullptr;  // kQueueCapacity * sizeof(Job), DRAM
  size_t queueHead_ = 0;
  size_t queueCount_ = 0;

  // Worker-owned decode target: a scratch framebuffer (PSRAM, panel-sized)
  // behind GfxRenderer::beginWithScratch. The converters write it (discarded)
  // and the .pxc (kept) — the same product the foreground's decode produces.
  // The ctor only stores the display reference (no allocation, no display
  // touch); beginWithScratch() points it at the scratch in begin().
  GfxRenderer scratchRenderer_{display};
  uint8_t* scratchFb_ = nullptr;
  size_t scratchFbBytes_ = 0;

  // Session-persistent static task storage (dram-x3 Finding 1 pattern).
  uint8_t* taskStack_ = nullptr;
  StaticTask_t taskTcb_ = {};
  bool taskCreated_ = false;
};

// The task TCB must never land in PSRAM (invalid for a task control block);
// allocation goes through the hard-capped factory below, the size assert is
// the secondary guard (FibpPrefetchWorker.h, kody-rr9).
#if !defined(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL)
#error \
    "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL missing from sdkconfig.h — ImagePredecodeWorker's TCB residency guard cannot compile"
#endif
static_assert(sizeof(ImagePredecodeWorker) <= CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL,
              "ImagePredecodeWorker grew past the PSRAM-prefer threshold; taskTcb_ would no longer be DRAM-resident");

// Pairs with the raw delete: the memory did not come from operator new.
struct ImagePredecodeWorkerRawDelete {
  void operator()(ImagePredecodeWorker* p) const {
    if (p == nullptr) return;
    p->~ImagePredecodeWorker();
    heap_caps_free(p);
  }
};
using ImagePredecodeWorkerPtr = std::unique_ptr<ImagePredecodeWorker, ImagePredecodeWorkerRawDelete>;

// Internal-DRAM-capped allocation (kody-rr9): the default allocator only
// PREFERS internal DRAM below CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL and may
// fall back to PSRAM under a fragmented heap — invalid for the TCB member.
inline ImagePredecodeWorker* makeImagePredecodeWorkerInternal() {
  void* raw = heap_caps_aligned_alloc(alignof(ImagePredecodeWorker), sizeof(ImagePredecodeWorker),
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (raw == nullptr) return nullptr;
  return new (raw) ImagePredecodeWorker();
}

#else  // !BOARD_HAS_PSRAM

// Inert stub (PSRAM-less boards keep today's on-demand decode).
class ImagePredecodeWorker {
 public:
  struct Job {};
  bool begin(Epub*, uint32_t) { return false; }
  void notifyGeneration(uint32_t) {}
  bool enqueue(const Job&) { return false; }
  void setForegroundBusy(bool) {}
  bool cancel() { return true; }
  bool active() const { return false; }
};

// The reader holds the worker through the same pointer type on all builds.
using ImagePredecodeWorkerPtr = std::unique_ptr<ImagePredecodeWorker>;

#endif  // BOARD_HAS_PSRAM
