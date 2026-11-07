# Design: Image Predecode at Index Time (Parallel Worker, X4 Pro)

**Status:** Draft — pending owner review (decisions in §7)
**Date:** 2026-11-06
**Base:** `develop` @ d541026a (origin one deps-bump ahead; line citations re-derive with `grep -n '<anchor>' <file>`)
**Target:** X4 Pro (`x4pro`, ESP32-S3 dual-core LX7, 8 MB PSRAM). C3 floor unchanged.
**Scope:** Design doc only — no source edits.

---

## 1. Goal

The **first** view of an image page stalls: `ImageBlock::render()` decodes the image
synchronously on the loop task (core 1) before the page can display. Every later view
loads the `.pxc` cache instead and is fast. Owner requirement: detect images **as part
of indexing** and hand them to a **parallel task** so the decode happens off the render
path; on PSRAM boards the compressed image lives **only in PSRAM** and SD receives **only
the processed image** (the 2-bit `.pxc`).

Success metric (§6): a first page-turn onto an image page costs ~a cache load, not a
decode. Buttons must never feel dead during any of this (standing hard rule).

## 2. Current flow (verified, citations)

**Index time — detection already exists, pixel work deferred.**
`ChapterHtmlSlimParser.cpp:~1668` ("Found image"): resolves the src, format-checks via
`ImageDecoderFactory::isFormatSupported`, assigns the cache name
(`imageBasePath + counter + ext`, prefix set at `Section.cpp:590`), and probes dimensions
with `ImageDimsProbe` from a 1 KB early-aborted stream read — "extraction is deferred to
the first render of the page … keeps first-open of an image-heavy chapter from stalling
for seconds per image". The `ImageBlock` is created with `(imagePath, srcPath, w, h)`.
Nothing pixel-related is decoded at index time; the compressed image is never staged.

**Render time — the stall.**
`ImageBlock::render()` (`blocks/ImageBlock.cpp:356`):

1. `renderFromCache()` (:192) loads the `.pxc` (2-bit, 4 px/byte) — RAM slot for the
   multi-pass gray page (:96–169), else streamed from SD.
2. Cache miss → first view: PSRAM boards pull the compressed image into the PSRAM pool
   (`extractItemToPsram`, `lib/Epub/Epub.cpp:1366`, gated by
   `isImageSizeWithinPsramLimit` ~4 MB) and decode straight from it
   (`renderFromPsram` :322). SD staging (`extractFn`) is only the fallback. **The owner's
   PSRAM/SD rule is already the implemented behavior on PSRAM boards** — the design must
   preserve it, not introduce it.
3. The decode streams through the format converters (`JpegToFramebufferConverter` etc.)
   into `DirectPixelWriter` (framebuffer) **and** `PixelCache` (`converters/PixelCache.h`),
   which writes `.pxc` incrementally in ≤24 KB row bands — designed so a full-page image
   never needs its packed size (~88 KB) in DRAM at once.

**Threading today.**
All rendering *and* section builds tick on the Arduino loop task (core 1):
`section->buildSomeMore(...)` is called from `EpubReaderActivity.cpp` at 6 sites
(foreground current-spine builds at :1238/:3110/:3219/:3229/:5541, next-spine prefetch at
:1179/:1201). `FibpPrefetchWorker` (`src/activities/reader/FibpPrefetchWorker.h`) runs
FIBP spine builds on **core 0** (`kCore = 0`; the loop task runs core 1 — repo rule
`.agents/rules/ui-activities.md` §Task and Core Affinity). There is **no** image decode
off core 1 anywhere today.

**Shared state that blocks naive parallelism.**

- `ImageDecoderFactory::getDecoder()` returns **lazy singletons** (`ImageDecoderFactory.cpp:9-11`) —
  concurrent decode from two tasks races both the lazy-init and the shared instance.
- `DirectPixelWriter::init(GfxRenderer&)` reads framebuffer target, render mode,
  orientation, dither state off the live renderer — a worker must not touch the loop
  task's `GfxRenderer`.
