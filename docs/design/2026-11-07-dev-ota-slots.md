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
  single-pass. The dance refuses to start unless the stable image carries the
  flash-scannable `kResumeHookMarker` (embedded next to the boot hook): the
  esp_app_desc_t version field is an opaque constant from the prebuilt Arduino
  core (`libesp_app_format.a`, e.g. `6671d0b`), so version comparison cannot
  identify hook-bearing images.

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

- `pio run -e x4pro` (and default env) green with the feature compiled in.
- Host unit tests for the manifest parser and the slot-descriptor reader.
- Release env compiles with the dev-channel settings entry gated out; the
  compiled-in install machinery stays dormant (§7) — release builds are not
  free of dev-channel code, only of dev-channel entry points.
- `bin/clang-format-fix` clean.
- On-device soak (owner): chooser switch stable↔dev, dev install single-pass
  and via the dance, power-off during install, Power+Next hatch (after the
  SDK follow-up lands).

## 12. Addendum (2026-11-08): per-build dev version stamping + dev list touch sizing

### Root cause (soak evidence)

The dev slot always displayed "the previous firmware" after a successful
install. `esp_app_desc_t.version` with the prebuilt Arduino core is a constant
string baked into `libesp_app_format.a` (see `OtaSlots.h:41-43`) — identical in
every dev build regardless of commit. `dev-build.yml` names no image-patching
step (its header comment records the design doc's `patch_firmware_image.py`
as never having shipped), so `platformio.ini`'s static `version =` reaches
flash unchanged. The install works; the slot chooser cannot tell the builds
apart.

### Decision

1. **CI-side stamping (dev-only).** After `pio run -e x4pro`, the
   `dev-build.yml` staging step rewrites the 32-byte version field of the
   image's `esp_app_desc_t` (magic `0xABCD5432` at offset 0x20, version at
   0x30 — the exact layout `OtaSlots::readSlotInfo` reads) to
   `<version>-dev-<shortsha>` padded with NULs. Stamping happens on the
   `dist/` copy **before** manifest SHA-256 computation so the manifest hash
   matches the published asset. Release builds are untouched: release flow
   gains no step, and its descriptors stay the signed-release constant.
2. **Dev list touch sizing.** The dev version list (`DevOtaActivity` LIST
   state) renders 10-pt rows whose hit bands follow the drawing geometry.
   Rows must adopt the settings menu's row geometry (its font/row height and
   spacing) so the tap targets are finger-sized; hit bands keep reusing the
   drawing geometry so they can never drift.

### Rejected

- Building with a `PROJECT_VER` injection: with the prebuilt Arduino core the
  descriptor comes from `libesp_app_format.a` and cannot be overridden per
  build from `platformio.ini` env — the post-build patch is the only reliable
  hook.
- NVS shadow copy of installed versions: violates the §3 rule of reading
  straight from image bytes (would diverge exactly like the resume-hook
  marker case).

## 13. Addendum (2026-11-08): the dev lane consumes the CI artifact

### Decision (owner: "the way we build the image — why is it different?")

dev-build.yml rebuilt the x4pro image itself (`pio run -e x4pro`) even though
ci.yml had just built the identical commit on every develop push and uploaded
the binary as an artifact (7-day retention). That is a fork of the build
system: two builds of every commit, and a dev asset that could diverge from
what CI actually tested — worse, the dev prerelease could receive a commit
whose full CI matrix (host tests, clang-format, the other boards) would fail.

The dev lane now follows the pr-firmware-links.yml pattern exactly:

- **Trigger**: `workflow_run` on "CI" completed for `develop`
  (`github.event.workflow_run.conclusion == 'success'`,
  `head_branch == 'develop'` — the D6 develop-only publication lane). A dev
  build is published only from a CI-green commit; there is no second build.
- **Source**: `gh run download <workflow_run.id> --name firmware-x4pro.bin`
  — the exact artifact ci.yml produced for that commit.
