#pragma once

// FibpPrefetchWorker — background chapter-index (FIBP) prefetch for the
// native-TTF reader. On S3-class builds (PSRAM + FreeType backend) it runs a
// low-priority worker task pinned to core 0 that lays out spine chapters
// ahead of the reader and commits their FIBP files to SD, so chapter entry
// hits the cache instead of the blocking "Indexing" popup. Everywhere else
// (single-core, PSRAM-less, stb rollback) the class compiles to an inert
// stub and the synchronous rebuild path is unchanged.
//
// Threading model (R2, FreeType's documented rules):
//   - FtFont face CREATE/DESTROY happens on the MAIN thread only (begin /
//     cancel), on the same thread as BookFontLoader's own face churn — the
//     documented "face creation/destruction from one thread at a time" rule
//     is satisfied without an extra lock.
//   - The worker thread only calls per-face metrics/load APIs
//     (FT_Load_Glyph and siblings), which FreeType documents as thread-safe
//     as long as each FT_Face is used by one thread at a time.
//   - All SD access goes through the runtime's adapters (HalFile /
//     storageMutex); no mutex is held across a layout step.
//   - ChapterLayout is instance-scoped (re-entrant), so the reader may keep
//     building on core 1 while the worker lays out on core 0.

#if defined(CROSSPOINT_TTF_READER)

#include <layout/ChapterLayout.h>

#include <atomic>
#include <cstdint>
#include <vector>

#include "FibpPrefetchPolicy.h"

#if defined(BOARD_HAS_PSRAM) && defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
#define FIBP_WORKER_ENABLED 1
#else
#define FIBP_WORKER_ENABLED 0
#endif

#if FIBP_WORKER_ENABLED
#include <BookFontLoader.h>
#include <FreeRTOS.h>
#include <Memory.h>
#include <esp_heap_caps.h>
#include <semphr.h>
#include <task.h>

#include "TtfBookRuntime.h"
#endif

namespace freeink {
namespace book {

class FibpPrefetchWorker {
 public:
  // Main-thread snapshot handed to begin(). `layout`'s pointer fields
  // (font/language/stylesheet/hyphenator) are IGNORED — the worker re-binds
  // them to its own face chain and language buffer (R2: never the render
  // path's faces).
  struct BeginContext {
    static constexpr size_t kEpubPathCap = 256;
    static constexpr size_t kCacheDirCap = 192;
    static constexpr size_t kFamilyCap = 48;
    static constexpr size_t kLanguageCap = 16;
    char epubPath[kEpubPathCap] = {};
    char cacheDir[kCacheDirCap] = {};
    char familyName[kFamilyCap] = {};
    uint16_t spineCount = 0;
    uint32_t generation = 0;
    LayoutParams layout{};  // scalar fields only; pointers re-bound
  };

#if FIBP_WORKER_ENABLED
  // NO destructor: the owner must call cancel() first. When cancel() returns
  // false (wedged join) the owner releases ownership without destroying —
  // member destruction cannot honor that conditional-leak contract.