- SD access serializes on `storageMutex`; PSRAM image pool is shared with the
  foreground probe/extract paths.

## 3. Architecture

```
loop task (core 1)                          ImagePredecodeWorker (core 0)
------------------------                    ----------------------------
buildSomeMore() tick ──► pages landed ──►   job queue (page order, capped)
  in LUT: scan new ImageBlocks                │ claim(path) ok?
  with !hasValidCache() ──► enqueue job ──►   │ extract compressed → PSRAM pool
                                              │ decode (cache-only sink) → .pxc on SD
page turn → ImageBlock::render()              │ release claim
  cache hit (warm) → fast load ✓              ▼
  cache miss → foreground claim wins,         pause while foreground active,
    worker cooperatively aborts current       heap-gated, throttled
    band-flush boundary and yields
```

### 3.1 Detection: scan the LUT, don't hook the parser

After each `Section::buildSomeMore()` tick (one site, `Section.cpp:676`), scan the pages
that landed in the LUT during the tick for `ImageBlock`s whose `hasValidCache()` is
false, and enqueue `{spine, srcPath, imagePath, width, height}` in page order.

- **DRY:** the built LUT is the single source of "which images exist and at what display
  size". A parser-level hook would duplicate that knowledge at a second site and miss
  sections whose LUT loaded from the section cache file — the LUT scan covers both.
- The parser's `imageRendering` gating (1/2) already prevents `ImageBlock` creation for
  suppressed images, so the scan inherits it for free.
- `hasValidCache()` re-check at enqueue time skips already-cached images cheaply; the
  worker re-checks again at dequeue (state can change between tick and dequeue).

### 3.2 Worker: `FibpPrefetchWorker` shape, not a new pattern

New `ImagePredecodeWorker` in `src/activities/reader/`, modeled on the proven lifecycle:

- static DRAM stack + static TCB, allocated once per reader session, reused behind the
  `exitedSem_` join + bounded idle-unlink wait (the dram-x3 static-task findings are
  directly applicable — do not re-derive them);
- `kCore = 0`, priority 1, self-exits when the queue drains or the activity exits
  (`vTaskDelete()` in `onExit()` per the task rules);
- generation awareness: jobs carry the spine's build generation; a generation change or
  `cancel()` drains the queue. Queue = fixed-capacity ring (≈64 small POD records, DRAM,
  no per-job allocation), enqueue drops oldest-when-full beyond the current+next spine.

### 3.3 The decode sink — decision needed (spike S1)

The worker needs the converter math (JPEG scale/crop, fixed-point resample, dither) but
must not write the loop task's framebuffer. Two options:

- **Option A — worker-owned `GfxRenderer` over a PSRAM scratch buffer (recommended; spike S1 verified the design).**
  `GfxRenderer`'s ctor is trivial (`GfxRenderer.h:147` — stores the display ref and
  defaults; no allocation) and everything the decode path reads is initialized by
  `begin()`, whose only display touch is `frameBuffer = display.getFrameBuffer()`
  (`GfxRenderer.cpp:181-190`). So the worker needs **no second display object**: construct
  the worker renderer against the existing global display reference and add one seam —
  `beginWithScratch(uint8_t* buf, int w, int h, int bytesPerRow)` — that performs
  `begin()`'s assignments against a caller-provided buffer (PSRAM scratch, X4 Pro fb =
  48 KB — trivial in 8 MB). The converters then run **unchanged**: decode writes the
  scratch fb (discarded) and the `.pxc` (kept) — the same product of the same code the
  foreground runs.
  *S1 findings (host-level code verification, 2026-11-06):*
  (a) `DirectPixelWriter::init()` reads renderer state (mode/orientation/strip/dither)
  that only affects the **framebuffer** write; the cache writer (`cw.writePixel`) receives
  the same post-dither 2-bit tone **before** any mode dispatch
  (`JpegToFramebufferConverter.cpp:~211-218`) — the cache payload is render-mode and
  orientation independent. A worker renderer at its defaults (BW, Portrait, no strip) is
  sound.
  (b) **Dither-origin dependence:** the Bayer dither indexes absolute `outX/outY`
  (`applyBayerDither4Level(gray, outX, outY)`), so `.pxc` bytes depend on the image's
  screen position. The job must carry the position captured at enqueue time:
  `PageImage` holds `xPos/yPos` (`Page.cpp:100-102`) and callers add the orientation's
  margins (`EpubReaderActivity.cpp:5421`). This position-dependence is **already an
  accepted property** of today's cache (a repeated image reuses the first-written cache
  regardless of later positions), so the worker must match today's semantics, not create
  a new invariant: capture `xPos/yPos` plus the current render spec's oriented margins in
  the job record (needs a `PageImage` position accessor — small, additive).
  (c) Parity is host-testable: decoders build on host, and the test infra exists
  (`test/gif_support/PixelCacheTest.cpp`, `HalFileTest` failure injection,
  `test/psram_image_staging/`). The parity test (same image, foreground path vs.
  worker path, identical `RenderConfig` + position → byte-identical `.pxc`) pins (a)+(b)
  permanently.