- **Unchanged**: the stamp (`git_branch.py --print-dev-version` →
  `patch_dev_version.py`), the `--validate` publish gate, the manifest
  carry-over/regeneration, and the 3-version prune. The workflow checks out
  `ref: ${{ github.event.workflow_run.head_sha }}` so the stamp and the
  manifest date derive from the triggering commit itself.
- **Trade-off accepted by the owner**: the dev build publishes ~10 minutes
  later (after the full CI matrix) instead of racing CI with a single-env
  build.

Rejected: keeping the in-lane build (a forked build system — DRY); gating on
`esptool image-info` (it exits 0 on checksum-invalid images — reports, does
not enforce; the script's `--validate` mirrors FirmwareFlasher and is the
enforcing verifier).

Note: `workflow_run`-triggered workflows execute the workflow file from the
default branch (develop) — the conversion is effective the moment it lands on
develop.

## 14. Addendum (2026-11-08): PR firmware is stamped too — one manifest schema

### Decision (owner: "the change to CI should also be present for PR — PR updates the manifest")

PR firmware installs through the same dev picker into the same dev slot as
develop builds, so it must get the identical treatment:

- **Stamping**: `pr-firmware-links.yml` stamps the PR asset with the SAME
  `git_branch.py --print-dev-version <head_sha>` derivation, with the branch
  taken from `github.event.workflow_run.head_branch` (the PR head branch —
  the publish lane's own checkout is on develop, so the branch is passed
  explicitly; git_branch.py gains an optional `--branch` override, its only
  new parameter). The stamp runs BEFORE the `--validate` gate and before the
  manifest hashes the asset.
- **Manifest**: `xerif-pr-*` entries carry a `version` field exactly like
  `xerif-dev-*` entries — the "PR entries get no version" exception is
  deleted. One manifest schema: every entry has name, url, sha256, date,
  version. The device display needs no change (it already prefers `version`
  and falls back to the name).
- **Fit rule unchanged**: a long PR branch name drops out of the stamped
  descriptor first (`<base>-dev-<sha>`), never the SHA tail.

### Superseded

§12 "dev channel only" stamping scope and the generator's "PR entries get no
version field" exception. Supersede reason: the slot descriptor must match
About for EVERY installable image, and the manifest is one schema —
per-entry exceptions are the DRY violation this addendum closes.

### Rejected

Pseudo-versions from the asset name: the device's name-fallback covers
manifests that predate this change; new entries carry the real derivation.

### 15. Install-time slot auto-selection (2026-11-08)

**Decision.** The install SOURCE picks the target slot; the user is never asked.

- `gh_release` (stable channel, `/releases/latest`): ALWAYS the stable slot
  (`ota_slots::stableSlot()`).
- Dev channel and PR builds (the `dev` prerelease manifest): the dev slot —
  already the behavior of `DevOtaClient::installFromManifestEntry`, unchanged.
- Serial upload: the stable slot (inherent — the serial flash offset is `app0`).
- SD-card installs follow the same rule. Classification is prefix-based on the
  exact asset grammar the CI lanes emit: `xerif-dev-` and `xerif-pr-` name the
  dev slot; every other `.bin` (including `xerif-<version>-<device>.bin` and
  unknown prefixes) names the stable slot.

**Rationale.** `OtaUpdater::installUpdate` writes
`esp_ota_get_next_update_partition(nullptr)` — the INACTIVE slot. Running
stable, a gh_release install silently overwrites the dev slot with a stable
image and destroys the dev install there. The dev channel never had this bug
because it hard-wires the dev slot.

**Stable→stable dance.** The running slot is never a write target (§3).
Installing a gh_release while running the stable slot therefore dances through
the dev slot — the mirror of the §5 dev dance: record the pending-install
request, reboot into the dev slot, and let the boot hook complete the install
into `app0`. This requires the dev slot to hold a marker-capable image
(`kResumeHookMarker`); when the dev slot is empty or too old, REFUSE with a
clear error instead of overwriting anything.

