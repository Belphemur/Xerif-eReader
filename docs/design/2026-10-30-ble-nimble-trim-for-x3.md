# Design: NimBLE heap trims to enable Bluetooth on the X3

Date: 2026-10-30
Author: Antoine Aflalo (Belphemur)
Status: proposed
Follows: `docs/design/2026-10-28-ble-pairing-restart-and-about-memory.md` (shipped in v2.13.0)
Related: Belphemur/Xerif-eReader#267

## Problem

On the Xteink X3 (ESP32-C3, no PSRAM), the BLE page turner is heap-gated by `radioVerdict`
(`lib/BlePageTurner/src/RadioPolicy.h`). The X3 profile requires **free ≥ 81,920 B**
(`kStartFreeBytes` = `kStackBytes` 49,152 + `kMinimumLargestBlockBytes` 32,768) and a
**largest block ≥ 32,768 B** at the pairing screen, where `kStartFreeBytes`/`kMinimumLargestBlockBytes`
are `RadioPolicy.h:23-26`.

Majkysek's captured log (`issues/267` comment 6527009881, `X3log.txt` v2) shows that even on a
pristine boot that resumes straight into the BLE settings (so WiFi and the UI session are not
consuming anything), the retry fails:

- boot 1, session heap at pairing start: `free=66120 largest=59380` (short 15,800 B)
- silent restart → boot 2, pristine retry at `[401]` ms: `free=71404 largest=61428`
  (short **10,516 B**); refused again; the #269 one-shot latch spent, no loop

Line 35 of that log: `required_free=81920 required_largest=32768`. The largest block (61,428)
clears the 32,768 requirement with a 28,660 B margin, so this is **not fragmentation** — the
device is short purely in total bytes, and the gap is the stack bytes plus the reader's 32 KiB
inflate ring (`lib/InflateReader/InflateReader.h:54`, `RING_BYTES = 32768`). Even a successful
start would carve the 61 KB block below the 32 KiB the reader needs when a book opens, which is
why the gate (correctly) refuses.

The measured stack take is 50,644-51,188 B in every X3 log (`RadioPolicy.h:25-26`). Closing the
gap therefore means making the real take drop by ~10.5 KB, then re-pinning `kStackBytes` to the
new measured value so the gate and its tests move together.

## Current NimBLE configuration

All BLE envs inherit `[ble_page_turner]` via `[firmware_tuned]` →
`[firmware_tuned_c3]` → every env (`default`, `gh_release`, `slim`). Source:
`platformio.ini:263-354` (the `custom_sdkconfig` block under `[ble_page_turner]`).

## Change 1 — trim the NimBLE pools and controller buffers

Edit only the existing knobs in `platformio.ini:[ble_page_turner]`. No new allocations, no code
paths.

| knob | current | new | rationale | saving (measured on X3) |
| --- | --- | --- | --- | --- |
| `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` | 4096 | 3072 | One connection, no GATT server; 3 KB is headroom for the host task. | ~1,024 B (task stack) |
| `CONFIG_BT_CTRL_BLE_MAX_ACT` | 4 | 2 | Only one connection + one scan ever needed at a time ("two activities at most"); the comment in `platformio.ini:340` already calls 4 "headroom". | ~1 KB (controller act contexts) |
| `CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT` | 12 | 6 | Discovery of a single keyboard uses a bounded number of ATT requests; halve the MSYS1 pool. | to be measured |
| `CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT` | 12 | 6 | Same: one remote, bounded MTU (25). | to be measured |
| `CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT` | 8 | 4 | MTU ceiling is 23 (`CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=23`); four ACL transport frames cover one connection with headroom. | to be measured |

**Bound check / risk.** `RadioPolicy.h:29-30` documents the MSYS pools as "room for GATT
discovery of a full keyboard (many services)". Cutting them too far can make pairing or bonding
unreliable on remotes with large GATT tables. Therefore `MSYS_*=6` is the floor for this pass,
and the verification step pairs a real (many-service) remote end-to-end. If pairing regresses,
revert `MSYS_*_BLOCK_COUNT` to 8 (keep `MAX_ACT=2`, task stack 3072, ACL 4) — that still nets a
few KB.