- **Option B — extract a pixel-sink interface** (`FramebufferSink` vs cache-only
  `CacheSink`) and refactor all three converters to write through it. DRY-pure but
  touches the most regression-sensitive code in the image path for no behavioral gain if
  A verifies.

**Decision log (DRY > SOLID > KISS):** A keeps the decode behavior in exactly one place
(DRY: the converters remain the single home of scale/dither knowledge) and adds a discard
shim at the HAL boundary, which the HAL already treats as replaceable (SOLID: the
renderer/display seam exists for exactly this). B duplicates risk: refactoring three
converters to reach a sink boundary the A-shim provides for free is machinery without a
job (KISS). A loses only if S1 finds a ctor/instance invariant that makes a second
renderer unsound — then B, scoped to the sink seam only, is the fallback.

### 3.4 Concurrency protocol (SOLID — these are load-bearing)

1. **Single writer per `.pxc`.** A claim registry keyed by `imagePath` (fixed table,
   DRAM). Worker takes the claim before decoding and releases it after (success,
   failure, or abort). A partial/aborted `.pxc` is deleted before release — a torn cache
   file must never survive (foreground `readValidCacheHeader` size-checks, but a
   size-valid torn payload is undetectable; deletion is the only safe abort).
2. **Foreground always wins.** `ImageBlock::render()` on a cache miss acquires the
   claim: free → foreground proceeds (worker's dequeue re-check skips it). Held by the
   worker → foreground sets the job's abort flag and waits bounded (~200 ms) on the
   claim; the worker checks the flag at every band-flush boundary (the natural
   granularity `PixelCache` already provides), deletes the partial file, releases, and
   the foreground re-decodes from scratch. Worst case is one wasted partial decode, never
   a corrupted cache and never a multi-second foreground wait.
3. **One decode at a time, cross-task.** A single decode mutex around
   `getDecoder()`+`decode*()` on **both** the worker and the foreground render path. It
   fixes the singleton lazy-init race and the shared-instance race in one place. It must
   never be held across an abortible wait: the worker releases it at band boundaries
   when a foreground waiter is flagged (same mechanism as rule 2 — one flag, one check,
   not two protocols).
4. **PSRAM pool fairness.** The worker uses the same `extractItemToPsram` pool as the
   foreground. Pool exhaustion or an in-flight foreground extract → job deferred
   (requeue at tail, bounded retries), never a foreground allocation blocked or failed by
   the worker.
5. **Foreground-activity pause.** Worker pauses while the reader is in the
   render+display path (an atomic "busy" flag the reader already toggles around page
   render, or a derived signal if one exists) and on any input activity — the
   never-dead-buttons rule outranks throughput. Between jobs: `vTaskDelay` ≥ one tick,
   DRAM heap gate (`freeHeap`/`maxAllocBlock` ≥ the `PXC_HEAP_RESERVE` precedent,
   `ImageBlock.cpp:110`), PSRAM budget check via `HalMemory` (separate internal/PSRAM
   reads per the hardware-resources rule).

### 3.5 Ordering and scope v1

1. Jobs arrive during the foreground build of the section being read — the decode of the
   first pages' images overlaps the remaining build ticks on core 1 (this is the owner's
   "parallel task as part of indexing").