**Recovery mode exception.** `SdFirmwareUpdateActivity` in recovery mode keeps
writing the NON-running slot: it is the escape hatch that heals a bricked dev
slot and must never touch the slot it runs from. The source rule applies to
normal (non-recovery) SD installs only.

**Rejected.** Keeping `esp_ota_get_next_update_partition` for gh_release
installs (loses the dev slot on every stable install while running stable);
refusing stable→stable installs outright (breaks updating an old stable while
running the newest stable).
### 16. The swap-slot update strategy (2026-11-08)

**Constraint.** A resident third OTA slot is physically impossible. The flash
budget: `app0` + `app1` = 2 x 6.25 MB, `spiffs` = 3.375 MB, flash = 16 MB;
the actual image is 5.45 MB, so three resident slots need >= 16.35 MB even with
ZERO data partitions. (§3 line: "three resident slots are physically
impossible on 16 MB".)

**Decision.** The two resident slots keep fixed roles (§3: app0 = stable,
app1 = dev). The SWAP SLOT is the non-running resident slot acting as the
swap WORKER: the device restarts into it and its image completes the write of
the TARGET slot. The worker's own content is NEVER overwritten by the install
it performs.

Every install path resolves the same way:
1. TARGET slot = the slot chosen by the §15 source rule (gh_release -> stable;
   dev/PR channel and dev/pr-classified SD files -> dev).
2. If TARGET is not the running slot: write it directly.
3. If TARGET IS the running slot: dance — stage the asset on the SD card
   (`/.crosspoint`, the swap SPACE), record the pending install, restart into
   the OTHER slot (the swap worker), and let the boot hook complete the write.
4. If the swap worker is unavailable (empty slot, or lacks
   `kResumeHookMarker`): REFUSE with a clear error. NEVER fall back to writing
   the wrong-role slot.

**Routing matrix.**

| Running | Source                        | Target slot | Path |
|---------|-------------------------------|-------------|------|
| stable  | gh_release                    | stable      | dance via dev worker |
| stable  | SD stable-classified          | stable      | dance via dev worker |
| stable  | SD dev/pr-classified          | dev         | direct write |
| dev     | dev channel / PR build        | dev         | dance via stable worker (§5) |
| dev     | SD dev/pr-classified          | dev         | dance via stable worker (§5) |
| dev     | SD stable-classified          | stable      | direct write |
| dev     | gh_release                    | stable      | direct write |
| any     | swap worker unavailable       | —           | refuse |

**Invariant.** A dev-channel update on an existing dev release NEVER touches
app0; a gh_release update NEVER touches app1. The only writes into a fixed
role's slot are installs whose SOURCE rule targets that role.

### 17. Dance consent identity and picker legibility (2026-11-09)

**Owner report (round 2 hardware).** Selecting a PR entry in the dev picker,
dancing into stable, and completing the install came back on the LAST
DEVELOP build instead of the picked PR build: the resume pass matched the
recorded payload by NAME and silently defaulted to the newest manifest
entry (index 0 = newest develop build) when the recorded name was missing —
pruning, a superseding push on the same PR (the rolling manifest rotates
both), and fetched entries beyond the parser cap all degrade to that silent
fallback.

**Decision.** The §16 refusal rule gains an explicit consent identity for
the §5 dev-channel dance, mirroring the §16 arm-time SHA-256 the SD dances
already record:

- **Arm**: the pending record carries the asset name AND the manifest
  entry's SHA-256 (the shared pending digest key). Arming fails closed when
  the entry carries no digest, or when the digest record cannot be
  written — the dance never leaves the ground either way: the selection
  is cleared and the device stays on its slot.
- **Resume**: `dev_ota::bindPendingEntry` binds the record to the freshly
  fetched manifest. `Exact` (name + digest) installs exactly that entry.
  `EntryMissing` / `DigestMismatch` / `ShaUnverifiable` all REFUSE: no
  fallback index, no "install the newest instead". A digest-less pending
  record (pre-consent firmware) keeps the name-only bind so an upgrade
  does not strand it.
- **Refusal unwind**: on every resume-side abort the record is cleared and,
  while the §16 write latch is still false, otadata is restored to the dev
  slot (the firmware the device ran before the dance — the
  `SdFirmwareUpdateActivity::unwindDance` contract). Dev-channel installs
  now mark that latch before the first erase so the guard is truthful;
  with the latch true the worker stays selected (never unwind a possibly
  half-written slot) and the FAILED screen is the exit point.
- **Labels**: PR entries lead with their PR number ("pr 303 772d2c5") —
  the number is what distinguishes a PR build from develop builds, even
  when a stamped version exists. Dev builds keep
  version -> "dev <sha>" -> raw name. The INSTALLING screen draws the
  exact entry being installed and says "resuming from stable slot" during
  a dance boot.
- **Manifest**: carried PR entries retain the version the PR lane stamped
  for the same asset — the develop publish lane regenerates the manifest
  without the PR context, and without retention it silently downgraded
  every carried PR entry back to the raw name.
- **Never "detached"**: dev version derivation resolves the branch from
  `CROSSPOINT_GIT_BRANCH` (CI override) or refs pointing at the commit,
  and degrades to "unknown" — CI checkouts are detached and a stamped
  version must never carry the detached label (it leaked into the compiled
  CROSSPOINT_VERSION shown in About).
- **Release stamping**: `release.yml` stamps both release assets with the
  plain tag version (the pio output otherwise carries the prebuilt core's
  constant esp_app_desc version), then runs the same verify + full-verify
  gates as the dev lane. §12's "release stays unstamped" note is superseded.

Host pins: `DevOtaPolicyTest` (the PR-entry-over-develop bind, the
refuse-instead-of-default contract, the legacy name-only bind, the label
rules) and `test_dev_version_stamp.py` (branch-override derivation, carried
PR retention, dev re-derivation).

### 18. The dev channel ships on X4 Pro releases (2026-11-09)

**Decision.** On the X4 Pro the dev-channel settings entry is present in
EVERY build — stable releases and RCs included (`CROSSPOINT_DEV_OTA=1` is
defined in `env:x4pro-gh_release` and `env:x4pro-gh_release_rc`, matching the
dev envs). Other boards keep the §7 gating (entry compiled out of release
builds).

**Rationale.** The machinery was never release-gated: the install plumbing,
the boot hook and the picker all compile into every build (§7), because the
running-dev dance's completion pass executes in whichever slot boots next.
Only the UI entry was hidden behind `#if CROSSPOINT_DEV_OTA`, which the
release envs did not define — so an X4 Pro on a stable release could not see
the channel that was one reboot away. That hiddenness bought nothing: the
entry cannot break a stable install (routing is by source, §15; a dev
selection lands in the dev slot and the running stable firmware is never
overwritten), and the recovery combo (Power + Next) always boots the stable
slot, so opting into a dev build remains reversible without cables.

**Consequence.** X4 Pro devices on a stable release can opt into a dev build
(and back) purely from Settings. A release image for X4 Pro now shows:
Check for Updates, SD Firmware Update, Firmware Slots, Dev Channel — all
unconditional. Non-X4-Pro release envs are unchanged.

### 19. Grouped picker order + local-time dates (2026-11-09)

**Decision (owner hardware report, X4 Pro).** The version picker's flat
newest-first mix interleaved PR and develop builds, and its row dates were
the manifest's raw UTC stamps (device clock read 15:02 while a row showed
20:56). The picker now:

