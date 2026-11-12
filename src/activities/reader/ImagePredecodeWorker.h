#pragma once

// ImagePredecodeWorker — background tone-cache producer for the TTF reader's
// image pass (design docs/design/2026-11-07-ttf-image-engine.md §5.1/§5.3).
// The sighting hook enqueues .fibp image records whose tone cache is absent;
// the worker claims the cache path in ImageDecodeGate and decodes through the
// SAME ttfimg::decodeAndCache path the foreground fallback runs (G3 parity by
// construction) into its PSRAM scratch; the finished .pxc is the shared
// 2-bit tone format the BITMAP engine reads back later at the blit site.
// The task shape mirrors FibpPrefetchWorker: static DRAM stack + static TCB
// (dram-x3 findings — exitedSem_ join plus the bounded idle-unlink reap wait
// before any buffer reuse), core 0, low priority, self-exit when the queue
// stays empty, respawn on the next enqueue. Inert stub on PSRAM-less builds —
// the C3 floor keeps today's on-demand decode exactly (§5.1).

#include <cstdint>
#include <memory>

class Epub;
class GfxRenderer;

namespace freeink {
namespace book {
class BookSource;
class ZipCatalog;
}  // namespace book
}  // namespace freeink

namespace ttfimg {
struct ImageSpec;
}

#if defined(BOARD_HAS_PSRAM) && defined(CROSSPOINT_TTF_READER)

#include <BookArena.h>  // freeink::book::Arena (book-side alias of freeink::font::Arena)
#include <FreeRTOS.h>
#include <GfxRenderer.h>
#include <esp_heap_caps.h>
#include <sdkconfig.h>
#include <semphr.h>
#include <task.h>

#include <atomic>
#include <memory>
#include <new>

#include "TtfImageEngine.h"

class ImagePredecodeWorker {
 public:
  // Main thread (reader entry, BEFORE the book open — the queue's DRAM block
  // must land while the heap is still whole, the same window and rationale as
  // the FibpPrefetchWorker stack reserve). Allocates the queue ring (DRAM),
  // the task stack (DRAM), the scratch framebuffer and the SDK decode arena
  // (both PSRAM) and begins the scratch renderer. Idempotent. False = the
  // worker stays disabled for the session and the foreground path is unchanged.
  bool reserve();
  // Main thread, after the book is open. Binds the decode sources, creates
  // the sync primitives, and arms the session (the task itself starts on the
  // first enqueue). Requires a successful reserve(). Re-arm re-runs reserve
  // after cancel() freed the buffers (book switch inside one reader session).
  bool begin(Epub* epub, freeink::book::BookSource* source, const freeink::book::ZipCatalog* zip, const char* cacheDir,
             uint32_t generation);
  bool begun() const { return sessionValid_.load(std::memory_order_acquire); }
  // Main thread. Layout generation change: later jobs not matching the new
  // generation are dropped at dequeue (their placement, and with it the .pxc
  // identity, is stale).
  void notifyGeneration(uint32_t generation) { generation_.store(generation, std::memory_order_release); }
  // Loop task (sighting hook). Pushes the job in reading order and spawns the
  // task when none is running. Never blocks: a spawn deferred by the reap
  // probe leaves the job queued for the next enqueue.
  bool enqueue(const ttfimg::ImageSpec& spec);
  // Loop task. True while the reader is inside the page render+display path;
  // the worker pauses BETWEEN jobs (never mid-decode) so it competes for no
  // SD bandwidth while the user is waiting on pixels.
  void setForegroundBusy(bool busy) { foregroundBusy_.store(busy, std::memory_order_release); }
  // Main thread. Stops the task (bounded join), releases any claim it still
  // holds, and frees the queue + scratch. False = wedged join: the caller
  // RELEASES ownership without destroying (leak beats use-after-free, the
  // FibpPrefetchWorker contract).
  bool cancel();
  bool active() const { return running_.load(std::memory_order_acquire); }

 private:
  static void taskTrampoline(void* arg);
  void run();  // worker task context (self-deletes)
  void processJob(const ttfimg::ImageSpec& spec);
  // Band-boundary abort hook handed to the engine (worker decodes only).
  struct AbortContext {
    uint64_t hash;
  };
  static bool abortTrampoline(void* ctx);
  // Bounded wait for the idle task's unlink of a self-deleted static task
  // (FibpPrefetchWorker reapSettled pattern): correctness-mandatory after a
  // live join before the stack/TCB are reused or freed.
  bool reapSettled(uint32_t maxMs);
  enum class SpawnResult : uint8_t { Started, Failed, ReapBlocked };
  SpawnResult spawnTask();
  bool spawnable();  // non-blocking exit-join + reap probe for enqueue
  void deferJob(const ttfimg::ImageSpec& spec);
  bool popJob(ttfimg::ImageSpec& out);
  void pushJob(const ttfimg::ImageSpec& spec);

