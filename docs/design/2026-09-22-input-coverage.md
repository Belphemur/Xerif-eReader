# Input coverage audit: sources, consumers, combinations (S11)

Date: 2026-09-22
Campaign: render-perf soak follow-ups (PR #158 `fix/input-replay`).
Owner directive: "be sure we have all possible input covered — touch screen,
power button, and any buttons of the other devices like the X3 with the menu
button. The home button must be driven ONLY by the home-button settings,
nothing else, no magics." + "access all the different ways a combination can
be done and be sure we support them all AND have tests for them".

This doc is the audit of record for the input path. It fixes the coverage
matrix (§2), the combination matrix (§3), the findings (§4), and the exact
refactor/fix plan (§5) before any code change of this round. Paradigm rules
(DRY > SOLID > KISS) applied per `coding-philosophy`.

## 1. Scope and contract

- App layer: `MappedInputManager` (src/MappedInputManager.{h,cpp}) is the ONLY
  logical-button authority; `HalGPIO` (lib/hal/HalGPIO.h) is the SDK adapter;
  `HomeButtonInput` (src/util/HomeButtonInput.h) is the capacitive-home-key
  classifier; main.cpp owns the power/sleep ACTIONS.
- SDK layer (submodule — NOT modified by this campaign; final pin
  `76d9cd5da51d8741bc88a71c8938314e8cb375ec`, develop's #157 digest, which
  contains the consume-on-check pop protocol the app integrates):
  `freeink-sdk/libs/hardware/InputManager` owns edges (consume-on-check pop
  protocol, per-button saturating pending counts, touch one-shot snapshot via
  `consumeTouchFrame()`), touch/multi-touch classification, and the
  capacitive-home-key event extraction.
- Contract: activities consume input ONLY through MappedInputManager's
  logical API (`Button::*`, touch getters, home-action accessors). Raw
  `HalGPIO::BTN_*` / `gpio.*` reads are allowed only in MappedInputManager
  itself, ButtonRemapActivity (physical capture), and main.cpp's power-policy
  zone (audited per-interaction in §2.1).

## 2. Coverage matrix (source × consumer × settings-driven × evidence)

### 2.1 Physical buttons (BTN_BACK..BTN_POWER)

| # | Source event | Consumer | Settings-driven? | Evidence |
|---|---|---|---|---|
| B1 | Front Back/Confirm/Left/Right press+release | All activities via `wasPressed/wasReleased/wasLongPressed/isPressed` | YES — `SETTINGS.frontButtonBack/Confirm/Left/Right` (mapButtonWith, MappedInputManager.cpp:129-139) | MappedInputManager.cpp:129-141 |
| B2 | Side Up/Down press+release | Reader page turns (`PageBack/PageForward`), menus (NavNext/NavPrevious) | YES — `SETTINGS.sideButtonLayout` + live-orientation swap (isNavDirectionSwapped) | MappedInputManager.cpp:141-180 |
| B3 | Power press/release | main.cpp power grammar (sleep on short click, power off on hold, refresh, footnotes, page-turn, PWR_CONFIRM carve-out) | YES — `SETTINGS.shortPwrBtn` (CrossPointSettings.h:281), `SETTINGS.getPowerButtonDuration()` (:445), `SETTINGS.doubleClickPwrLight` (:395) | main.cpp:1047-1104, MappedInputManager.cpp:360-413 |
| B4 | Power hold > duration | `enterPowerOff()` | YES — duration from settings | main.cpp:1056-1068 |
| B5 | Power click (SLEEP binding) | `enterDeepSleep(false)` on release | YES — shortPwrBtn==SLEEP | main.cpp:1073-1078 |
| B6 | Power click (FORCE_REFRESH) | forced refresh | YES | main.cpp:1093-1099 |
| B7 | Power click (FOOTNOTES) | reader footnote toggle (reader-only action) | YES | EpubReaderActivity.cpp:1304-1306 |
| B8 | Power click (PAGE_TURN) | reader page turn | YES | ReaderUtils.h:59-60 |
| B9 | Power click (PWR_CONFIRM) | Confirm injection (`wasPowerConfirmClick`) | YES — carve-out inside the manager window | MappedInputManager.cpp:396-405, 423-435 |
| B10 | Power+Down combo | screenshot (press side), release tick shielded from activities | PARTIAL — fixed combo, not remappable (stock parity; both buttons exist on every board) | main.cpp:1005-1027, EpubReaderActivity.cpp:1356-1358 |
| B11 | Power held at boot (+Down/Up) | recovery firmware mode | fixed (boot-strap constraint: BTN_UP=GPIO0 strap on X4-class, so DOWN) | main.cpp:701-705 |
| B12 | Wake hold | wake verification + wake-release swallow (`wakePowerReleasePending`) | YES — SLEEP binding decides ghost-wake debounce (main.cpp:727-734) | main.cpp:88-90, 727-734, 1000-1003 |
| B13 | Long-press Confirm (menu-button boards) | reader long-press function (bookmark/footnotes/… from home-button long-press mapping) | YES — `SETTINGS.homeButtonLongPressAction` | EpubReaderActivity.cpp:1217-1230 |
| B14 | Long-press Back / Confirm / page buttons in lists | go-home/file-browser/mode toggles | fixed thresholds (GO_HOME_MS 1000, LONG_PRESS_MS) — consumer-local UX constants | ReaderUtils.h:246-260, FileBrowserActivity.cpp:538, LibraryListActivity.cpp:713 |
| B15 | Sticky shared OK/Power GPIO | CONFIRM vs POWER split | YES — `setSharedConfirmPowerShortPressEmitsPower(SETTINGS.shortPwrBtn == SLEEP)` main.cpp:995 | main.cpp:995, InputManager.h:214-217 |
| B16 | Long-press threshold events (`wasLongPressed`) | one-shot + release suppression; consumed globally by ActivityManager | n/a (mechanism) | MappedInputManager.cpp:455-461, ActivityManager.cpp:76 |

Audit result: every button consumer reachable in this repo resolves through
`mapButtonWith` (logical enums) or the main.cpp power zone. Of the power
zone's interactions, B4, B5, B6, and B12 read their binding from SETTINGS;
B10 (the screenshot combo) is deliberately FIXED policy — not a settings
binding, kept stock-parity on purpose. No undiscovered raw-BTN consumer
exists: `grep -rn "HalGPIO::BTN_" src` returns only MappedInputManager.cpp,
main.cpp, and ButtonRemapActivity.

### 2.2 Capacitive home key (X4 Pro-class; GT911 `hasHomeKey`)

| # | Source event | Consumer | Settings-driven? | Evidence |
|---|---|---|---|---|
| H1 | Home tap / double-tap / hold | `HomeButtonInput::update` classifier → action | YES — the ONLY inputs are `SETTINGS.homeButtonTapAction / DoubleTapAction / LongPressAction` (CrossPointSettings.h:396-401) | MappedInputManager.cpp:41-49, HomeButtonInput.h:25-56 |
| H2 | Resolved action (global) | `dispatchGlobalHomeButtonAction` (main.cpp:274): ToggleFrontlight/Refresh/Sleep/Screenshot/GoBack + reader-scoped passthrough | YES — action enum from settings | main.cpp:274-312 |
| H3 | Resolved action (reader) | NextPage/Footnotes/ReaderMenu/Bookmark… consumed in reader loop | YES | EpubReaderActivity.cpp:1264+ |
| H4 | Home-key press edge | `wasHomeKeyPressed()` — consumed by the manager classifier only (update args) | n/a | MappedInputManager.cpp:45 |
| H5 | Home as "wasHomeGesture" | bottom-edge swipe fallback on non-home-key boards | YES — key maps Home; boards without key use the gesture | MappedInputManager.cpp:330-333 |

Audit result: the home key has exactly ONE read site in the app
(MappedInputManager.cpp:41-49, gated `gpio.hasHomeKey()`), feeding the
settings-mapped classifier. `grep wasHomeKey` outside lib/hal + manager: none.
Tap/double/long all resolve through HomeButtonInput with the three persisted
action slots — no magic. The strict rule is satisfied; no violation found.

### 2.3 Touchscreen (single + multi)

| # | Source event | Consumer | Settings-driven? | Evidence |
|---|---|---|---|---|
| T1 | Tap (+coords) | ReaderUtils page-turn zones / menu tap; UiListActivity rows; Home; BmpViewer; Wifi/Opds browsers; popups | YES — zones/styles from settings (`touchReaderControls`, `showReaderMenu`) | ReaderUtils.h:77-107, 116-133; UiListActivity (rowTouch); per-activity |
| T2 | Tap-candidate (touch-down select) | row/col helpers (`RowTouch::Down`) | n/a | MappedInputManager.cpp:207-243 |
| T3 | Swipe (dir + endpoints) | reader swipe page turn, side-light gestures, back gesture, bottom-edge home/reader-menu | YES — `frontlightSideGestures`, menu-style settings | ReaderUtils.h:80-90, EpubReaderActivity.cpp:1125, MappedInputManager.cpp:300-341 |
| T4 | Touch long-press | reader dictionary/long-press overlays | YES — `touchLongPressBehavior` | EpubReaderActivity.cpp:1180-1215 |
| T5 | Multi-touch swipe/rotation/pinch | NO consumer in the firmware today (SDK exposes the queues; app reads none) — documented dead capability | n/a | SDK InputManager.h:137-160; grep: no app callsite |
| T6 | Touch activity (idle reset) | inactivity timer + power governor | n/a | main.cpp:993-998 |
| T7 | Home-key-as-touch events | see §2.2 | — | — |

Audit result: every touch consumer goes through the manager's touch getters,
which read ONLY the per-tick snapshot built by `consumeTouchFrame()`
(MappedInputManager.cpp:22). No activity touches `gpio.wasTouch*` directly
(verified by grep: `src/activities` gpio reads are deviceIsX3/hasEdgeSide
capability checks only — EpubReaderActivity.cpp:86, 4077; KeyboardEntry
:430/:737; SleepActivity :1063; Interval/Percent :140-144 — none are input
edge reads). T5 is the only untouched surface, recorded deliberately.

### 2.4 Cross-device (board variants)

| Board | Touch | Home key | Front set | Side Up/Down | Power | Menu/Confirm notes | Evidence |
|---|---|---|---|---|---|---|---|
| x4pro (S3) | GT911 | YES | 4 remappable | YES | GPIO w/ hold | — | BoardConfig.h:788-796 |
| x4c (X4 Classic, S3) | — | — | 4 remappable | YES | GPIO | — | BoardConfig.h:1404 |
| X3 (C3) | NO (NO_TOUCH) | NO | 4 remappable (ADC ladder {0,1,2,3,4,5}, power=3) | YES | GPIO3 | Confirm doubles as menu/OK → `FREEINK_CAP_MENU_BUTTON` derived from `InputPins::confirm != PIN_UNASSIGNED` | BoardConfig.h:906-921; BoardFeatures.h:4-19; board_features_dump.c:6,76 |
| sticky (C3) | NO | NO | 4 remappable | YES | shared w/ Confirm (B15) | OK/confirm=power per stock demo | BoardConfig.h:1465 |
| papermono (S3) | — | — | 4 remappable | YES | PMIC one-tick click | hold path impossible → click-sleep special case | main.cpp:1080-1091, PaperMonoBoard.h:13 |

- Degradation guards are runtime feature detection, not compile-time
  assumptions: touch (`gpio.hasTouch()`), home key (`gpio.hasHomeKey()`),
  edge side buttons (`hasEdgeSideButtons()`), frontlight
  (`Frontlight.present()`). Manager paths that need a missing source return
  inert values (home classifier never runs without the key,
  MappedInputManager.cpp:41; `wasReaderMenuSwipeUp`/`wasHomeGesture` branch on
  `hasHomeKey`, cpp:330-333).
- X3 "menu button": reachable through the SAME settings paths as X4 Pro front
  buttons — physical remap (Settings → Controls → Remap Front Buttons,
  SettingsActivity.cpp:91) and menu-button long-press → home-button long-press
  mapping (EpubReaderActivity.cpp:1217, `FREEINK_CAP_MENU_BUTTON` true for X3
  because its Confirm pin is assigned). The Home-button settings screen shows
  on menu-button boards (SettingsList.h:368). No gap.

## 3. Combination matrix

Legend: ✅ supported + tested (host test named) · 🔒 supported by design,
verified by inspection/code-read (no host test — justification given) · SDK =
property of the pinned SDK layer, outside this round's submodule bump.

Same-source sequences:

| Combination | Status | Test / justification |
|---|---|---|
| Two presses of one button queued before one check → two consumable edges (JF7z) | 🔒 SDK (pending counts, saturating) | SDK test_event_pop.cpp:166-170 asserts 255 saturation; manager delivers one edge/tick, defers the rest — no loss (count semantics) |
| Rapid-repeat burst, saturation boundary at 255 | 🔒 SDK | test_event_pop.cpp saturation test; manager unchanged |
| Press+release pairing (both edges one tick) | 🔒 SDK + manager snapshot | debounce guarantees release only after press; masks carry both bits |
| Power: long-press vs click-hold disambiguation (≤300ms click, >300ms hold, ≤confirm-duration carve-out) | ✅ | PowerClickWindowTest: ClickHoldVsLongPress, ConfirmCarveOut, ConfirmCarveOutExceeded |
| Power double-click within 500ms window → frontlight, both releases swallowed | ✅ | PowerClickWindowTest: DoubleClickSwallowsBoth |
| Window expiry without second click → deferred release delivered (+ Confirm edge for PWR_CONFIRM) | ✅ | PowerClickWindowTest: ExpiryDeliversRelease, ExpiryRaisesConfirm |
| New physical release on the SAME tick as window expiry | ✅ (fix F3) | PowerClickWindowTest: ExpiryPreservesNewRelease — deferred release published AND new release re-armed |
| Screenshot combo release (Power+Down) must not arm the click window | ✅ (fix F2) | PowerClickWindowTest: ComboReleaseNotArmed |
| Combo release staggered (Down released earlier tick, Power released solo) | ✅ (fix F2, manager cancel API) | PowerClickWindowTest: CancelDiscardsHeldRelease + main.cpp combo-end wiring |
| Long-press arms release suppression; suppression consumed exactly once | ✅ (fix F1) | ReleaseSuppressionTest: ConsumeClearsBothSets |

Cross-source interference (replay-class):

| Combination | Status | Test / justification |
|---|---|---|
| Button edge during touch gesture | 🔒 SDK | buttons + touch flow through independent queues (InputManager.h:259-293); manager snapshot reads both masks every tick; soak-verified |
| Tap during e-ink refresh window → queued, delivered after | 🔒 SDK | async poll task queues taps (popTouchTap); design 2026-09-20-async-input §2; soak-verified |
| Button press inside an e-ink refresh window → queued, delivered after | 🔒 SDK | async poll task latches press+release edges on EVERY board (`XERIF_ASYNC_INPUT` in [base]; HalGPIO.cpp beginAsync gate); design 2026-10-26-x3-x4-async-input §2 — replaces the historical X3/X4 press-loss (issue #259) |
| Swipe followed by tap within one tick / reverse | 🔒 SDK | swipe + tap are separate one-shots from one release edge; classifier exclusivity enforced by moved-beyond-slop gating (InputManager.h fields touchMovedBeyondTapReleaseSlop / touchSuppressed); SDK unit-covered (event pop + classification contract) |
| Multi-touch arrival while single-touch classifier mid-gesture | 🔒 SDK | `touchMultiContactSequence` suppresses single-contact classifiers until full release (InputManager.h field + multitouch math test) |
| Home-key tap while touchscreen gesture active | ✅ | HomeButtonInputTest: SwipeCancelsPendingTapAndHoldIsConsumed (extended) — the manager feeds `wasSwipe()` into the classifier, which resets; taps during a hold resolve by gesture precedence |
| Button press while touch held (two sources one tick) | 🔒 | masks + touch snapshot are read in one update() pass; consumers see both the same tick; no ordering hazard (single-threaded loop) |

Ordering / interleaving:

| Combination | Status | Test / justification |
|---|---|---|
| Tap consumed then swipe same tick (and reverse) | 🔒 | one release edge classifies as EXACTLY ONE of tap/swipe/long-press (SDK classifier contract; manager reads snapshot once per tick — the second getter observes the same frame, no double consumption) |
| Two taps in one poll period | 🔒 SDK | two queued taps, two consumeTouchFrame() ticks deliver one each (snapshot events-OR merge documented InputManager.h TouchFrameState) |
| Event arriving between snapshot() and consumer read | 🔒 | impossible at app layer: masks are plain uint8 written once in update() on the app task; consumers are same-task, later-in-frame (inspection proof) |
| Manager edge multi-read safety (wasPressed read twice, one edge) | 🔒 | snapshot semantics by construction (MasksServedEveryRead in PowerClickWindowTest via ReleaseSuppression helper contract) |

Settings permutations:

| Combination | Status | Test |
|---|---|---|
| Home tap × each legal action (14 HomeButtonAction values) | ✅ | HomeButtonInputTest: GestureActionMatrixTap (extended) |
| Home double-tap × each legal action | ✅ | GestureActionMatrixDouble |
| Home long-press × each legal action | ✅ | GestureActionMatrixHold |
| Home tap action = Ignore (double-tap window removed) | ✅ | DisabledDoubleTapRemovesDelay (existing, kept) |
| sideButtonLayout swap (all 5 values: PREV_NEXT/NEXT_PREV/PREV_PREV/NEXT_NEXT/DISABLED) | 🔒 | pure settings→enum lookup in mapButtonWith; compile-verified both envs; inspection |
| Front-button remaps (4! assignments) | 🔒 | pure settings→index lookup (cpp:129-139); ButtonRemapActivity persists + reload test on device |
| shortPwrBtn ∈ {SLEEP, IGNORE, PAGE_TURN, FORCE_REFRESH, FOOTNOTES, PWR_CONFIRM} × X4Pro doubleClickPwrLight on/off | ✅ (policy part) | PowerClickWindowTest: windowEnabled true/false × confirm carve-out on/off |

Board degradation:

| Combination | Status | Test / justification |
|---|---|---|
| Classifier on boards without touchscreen | 🔒 | touch getters gate on `hasTouch()` (ReaderUtils.h:79, MappedInputManager touch getters → gpio, inert without controller); no crash path (SDK controllers None → no events) |
| Classifier without capacitive home key | 🔒 | home classifier never invoked (`hasHomeKey()` gate, MappedInputManager.cpp:41); homeAction stays Ignore |
| C3 vs S3 builds | 🔒 | both envs built every round (pio default + x4pro gates); FREEINK_CAP_* macro pipeline (BoardFeatures.h + gen_board_features.py) |

## 4. Findings

**F1 — Suppressed release observed twice (kody PRRT…xcAF, HIGH, VALID).**
`consumeSuppressedRelease()` (MappedInputManager.cpp:468-480) detects the
armed release via non-consuming `edgeSnapshot()` but never clears
`frameReleasedEdges`, so a later `wasReleased()`/`wasAnyReleased()` in the
same tick still sees the edge the suppression was meant to prevent
(e.g. Confirm long-press in a list: ActivityManager consumes the suppressed
release first, but the same-tick release bit remains servable). FIX: clear
the button's bit from `frameReleasedEdges` on the matched consume.

**F2 — Screenshot combo can arm the frontlight click window (kody
PRRT…xb0n, HIGH, VALID).** `resolvePowerDoubleClickWindow` strips a solo
Power release from the mask to arm the window (cpp:385). Two gaps:
(a) same-tick Power+Down combo release gets armed → at +500ms expiry the
deferred release fires the configured short-power action (SLEEP binding →
device sleeps after a screenshot);
(b) staggered release (Down released on an earlier tick) arms on the solo
Power release identically. FIX: (a) do not arm when the same tick carries a
Down release edge — deliver; (b) main.cpp's combo-end branch cancels the
window via a new `cancelPowerClickWindow()` (the combo state lives in main;
the manager exposes the cancel). This changes observable behavior — it is a
FIX (bug), permitted ahead of refactors.

**F3 — New release lost on window-expiry tick (coderabbit PRRT…del7, MINOR,
VALID).** The expiry branch ORs the deferred release into the mask and
returns; a physical Power release captured in the same snapshot is merged
into that one bit and never gets its own window armed (a legitimate
double-click spanning the expiry boundary is misread as one click). FIX:
classify the new physical release separately: publish the deferred release,
then run the new release through the same classify rules (re-arm / double
/ deliver) instead of discarding it.

**F4 — Power+Down reader swallow now (probably) unreachable (kody
PRRT…xb68, HIGH, AS-DESIGNED — no code change).** Under sticky per-tick
masks, EpubReaderActivity.cpp:1356-1358 (`wasReleased(Power) &&
wasReleased(Down)`) can only fire on a tick where both release bits are
served — but main.cpp's combo handler (1005-1027) returns on that tick, so
activities never see it. The swallow is defense-in-depth kept intentionally:
it costs 3 lines, protects against future reordering of main's combo block,
and removing it would change reader behavior if any path ever serves the
combo tick. Recorded in the combination matrix; answered to the reviewer
with this evidence.

**F5 — main.cpp loop zone reads raw levels (`gpio.isPressed(BTN_POWER/DOWN)`,
main.cpp:1000,1007,1019,1048,1057).** These are LEVEL reads (no edge
consumption) so they are not contract violations, but routing them through
`mappedInputManager.isPressed(Button::Power/Down)` unifies the source of
truth (DRY) at zero behavior cost (Power/Down map 1:1, cpp:143-149). This
satisfies the audit rule "no raw BTN_* reads outside the manager/ButtonRemap"
with one exception retained: `getPressedFrontButton()` in the manager
(capture path) and ButtonRemapActivity itself.

**F6 — No host tests exist for the power grammar or the home-action matrix
(the pop-pattern commit 02b79a38 shipped pio-only).** The two classification
engines (`HomeButtonInput`, the power click window) are pure decision logic —
extractable and host-testable like HomeButtonInput already is
(test/home_button/). FIX: extraction (§5) + new/extended suites.

Non-findings (checked, OK): Sticky shared Confirm/Power split is
settings-driven (B15); multi-touch app exposure intentionally absent (T5);
BoardConfig mapping tables are single-profile data with no duplicated
per-board switch cascades in the app (mapping lives in SDK BoardConfig
profiles).

## 5. Plan (commits in order; fixes before refactors)

1. **[task1] docs** — this design doc (no code).
2. **[task2] fix(input)** — F1, F2, F3 in `src/MappedInputManager.cpp/.h` +
   main.cpp combo-end wiring; extract the two pure policy units so the fixes
   are host-testable:
   - `src/util/PowerClickWindow.h` (NEW, header-only, no settings dependency —
     parameters passed in): release classification
     `{None, Arm, DoubleClick, Confirm, Deliver}` for the window-open path and
     expiry resolution `{DeliverDeferred, DeliverDeferredPlusArm, ...}`;
     `cancel()` semantics. The manager's `resolvePowerDoubleClickWindow()`
     becomes a thin adapter (snapshot mask + timing in, verdict applied out).
   - `src/util/ReleaseSuppression.h` (NEW, header-only): armed-suppression
     mask + `consume(edges)` that clears BOTH the armed set and the served
     edge bits — one owner for the exactly-once rule (F1).
   - Host tests: `test/input_grammar/` (new suite, gtest): PowerClickWindowTest
     + ReleaseSuppressionTest covering every ✅ row above.
   Constraints: only the two reviewed bug behaviors change (F1/F2/F3 as
   specified); no allocation; no API removal.
3. **[task3] refactor(input)** — behavior-identical:
   - main.cpp loop-zone level reads → manager logical reads (F5).
   - Extend `test/home_button/HomeButtonInputTest.cpp` with the gesture ×
     action matrix + missing classifier sequences (§3 settings-permutation
     rows).
   - Design-doc as-built addendum (decision log rows for F1-F3 + extraction).
4. **Gates (single batched pass at the end, per directive)**: host ctest,
   `./bin/clang-format-fix` (whole tree, no -g), `bin/cppcheck-check`,
   `pio run -e default` + `pio run -e x4pro` → /tmp/pio_default.log,
   /tmp/pio_x4pro.log. Push, CI green, `pre-merge --pr 158`, triage threads.

SDK files: NOT touched by this campaign — final pin 76d9cd5 (develop's #157
digest, contains the consume-on-check protocol; validated by the SDK host
suite at that revision and by the app's 675-test host suite + both firmware
envs). SDK-side combination rows remain covered by the SDK's own host suite
(assert-run) and the async input design; recorded as such in the final
report.

## 6. Risks

- F2/F3 alter device-observable power behavior by design (bug fixes); both
  are X4 Pro doubleClickPwrLight-path only (default ON), covered by the owner's
  device-soak checklist: screenshot combo (immediate + 600ms), frontlight
  double-click, single-click sleep, PWR_CONFIRM shortcut, hold power-off.
- `cancelPowerClickWindow()` is a new public manager method — no existing
  caller semantics change.
- Pure extractions add two small headers; zero DRAM/flash cost beyond code
  (all inline, no tables).

## 7. As-built (2026-09-22, implementation decisions)

- `src/util/PowerClickWindow.h` extracted as planned; `tick(windowStart,
  physicalRelease, comboRelease, now, heldMs, confirmHoldMs)` carries the
  whole per-tick decision (expiry branch FIRST — the deferred release
  publishes + Confirm edge raises, then the same-tick physical release
  re-classifies: short click re-arms, hold re-delivers/carve-outs, combo
  never arms). Manager's `resolvePowerDoubleClickWindow()` is now a thin
  adapter; `powerDoubleClickFrame`/`powerConfirmClickFrame` semantics
  unchanged (per-tick edges).
- `ReleaseSuppression.h` was NOT extracted: the armed mask is keyed by
  LOGICAL button while the exactly-once clear must hit PHYSICAL edge bits,
  and composite logical buttons (Nav*/Page*) map to several physicals with
  `||` short-circuit semantics. The fix lives in
  `consumeSuppressedRelease()` directly (probe the mapping, clear each
  mapped physical bit) — one consumer, no abstraction (KISS after DRY).
- `cancelPowerClickWindow()` used by main.cpp's staggered-combo-end branch
  (the combo tail release was swallowed by the arm, so main's
  `wasReleased(Power)` is false on that branch — verified against the mask
  flow); the immediate-combo case needs no main-side call: the manager
  serves the combo release itself (F2a).
- Review round 2 (kody): the expiry + new-short-click case sets BOTH
  `serveRelease` and `holdRelease` — the adapter must HOLD the mask bit in
  that combined case (the re-armed click delivers at its own window's
  resolution; one mask bit cannot serve and hold simultaneously). Fixed in
  `resolvePowerDoubleClickWindow()` with a dedicated first branch.
- Level reads in main.cpp loop zone now route through
  `mappedInputManager.isPressed(Button::Power/Down)` (F5); `gpio.isPressed`
  raw reads remain only in the manager, ButtonRemapActivity capture, and the
  SDK.
- Test suites: `test/input_grammar/` (new, 14 tests: window policy +
  verdict matrix) and `test/home_button/HomeButtonInputTest.cpp` (+6:
  gesture × action matrix ×3, second-contact bridge, hold-drops-tap,
  quiet-tick invariants). SDK untouched (pin unchanged).

## 8. Qodo round (2026-09-22, post-S11 review)

Two findings on the per-frame snapshot/power-window handling, both verified
against HEAD before this round's code changes:

- **Q1 (high): one PWR_CONFIRM release reported as both press and release.**
  `wasPressed(Confirm)` and `wasReleased(Confirm)` both route through
  `wasPowerConfirmClick()`; on non-X4 touch boards the underlying predicate
  reads the persistent `frameReleasedEdges` snapshot, so one short power
  release makes BOTH APIs true for the whole tick. Pre-snapshot, the
  underlying `gpio.wasReleased` was consumed by the first read, so only one
  API ever saw it. Consumer evidence: every Confirm consumer is
  release-driven (`UiListActivity.cpp:52`, `EpubReaderActivity.cpp:1164/1280/
  5359/5413/5588`) — no consumer uses `wasPressed(Confirm)`. Fix: the power
  confirm click surfaces ONLY through `wasReleased(Confirm)`; the
  `wasPowerConfirmClick()` shortcut is removed from `wasPressed(Confirm)`
  (it falls through to the front-button press edge, untouched by a power
  release). The home-key Confirm (`homeAction == Confirm`) keeps its
  synthetic both-true shape — pre-existing, out of this PR's scope.
- **Q2 (medium): the first X4 Pro double-click candidate does not count as
  activity.** `resolvePowerDoubleClickWindow()` withholds the first short
  power release from `frameReleasedEdges` while the window is armed (and in
  the serve+hold re-arm case), so `wasAnyReleased()` misses it and
  `lastActivityTime` is not reset — the device can auto-sleep during the
  500 ms window despite a real click. Fix: a per-tick `frameHiddenActivity`
  flag, set whenever a physical power release present this tick is withheld
  from the served mask (any end state where `physicalRelease` was true but
  the BTN_POWER bit is not set afterwards), cleared in `update()` with the
  masks; `wasAnyReleased()` returns `frameReleasedEdges != 0 ||
  frameHiddenActivity`.

Regression tests: the policy layer these fixes key off is already locked by
`PowerClickWindowTest` (withhold-then-deliver `SingleClickArmsThenExpiryDelivers`,
carve-out `ConfirmCarveOut*`, re-arm `ExpiryReArmHoldsTheMaskBit`). Adapter-level
tests (repeated `wasPressed`/`wasReleased(Confirm)` reads, `wasAnyReleased()`
during an armed window) need a `MappedInputManager` host harness, which does not
exist: `HalGPIO` is concrete with zero virtuals and includes `Arduino.h`, so it
is not host-injectable, and building the fake-Arduino + fake-InputManager +
GfxRenderer chain is a follow-up task, deliberately not absorbed into this
review round (scope discipline; recorded for the harness follow-up).

## 9. Qodo round 2 (/review trigger, 2026-09-22): three more findings

- **T1 (high): blocking-transfer pumps discard button input.** `update(true)`
  (OpdsBookBrowserActivity:531, FontDownloadActivity:599,
  CrossPointWebServerActivity:383) clears both masks and drains every physical
  edge each pump, but the blocking callbacks inspect only Back/Home/touch.
  Pre-snapshot, un-inspected edges latched in gpio until the post-transfer
  dispatch read them; the snapshot's per-tick clear made this a regression.
  Fix: a `carryEdges` flag — deferred pumps OR fresh edges into the surviving
  masks and keep the flag set; the FIRST normal dispatch after blocking
  merges the carried edges, then resumes the per-tick clear. The callback
  comment already promised this ("other configured actions are deferred to
  the next main-loop pass"); the code now honors it.
  Review round 3 (coderabbit 5r7c, resolved): the earlier "Power release
  mid-transfer drops its Confirm edge" limitation is GONE — the same carry
  contract that preserves button edges preserves the window verdicts: a
  verdict raised mid-transfer stays set (per-frame reset runs only on a
  fresh dispatch), so the resolved Confirm / double-click dispatches with
  the first post-transfer frame. The classifier now reads only newly
  captured edges (5r7g), so carried/published mask bits can never re-arm
  the window.
- **T2 (high): wake-up can trigger a power action.** The wake-release branch
  (main.cpp:1002) returns without cancelling the click window the wake
  release may have armed in that tick's `update()`; on an X4 Pro with
  `doubleClickPwrLight` the window later publishes the deferred release +
  Confirm edge and fires the configured short-power action post-wake. Fix:
  `cancelPowerClickWindow()` in the wake-release branch before returning —
  the branch's contract is "consume the wake input frame, dispatch nothing".
- **T3 (medium): power clicks lost at timer wrap.** `PowerClickWindow` used
  `windowStart != 0` as its open/closed sentinel while `tick()` arms with
  `millis()`; a release exactly at a wrap (or within the first ms after
  boot, where millis() starts at 0) armed a window that reads as closed —
  the release is withheld and never delivered. The same sentinel leaked
  into `isPowerClickWindowPending()`/`cancelPowerClickWindow()`. Fix: the
  window state becomes `WindowState {bool open; uint32_t start;}`; the
  sentinel is gone from the policy and both manager readers. Host-testable:
  `ArmAtTimerWrapSurvives` locks the wrap case in `PowerClickWindowTest`.
  T1/T2 are adapter/main-level and share the harness defer recorded in §8.

## 10. Rebase round (S12, 2026-09-22): develop-delta re-integration + coderabbit round 3

- Rebased onto 175c8134 (SDK 76d9cd5). The replayed branch pin bump
  (b622c4c) had overwritten develop's newer digest — restored 76d9cd5
  (b622c4c is its ancestor; the consume-on-check protocol is in both).
  Develop-side deltas re-integrated on top of the snapshot architecture:
  upstream's PREV_PREV/NEXT_NEXT side-button modes; develop's
  `beginInputFrame()` call is superseded (the pinned SDK's consume-on-check
  protocol needs no frame ack; this branch's HalGPIO exposes only
  `consumeTouchFrame()`).
- Coderabbit round 3 findings (all resolved):
  - **3dSJ (major):** on the expiry + new-short-click tick the SERVED bit
    carries the EXPIRED release — publish it; the new click's edge was
    consumed by the snapshot and lives only in the re-armed window. The
    S11 hold-both reading (kody) is superseded: one mask bit cannot serve
    and hold, but the two release EVENTS are distinct — the mask carries
    the expired one, the window carries the new one.
  - **5r7g (major):** the classifier must read only NEWLY captured edges —
    the frame mask carries published bits across blocking pumps, and
    feeding those back re-arms/re-defers the already-resolved release.
    `resolvePowerDoubleClickWindow(newReleasedEdges)` now takes the fresh
    snapshot; publication still works on the full mask.
  - **5r7l:** the double-click/Confirm verdicts reset on every fresh
    dispatch (pump/first-post-transfer frames keep them, matching the
    edge-carry contract) — a verdict can no longer outlive its dispatch
    frame through an exclusive-storage loop.
  - **5r7c (major):** the T1 "Confirm edge dropped mid-transfer" limitation
    is resolved by the same carry contract (verdicts ride the first
    post-transfer dispatch); the doc's limitation paragraph is replaced.
  - Doc fixes: B10 marked fixed-policy (not a settings binding); the
    sideButtonLayout matrix lists all five values; the SDK pin/validation
    revision recorded.
- Adapter test renamed: `ExpiryReArmPublishesTheExpiredRelease`.