- **Groups by build kind** (`dev_ota::pickerRows`, built on the pure
  `pickerOrder` in `DevOtaPolicy`): a bold "Pull requests" header row, the
  PR builds ordered by PR id DESCENDING (newest build instant first within
  one PR), then a "Dev builds" header row, the develop builds newest first.
  A group with no entries contributes no header. Header rows are neither
  selectable nor touch targets — the selection index stays a manifest entry
  index (the §16 dance binds by name+SHA against that index, unchanged), and
  Up/Down traversal plus touch rects skip the separators. The scroll window
  and the scrollbar thumb count display rows (entries + headers).
- **Local-time dates** (`compactDevDate(iso, offsetMinutes, …)`): the
  stamp's own ISO offset — dev stamps carry the commit's zone (`git show -s
  --format=%cI`, e.g. `+02:00`), PR/carried entries are GitHub `created_at`
  (`Z`) — is subtracted to the UTC instant, then the device clock's UTC
  offset is added. The offset is resolved by the activity through
  `SETTINGS.clockEffectiveOffsetMin()` (the status bar clock's resolution:
  live IANA zone, DST folding, cached fallback offline) at render time. A
  stamp that shapes as a date but is not a real instant falls open to the
  previously-shown verbatim UTC slice; an unrecognizable shape still hides
  the date (as before).