  // Session constants (design §6 budget; the #292 heap-gate precedents).
  static constexpr size_t kStackBytes = 16 * 1024;
  // Owner-audit iteration 3 (2026-11-11): a deflated full-page JPEG needs
  // ~74–81 KiB inside the decode arena (inflate window + tinfl ~46 KiB,
  // skipBuf + TJpgDec ~9 KiB, srcW×16 band 15.5–22 KiB, RowScaler ~3.5 KiB);
  // 32 KiB forced every deflated 934–1400 px page through renderJpeg's
  // arena-retry step-down (half-res, uncached). 128 KiB covers it with
  // margin; PSRAM-backed (HalMemory::allocatePsram), outside the DRAM pool.
  static constexpr size_t kDecodeArenaBytes = 128 * 1024;
  static constexpr uint8_t kMaxDeferrals = 4;  // gate-failed retries before a job drops
  static constexpr uint32_t kIdleTimeoutMs = 4000;
  static constexpr uint32_t kJobDelayMs = 4;
  static constexpr uint32_t kJoinTimeoutMs = 3000;
  static constexpr uint32_t kReapWaitMaxMs = 50;
  static constexpr uint32_t kReapWaitStepMs = 2;
  static constexpr UBaseType_t kPriority = 1;
  static constexpr BaseType_t kCore = 0;  // the loop task (all rendering) runs on core 1
  static constexpr size_t kQueueCapacity = 64;

  std::atomic<bool> cancel_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> foregroundBusy_{false};
  std::atomic<bool> sessionValid_{false};  // epub_/source_/zip_ dereference gate
  std::atomic<uint32_t> generation_{0};

  Epub* epub_ = nullptr;                         // reader-owned; cleared by cancel()
  freeink::book::BookSource* source_ = nullptr;  // reader-owned (same lifetime)
  const freeink::book::ZipCatalog* zip_ = nullptr;
  char cacheDir_[160] = {};

  SemaphoreHandle_t exitedSem_ = nullptr;
  SemaphoreHandle_t queueMux_ = nullptr;
  SemaphoreHandle_t queueSem_ = nullptr;
  TaskHandle_t task_ = nullptr;

  // Fixed-capacity ring of POD specs; one session-lifetime DRAM heap block
  // (the FibpPrefetchWorker / #292 trade — inline record size dominates).
  uint8_t* queueStorage_ = nullptr;  // kQueueCapacity * sizeof(ttfimg::ImageSpec), DRAM
  size_t queueHead_ = 0;
  size_t queueCount_ = 0;

  // Worker-owned decode surfaces (reserve()-time allocations).
  uint8_t* arenaBytes_ = nullptr;
  freeink::book::Arena* decodeArena_ = nullptr;  // placement-new over arenaBytes_
  uint8_t* scratchFb_ = nullptr;
  size_t scratchFbBytes_ = 0;
  // GfxRenderer by value: the ctor only captures the display reference (no
  // allocation); beginWithScratch() points it at the scratch in reserve().
  GfxRenderer scratchRenderer_{display};
  bool scratchBegan_ = false;

  // Session-persistent static task storage (dram-x3 Finding 1 pattern).
  uint8_t* taskStack_ = nullptr;
  StaticTask_t taskTcb_ = {};
  bool taskCreated_ = false;
};

// The task TCB must never land in PSRAM (invalid for a task control block);
// allocation goes through the internal-DRAM-capped factory below, the size
// assert is the secondary guard (FibpPrefetchWorker.h, kody-rr9).
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

#else  // !BOARD_HAS_PSRAM || !CROSSPOINT_TTF_READER

// Inert stub (PSRAM-less boards keep today's on-demand decode). Methods are
// static: the stub touches no state, and cppcheck's functionStatic flags the
// implicit this otherwise. The reader's full call surface (reserve/begun,
// the owning pointer and its internal-DRAM factory) stays compile-clean so
// a rollback build keeps on-demand decoding; reserve() false keeps every
// enqueue path off.
class ImagePredecodeWorker {
 public:
  static bool reserve() { return false; }
  static bool begin(Epub*, freeink::book::BookSource*, const freeink::book::ZipCatalog*, const char*, uint32_t) {
    return false;
  }
  static bool begun() { return false; }
  static void notifyGeneration(uint32_t) {}
  static bool enqueue(const ttfimg::ImageSpec&) { return false; }
  static void setForegroundBusy(bool) {}
  static bool cancel() { return true; }
  static bool active() { return false; }
};

struct ImagePredecodeWorkerRawDelete {
  void operator()(ImagePredecodeWorker* p) const { delete p; }
};
using ImagePredecodeWorkerPtr = std::unique_ptr<ImagePredecodeWorker, ImagePredecodeWorkerRawDelete>;

inline ImagePredecodeWorker* makeImagePredecodeWorkerInternal() { return new (std::nothrow) ImagePredecodeWorker(); }

#endif  // BOARD_HAS_PSRAM && CROSSPOINT_TTF_READER
