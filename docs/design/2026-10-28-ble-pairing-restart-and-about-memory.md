# Design: BLE pairing silent restart + About-page heap diagnostics

Date: 2026-10-28
Author: Antoine Aflalo (Belphemur)
Status: draft for review
Fixes: Belphemur/Xerif-eReader#267 (Bluetooth half; the input-lag half is #263)

## Problem

On X3 (C3, no PSRAM, ~380 KB usable RAM) the BLE page-turner radio is heap-gated
by `radioVerdict` (`lib/BlePageTurner/src/RadioPolicy.h`): the X3 profile needs
≥81,920 B free (49,152 stack + 32,768 reader block) and a ≥32,768 B largest
block. When the user enables Bluetooth from Settings → Connectivity → Bluetooth
Page Turner and the heap refuses, the only feedback is
`STR_BT_START_FAILED` ("Could not start Bluetooth") — the designed graceful
refusal, but with **no recovery path**:

- The heap-restart consumer (`Runtime.cpp:543-551`) fires only in the Reader
  (`visit.attempted && reader && s.pageShown && !s.wifiOn`), so a pairing-screen
  refusal arms `heapTracker.wanted` but never triggers the restart.
- Worse, the armed `wanted` survives: if the user later opens a book with WiFi
  off, the consumer fires there and reboots them **into the reader** — for a
  Bluetooth intent from a settings screen they may have left long ago.
- Bug reporters cannot be asked for serial logs; there is no on-device way to
  see the heap numbers that decide every gate (web server, TLS clock sync, BLE
  radio, font variants), so reports like #267 arrive without the one piece of
  evidence that would settle them.

## Change 1 — About page heap snapshot (diagnostics)

`src/activities/settings/AboutActivity.{h,cpp}` gains four hardcoded-English
rows (the page's labels are deliberately untranslated so support screenshots
read the same in every language), appended after MAC Address:

| Row | Source | Why |
| --- | --- | --- |
| Free Heap | `ESP.getFreeHeap()` | first input of every gate |
| Min Free Heap | `ESP.getMinFreeHeap()` | worst watermark since boot — the OOM signal |
| Largest Free Block | `ESP.getMaxAllocHeap()` | second input of every gate (fragmentation) |
| PSRAM | `ESP.getPsramSize()` (`No` when 0) | separates S3 profiles from the C3 baseline |

Values are a snapshot taken in `onEnter()` (the activity re-enters on every
navigation to it), which is exactly the moment a support conversation cares
about: the user walks to About *after* the failure. No allocations beyond the
existing `rowValues_` strings.

The simulator's ESP shim (`crosspoint-simulator/src/Arduino.h`) gains the
missing `getPsramSize()` stub returning 0, per its HAL-stub rule.

## Change 2 — silent restart for enabling Bluetooth