  // Main thread. Builds the worker's own face chain from the selected
  // family (reading the same font bytes) and spawns the worker task.
  // Returns false (worker inert, sync path unchanged) when the family is
  // unknown, a face fails, or the task cannot start.
  bool begin(const BeginContext& ctx);
  // Main thread. Reports the reader's current position (0-based page inside
  // the spine, pageCount total pages of that chapter). The WORKER applies
  // the 10%-remaining policy (fibp::shouldPrefetchNext): the next chapter is
  // enqueued only when the position crosses the threshold — page turns
  // inside the same spine store the position but never rebuild the queue.
  // `lengthFinal` is false while a build still owns the chapter, so a
  // growing page watermark is never mistaken for a short chapter.
  void notifyChapterProgress(uint16_t spine, uint16_t page, uint16_t pageCount, bool lengthFinal = true);
  // Main thread. Settings/geometry changed: the worker aborts any in-flight
  // session at the next chunk boundary and re-seeds the queue under the new
  // generation (scalar params are swapped under paramsMux_).
  // Respawns the task when it had already finished ("fully indexed").
  void notifyGeneration(uint32_t generation, const LayoutParams& pods);
  // Main thread. Marks the one-shot task stack as reserved (see the
  // taskStack_ members) WITHOUT spawning. Called by the reader at reader
  // entry, before the EPUB open loads the ZIP index cache — that cache can
  // leave no contiguous 32 KB DRAM block for the first spawn (2nd soak,
  // build 0e652a97). Idempotent; returns false once the reserve failed, and
  // the reserve is NOT retried later (prefetch stays disabled for the
  // session, same failure shape as a failed first spawn). The StaticTask_t
  // TCB needs no allocation: it is the DRAM-resident member taskTcb_,
  // pinned by the static_assert below.
  bool reserveStack();
  // Main thread. Spawns the worker task when it is not running (initial
  // spawn and self-exit respawns — the capped window makes the task exit
  // after each window drains, so chapter entries must respawn it). Returns
  // true when a task is (or already was) running.
  bool ensureTask();
  // Main thread. Soak addendum: the reader hands a PARTIAL current chapter
  // to the worker so it finishes off-task while the reader paints its target
  // page from the partial immediately. Idempotent; the run loop claims it at
  // the next pass boundary. Cleared by the worker after it builds (or finds
  // the cache already complete) and invalidated on cancel().
  void requestResumeClaim(uint16_t spine) { resumeSpine_.store(spine, std::memory_order_release); }
  // Main thread. Stops the task (bounded join) and frees the worker's
  // faces, bytes, and runtime. Returns false when the join times out: the
  // caller must then RELEASE ownership without destroying the object (its
  // state is still touched by the wedged task — leak beats use-after-free).
  bool cancel();
  // True while the worker task is alive (spawning → joined).
  bool active() const { return running_.load(std::memory_order_acquire); }
  // Spine the task is currently laying out (fibp::kNoChapter when idle) —
  // the reader defers its own build of that chapter to the worker.
  uint16_t buildingSpine() const { return building_.load(std::memory_order_acquire); }
  // Pages the current build session has laid out (0 when none) — the
  // reader's estimate refines with this while a resume claim is in flight.
  uint16_t buildingProgressPages() const { return progressPages_.load(std::memory_order_acquire); }

  // R1 stack budget: 32KB DRAM start (Adobe CFF frames are deep even for
  // advance-only loads). 24KB overflowed on device: a long 93-page spine
  // measured HWM 1792B — below the 8KB floor this telemetry is gated on.
  // Validated via the per-spine HWM telemetry.
  static constexpr size_t kStackBytes = 32 * 1024;
  // Heap floors mirrored from the reader's background build gate (R1 DRAM
  // budget): the worker waits instead of indexing below these.
  static constexpr size_t kMinFreeHeap = 32 * 1024;
  static constexpr size_t kMinMaxAllocHeap = 16 * 1024;

 private:
  static void taskTrampoline(void* arg);
  void run();  // worker task context (self-deletes when done)
  bool buildFaces();
  void teardownFaces();
  void copyScalarParams(const LayoutParams& from);
  // True when a complete or partial cache for spine+generation exists on
  // SD (the reader serves those; a rename over an open reader cache is
  // avoided by never rebuilding existing files).
  bool spineHasCache(uint16_t spine, uint32_t generation);
  // Runs one spine's build session to completion (or the next cancel /
  // generation change). Returns the run outcome.
  ChapterRun buildSpine(uint16_t spine, uint32_t generation);
  // Blocks until paramGen_ (read under paramsMux_) reaches `gen` — notifyGeneration
  // stores gen_ before the params swap, so a build without this wait snapshots
  // stale scalars and buildSpine's last-line guard rejects them as Cancelled.
  // Returns false when the session is over (cancel or generation bump): the
  // caller must not build and should release its claim/claim-adjacent state.
  bool waitForParamsPublished(uint32_t gen);
  // Replans the queue from the current notified spine (capped window) and
  // returns the spine snapshot the plan was built from — callers must use it
  // for their notified-spine change detection so plan and comparison share
  // one read.
  uint16_t replan(uint32_t generation);
  // static-spawn helper (dram-x3 Finding 1): allocates taskStack_ once and
  // spawns the worker task on it; waits for the idle task to unlink a
  // self-deleted previous static task before reusing the buffers. ReapBlocked
  // leaves the exit token back on exitedSem_ so the next trigger retries the
  // same wait instead of timing out on a dead join (spawn failure keeps
  // today's delete-sem invariant).
  enum class SpawnResult : uint8_t { Started, Failed, ReapBlocked };
  SpawnResult spawnTask();
  SpawnResult waitForReap();
  static bool yieldHook(void* ctx, uint16_t pagesBuilt);

