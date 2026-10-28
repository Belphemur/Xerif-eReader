# Async input sampling on the X3/X4 (C3, no PSRAM) — extending the 2026-09-20 design

Date: 2026-10-26
Author: Hermes (orchestrator), on behalf of Antoine Aflalo (Belphemur)
Tracking issue: https://github.com/Belphemur/Xerif-eReader/issues/259
Baseline: `develop` @ `594745dc`, SDK pinned `d2db750`

## 1. Problem

Issue #259 (Xteink X3, Xerif v2.11.0): rapid button presses in menus are
dropped — each press must be spaced 0.5–1 s apart. The 2026-09-20 async-input
design (`docs/design/2026-09-20-async-input.md`) fixed exactly this class of
bug, but only for PSRAM boards.

### Root cause (verified against `594745dc` / SDK `d2db750`)

The X3/X4 ship in the combined C3 envs (`platformio.ini:363-377`,
`-DFREEINK_DEVICE_X3=1 -DFREEINK_DEVICE_X4=1`, no `BOARD_HAS_PSRAM`). Their
input path is the **sync polled** path:

- Sampling happens only in `HalGPIO::update()` → `inputMgr.update()`
  (`lib/hal/HalGPIO.cpp:118`), called once per loop tick
  (`src/main.cpp:1252` via `MappedInputManager.cpp:31`).
- Sync edges are one-shot and cleared at every sample
  (`freeink-sdk/.../InputManager.cpp:704-718`). A press whose full
  down+up cycle lands between two samples is lost.
- The dominant loop stall is the e-ink flush: a menu nav move ends in
  `OptionPopup::processRender()` → `renderer.displayBuffer()`
  (`src/components/OptionPopup.h:186`) → `Uc8279Driver::display()` →
  `displayFinish()`, which waits out the waveform inline
  (`Uc8279Driver.cpp:120-122, 203+`). During the flush the loop task is
  blocked and nothing samples input.
- The idle-trunk fresh-slice sampling (`src/main.cpp:1655-1675`) only runs in
  the power-saving inactive state, never during active navigation.

### Why the existing fix does not apply

`inputMgr.beginAsync(2, 10, 16)` is compiled only under
`#if defined(BOARD_HAS_PSRAM)` (`lib/hal/HalGPIO.cpp:108-115`). The comment
there records the 2026-09-20 decision: ~6.5 KB DRAM (4 KB task stack +
queues, SDK `InputManager.cpp:317-324`) was judged too expensive for the C3.
The X3/X4 therefore keep the historical polled behavior — issue #259 is the
residual gap.

The upstream comparison (issue question "why isn't this in CrossPoint?") was
audited statically: upstream's sync input semantics, `OptionPopup`,
`UiListActivity`, `HalDisplay`, and the X3 `Uc8279Driver` are identical to
this fork's; only runtime dynamics can differ. The reporter's expected
behavior ("click down 3× quickly, cursor moves 3 spaces") is exactly the
SDK's async consume-on-check semantics: edges queue with saturating counts
(`InputManager.cpp:801-867`), one edge delivered per tick.

## 2. Decision

Enable async input sampling on the Xteink C3 boards. The gate at
`lib/hal/HalGPIO.cpp:108` becomes:

```cpp
#if defined(BOARD_HAS_PSRAM) || defined(FREEINK_DEVICE_X3) || defined(FREEINK_DEVICE_X4)
```

Rationale:

- Both X3 variants (`XteinkX3`, `XteinkX3Uc8279`) and `XteinkX4` are compiled
  into the combined envs via the two device defines; detection is runtime
  (`XteinkDetect`), and `asyncActive()` degrades to sync automatically if
  task creation fails (SDK `InputManager.cpp:324` ignores an
  `xTaskCreate` failure — `_asyncTask` stays null and every consumer's
  `!inputMgr.asyncActive()` branch keeps the sync behavior). Degradation is
  safe by construction.
