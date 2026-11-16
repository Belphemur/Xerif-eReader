# Power short-click dead after the upstream #4407 merge — design

Date: 2026-11-16 · Branch: fix/pwr-short-click · Baseline: origin/develop 9e7dc43c0

## Symptom

On the X4 Pro the short power-button click (shortPwrBtn = SLEEP) no longer
sleeps the device. Reported after the second upstream merge
(4013bad59 ← 29df84f7f, the upstream #4407 port "power-button gesture
disambiguation across soft restarts"). The GT911 park commit (9e7dc43c0) is not
involved — it touches only the touch wake path.

## Failing dispatch paths (file:line evidence at 9e7dc43c0)

`MappedInputManager::update()` consumes every button edge exactly once, at the
top, into the drained frame snapshot
(`src/MappedInputManager.cpp:58-62` — the SDK `InputManager::wasReleased` is
consume-on-read in async builds, `freeink-sdk/libs/hardware/InputManager/src/InputManager.cpp:856-866`,
"soak-fix7": *a second check of the same edge is false by contract*). All Xerif
envs run the async poll task (`XERIF_ASYNC_INPUT` defined in `[base]`,
`lib/hal/HalGPIO.cpp:127-131`), so on device every LATER direct
`gpio.wasReleased(...)` read returns false.

The #4407 merge added three such dead direct reads downstream of the drain:

1. `src/MappedInputManager.cpp:200` — `powerReleased = gpio.wasReleased(BTN_POWER)`
   is consumed by the drain before it, so the wake/restart state machine
   `powerButtonInput.update(...)` at :205 never sees a release
   (`wasShortReleased()` can never become true).
2. `src/MappedInputManager.h:125-127` — `wasShortPowerReleased()` re-reads
   `gpio.wasReleased(BTN_POWER)` directly: always false on device.
3. `src/MappedInputManager.cpp:201-204` — `sharedConfirmRelease`
   (`gpio.wasReleased(BTN_CONFIRM)`) is equally dead on
   DigitalConfirmPowerHold boards.

Consequence per binding:

- **doublePwrBtn set (default PWR_FRONTLIGHT)**: `classifyPower` is true
  (:228), so the classifier at :231 is the only short-click dispatcher — but
  its release input `wasShortPowerReleased()` (:232) can never fire, so no
  Single/Double verdict is ever produced. The fork's sleep-on-release fallback
  (`src/main.cpp:1901-1906`) is withheld on purpose while
  `x4ProAwaitingClickWindow` (:1871-1873) is true, and
  `getPowerButtonHeldTime()` keeps reporting the completed short hold (≤300 ms)
  until the next press, so the gate never opens for that release either
  (`PowerButtonClickInput::MAX_CLICK_HOLD_MS` is 300). Net: the short click
  dispatches nowhere.
- **doublePwrBtn == IGNORE**: `classifyPower` is false, the classifier is
  skipped and main's sleep-on-release branch reads the SERVED mask
  (`wasReleased(Button::Power)`, non-consuming snapshot), which the drain
  still fills — this path survives, which is why the bug shows only with the
  double-click enabled.

The extraction harness could not see any of this: its stub HAL models
`wasReleased` as a non-consuming flag (`test/test_remote_button_input.py`
stub), so the second direct read still returned true.

## Hypotheses

1. **Consume-on-read edge lost (CONFIRMED — root cause).** As above. The
   pre-merge code served all consumers from the drained frame snapshot; the
   #4407 port re-read the GPIO directly because upstream's own HAL is sync
   (bit lives until the next `update()`), where a second read is legal.
2. **doublePwrBtn == IGNORE loses SLEEP (REFUTED).** The `!classifyPower`
   probe at :280 intentionally maps only BOOKMARK/SYNC/READER_MENU; SLEEP
   dispatches through main's served-mask branch (main.cpp:1901), which keeps
   working. No change needed there.
3. **Wake/restart state machine over-qualification (REFUTED).** `beginPowerInput`
   (main.cpp:1371-1375) arms `Suppressed` for wake/restart holds; the state
   machine only stays suppressed while the button is held, the release is
   consumed (`wasPowerReleaseConsumed()`), and the classifier is gated off that
   same tick (:230). A fresh click after the wake release dispatches normally.
   The qualification is correct; only its release input was dead.
4. **Harness blind spot (CONFIRMED).** The stub must model consume-on-read so
   the single-read discipline is testable.

## Fix design

Read each edge exactly once — into the drained frame snapshot that already
exists — and serve every consumer from it:

- In `update()`, replace the two direct reads (:200-204) with the drained
  counters: `powerReleased = drainedRelease[BTN_POWER] != 0`,
  `sharedConfirmRelease = drainedRelease[BTN_CONFIRM] != 0`. The raw physical
  edge (not the window-resolved served mask) is what the wake/restart state
  machine must qualify, exactly as upstream fed it.
- Add a frame-scoped `powerReleaseFrame` member, set each `update()` from the
  drained Power release, and make `wasShortPowerReleased()` read it instead of
  the GPIO: `powerReleaseFrame && powerButtonInput.wasShortReleased()`. All
  in-frame consumers (classifier :232, `!classifyPower` probe :280,
  `wasPowerConfirmClick` :789, `wasPowerSingleClick` fallback
  MappedInputManager.h:106) keep their semantics.
- No #4407 machinery is removed: window arming/expiry, wake/restart
  suppression, screenshot-combo consumption, restart payload flag and
  `beginPowerInput` arming all stay byte-for-byte.
- Harness stub (`test/test_remote_button_input.py`): `wasPressed`/`wasReleased`
  become consume-on-read (the edge flag clears on the read that returns true),
  matching soak-fix7's contract, plus a regression section that reproduces
  wake → release → fresh short click under both doublePwrBtn bindings.

## Behavior matrix (must hold after the fix)

| Context | Input | Expected |
| --- | --- | --- |
| X4 Pro, shortPwrBtn=SLEEP, doublePwrBtn=IGNORE | short click | sleep (served-mask release branch) |
| X4 Pro, shortPwrBtn=SLEEP, doublePwrBtn=IGNORE | long hold | power off on button-down |
| X4 Pro, shortPwrBtn=SLEEP, doublePwrBtn set | single click | sleep via classifier (single verdict at window expiry) |
| X4 Pro, shortPwrBtn=SLEEP, doublePwrBtn set | double click | configured doublePwrBtn action, exactly once |
| X4 Pro, any binding | wake press held through boot, then release | dispatches nothing; a later fresh click dispatches normally |
| X4 Pro, any binding | restart hold (silent reboot with suppression) | release consumed, no dispatch |
| Any shortPwrBtn binding (BOOKMARK/SYNC/READER_MENU, non-classifier boards) | short click | action dispatches |
| Paper Mono | one-tick PMIC click | unchanged |