  static constexpr UBaseType_t kPriority = 1;
  static constexpr BaseType_t kCore = 0;         // the loop task (all rendering) runs on core 1
  static constexpr uint32_t kPageDelayMs = 1;    // R4: vTaskDelay(1+) between pages
  static constexpr uint32_t kSpineDelayMs = 30;  // R4: 20-50ms between spines
  static constexpr uint32_t kHeapWaitMs = 250;
  static constexpr uint32_t kJoinTimeoutMs = 3000;
  // Soak addendum: indexing progress is LOG_INF every N pages (first and
  // last page always log) so a stuck build is diagnosable at any LOG_LEVEL.
  static constexpr uint16_t kIndexingProgressLogEveryPages = 10;

  std::atomic<bool> cancel_{false};
  std::atomic<bool> running_{false};
  std::atomic<uint32_t> gen_{0};
  std::atomic<uint16_t> notifiedSpine_{fibp::kNoChapter};
  std::atomic<uint16_t> lastPrefetchFiredSpine_{fibp::kNoChapter};  // spawn dedup (notifyChapterProgress)
  std::atomic<uint16_t> building_{fibp::kNoChapter};                // spine the task is laying out
  std::atomic<uint16_t> resumeSpine_{fibp::kNoChapter};             // reader-requested partial resume claim
  SemaphoreHandle_t exitedSem_ = nullptr;                           // worker gives before self-delete
  SemaphoreHandle_t paramsMux_ = nullptr;                           // guards params_ scalar swaps
  TaskHandle_t task_ = nullptr;

  // Session-persistent static task storage (dram-x3 Finding 1): the worker
  // self-exits per one-spine plan, and respawning a dynamically-created task
  // frees its 32 KB stack right where fractional DRAM blocks (image pages)
  // pinch the hole — after a few cycles MaxAlloc settles below the spawn
  // requirement and prefetch dies for the session. Allocating the stack once
  // and reusing it with a static TCB removes the create/destroy churn: a
  // respawn can never fail on fragmentation because its required block is
  // never returned to the heap. 32.8 KB DRAM is then held for the whole
  // session — acceptable while reading, where no other subsystem requests
  // >32 KB DRAM (inflate/TTF-scratch/image decode go to PSRAM).
  //
  // Stack and TCB must stay DRAM-internal (a task stack in PSRAM would fault
  // the windowed ABI once the cache flips); heap_caps_aligned_alloc with
  // FreeRTOS' portBYTE_ALIGNMENT satisfies the kernel's stack-alignment
  // assert. Reuse is legal only after the previous task is fully deleted:
  // ensureTask() reuses them behind the exitedSem_ join plus a bounded wait
  // for the idle task's unlink of the self-deleted task.
  uint8_t* taskStack_ = nullptr;  // one-shot kStackBytes DRAM block, freed in cancel()
  StaticTask_t taskTcb_ = {};     // static TCB, part of this (DRAM-resident) object
  // Set by reserveStack() and cleared again in cancel() (the session
  // boundary). While set, spawnTask() never allocates: a failed reader-entry
  // reserve keeps prefetch disabled for the session (no spawn-time retry),
  // and a successful one makes the spawn-path allocation dead code.
  bool stackReserveAttempted_ = false;