- Scope stays Xteink: sticky (no PSRAM by design, `platformio.ini:462`) and
  any other non-PSRAM board keep the sync path. The env-flag matrix must be
  verified (§4 Task 1): no env defines `FREEINK_DEVICE_X3/X4` unintentionally.
- Hardware feasibility is established: the Xteink ADC ladder polls dedicated
  GPIO1/GPIO2 (`InputManager.cpp:189-192`), never the display DC/RST pins;
  the digital nav-key branch is already excluded for `XteinkAdcLadder`
  (`HalGPIO.cpp:186-196`). The concurrent `rawInputActive()`/poll-task ADC
  combination is already in production on the X4 Pro.

Memory budget: ~6.5 KB DRAM at runtime (4 KB poll-task stack + ~1 KB queues +
TCB, `InputManager.cpp:317-324`). Static DRAM is unchanged; the allocation is
runtime heap. This must be validated on device (§5) given the X3's heap
sensitivity (`a87d4018`, `4829fab8`).

> **Amendment 2026-10-27 (owner decision, supersedes the gate form above).**
> Enablement is the build flag `XERIF_ASYNC_INPUT`, defined in `[base]` of
> platformio.ini so every firmware environment carries it; the gate is
> `#if defined(XERIF_ASYNC_INPUT)`
> (lib/hal/HalGPIO.cpp) — the flag alone, with no `BOARD_HAS_PSRAM` clause
> (second owner decision: the flag already covers every firmware env, so the
> PSRAM clause added nothing). The device-define form sketched above
> (`FREEINK_DEVICE_X3/X4`) is superseded — it scoped the change to Xteink
> builds only, while the owner wants async sampling everywhere with the
> per-env `-UXERIF_ASYNC_INPUT` as the opt-out (used only on later evidence,
> never speculatively). The env-flag matrix (§4 Task 1) remains the
> verification that no env defines the device flags unintentionally AND that
> `XERIF_ASYNC_INPUT` is inherited by every firmware env from `[base]`
> (opt-out = an explicit `-UXERIF_ASYNC_INPUT` in that one env, never
> silent); the host-only simulator defines its own `FREEINK_DEVICE_X4=1`
> without inheriting `[base]` and is excluded from this firmware matrix — it
> is unaffected because it ignores `hal` (platformio.ini:1018).

## 3. Interactions that must be re-audited (NOT optional)

### 3.1 Auto-repeat stall discount — expected to remain CORRECT as-is

`kRepeatHoldExcludesStalls()` (`src/MappedInputManager.h:55-68`) gates
`stallWindow` accounting to non-PSRAM boards, keyed on the premise "update()
runs on the loop task". The premise still holds after this change: the app
dispatch (`MappedInputManager::update()`) still runs on the loop task and its
inter-dispatch gaps still include flushes; only *sampling* moved off-task.
The held clock is wall-clock in both modes (`InputManager.cpp:869-873`,
`millis() - buttonPressStart`), and in async mode `buttonPressStart` is
stamped at 10 ms cadence on the poll task — *more* accurate. The discount
(`src/util/RepeatHoldDiscount.h`, subtracting gaps over
`kFrameStallThresholdMs`) therefore remains required and correct on the X3/X4.

pi must verify this reasoning with file:line evidence and extend the host
test coverage (the existing C3 sync-path suite is
`freeink-sdk/libs/hardware/InputManager/test/host/test_x4_sync_path.cpp`)
to pin the async-mode repeat semantics: a press held across a simulated
multi-second render must answer auto-repeat exactly once after the render,
not twice. If the existing suite already covers it, cite the case.

### 3.2 Activity-transition input wait — semantics move to the queue

`src/main.cpp:1215-1226` calls `gpio.update()` twice around an activity
switch to pin a 5 ms debounce window. In async mode `update()` is a no-op off
the poll task (`InputManager.cpp:687-695`); edges flow through the queue and
are consumed by `MappedInputManager::update()`'s per-button checks. Verify
the transition window loses nothing (the queued-edge multiplicity covers the
debounce rationale), with evidence.

