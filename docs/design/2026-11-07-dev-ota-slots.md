# Dev OTA slots — dual-slot firmware chooser, CI dev channel, recovery ladder

Date: 2026-11-07
Status: Draft for owner review (owner-directed implementation to follow)
Scope: firmware + CI. SDK-side `RecoveryBoot` port is a **follow-up job** (see §8) —
it changes the freeink-sdk submodule and must land as its own PR before the pin
can move.

## 1. Summary

Development builds today reach the X4 Pro only by USB. This design gives the
device, over WiFi:

1. **A firmware chooser** — every build (release included) can boot either
   slot from a settings menu. The choice is instant (no reflash) and sticky.
2. **A CI dev channel** — every merge to `develop` builds the x4pro image,
   publishes it to a public rolling `dev` prerelease, and keeps the last
   **3 dev versions**. The device lists them and installs one on demand.
3. **A recovery ladder** — from an in-UI slot switch down to the ROM
   serial mode, with the SDK's dormant `RecoveryBoot` shim wired in as the
   held-button escape hatch.

Flash roles are fixed by position: **`app0` = stable (last release),
`app1` = dev (latest installed dev build)**. No partition-table change; the
release partition layout is untouched and release firmware behavior is
byte-identical (§7 gating).

## 2. Current state (verified)

| Concern | Today | Evidence |
|---|---|---|
| Partition table | `app0`/`app1` = `ota_0`/`ota_1`, 6.25 MB each; ~3.4 MB spiffs; 16 MB flash | `partitions.csv` |
| Actual image size | x4pro `firmware.bin` = 5,453,648 B (5.2 MB) — three resident slots are physically impossible on 16 MB | `.pio/build/x4pro/firmware.bin`, 2026-11-07 |
| Boot selection | Bootloader reads `otadata` only; firmware already writes a fresh entry directly | `src/network/OtaBootSwitch.h` (`ota_boot::switchTo`) |
| Install path | Raw `esp_partition_erase_range`/`write` + `otadata` switch, bypassing `esp_image_verify` (patched vendor images) | `src/network/FirmwareFlasher.*`, `freeink-sdk/libs/hardware/RecoveryBoot/FirmwareFlasher.*` |
| Release OTA | Signed-manifest flow from `releases/latest`, repo host parameterized (`OTA_REPO`); unsigned manifests tolerated with SHA-256 pin + board guards | `src/network/OtaUpdater.cpp:30,107-114` |
| Recovery shim | SDK `RecoveryBoot` library: boot-combo read as first line of `setup()`, repoints `otadata` at slot 0; **dormant** — commented out in `platformio.sample.ini:53`, zero references in `src/` | `freeink-sdk/libs/hardware/RecoveryBoot/` |
| Manual flash floor | ROM serial download mode (hold BOOT = GPIO0) + web flasher / esptool; external SPI programmer guide | `docs/fix-bricked-xteink.md` |
| X4 Pro input | Two nav keys Left=GPIO0, Right=GPIO7 ("next"), Power=GPIO3, plain active-low digital GPIOs; **GPIO0 is a boot strap — must not be held during reset** | `BoardConfig.h:1901-1907` |

## 3. Slot roles and the chooser (all builds)

- Roles are **fixed by partition position**: `app0` stable, `app1` dev. Every
  writer (release OTA, dev installer, recovery flasher) knows its target from
  the role, never from "whatever isn't running."
- A new **slot chooser activity** ships in **every build**: it lists both slots
  with the version read directly from each image's `esp_app_desc_t` header
  ("Dev: empty" when nothing is installed), marks the running slot, and boots
  the other slot via `ota_boot::switchTo()` + reboot.
- The choice lives in `otadata` only — sticky until changed, no boot-time UI,
  no boot delay, no NVS shadow copy.

## 4. CI dev channel (dev builds only)

- New `.github/workflows/dev-build.yml`: `on: push: develop` plus manual
  dispatch. The publish job is gated to `develop` (a dispatch on another ref
  never publishes), and the workflow concurrency group does **not** cancel
  in-progress runs — a newer push can never kill a run that is publishing.
- Builds **x4pro only**, publishes the pio build output as-is (identical to
  the release flow's asset staging; the design originally named a
  `patch_firmware_image.py` step that this repository does not ship), and
  stamps the version with the short SHA.
- Publishes to a public rolling prerelease tagged `dev`:
  - assets named `xerif-dev-<sha>-x4pro.bin`,
  - a regenerated `manifest.json` listing the **newest 3** versions (name,
    URL, SHA-256, build date); a retained entry is kept only when its binary
    is still present, so no manifest URL can 404,
  - in-place publication: the release is created only when missing, assets
    are uploaded with `--clobber`, and assets outside the newest-3 window
    are pruned after the new manifest lands. The release is never deleted,
    so a failed or cancelled run cannot wipe the channel.
- Manifests are **unsigned** — SHA-256 pinned only, matching the existing
  unsigned-manifest fallback in `OtaUpdater` (`OtaUpdater.cpp:107`). Dev builds
  are not release artifacts; the device rejects any download whose SHA-256
  does not match the manifest (fail closed).

## 5. Device dev channel UI (dev builds only)

- Header shows what is running (`stable vX` / `dev <sha>`) and which slot.
- **Install**: list the 3 CI dev versions → download → SHA-256 verify against
  the manifest → validate with the existing full image check → raw-write
  `app1` → `switchTo(dev)` → reboot.
- **The running-dev dance**: a running slot can never be overwritten, so when
  the device boots dev and the user installs a new dev build, the firmware
  writes an NVS `pending dev install <version>` flag, reboots into stable,
  and a boot hook completes the flow (download → write `app1` → `switchTo`
  → reboot). Fully automatic; two reboots. When running stable the install is
  single-pass.