2. Then the next spine's jobs as its prefetch build lands them; piggyback on the
   existing prefetch trigger rather than a second scheduler.
3. No look-ahead beyond next spine in v1; the queue cap (3.2) bounds memory and SD work.

### 3.6 Memory budget (X4 Pro)

| Item | Where | Size |
| --- | --- | --- |
| Worker stack + TCB | DRAM, static, session-resident | sized from measured HWM (start candidate 12 KB, verify by spike) |
| Compressed image | PSRAM pool (existing) | ≤ ~4 MB cap, one at a time |
| Scratch framebuffer (Option A) | PSRAM | 48 KB |
| Decoder + band buffers | DRAM/PSRAM per existing converters | ~44 KB working set (JPEGDEC ~20 KB + 24 KB band) |
| Job queue + claims + mutexes | DRAM | < 2 KB fixed |

DRAM check before every decode per rule 5; a decode that fails its gate is deferred, not
forced. No new PSRAM carve-outs — the pool's existing contract stands.

### 3.7 C3 / non-PSRAM boards

Compile-time gate on `BOARD_HAS_PSRAM` (mirrors `renderFromPsram`'s gating): C3 keeps
today's behavior exactly — no worker, no scratch renderer, no queue. A future SD-staged
predecode for C3 is explicitly out of scope; its 55 KB free heap makes background decode
a heap-fragmentation hazard, and the owner's rule targets PSRAM boards.

## 4. What this does NOT change

- `.pxc` format, header validation, the per-render RAM slot, the tiled-grayscale pass
  structure, and `ImageBlock`'s render contract (cache-first, placeholder on failure).
- The failure bookkeeping (`failedImageHashes`) stays foreground-only; a worker decode
  failure just leaves the cache absent — the foreground path discovers and handles it
  exactly as today on first view.
- FIBP prefetch scheduling, section cache format, indexing popups.

## 5. Measurement gate ("if it makes sense")

Before implementation, on-device baseline on X4 Pro with an image-heavy EPUB
(`BOOK_PROFILE` phase timings + serial log via `scripts/debugging_monitor.py`):
first-view image-page turn wall time, split decode vs. panel refresh vs. gray passes.
Repeat after implementation. Acceptance target set with owner (proposal: first-view
image-page turn within ~250 ms of a text-only page turn at equal panel refresh; decode
no longer visible in the interactive path). If the baseline shows decode already
overlapped by the e-ink refresh window in practice, this doc is shelved with the data —
that is a legitimate outcome of the gate.

## 6. Verification plan

1. **Host tests** (decoders are host-buildable): parity test — same image, foreground
   config vs. worker path, `.pxc` payload byte-identical (pins S1(b) permanently);
   claim/abort protocol unit tests including the partial-file deletion invariant.
2. **Gates per commit:** host ctest; `pio run -e x4pro` and `-e default` (C3 floor).
3. **On-device:** X4 Pro soak — image-heavy book cold open, jump to image page, page
   turns around image boundaries; verify no watchdog, no dead-input windows, `.pxc`
   files well-formed (foreground loads them), and worker never holds storageMutex during
   a page turn (log-verified).
4. **firmware-handoff** before any commit touching firmware; PR to the fork per the
   crosspoint PR-targeting rule.

## 7. Decisions (owner, 2026-11-06)

1. **Sink approach** — Option A (worker-owned renderer over PSRAM scratch via a
   `beginWithScratch` seam), per spike S1 findings in §3.3.
2. **Scope v1** — current + next spine only, in reading order, bounded queue.
3. **Acceptance target** — first-view image-page turn within ~250 ms of a text-only
   page turn at equal panel refresh (§5 gate measures it).
4. **Worker enablement** — all `BOARD_HAS_PSRAM` boards behind the existing flag; C3
   unchanged. Soak evidence comes from x4pro.

## 8. Execution plan (owner-approved 2026-11-06)

Supervised implementation run (pi): worker skeleton on the FibpPrefetchWorker pattern,
`beginWithScratch` seam, LUT-scan enqueue with position capture, claim/abort protocol,
host parity + protocol tests, `pio run -e x4pro` + `-e default` gates, on-device
measurement per §5. Standing rules in the brief: default/global git config only, no
Co-authored-by, separate commits for core/device/tests, firmware-handoff before
declared done, PR to the fork.

---

## 9. As-built (2026-11-06, branch `feat/image-predecode`)

Implemented on top of §7 with the following documented deviations:

1. **Detection site.** Pages are serialized to the section file and destroyed inside
   `Section::onPageComplete` (`Section.cpp:186`) — the LUT holds only file offsets, so a
   post-tick scan of Page objects is impossible. The "pages that landed this tick" scan
   fires from `onPageComplete` instead (single site, same coverage for build-tick pages;
   sections loaded wholly from cache have no build ticks under either formulation, and
   their images decode on demand as today). The §3.3(b) "PageImage position accessor"
   was not needed: `PageElement::xPos/yPos` are already public.
2. **§3.4 rule 3 mechanism.** No decode-spanning mutex. The converters' decode state is
   call-local (local `JPEGDEC`/`PNG`/`AnimatedGIF` per call — verified in the three
   converters), so concurrent decodes are safe by construction; the one real race is the
   factory's lazy singleton init, now guarded by a static init mutex inside
   `ImageDecoderFactory::getDecoder()`. A decode-spanning mutex would serialize worker
   decodes against the parser's in-build dimension probes
   (`ChapterHtmlSlimParser.cpp:1710/:1741`), which run on the loop task during indexing —
   blocking it breaks the buttons rule, while failing the probe yields zero-dimension
   ImageBlocks (broken layout).
3. **Worker enablement.** All `BOARD_HAS_PSRAM` boards per §7.4, but only for
   legacy-engine sessions (`SETTINGS.readerFontEngine == READER_ENGINE_BITMAP` on
   TTF-capable builds): the `.pxc`/`ImageBlock` path belongs to the Section reader — the
   native-TTF chain renders images through the SDK's `ImageRenderer` with no `.pxc`, so a
   TTF session would only idle the reservations (~14 KB DRAM queue + 48 KB PSRAM scratch).
4. **§3.6 queue budget.** The "<2 KB" line is unreachable with inline path storage
   (~224 B/record × 64). The ring is one session-lifetime DRAM heap block (~14 KB)
   allocated at reader entry (before the EPUB open, same window as the FibpPrefetchWorker
   stack reserve), not `.bss`.
5. **GIF images.** `GifToFramebufferConverter` has no memory-source decode variant, so a
   GIF job fails at the worker the same way the foreground's PSRAM path does; the cache
   is produced on first view by the existing SD-staging path. Not a regression.
6. **Sighting scope.** Pages behind the reader's current page are skipped at enqueue
   (their cache only matters when paging back, which decodes on demand as today).
7. **Cancel-time aborts.** `ImageDecodeGate::abortAllWorkerClaims()` flags every
   worker-held claim at reader teardown so an in-flight decode stops at its next band
   boundary instead of outliving the bounded `cancel()` join.

### 9.3 As-built addendum (review round 3)

- **Exit semaphore starts armed.** `begin()` gives the initial token after
  creating the exit semaphore; the token means "no live worker task". An
  empty binary semaphore made the first `spawnable()` take fail forever —
  the worker never spawned, and every `cancel()` burned its full 3 s join on
  a token no task could give. `spawnable()` re-creates the semaphore armed
  after a spawn failure deletes it. Supersedes the §3.3 "created empty" line.
- **Worker hardening batch (review round 3).**
  - `notifyGeneration()` is null-guarded in `renderBook()`: the worker object
    is optional per session (factory allocation or `reserve()` failure).
  - `enqueue()` refuses while the session is invalid: a `begin()` that OOM'd
    on its semaphores nulls the mutex while the buffers stay reserved.
  - The 16 KB task stack is a `reserve()`-time allocation (same DRAM window
    as the queue ring); a lazy first-spawn allocation could silently disable
    the worker on a fragmented heap.
  - Duplicate suppression refreshes a queued record captured under an older
    render generation instead of discarding the fresh sighting (which the
    dequeue would then drop as stale).
  - The idle-exit decision clears `running_` under the ring mutex, so an
    enqueue racing the exit either defers or respawns; a queued job is never
    left without a worker.
  - Gate claims (worker and foreground) key on the **.pxc path**, not the
    image path: two image files whose basenames collide after the extension
    strip must never hold two claims onto one cache file.
  - `processJob()` re-checks cache validity after acquiring the claim: the
    foreground can publish the cache while the job waits for the claim.
  - Jobs carry the capture-time `GfxRenderer::Orientation`; the worker
    renderer decodes under it, so landscape sessions no longer zero-fill
    columns past the portrait screen width.
  - `Epub::ZipFileCache` lookups are mutex-guarded and return `FileStatSlim`
    by value: the worker task reads entries beside the loop task's decode
    path, whose `invalidate()` could otherwise race a lookup or free the
    referenced node.
- **Test host fixes.** Per-process temp directory (parallel ctest safety);
  size guards before every byte comparison (`readAll()` returns an empty
  vector for a missing file, which made equality vacuous); `hashOf` is no
  longer `constexpr` (GCC 11 / C++20 rejects it — `pathHash` is runtime
  string based); JPEGDEC patches a build-owned copy in the build directory
  with the patch list registered as a CMake configure dependency.

### 9.4 Rebutted review findings (evidence, not applied)

- **PNG abort latency (coderabbit).** The pinned PNGdec stops on the first
  draw callback that returns 0: `png.inl:980-983` sets
  `pPage->iError = PNG_QUIT_EARLY; bDone = TRUE;`. The converter's callback
  (PngToFramebufferConverter.cpp:221-227) fires per row, sets
  `caching=false; aborted=true` and returns 0 — the remaining inflate is at
  most the current scanline, and the abandoned decode is strictly cheaper
  than the same decode without abort. A bounded-wait placeholder that
  self-heals on the next render pass is the documented §3.4 outcome.
- **Stale-position .pxc capture (qodo #9 / coderabbit).** A job dequeued
  under generation N-1 that publishes under N is accepted by
  `readValidCacheHeader()` only when dimensions coincide (±1). A cache whose
  dimensions match at a new capture position is in exactly the same class as
  the shipped foreground behavior on `develop`: the .pxc header carries only
  width/height (ImageBlock.cpp:43-58), so the foreground consumes a
  position-shifted cache the same way. Any position stamp/widening of the
  cache header is a cache-format change for both producers (owner §7 scope).
- **RAII for cap-matched raw pointers (kody).** The heap-caps allocation +
  raw pointer + explicit free on every failure path is the established
  FibpPrefetchWorker pattern (same review lineage: the kody-rr9 ruling on
  that campaign rejected smart-pointer wrapping for cap-matched blocks); the
  reader-facing object IS held by a `unique_ptr` with a `heap_caps_free`
  deleter (`ImagePredecodeWorkerPtr`).
- **Sighting coverage for fully cached sections (qodo #8).** Sightings fire
  from `Section::onPageComplete` (as-built §9 item 1): a section restored
  wholly from cache has no build ticks under either formulation, and its
  missing .pxc decodes on demand exactly as on `develop` — not a regression
  introduced by predecode. Extending sightings to the restore path is a
  reader-side change deferred past this PR.
- **Worker-held `Epub*` lifetime (coderabbit).** The reader calls
  `stopImagePredecode()` before section/EPUB teardown
  (EpubReaderActivity.cpp:389-391, :2528, :3019), and `cancel()` closes the
  session gate and aborts worker claims before its bounded join. The
  residual window (a decode-wide extract surviving the 3 s join) is the
  documented leak-beats-UAF trade of the FibpPrefetchWorker contract;
  shared `Epub` ownership is a lib surface change deferred past this PR.
