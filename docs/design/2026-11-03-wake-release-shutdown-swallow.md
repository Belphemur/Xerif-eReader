# Wake-release swallow for shutdown boots (X4 Pro) — design

Date: 2026-11-03. Status: approved contract for this run (design doc before code, owner rule).

## Symptom

On the X4 Pro (`env:x4pro`, ESP32-S3), waking from a SHUTDOWN (manual power off) with a long
press on the power button boots the device, then the device immediately goes back to sleep:
the single long-press gesture is replayed as a long press AND a short press back to back. The
owner does not want the wake press queued into the fresh boot.

## Evidence chain (verified at base 390b1a7e)

1. Power off is `HalPowerManager::enterPowerOffSleep()` (`lib/hal/HalPowerManager.cpp:252-310`):
   it drives the power latch pins LOW and `gpio_hold_en()`s them ("the master rail is
   deterministically dead for the whole sleep"), then `deepSleepUntilPowerButton()`. The next
   press re-energizes the rail and is a genuine POWERON cold boot (`ESP_RST_POWERON`), not
   `ESP_RST_DEEPSLEEP`.
2. `HalGPIO::getWakeupReason()` (`lib/hal/HalGPIO.cpp:370-395`) maps a POWERON cold boot to
   `WakeupReason::PowerButton` only when `coldBootImpliesPowerButton()` is true.
3. `coldBootImpliesPowerButton()` (`lib/hal/HalGPIO.cpp:357-368`) covers `isXteinkDevice()`
   (X3 / X3Uc8279 / X4 only — `HalGPIO.cpp:302-306`), PaperMono, Sticky, Metalio. The X4 Pro
   (`Board::XteinkX4Pro`, `freeink-sdk/.../BoardConfig.h:453`, profile at `:1864` with
   dedicated power GPIO3 and `InputStyle::DigitalButtons`) is NOT covered, so its shutdown
   wake classifies as `WakeupReason::Other`.
4. `src/main.cpp:1098-1109`: only `case WakeupReason::PowerButton` arms
   `wakePowerReleasePending = true` (line 1108). `Other` arms nothing.
5. The wake hold is still held when the input subsystem first samples. In async mode the poll
   task (`inputMgr.beginAsync(2, 10ms, 16)`, `lib/hal/HalGPIO.cpp:123`) runs
   `InputManager::asyncPoll()`, whose first `update()` synthesizes the BTN_POWER press edge
   (`InputManager::applyStateChange` stamps `powerButtonPressStart`,
   `freeink-sdk/libs/hardware/InputManager/src/InputManager.cpp:543`) and queues both press
   and release edges (`asyncPoll()`, same file ~:371-390). The release edge arrives during or
   after boot.
6. Boot takes well over 2s (SD init, fonts, display, splash all run inside `setup()`), and
   `allowSleepAt = millis() + 2000` is only stamped at the END of `setup()`
   (`src/main.cpp:1276`). So the grace covers nothing that happened before it was set, and any
   release drained after it expires fires the sleep branch. Two concrete manifestations:
   - (A) the user keeps holding through boot and releases more than 2s after `setup()` ends —
     the release is served as a configured short power click;
   - (B) the user releases DURING boot, the press+release edges queue (async input exists
     precisely so boot renders do not drop edges), and a loop tick blocked by the boot paint
     drains them after `allowSleepAt` expires — the queued release replays as a short click.
7. The release handler `src/main.cpp:1573-1578`:
   `shortPwrBtn == SLEEP && millis() >= allowSleepAt && wasReleased(Power)` →
   `enterDeepSleep()`. The `powerReleasedSinceWake` gate (`src/main.cpp:1545-1546`) protects
   only the power-off HOLD path (1554), not this release path. The comment at 1571-1572 claims
   `allowSleepAt` covers the wake release — it does not (see 6).

The same release also replays into the non-SLEEP bindings on the boot frame (FORCE_REFRESH
dispatches a manual refresh; PWR_CONFIRM synthesizes a Confirm press against a freshly painted
activity), so the fix must swallow the whole release frame, not only the sleep path — the
existing swallow's contract is already "consume the wake input frame, dispatch nothing".

## Contract points (from the brief)

1. The wake press must never dispatch any in-app action on the boot it caused: no short-click
   sleep, no refresh, no reader-shortcut mapping, on ANY wake path (deep-sleep wake AND cold
   boot after power off), for every board with a power button.
2. Do NOT fix this by adding the X4 Pro to `coldBootImpliesPowerButton()` (commit `e37164ca`
   deliberately excluded it: the X4 Pro has no confirmed USB-detect pin — `BoardConfig.h`
   `XTEINK_X4_PRO` marks usbDetect "unconfirmed" — so a post-flash POWERON would misclassify
   as a power-button wake and, for `shortPwrBtn != SLEEP` users, `verifyPowerButtonWakeup()`
   would fail and re-sleep right after a flash). Preserved.
3. Prefer reusing the existing swallow mechanism (`wakePowerReleasePending`, consumed at
   `src/main.cpp:1487-1494` together with `mappedInputManager.cancelPowerClickWindow()`)
   over inventing a new one.
4. The swallow must stay one-shot and self-consuming: if the button is NOT held after boot,
   arming must cost nothing observable (at most one early-returned loop tick) and must not
   swallow a genuine post-boot click (a press that begins AFTER the first dispatch).
5. Keep the silent-reboot path (`src/main.cpp:1251-1274`, `discardQueuedButtonEdges`)
   semantics intact.
6. Firmware-only change (this repo, `src/` + `lib/hal/`); no SDK change is needed — the SDK
   already exposes `InputManager::isPowerButtonPhysicallyPressed()`
   (`freeink-sdk/.../InputManager.h:36`, raw pin read honoring the declared polarity, returns
   false when `input.power < 0`).
7. Preserve the behaviors the swallow already guards: deep-sleep wake release swallow,
   screenshot combo, X4 Pro frontlight double-click window interplay
   (`src/MappedInputManager.cpp:638-679`, `src/util/PowerClickWindow.h`).

## Chosen fix — candidate (a), refined: arm from the held-at-input-init sample

**Shape (a): arm the swallow in `setup()` when the power button is physically held after the
input subsystem is up, regardless of wake classification.** Chosen, with one refinement that
closes the sampling race:

- `HalGPIO::begin()` (`lib/hal/HalGPIO.cpp:88-125`) currently calls `inputMgr.begin()`
  (configures the power pin's pinMode) and then `inputMgr.beginAsync(...)`, which spawns the
  poll task; `asyncPoll()` samples on its FIRST iteration with no initial delay, so on a
  multi-core board the first edge sample can race any sample main.cpp takes afterwards.
  **The sample must happen strictly between `inputMgr.begin()` and `inputMgr.beginAsync()`** —
  at that point the poll task does not exist yet, so no edge can have been queued, and the raw
  pin read is the ground truth. `HalGPIO::begin()` records it into a new member and main.cpp
  reads it back after `gpio.begin()`. This makes the arm decision deterministic for both
  post-boot shapes:
  - release before the sample → the poll task never saw a press (its first sample reads
    released, no edges queued) → nothing to swallow, nothing dispatches;
  - release after the sample → the sample read held → armed → the queued release edge is
    swallowed at the first loop tick where the level reads released.
- In `setup()`, arm the existing `wakePowerReleasePending` from that sample, gated to skip
  silent maintenance reboots (contract 5: the silent path owns its boot boundary with
  `discardQueuedButtonEdges`, and its held-button release is delivered deliberately — arming
  there would change that). The sample is classification-independent, so it covers the X4 Pro
  shutdown wake (`WakeupReason::Other`) and, by the same rule, any board whose boot a power
  press started or a press the app never saw begin; the classified
  (`WakeupReason::PowerButton`) arm stays exactly as it is for deep-sleep wakes whose press
  typically ends before `setup()` can sample.
- The consume path is untouched in behavior: one-shot, consumed on the first tick where the
  power level reads released, together with `cancelPowerClickWindow()` so the X4 Pro
  frontlight window cannot republish the swallowed release (qodo T2). Both decisions (arm,
  consume) are extracted into a header-only pure policy (`src/util/WakeReleaseSwallow.h`, the
  repo's established pattern for input policies — `src/util/PowerClickWindow.h`,
  `src/util/QueuedEdgeDrain.h`) so host tests can drive the state machine.
- Boot log at the swallow decision point: one `LOG_DBG("MAIN", ...)` line showing the wake
  reason and the power-held-at-input-init state (plus whether the swallow armed), so the
  owner's serial soak confirms the path.

Why arming from a held sample cannot swallow a genuine post-boot click (contract 4): a click
that begins after the first dispatch necessarily begins after the input-init sample, which
read released; the swallow was never armed, or was already consumed one-shot by the wake
press's own release. The only window where arming misfires is a release-then-repress of the
power button inside a single poll tick (10ms) while the wake hold is still pending — the
wake release drains in the same frame as a new press and the swallow does not consume (level
reads pressed), so the new press's own release can still dispatch. That window exists today
for deep-sleep wakes with the identical consume condition and is physically implausible for a
wake gesture; no regression.

Behavior changes accepted by this design (small, conservative):

- A power press held across a USB-powered boot (`AfterUSBPower`) or a post-flash boot is now
  swallowed on its release instead of dispatching a short click/refresh at the first
  post-grace tick. On rail-button boards a held-at-boot press is the press that energized the
  boot; on other boards it is a press the app never saw begin. Swallowing it costs nothing
  observable and cannot dispatch an action against a UI the user never saw react.
- Boards whose wake press is fully released before input init (Metalio, PaperMono clicks) are
  unaffected: the sample reads released, nothing is armed, behavior is byte-identical.

## Rejected alternatives

### (b) Arm when the first input frame after boot shows a Power press or release edge

Rejected. The first frame's edge set is drained by `mappedInputManager.update()`
(`src/main.cpp:1299`) before any main-loop decision, so "first release edge" cannot
distinguish the wake press's tail from a genuine post-boot click — arming on it would swallow
a real click (violates contract 4), and distinguishing them needs exactly the pre-sample held
state that shape (a) records. It is also racy: arming only after the poll task's first sample
leaves the release-between-samples window unclassified, which is the bug shape (B). Shape (a)
with the input-init sample subsumes it deterministically.

### (c) Arm for `WakeupReason::Other` when the RTC shutdown marker says the previous session ended in a clean power off

Rejected — verified dead on this codebase for a true rail cut. The marker IS staged for the
manual power off (`src/main.cpp:804` `powerManager.stageUserPowerOff()` immediately before the
`[[noreturn]]` `enterPowerOffSleep()` at 805). It lives in RTC slow RAM at 0x50000000/0x50000004
(`freeink-sdk/libs/hardware/PowerManager/src/PowerManager.cpp:202-224`), whose documented
survival guarantee is "survives deep sleep and most resets" — retention requires the RTC domain
to stay powered. But `enterPowerOffSleep()` cuts the master rail and holds the latches LOW
through the isolation ("the master rail is deterministically dead for the whole sleep",
`lib/hal/HalPowerManager.cpp:278-286`), so the chip's RTC SRAM is unpowered for the whole
sleep and the cells are indeterminate at the next cold boot (a genuine `ESP_RST_POWERON`, not
a retained-sleep wake). `takeShutdownReason()` finds no magic and returns 0, so
`takeLastShutdownKind()` (`src/main.cpp:966`) reports `None` on exactly the boot this bug is
about. The marker can only distinguish power-off boots on boards whose power-off keeps the
chip powered (none in the power-off path today). Candidate (c) is dead; rejected with this
evidence.

### Classification fix (adding the X4 Pro to `coldBootImpliesPowerButton()`)

Forbidden by contract 2 (commit `e37164ca` reasoning: post-flash POWERON misclassification →
`verifyPowerButtonWakeup()` ghost-wake re-sleep). The chosen shape never uses classification
for arming, so the exclusion stands untouched.

### SDK change

Not needed: `InputManager::isPowerButtonPhysicallyPressed()` already exists; the sample point
(`between begin()` and `beginAsync()`) is inside this repo's `HalGPIO::begin()`. No submodule
pin change.

## Implementation plan

1. `src/util/WakeReleaseSwallow.h` — pure policy: `shouldArm(classifiedWake,
   powerHeldAtInputStart, silentReboot)` and `shouldConsume(pending, powerPressed)`.
2. `lib/hal/HalGPIO.{h,cpp}` — sample the raw power pin inside `begin()` between
   `inputMgr.begin()` and `inputMgr.beginAsync()`; store + getter
   (`powerButtonHeldAtInputStart()`). Unguarded like the adjacent `inputMgr.begin()` call —
   host/simulator builds do not compile `lib/hal`, and the member's `= false` default makes the
   getter read constant false wherever input hardware is absent.
3. `src/main.cpp` — arm `wakePowerReleasePending` via the policy right before the wake switch
   (where the reason is known), with the one-line `LOG_DBG("MAIN", ...)` decision log; route
   the existing consume condition through `shouldConsume` (no behavior change).
4. `test/input_grammar/WakeReleaseSwallowTest.cpp` — host tests: wake press held through boot
   → no sleep dispatch; released during boot (queued press+release) → no sleep dispatch;
   genuine post-boot click → still sleeps; silent reboot not armed; classified deep-sleep wake
   still armed; one-shot consumption.

## Verification plan

- Host: build + run the gtest suite (`test/input_grammar`), full host suite green.
- Gates: `clang-format` on touched files, host tests, `pio run -e x4pro`.
- firmware-handoff skill checks per AGENTS.md (four review axes, two rounds max).
- Hardware (owner): flash `env:x4pro`, power off with a long press, wake with another long
  press → device boots and STAYS awake; release after boot dispatches nothing. Serial log
  evidence: one `MAIN` debug line at the swallow decision point
  (`Wake-release swallow armed: reason=<n> powerHeldAtInputStart=<0|1>`, logged only when the
  swallow actually arms) at boot, and the absence of
  "Sleep triggered by power-button click" for the wake release. A genuine short click after
  boot (with Short Power Button Press = Sleep) still sleeps.

## Risk assessment

- New statics/heap: none. The sample is one `bool` member in `HalGPIO`; the policy is
  header-only constexpr logic. DRAM delta: 1 byte in .bss.
- Boot-time cost: one `digitalRead` in `begin()`, one `LOG_DBG` (compiled out below LOG_LEVEL 2
  in release builds, kept as a volatile-free pure argument list).
- Interaction with the X4 Pro frontlight double-click window: the consume site already cancels
  the window (`src/main.cpp:1487-1494`); the wake release's held time is far above
  `kClickMaxHoldMs` so the window would deliver, not arm — swallowing the frame first and
  cancelling keeps that invariant (qodo T2 semantics unchanged).