The pure policy functions are pinned in `DevOtaPolicyTest` (group order,
within-PR ordering, undated entries running last in manifest order,
header-row classification, +6h/−5h/minute-offset conversions, day and year
boundary rolls, stamp-own-offset handling, verbatim fall-open).

### 20. Slot-chooser prints the stamped build identity (2026-11-09)

**Decision (owner hardware report, X4 Pro).** The Firmware slots screen read
each image's `esp_app_desc_t.version` verbatim: a dev-stamped field is the
truncated fit product, and the row garbled — no PR number, a mangled value
against the label. The dev row's real identity is this two-step: the slot's
stamped dev version cross-references the CURRENT dev manifest (the picker
can fetch it — `pr-firmware-links.yml` stamps with the head branch, so a
stamp can never carry a PR number; the number exists only in the asset
name). The grammar knowledge for both consumers lives in ONE module
(`dev_ota` in `DevOtaPolicy.{h,cpp}`):

- `parsePrName` — the PR asset name parse (`"xerif-pr-<num>-<sha>"`),
  moved from the picker's file-local helper to the public record the
  picker (`entryLabel`, `entryGroup`) AND the slot chooser share.
- `parseDevStamp` — the stamped dev version parse
  (`"<base>-dev-<branch>-<sha>"`, self-identifying by the `-dev-` marker;
  the sha is the segment after the final dash, 4..12 hex chars —
  `fit_version` guarantees the `-dev-<sha>` tail never gets cut). Stable
  releases, the prebuilt core's constant and erasure padding return false.
- `slotRowValue` — one composition for both slot rows: a manifest PR sha
  match (prefix compare, first manifest entry wins) renders the picker's
  own `"pr <num> <sha>"` badge; otherwise the stamped version itself (the
  honest build identity). Then the value shrinks by recognized shapes only
  (full stamp → branch-dropped → shorter sha → shorter base) until the
  value plus the running marker fits the row's value slot, measured in the
  theme's small font from the same content-band geometry `buildScreen`
  renders with. Bad input prints verbatim and never becomes "unknown".

The slots screen never owns the radio: the manifest fetch runs in
`onEnter` only when WiFi is already connected (best-effort, memory-claimed
like the picker's), and without it the stamp itself stays the row value —
no connection prompt, no silent restart on exit. Deviation from the
brief's one-entry-point sketch: `prNumOut` was removed from the stamp
parse (a stamp cannot carry a PR number — that is what made the cross-
reference necessary); the returned sha plus the manifest cross-reference
produce the same identity with no dead output. Pinned in
`DevOtaPolicyTest` (§20 suite: grammar matrix 7..12-char shas, base/
branch/sha decode, cross-reference resolution and first-entry conflict
rule, no-manifest/no-match verbatim, fit-or-shape-clamp over every
capacity, branch-then-sha shrink steps, badge shrink-to-number, garbage
verbatim, entry-vs-slot one-badge parity).