`CONFIG_BT_NIMBLE_MAX_BONDS=4` is **left untouched**: bond records live in NVS, not heap, so
trimming it gains nothing here.

## Change 2 — re-pin the X3 profile constants to the new take

The implementer (a supervised coding agent) has no device, so the re-pin is derived, not
measured: every knob in Change 1 shrinks a pool whose byte size is fixed at build time
(`block_count × block_size` for the MSYS pools and the ACL transport, one task stack of
exactly 1,024 B, the controller activity contexts). The exact static deltas are read from
the generated sdkconfig of a build with the new values (`block sizes` for
`CONFIG_BT_NIMBLE_MSYS_1/2_BLOCK_SIZE`, the transport buffer size, `CONFIG_BT_CTRL_*`
scan structures) — not estimated.

In `RadioPolicy.h`:

- `kStackBytes` (currently 49,152, `RadioPolicy.h:28`) moves down by exactly the computed
  static delta: the previous take (50,644-51,188 B) and the constant shift together, so the
  same padding relationship between gate and reality is preserved.
- `kStartFreeBytes` (= `kStackBytes + kMinimumLargestBlockBytes`, `RadioPolicy.h:30`) follows.
- The measured-take comment block `RadioPolicy.h:25-26` gets a dated line recording the
  computed delta and the on-device confirmation step that remains open.

The X3 is the only profile affected by these two constants; the S3 (`kS3Profile`) is untouched.
This is deliberate: the S3 has PSRAM and is not short.

**Open item carried past the PR (owner, on device):** confirm the computed gate on a real X3 —
the `free=`/`largest=` line at the pairing screen must now pass, a real many-service HID remote
must still pair and page-turn, and `kStackBytes` is re-pinned once more from the fresh measured
take if the computed figure and the measured one disagree. The PR states this honestly instead
of claiming device verification.

## Change 3 — update the asserted constants in the host tests

`lib/BlePageTurner/test/RadioVerdictS3Test.cpp:33-38` hard-codes the current numbers:

```
EXPECT_EQ(kStackBytes, 49152u);
EXPECT_EQ(kStartFreeBytes, 81920u);
```

These become stale the moment `kStackBytes` moves. Update them to the new measured values in the
same commit that re-pins Change 2, and add a one-line `EXPECT_LE` comment that ties the constant
to the trim commit so the drift is caught next time. The S3-side assertions at lines 42-46 stay
fixed.

## Out of scope (explicitly)

- Any change to the 32,768 B reader block (`InflateReader.h:54`) — a DEFLATE window cannot be
  smaller.
- PSRAM options — the X3 has no PSRAM.
- The timezone half of #267 — tracked separately; this is the memory side only.
- `BLE_PAGE_TURNER_PROBE` is a diagnostic-only define, not shipped; use it to measure, do not
  leave it in `build_flags`.

## Verification

Build parity and host tests are the implementer's CI gate (steps 1-2). The device steps (step 3)
are the owner's and may land after the PR opens — the PR body says so explicitly.

1. Build parity: `pio run -e default` (C3 baseline incl. X3 config) and `-e x4pro` and `-e slim`
   — all must compile with the trimmed pools. S3/X4 use the same `[ble_page_turner]` block; a
   regression there shows as a failed GATT discovery in step 3.
2. Host tests: the `RadioVerdictS3Test`/`RadioVerdictTest` binaries reflect the new
   `kStackBytes`/`kStartFreeBytes` (step 3 assertions) and still pass.
3. X3 device (owner, post-PR), flash `pio run -e default -t upload -t monitor`:
   - trigger Settings → Bluetooth; capture the `required_free`/`largest` line and the new
     `free=`/`largest=` pair. Assert `free ≥ 81,920` (or the re-pinned value) and
     `largest ≥ 32,768` are now satisfied and the `Could not start Bluetooth` refusal is gone.
   - confirm the "Loading" overlay no longer appears (no silent restart needed).
   - pair a real BLE page-turner remote (many-service HID profile) and turn a page; assert the
     radio stays up once a book opens (the `RadioVerdict` Running phase must still see the bond).
   - second toggle off/on must not reboot-loop (the #269 latch + the new, lower constant).