  char epubPath_[BeginContext::kEpubPathCap] = {};
  char cacheDir_[BeginContext::kCacheDirCap] = {};
  char familyName_[BeginContext::kFamilyCap] = {};
  char language_[BeginContext::kLanguageCap] = {};
  uint16_t spineCount_ = 0;

  // Own face set (R2): created on the main thread, used by the worker task,
  // destroyed on the main thread after the join. Bytes are PSRAM-only
  // (poolMakeBytes with the loader's per-face size guard) so the fingerprint
  // the worker derives is content-identical to the loader's. The chain holds
  // non-owning pointers; the owners outlive it (reset after chain teardown).
  std::unique_ptr<NativeFace> faceOwners_[4] = {};
  FontChain chain_;
  PoolBytes fontBytes_[4];
  // §14.5 parity: streamed faces (beyond the loader's residency guard) get
  // their own open HalFile + PSRAM head prefix; created/destroyed with the
  // faces on the main thread.
  struct StreamSource {
    HalFile file;
    PoolBytes prefix;
    uint32_t prefixLen = 0;
  };
  StreamSource streamSources_[4];
  static unsigned long streamReadThunk(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count);
  uint32_t fingerprint_ = 0;  // content parity with BookFontLoader::computeFingerprint()

  // Session params: pointer fields bound to worker-owned objects at begin;
  // the scalar fields are swapped under paramsMux_ on notifyGeneration and
  // copied out at each spine start. paramGen_ is the generation the current
  // scalar set belongs to; it is published under paramsMux_ together with
  // the params (gen_ alone lands earlier, to abort the in-flight session),
  // so the worker never pairs a new generation with stale parameters.
  LayoutParams params_{};
  uint32_t paramGen_ = 0;  // guarded by paramsMux_ (set lock-free only in begin, pre-spawn)

  // Worker-thread build state.
  std::unique_ptr<TtfBookRuntime> runtime_;
  std::vector<uint8_t> failed_;  // 1 per spine that hard-failed this book
  std::vector<uint16_t> queue_;  // pending spine indices (worker-local)
  uint16_t queueCursor_ = 0;     // next index into queue_ to consider
  uint32_t sessionGen_ = 0;      // generation the current pass builds under
  uint16_t lastPages_ = 0;       // telemetry: pages of the last build
  // Live indexing progress for the reader's page-count estimate while the
  // worker holds a resume claim (0 when idle). Worker-task written, reader
  // polled — relaxed is fine for a display estimate.
  std::atomic<uint16_t> progressPages_{0};
  // Progress-log state (soak addendum): spine being indexed, build start,
  // and the previous yield's timestamp for the per-page PROF sample.
  uint16_t logSpine_ = 0;
  uint32_t buildStartMs_ = 0;
  uint32_t lastHookMs_ = 0;
#else
  // Inert stub (single-core / PSRAM-less / stb-rollback builds).
  bool begin(const BeginContext&) { return false; }
  void notifyChapterProgress(uint16_t, uint16_t, uint16_t, bool) {}
  void notifyGeneration(uint32_t, const LayoutParams&) {}
  void requestResumeClaim(uint16_t) {}
  bool cancel() { return true; }
  bool active() const { return false; }
  uint16_t buildingSpine() const { return fibp::kNoChapter; }
  uint16_t buildingProgressPages() const { return 0; }
#endif  // FIBP_WORKER_ENABLED
};

#if FIBP_WORKER_ENABLED && defined(SPIRAM_MALLOC_ALWAYSINTERNAL)
// The static TCB resides in this heap-allocated object; the allocator only
// keeps a block in internal DRAM while it is below the PSRAM-prefer
// threshold. A task TCB in PSRAM is invalid — pin the residency with a
// compile-time size check.
static_assert(sizeof(FibpPrefetchWorker) <= SPIRAM_MALLOC_ALWAYSINTERNAL,
              "FibpPrefetchWorker grew past the PSRAM-prefer threshold; taskTcb_ would no longer be DRAM-resident");
#endif

}  // namespace book
}  // namespace freeink

#endif  // CROSSPOINT_TTF_READER