### 3.3 Already async-aware call sites (verify, do not touch)

- `HalGPIO::verifyPowerButtonWakeup` (`lib/hal/HalGPIO.cpp:283-287`) —
  `needsOwnSampling = !inputMgr.asyncActive()` ✓
- `HalGPIO::consumeTouchFrame` — no-op on sync, consume-on-check in async ✓
  (the X3 has no GT911; queues stay empty for touch).
- Idle trunk `anyPhysicalButtonHeld()` reads live hardware + level latch —
  mode-independent ✓.

### 3.4 Battery/telemetry ADC contention

The poll task and `BatteryMonitor`/`rawInputActive()` both call
`analogRead` on C3 ADC1 channels. This combination is already shipped on the
X4 Pro (S3) without a known defect; flag it as a watch item in the PR body,
do not add locking speculatively (no evidence it is unsafe; `coding-philosophy`:
no mechanism without requirement).

## 4. Implementation order

1. **Env-flag matrix**: grep `platformio.ini` for every firmware env defining
   `FREEINK_DEVICE_X3`/`FREEINK_DEVICE_X4`; confirm the set is exactly the
   firmware builds that carry the C3 combined profiles (default, gh_release,
   gh_release_rc, slim — `env:cppcheck` is NOT a firmware build, it is the
   static-analysis env run via `pio check`, so it lists no device flags of
   its own and inherits whatever it extends only for include/define parity)
   and that sticky/papermono/picco/eego_a4/metalio/x4c/x4pro do not define
   them. Also confirm every firmware env inherits `XERIF_ASYNC_INPUT` from
   `[base]` (opt-out = an explicit `-UXERIF_ASYNC_INPUT` in that one env).
   Exclude the simulator's host-only `FREEINK_DEVICE_X4` from this
   matrix. Record the matrix in the PR body.
2. **Gate change**: `lib/hal/HalGPIO.cpp:108` — condition + comment rewrite
   (the comment must state WHY the C3 boards now take the cost: issue #259,
   flush-window press loss, ~6.5 KB budget, safe degradation).
3. **§3 audits**: stall-discount verification + host test extension;
   transition-wait verification. No code change expected in 3.1/3.3 unless
   evidence says otherwise — any deviation from this doc is a STOP and
   report, not a silent fix.
4. **Docs**: CHANGELOG entry (input section, user-facing phrasing); update
   the `2026-09-22-input-coverage.md` rows that say "press during e-ink
   refresh | lost" for the X3/X4 (now latched/delivered); append an addendum
   to `docs/design/2026-09-20-async-input.md` §2.2 (gate extension, pointer
   to this doc).
5. **Gates (once, after the last code change)**: host ctest suite,
   clang-format, `pio run -e default` (C3 combined) and `pio run -e x4pro`.
   Long builds run detached with log polling — never a blocking foreground
   call.
6. **Commit + push + PR**: commits with the default/global git config only
   (never set `user.name`/`user.email`, no Co-Authored-By trailers). Push
   `feat/x3-x4-async-input` to origin, open the PR to `develop` with
   `Closes #259` as the first body line, plus: the env-flag matrix, the heap
   cost statement, and the on-device verification steps from §5. Then run
   the review loop to convergence (0 open threads, CI green).

## 5. On-device verification (owner, after PR)

- Flash the `default` (x3-x4) build to the X3.
- Reproduce #259: options menu, click down 2–3× rapidly → cursor must step
  2–3 times (queued edges delivered one per tick).
- Capture `MEM` free/min-free at boot and after 10 min (web server + clock
  sync active) — compare against a pre-change boot log; the ~6.5 KB drop is
  expected and must not bring min-free into the failure zone seen in
  `4829fab8`.
- Regression pass: holds/auto-repeat (no phantom repeat after a page turn),
  power-button wake, screenshot combo, idle sleep/trunk wake.