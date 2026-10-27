# Queued input aggregation — jump instead of replay

Date: 2026-10-27
Issue: #259 (reporter feedback, [comment 6445472113](https://github.com/Belphemur/Xerif-eReader/issues/259#issuecomment-6445472113))
Status: Proposed — implementation lands in PR #263 on `feat/x3-x4-async-input`

## 1. Problem

The async-input fix (PR #263) made queued presses lossless, but the reader
now replays them one at a time. The reporter:

> in yours it'll queue the three actions, then refresh the screen three times
> to show it moving down, but in Crosspoint it'll just immediately jump to the
> third one.

Three quick Next presses produce three sequential full-screen refreshes
(each 1–2 s on the C3) stepping one page each, instead of one refresh landing
three pages ahead. Same complaint shape applies to list cursors.

## 2. Root cause (file:line evidence)

1. The async poll task queues every press AND release edge
   (`freeink-sdk/libs/hardware/InputManager/src/InputManager.cpp:317-324`,
   per-button saturating queues, `queueLen=16`).
2. The app layer drains exactly ONE edge per button per frame into a bitmask
   (`src/MappedInputManager.cpp:44-49`: `pressedEdges |= 1u << physical`).
   The count is destroyed here; leftover edges stay in the SDK queue and are
   consumed one per subsequent frame (SDK consume-per-check semantics,
   `InputManager.cpp:801-836`).
3. The reader consumes one turn per dispatch
   (`src/activities/reader/EpubReaderActivity.cpp:1924`, `:1985`, trigger
   mapping in `ReaderUtils.h:66-84`), and every turn ends in a full-page
   render. So N queued presses = N dispatch/refresh cycles.

## 3. Design — app-side edge counting, no SDK change

`InputManager::wasPressed()` consumes one queued edge per call. Calling it in
a loop drains the queue, so the count is recoverable at the app layer without
any SDK API addition or pin bump.

### 3.1 Counting primitive (MappedInputManager)

In `MappedInputManager::update()`, replace the single
`gpio.wasPressed(physical)` check per button with a drain loop:

```cpp
uint8_t pressedCount = 0;
while (gpio.wasPressed(physical) && pressedCount < kMaxQueuedEdges) ++pressedCount;
```

and the same for released edges. Latch `pressedEdgeCount[physical]` /
`releasedEdgeCount[physical]` alongside the existing bitmasks.

New API: `uint8_t wasPressedCount(Button)` / `wasReleasedCount(Button)`
(0 = no edge this frame). `wasPressed(Button)` keeps its existing meaning
(`count > 0`) so every existing consumer is unchanged.

`kMaxQueuedEdges = 16` matches the SDK queue length; the queue is saturating,
so the count is a lower bound only when the queue overflowed (pre-existing
behavior).

### 3.2 Consumer policy

- **Reader page turns (Phase 1)** — `EpubReaderActivity` (+ `XtcReaderActivity`):
  take the drained counts of the direction trigger sets, net them
  (`steps = nextCount - prevCount`), and jump `|steps|` pages in ONE render
  via a new `pageTurnBy(int steps)` that loops the existing single-step logic
  from `pageTurn(bool)` (`EpubReaderActivity.cpp:2515`: chapter crossing,
  end-of-book handling, prefetch promotion) without rendering between steps.
  `pendingManualTurn` becomes magnitude-carrying (`int8_t steps`) through the
  render gate. Reading stats count every page crossed (per-step), not one per
  jump, to preserve `totalPagesTurned` semantics.
  Tilt, power-button, and home-key triggers stay single-shot (they are state
  flags, not queued edges).
- **List cursors (Phase 2)** — options popup, `CatalogActivity`,
  `UiTabListActivity`, `LibraryListActivity`: cursor jump `min(count, cap)`
  rows in one render. Same primitive, per-activity opt-in.

### 3.3 Hold / auto-repeat interplay

A held button queues exactly ONE press edge; auto-repeat is generated
app-side from held time (`MappedInputManager.h:175`), not by extra queue
edges. Drain counting is therefore hold-safe by construction: held = count 1,
and repeat steps stay one-per-repeat-period (continuous scroll feel is
preserved, never aggregated). Long-press mode
(`ReaderUtils.h:67,75`: press-mode vs release-mode triggers) counts the same
edge class the activity actually checks.

### 3.4 Direction netting

Netting is per dispatch frame: `next` and `prev` counts are drained
independently; the reader applies the net. Net zero (e.g. Next, Prev, Next,
Prev) = no-op. Cross-frame residue is impossible: the drain loop empties the
button queue every frame.

## 4. Alternatives rejected

- **SDK count/peek API** (`queuedPressCount(btn)`): behavior-identical result
  but requires an SDK PR + submodule pin bump. The app-side drain loop needs
  neither. Revisit only if a time-window coalescing need appears.
- **Time-window coalescing** (defer dispatch X ms to batch presses): adds
  latency to every single press. Rejected — the UI must never feel dead
  (owner hard requirement). Count-what-is-queued-now is window-free: presses
  that arrived during the previous render are naturally batched.

## 5. Mandatory audits (deviation = STOP and report)

1. `beginInputFrame()` / drain-latched edge interaction
   (`MappedInputManager.cpp:19`, pinned-SDK note) — the drain loop must not
   fight the frame latch.
2. `resolvePowerDoubleClickWindow()` and every other consumer of the released
   edge bitmask (`MappedInputManager.h:83`) — behavior must be identical when
   counts are 1.
3. End-of-book + chapter-crossing under jump-N (`pageTurn` boundary paths,
   `saveFinishedProgress()` synchronous persist).
4. `pendingManualTurn` magnitude carry + render-gate re-entrancy (a jump
   pending when the gate opens must apply the full magnitude once).
5. Reading-stats dwell attribution when several pages are crossed in one
   render (`BookReadingStats`), plus remote-turn ordering (`RemoteTurnGate.h`).
6. Adjacent `wasPressed()` consumers (ButtonRemap, KeyboardEntry, SdFirmware
   confirm, etc.) stay behavior-identical.

## 6. Verification

- Host tests: queue three presses → `wasPressedCount() == 3`; jump renders
  once and lands +3 pages; netting (2 next + 1 prev → +1); hold produces
  count 1; end-of-book clamps.
- On device (X3): open a book, press Next 3× rapidly → ONE refresh lands
  3 pages ahead; options menu cursor jumps 3 rows; held button still
  auto-repeats stepwise; regression: single press, hold, power click window,
  EOB.