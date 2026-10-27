# Async input sampling: buttons and touch that never miss a window

Date: 2026-09-20
Campaign: render-perf (PR #155) soak follow-up — owner directive: "make the
button / UX using the touch screen interruption and have full control of the
UI when those happen", "check how to do async for the button".

## 1. Problem

Input on this firmware is **polled**: `InputManager::update()` samples the
button ladder once per main-loop tick and computes edges ("since the previous
update()"). Any main-task window longer than a human press (~150–300 ms)
therefore **drops** presses — the edge pair (down+up) happens entirely between
two samples, `currentState == lastState`, and no edge is ever emitted.

Measured busy windows on the render-perf branch (X4 Pro soak logs):

| window | length | source |
|---|---|---|
| cold-open inline build | 25.8 s | render pass pumping chapter layout (2–3.5 s per page) |
| full-page render (aa=1) | ~2.3 s | layout + base paint + dual-plane pass + flush |
| inline resume/takeover build | 2.9–7.1 s | reader-side build pump tick |
| prerender slice | ≤120 ms | chunked pump ([soak-fix6]) |
| e-ink full refresh | 1.3–2 s | driver busy-wait (yields via delay) |

`[LOOP] New max loop duration: 27584 ms` — for 27 s the loop never samples
input. The owner experienced exactly this: "can't do page turn while the
indexing bg is happening"; earlier: presses "feel dead" around turns.

The SDK already ships the machinery: `InputManager::beginAsync()` spawns a
polling task (default 15 ms) that keeps sampling during any busy window and
latches press edges into FreeRTOS queues (`popPress`, `popTouchTap`,
`popSwipe`, `popMultiTouch*`). It is **not wired into the firmware**, and its
contract is unusable for this app: "when async polling is active the app must
NOT call update()/wasPressed() itself" — it would drain presses only, losing
**release edges** (the reader turns pages on release), **level state** (holds,
long-press, auto-repeat), and the **hold machinery** (Confirm/Back digital
holds, power holds) that lives inside `update()`.

## 2. Design

Keep every existing app semantic. Make `update()` itself async-aware; the app
never learns about the queues.

### 2.1 SDK — InputManager

**Event record.** The async queue currently carries a bare `uint8_t` button
index and only press edges. Change the element to a packed record:

```cpp
struct AsyncInputEvent {
  uint8_t button;  // BTN_* index
  uint8_t kind;    // 0 = press, 1 = release
};
```

The async poller (`asyncPoll()`) queues **both** edge kinds after each of its
own `update()` passes (it can see `wasPressed`/`wasReleased` at 15 ms cadence,
so no edge can be missed). Synthetic events emitted by the hold machinery
inside the async task's `update()` (e.g. confirm-click on hold) flow out
through the same `wasPressed` scan.

**Async-aware `update()`.** Split by caller:

- *Called from the async task* (`xTaskGetCurrentTaskHandle() == _asyncTask`):
  the full real path — sample, debounce, hold machinery — exactly as today.
  This task now owns edge state at 15 ms cadence.
- *Called from any other task* (the app loop, and HalGPIO's wait loops):
  the **drain path** — pop every queued event and OR it into the latched edge
  registers (`pressedEvents` / `releasedEvents`), and refresh `currentState`
  from the async task's committed level snapshot. No hardware sampling, no
  debounce, no hold logic (the async task does that). Returns immediately.

**Latch semantics.** Drained edges accumulate (`|=`) until the app frame
reads them. `update()` must NOT clear the edge registers at entry in async
mode (today's one-shot semantics are preserved for the sync path). The frame
boundary moves to the app:

- `MappedInputManager::update()` (called exactly once per activity tick)
  calls a new `InputManager::beginInputFrame()` **before** `HalGPIO::update()`:
  clears the latched registers. Every edge queued since the previous frame
  (including those drained by HalGPIO's wait-loop `update()` calls at
  HalGPIO.cpp:239/242) is then visible to the activity for that tick, and
  exactly once.

**Level state.** `isPressed()` / `getState()` read `currentState`, which the
async task maintains; the drain path copies it (single-byte, benign race).
Hold timestamps (`buttonPressStart`/`buttonPressFinish`) are likewise
maintained on the async task and only read elsewhere.

**Touch.** `asyncPoll()` already queues taps/swipes/multi-touch. The drain
path pops each queue and replays into the one-shot flags + coordinate members
the sync path would have set (`touchPressedEvent`, `touchLongPressEvent`,
`multiTouchSwipeEvent`, `multiTouchRotationEvent`, `multiTouchPinchEvent`,
`touchHomeKeyEvent`…). Multiple drained events of one kind collapse to the
most recent coordinates for that frame (documented; tap routing is
position-agnostic in all current consumers, which take the single event per
tick).

**Sync behavior untouched.** When `beginAsync()` was never called, `update()`
keeps today's exact semantics (one-shot edges, cleared each call). No
behavior change for builds without the feature.

### 2.2 App wiring

- `HalGPIO::begin()` → `inputMgr.beginAsync(prio 2, pollMs 10, queueLen 16)`,
  **gated on PSRAM** (`BOARD_HAS_PSRAM`): the 5 queues + 4 KB task cost
  ~6.5 KB DRAM, which the C3/Classic (380 KB, no PSRAM) should not spend; the
  problem class being fixed is the X4 Pro/TTF reader. Non-PSRAM boards keep
  the current polled behavior.
- `MappedInputManager::update()` → `inputMgr.beginInputFrame()` first.
- `HalGPIO.cpp:239/242` wait-loop `update()` calls: no change — in async mode
  they drain-and-latch (they previously *preserved* liveness during e-ink
  waits; now they also pick up task-queued edges).

> **Addendum 2026-10-27 (every-board enablement, issue #259).** The
> `beginAsync()` gate is now `#if defined(BOARD_HAS_PSRAM) ||
> defined(XERIF_ASYNC_INPUT)`, with `XERIF_ASYNC_INPUT` defined for every env
> in `[base]` of platformio.ini — the C3 combined builds (Xteink X3/X4) run
> this same path, closing the press-loss-during-refresh gap that motivated
> the original PSRAM-only scoping above. That scoping is superseded by owner
> decision; the runtime cost is unchanged (~6.5 KB DRAM) and degradation on
> `xTaskCreate()` failure stays safe by construction (`asyncActive()` false
> keeps every consumer's `!asyncActive()` branch on the sync path). Board
> opt-out is the escape hatch only on later evidence: `-UXERIF_ASYNC_INPUT`
> in that one env. Root-cause evidence, interaction audits, and device
> verification: `docs/design/2026-10-26-x3-x4-async-input.md`. Note: the SDK
> host suite `test_x4_sync_path.cpp` (header comment in `run_x4_sync.sh`)
> still describes the sync path as "the path the C3 release binary runs" —
> that suite remains valid as a unit test of the sync path in isolation (its
> `InputManager` instance never calls `beginAsync()`), but the shipped C3
> firmware now runs async.

### 2.3 What each busy window becomes

| window | before | after |
|---|---|---|
| any of the table in §1 | presses dropped | sampled at 10 ms by the input task; edge delivered on the next app frame |
| page turn during bg indexing | dead | turn target advances immediately; render follows when the target page exists |
| press during e-ink refresh | lost | latched, delivered next frame |

Full UI control is then bounded only by *scheduling* (what the UI chooses to
do first when the frame sees the edge), not by *sampling*.

## 3. Alternatives rejected

- **App-side popPress() rewrite** (drain queues in the reader only): loses
  release edges, hold machinery, and levels — the reader's entire input
  grammar is release-driven. Would fork input semantics per activity.
- **GPIO ISR latching**: the six buttons are resistor-divider **analog**
  reads multiplexed on two ADC pins (readButtonAdc) — there is no per-button
  edge IRQ to hook. Touch (GT911) has an INT line but only for touch.
- **Making long windows shorter everywhere instead** (prerender slicing,
  per-tick pumps — already shipped in soak-fix2/3/5/6): necessary but
  insufficient — page-granularity layout (~2–3 s) cannot be split, and e-ink
  refreshes are driver-owned. Sampling must move off the main task.

## 4. Concurrency analysis

- Edge registers: written by drain path (main task) and async task's own
  `update()`; both are single-word OR/assign — races collapse to a lost
  duplicate of the SAME event at worst (async queued it; drain re-marks it).
  Acceptable: duplicate edges are idempotent for all consumers (an edge is a
  boolean per tick, not a count).
- Queues: FreeRTOS `xQueueSend/Receive` — thread-safe by construction.
- `currentState` snapshot: single byte, read-only on the main task.
- Hold timestamps: written on the async task, read on the main task — word
  reads are atomic on Xtensa/RISC-V; torn reads impossible.
- `s_buttonHook` (board I2C expander): called from the async task after
  begin — hook implementations must be thread-safe; the only hook
  (board buttons) is a digital read. Documented at the hook.

## 5. Test plan

- **Host (SDK suite)**: the drain/latch semantics are testable without
  FreeRTOS by injecting events through a test seam
  (`_testInjectAsyncEvent()` under a build guard, or a thin virtual): frame
  boundary clearing, accumulate-across-wait-loops, press+release in one
  frame, level snapshot. If the existing InputManager host suite cannot link
  FreeRTOS, cover the latch/frame policy in a new pure test unit and keep the
  queue plumbing device-verified.
- **Device soak (owner)**: `[INP]` LOG_DBG line when an edge is delivered on
  the first frame after a busy window >500 ms ("late edge: BTN_x, window
  Nms") — proves the press survived; page turns during bg indexing; holds and
  auto-repeat regression pass; touch taps during renders.

## 6. Risks

- Hold machinery now runs at 15 ms cadence on a lower-priority task: hold
  timing granularity improves (was loop-tick cadence, sometimes seconds).
  No consumer depends on coarse timing.
- The async task calls `update()` → `serviceTouch()` (GT911 I2C at ~66 Hz):
  same I2C bus as other per-tick work; the GT911 low-power throttle hook
  (`setLowPowerPolling`) must be honored by the async cadence (it is — same
  code path).
- Two `update()` callers racing (HalGPIO wait loop + async task): the
  wait-loop call takes the drain path (not the sampling path) — no
  concurrent hardware sampling. The ONLY sampler is the async task.

## 7. Implementation order (as-built @ SDK 3874506 / app 92227c4d+)

1. SDK: event record + release-edge queuing in `asyncPoll` — DONE (3874506).
2. SDK: task-local real path vs drain path in `update()`; latch registers;
   `beginInputFrame()` — DONE (3874506). Follow-ups (review-r6): the real
   path's internal reads of `releasedEvents`/`pressedEvents` are mode-aware
   (task registers on the poll task, shared latch on the app task — the
   shared latch is another task's data there), and `asyncActive()` exposes
   the mode to wait loops that must not drain.
3. SDK: touch queue drain replay — DEFERRED to v2 (documented in §2.1:
   touch one-shot flags accumulate on the poll task and are acknowledged
   per frame; queue replay would re-latch every poll and flood). The
   `pop*()` API stays for standalone consumers.
4. App: `MappedInputManager` frame boundary; `HalGPIO::begin` wiring
   (PSRAM-gated) — DONE (4377a802). Wait-loop audit (review-r6):
   `verifyPowerButtonWakeup` skips its `update()` calls while async
   polling is active (it reads physical levels only; draining there would
   erase queued edges unread).
5. Gates (ctest, clang-format, pio default + x4pro) + docs — DONE per
   commit; CHANGELOG and PR body carried the async-input section.