Reuse the shipped silentRestart machinery (#266) — no new state machine.

### Runtime target

New `SILENT_REBOOT_TARGET_BLE_SETTINGS = 5` in `main.cpp` (update
`SILENT_REBOOT_TARGET_MAX`), plus `silentRestartToBleSettings()` declared in
`SilentRestart.h`. It uses the **guarded** restart path
(`deepSleepInProgress`/`powerOffInProgress` + the `FREEINK_CAP_TOUCH`
`BoardConfig::hasTouch()` skip from the guarded restart path, `main.cpp`):
the reboot lands where a radio is about to come up, the same hardware-sensitivity
class as the network flows. Consequence, accepted: touch boards (X4 Pro, Sticky,
…, all with roomier heaps) keep today's behavior — refusal message, no reboot.
The X3 button-class devices — where #267 lives — get the recovery.

### Runtime wiring (lib/BlePageTurner)

1. Give the pairing screen a name in the Scene: `main.cpp:1267-1275` sets
   `scene.where = Where::PairingScreen` when the foreground activity is the
   BLE settings screen (ActivityManager exposes a predicate like
   `isForegroundBleSettings()`; the enum value already exists in
   `BlePageTurner.h:134` but is never set today).
2. The refusal reaches `tick()` through a dedicated one-shot latch, not the reader's
   3-streak counter: the reader waits for `kRefusalsBeforeRestart` because its starts
   come and go on their own, but at the pairing screen the start is the user's
   explicit ask — one refusal is enough.

   - `Tracker` (RadioPolicy.h) gains `pairingWanted` +
     `requestPairingRestart()`: arms only when the RTC memo is not spent.
   - New public API `bleturner::requestPairingHeapRestart()` (Runtime.cpp): reads
     the last start's verdict (`startRefusal`), arms the latch only for
     `Why::HeapInPieces` — a heap short in total is not the restart's to fix, and
     a start failed for another reason says nothing about the heap.
   - `BleSettingsActivity` calls it right after `switchOn()` returns false
     (Enable toggle, `:213-218`) — only there: a start that fails on ordinary
     navigation into the screen (saved opt-in in `onEnter()`) must not reboot the
     device unasked.
   - `restarting()` and `radioUp()` clear the latch, so the one-restart-per-radio-up
     bound and the boot-loop guard carry over unchanged.
   - `Host` gains `restartIntoPairing` (no-op default via `noRestart` in
     `kNoHost`; test hosts updated). The firmware side maps it to
     `silentRestartToBleSettings()` next to `restartIntoBook()`, and the guarded
     restart helper is generalized: `silentRestartForNetworkMode` becomes
     `silentRestartForRadioFlow(target, name)`, shared by Join Network, Hotspot,
     and BLE settings — one guarded body for every flow that brings a radio up.

   The consumer branch in `tick()` (next to the reader branch at `Runtime.cpp:543`)
   runs with the same guard set minus the reader-only terms:

   ```cpp
   if (detail::pairingRestartWanted() && config->enabled && s.where == Where::PairingScreen &&
       !s.sleeping && !s.storageBusy && !busy() && !s.wifiOn) {
     heapTracker.restarting();
     host->restartIntoPairing();
   }
   ```
3. The reader branch keeps its existing condition and target — the fix removes
   the stale-`wanted`-fires-in-the-reader surprise only in the sense that the
   pairing screen now consumes the latch first; a `HeapInPieces` streak from a
   book still restarts into the book as before. `Tracker` semantics (one restart
   per radio-up, RTC memo, `kRefusalsBeforeRestart = 3`) are untouched — the
   boot-loop bound carries over for free.

### Resume path

`setup()` resume dispatch (`main.cpp:1186-1197`) gains the
`SILENT_REBOOT_TARGET_BLE_SETTINGS` branch → a new
`ActivityManager::goToBleSettings()` wrapper (mirrors the other `goTo*`
wrappers, `ActivityManager.h:100-118`; constructs `BleSettingsActivity` the same
way `SettingsActivity.cpp:354` does). Retry is automatic: the enable toggle
persists `SETTINGS.ble.enabled = 1` (`BleSettingsActivity.cpp:213-218`) even
when `switchOn()` refuses, and `onEnter()` re-runs
`if (SETTINGS.ble.enabled) bleturner::switchOn();` (`:35`) — the same
"land in the flow, the flow retries" shape as `silentRestartToHotspot()`.

### Known limitation (surfaced, not silently decided)

If WiFi auto-reconnects at boot, the pristine heap the reboot bought can be
partly consumed again before the user re-toggles. The reader branch already
requires `!s.wifiOn`; the pairing branch adopts the same guard, so a heap short
*in total* (HeapLow, e.g. WiFi working set resident) is still refused with the
message rather than rebooted — a restart cannot give back bytes WiFi really
uses. If field logs show HeapLow dominating on X3, the follow-up is a one-shot
"skip WiFi auto-reconnect for this boot" payload bit next to
`SILENT_REBOOT_LIGHT_ON` — deliberately out of scope here.

## Tests

- Host: extend `RadioVerdictS3Test`/`HomeStartTest` hosts for the new callback;
  add cases for the pairing-branch condition (fires at PairingScreen on
  HeapInPieces; does not fire on HeapLow, with WiFi on, while sleeping, or when
  the latch is spent).
- Build: `pio run -e default` (C3 baseline) and `-e x4pro`; simulator builds
  after the shim stub.
- Device (owner): X3, Settings → Bluetooth after a WiFi session → expect the
  Loading overlay, a silent reboot, and the radio up on re-entry; second
  failure must not loop (latch).

## Out of scope

- HeapLow-with-WiFi-on recovery (see limitation).
- The timezone row's read-only behavior (`ClockSettingsActivity.cpp:80`) is by
  design; the #267 reply should state it, not change it.