## 6. Rollback semantics

- A freshly written dev slot is recorded with `ota_state = NEW`
  (`kOtaImgNew`, `OtaBootSwitch.h`). The running firmware marks the slot valid
  only after a successful full boot (fonts loaded, display painted).
- If the dev build never confirms, the bootloader falls back to the other
  slot on its own. Requires `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` on the
  x4pro env — verify as an implementation task.

## 7. Gating matrix

| Feature | Release builds | Dev builds |
|---|---|---|
| Slot chooser activity | ✅ | ✅ |
| Release OTA (signed manifest) | ✅ unchanged | ✅ |
| Dev-channel settings entry | ❌ gated (`CROSSPOINT_DEV_OTA`) | ✅ |
| Install machinery + boot hook (resume path) | ✅ compiled in, dormant | ✅ |
| `dev-build.yml` publishing | n/a (CI only, on develop) | — |

Only the dev-channel **settings entry** is behind `CROSSPOINT_DEV_OTA`. The
install machinery and the boot hook compile into every build — including
release envs — because the running-dev dance's completion pass runs in
whichever slot boots next (§5), and that slot is normally a release build
with the entry gated out. In release builds the machinery is dormant:
nothing ever arms the pending-install flag, so the boot hook costs one NVS
read on the first loop pass and the release OTA path is unchanged. (As-built
deviation, owner-reviewed 2026-11-07: an earlier draft of this section said
"compiled out".)

Documented quirk (standard 2-slot OTA): a release OTA while booted from stable
lands in the non-running slot and therefore overwrites the dev slot. The dev
build is restorable from the menu in one action — accepted.

## 8. Recovery ladder (in order)

1. **Slot chooser** (all builds) — for a dev build that boots but misbehaves.
2. **Boot-combo escape hatch** — Power (GPIO3) + Next (GPIO7) held at
   power-on → `RecoveryBoot` repoints `otadata` at slot 0 (stable) and
   reboots. Chosen over any Left/Up variant because **GPIO0 (Left) is a boot
   strap** — holding it at reset enters ROM download mode
   (`BoardConfig.h:1904`). Requires wiring `RecoveryBoot` into the firmware
   and making the combo per-board (X4 Pro: power+next) — an SDK change,
   **follow-up job + SDK PR + submodule pin re-point**.
3. **SD-card manual reflash** — `RecoveryBoot/FirmwareFlasher` validates the
   image the way the bootloader would (magic, chip-id, segment walk, XOR
   checksum, SHA-256 trailer) and raw-writes a slot. Same enablement as #2.
4. **ROM serial download mode** — hold BOOT (GPIO0) + USB → esptool / web
   flasher. Hardware floor, exists today.
5. **External SPI programmer** — `docs/fix-bricked-xteink.md`. Last resort.

**Rejected: a factory/safeboot recovery partition.** 16 MB is full (2 × 5.2 MB
images live in 6.25 MB slots + 3.4 MB spiffs); adding a factory slot forces a
partition migration, and it only helps when *both* OTA slots are unbootable —
a state the role rules (stable slot never written by dev installs, §3) make
practically unreachable. The honest gap: `RecoveryBoot` lives inside the main
firmware and cannot save a build that dies before `setup()` — that is what
path #4 is for.

## 9. Decision log (DRY > SOLID > KISS)

- **DRY** — the dev installer reuses `OtaUpdater`'s fetch/SHA-256 machinery and
  the `FirmwareFlasher` raw-write+otadata path instead of a second flash
  implementation; the chooser reads `esp_app_desc_t` straight from the slot
  instead of adding an NVS version registry (a second copy of "what version is
  in slot N" that would silently diverge from the image header). What the loser
  would have cost: two code paths to flash and two version stores to keep in
  agreement.
- **SOLID** — single-writer rules make the dangerous states structurally
  impossible: the running slot is never a write target; `otadata` is the only
  boot-choice store; slot roles are fixed by position so every writer has
  exactly one legal target; a SHA-256 mismatch drops the download (fail
  closed, never half-written). `ota_state = NEW` + mark-valid-after-boot
  closes the partial-failure window rather than relying on the user to notice
  a bad dev build.
- **KISS** — the third OTA slot, a factory recovery partition, a boot-time
  chooser UI, and an NVS boot-shadow are all rejected: each adds machinery
  whose job is already covered (§8 ladder + sticky `otadata`). Once knowledge
  exists once, the simplest structure wins.
- **Lifetime rule** — manifest entries and slot descriptors are POD structs
  copied by value; no new raw owning pointers anywhere in the design.

## 10. Consequences

- **No partition migration, no reflash** to adopt the scheme — the existing
  table is exactly right for a 5.2 MB image.
- Dev builds become public artifacts of the public repo (rolling `dev`
  prerelease, unsigned, SHA-pinned) — accepted by the owner 2026-11-07.
- CI gains a build job per develop merge (x4pro only).
- The device holds the last stable release and the last installed dev build
  simultaneously, switchable in one tap; CI-side history of 3 dev versions
  allows reinstalling any of the last three without touching stable.
- RecoveryBoot wiring is deferred to the SDK follow-up job; until then the
  ladder is chooser → ROM mode (paths #1, #4).

## 11. Verification plan

- `pio run -e x4pro` (and default env) green with the feature compiled in;
  release env compiles with zero dev-channel code paths.
- Host unit tests for the manifest parser and the slot-descriptor reader.
- Release env compiles with the dev-channel settings entry gated out; the
  compiled-in install machinery stays dormant (§7).
- `bin/clang-format-fix` clean.
- On-device soak (owner): chooser switch stable↔dev, dev install single-pass
  and via the dance, power-off during install, Power+Next hatch (after the
  SDK follow-up lands).
