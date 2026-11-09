# Design addendum: GT911 idle sleep — fail-safe park entry + poll self-heal

Addendum to `2026-09-24-gt911-idle-sleep.md` §3 Tier B. Date: 2026-11-09.
Status: accepted, to be implemented by the `fix/gt911-park-recovery` PRs.

## 1. Defect report that falsifies Tier B's assumptions

X4 Pro (battery use, `touchIdleSleep` at its default of 1): after ~5 min of
idle the touchscreen dies. Physical buttons keep working. A single button
press — the wake trigger the design intends — does **not** restore touch.
The only recovery is a deep-sleep cycle, which is a chip reset and re-runs
the `beginGt911()` probe. Reproduced 2026-11-09 on develop (`2cc1f604e`).

## 2. Which claims this falsifies

§3 Tier B assumed the sleep-entry probe in `InputManager::enterGt911Sleep()`
(freeink-sdk `InputManager.cpp`) reliably distinguishes the three outcomes:

| probe result | code assumed | code path taken |
| --- | --- | --- |
| chip still awake | 0 (ACK) | "not confirmed; staying awake" — correct |
| chip asleep | 2 (address NACK) | `gt911Asleep = true` — correct |
| anomaly | 1/3/4/5 | UNKNOWN, stays awake — treated as safe |

Three falsified assumptions:

1. **The probe is not racy.** The command commit and the probe run ~100 µs
   apart with no delay between them (the write and the probe are consecutive
   `Wire` transactions). A chip that ACKs the probe inside its §8.1.d
   transition window enters sleep a moment later with `gt911Asleep == false`.
2. **A re-park attempt is harmless.** The app retries parking every
   `GT911_PARK_RETRY_MS` (30 s) while `!isTouchAsleep()`. That retry writes
   the sleep command to a chip that is already asleep, so the write NACKs
   (`sent == false`), the probe never runs, and the state machine stays
   "awake" forever. The two failure modes lock in each other.
3. **A wrong "awake" is safe.** It is the worst outcome: the app's wake path
   is gated on `isTouchAsleep()` (firmware `src/main.cpp` idle trunk), so no
   wake is ever attempted, `pollGt911()` returns silently on every failed
   status read, and the chip is unreachable until the next chip reset. The
   doc's own failure discipline ("staying awake" on uncertainty) optimizes
   for the wrong failure.

## 3. Decisions

Decided in DRY > SOLID > KISS order; each names the winning principle and
what the loser would have cost.

1. **A sent sleep command parks unconditionally (KISS).** Once the 0x05
   write ACKs, the chip *will* sleep; the probe adds no information the
   command has not already committed. `enterGt911Sleep()` sets
   `gt911Asleep` on any outcome of a *sent* command (the probe result stays
   a diagnostic log line). A wrong "parked" costs one ≤ ~230 ms wake
   handshake on the next activity edge; the rejected "probe as proof"
   (a SOLID-state purity argument) cost a permanently dead touchscreen.
2. **Poll self-heal ladder (KISS, one mechanism per failure).** While the
   controller is considered awake, N consecutive failed 0x814E status reads
   (N = 5) force one INT-high wake handshake; a handshake that fails its
   200 ms window triggers a full `beginGt911()` re-probe, rate-limited to
   once per 60 s. This covers every remaining dead-state variant: NACKed
   command writes, chip self-sleep, and a wake handshake the chip ignores.
   No new heap allocation: a counter and the two existing sequences only.
3. **Rejected: app-side recovery on failed polls.** Duplicating the SDK's
   controller-state knowledge in the firmware idle trunk violates DRY and
   still cannot see the chip; the SDK owns the state, the SDK heals it.
4. **Rejected: touch-to-wake via Gesture mode.** Remains unverified
   hardware on X4 Pro (parent doc §4); out of scope.
5. **Bus serialization of the runtime re-probe** (review round): the
   ladder's `beginGt911()` re-init holds `freeink::I2cBusLock`, which is a
   real mutex on the profiles that enable it (Metalio EINK4, EEGO A4) and a
   no-op elsewhere — enabling it on the X4 Pro shared bus (GT911 + PCF8563
   Rtc + CW2017 gauge, SDA39/SCL38) is a cross-cutting profile change (Rtc,
   BatteryMonitor and every other shared-bus user must take it), recorded
   here as a follow-up for the SDK owner rather than smuggled into this
   defect fix.

## 4. Behavior contract after the fix

- After any idle park, one mapped button edge restores touch (≤ ~230 ms
  block, once) — unchanged intent, now guaranteed.
- With no input at all, a controller stuck in the misclassified state
  recovers autonomously within the ladder (N failed polls → wake handshake
  → re-probe), with one `LOG` line per ladder step.
- A controller **considered awake** that stops answering is re-probed instead
  of dying until reboot; a re-probe that finds no chip on the bus logs one
  line per attempt (rate-limited to once per 60 s) and the ladder keeps
  retrying until the chip answers again — a failed re-probe must not strand
  `gt911Addr`/`touchDataEnabled` and turn touch permanently unreachable
  (that was the ladder's own failure mode, caught in review round 1).
- Known residual (Kody, app PR review): the ladder runs only while the
  controller is considered awake — `pollGt911()` returns early while parked,
  so a controller that is parked AND unrevivable by the INT-high wake
  handshake is retried on every mapped button edge but never escalates to a
  re-probe; deep sleep remains that state's recovery. The wake handshake is
  exercised on hardware, so the residual needs a chip that wedges between
  an ACKed sleep command and the first wake attempt. Routing repeated wake
  failures into the ladder (drop the parked assumption after N failed
  handshakes so the poll ladder takes over) is the accepted follow-up —
  it is a code change, deliberately not folded into this doc landing.

## 5. Verification

- **Host tests** (freeink-sdk InputManager suite): park entry parks on an
  ACKed command regardless of probe code; the ladder fires the handshake
  after N failed reads and re-probes on handshake failure; rate limit holds;
  a failed re-probe retries at the rate limit and recovers a chip that
  comes back; a handshake that cannot clear the controller's latched report
  does not re-arm the level-wake INT.
- **Device bench build**: `XERIF_BENCH_TOUCH_PARK=1` per the parent doc's
  bench escape; serial shows `GT911 sleep entered` on park and
  `GT911 wake, resuming poll` on the button edge.
- **Owner soak**: idle > 5 min on battery → one Up/Down press → touch
  alive every time.
- **Ladder soak (fault state)**: the autonomous ladder only engages while
  the controller is considered awake but stops answering on I2C — the
  pre-fix misclassified park, a chip self-sleep, or a NACKed park retry. A
  healthy post-fix park parks the controller correctly, so the no-button
  soak is NOT evidence for the ladder on healthy hardware; correctness is
  pinned by the host suite, and any ladder line in a device log (`GT911
  poll: 5 failed reads…`, `…re-probing…`) is evidence it engaged in a
  genuine fault state.
