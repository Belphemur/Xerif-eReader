# Dev OTA PR firmware — build snapshots of open pull requests in the rolling dev pre-release

Extends [design docs/design/2026-11-07-dev-ota-slots.md](./2026-11-07-dev-ota-slots.md) §4
(the rolling `dev` prerelease and manifest contract). That doc's §4/§7/§9 conventions
apply; this doc only adds the pull-request publishing lane.

## 1. Summary

Today the X4 Pro dev channel (`dev-build.yml`) publishes only develop-merge builds:
newest 3 `xerif-dev-<sha>-x4pro.bin` assets plus `manifest.json` on the rolling
`dev` prerelease. Pull-request firmware is only attached as CI artifacts
(`pr-firmware-links.yml` comment) — the device cannot install them OTA.

This design adds: **open PRs get firmware on the same `dev` prerelease**, so a
candidate patch is installable from the device's Dev channel UI. Constraints:

- Only PRs **not opened by a bot** (author login contains "bot" case-insensitively
  → the publish lane skips silently: no asset upload, no manifest write; the
  artifact-links comment is unaffected).
- When the PR is **closed for any reason**, that PR's firmware is removed from the
  prerelease and its manifest entries are dropped.
- Fixed cap: **latest-per-PR, N=6** PR entries in the device list (newest first).
- The manifest is **always uploaded last** within any publisher; **publish and
  delete share one repo-wide concurrency group; delete has priority and cancels
  publish**.

## 2. Current state (verified, 2026-11-08)

- `dev-build.yml` — on every merge to develop: builds x4pro, stages
  `xerif-dev-<sha>-x4pro.bin`, carries over the existing manifest's entries,
  regenerates `manifest.json` (newest 3, dedupe by name, 4096-byte device cap —
  current manifest is 951 bytes, ~250 bytes per entry), uploads with `--clobber`,
  then prunes any asset not in the keep-set.
- `pr-firmware-links.yml` — on CI completion for pull_request events: updates the
  PR's comment with artifact links (ZIPs, 7-day expiry, browser + flash-tools
  required). Read-only against the release; never touches the `dev` prerelease.
- `DevOtaClient` (device) — fetches `manifest.json`, parses the `versions` array,
  shows each entry name as the version, downloads by URL, verifies SHA-256.

## 3. Design decisions

| id | decision |
|----|----------|
| D1 | **Trigger**: extend `pr-firmware-links.yml` (workflow_run on CI completion). It already runs on the base repo (write permissions work even for forked PRs), already resolves the PR, and runs after the CI build that produced the x4pro artifact. Publishing the artifact is the same event. |
| D2 | **Bot filter**: before any publication, skip silently if the PR author login contains "bot" (case-insensitive). Bare string containment, per owner instruction — no enumerable deny-list. |
| D3 | **Asset name**: `xerif-pr-<pull_number>-<short_sha7>-x4pro.bin`. Distinct prefix from `xerif-dev-`, so the dev-build prune step keeps it (keep-set = manifest names + manifest.json). Same PR number, new sha → replaces the old asset (same name, new sha). |
| D4 | **Device list cap**: fixed window N=6 PR entries in the manifest, newest first (by asset date). When the window overflows, the oldest PR entries are dropped from the manifest and their assets pruned. |
| D5 | **Manifest**: PR entries go into the same `versions` array — no device-side changes. The device list shows develop builds + PR builds together. |
| D6 | **Concurrency**: one repo-wide group `dev-release-manifest` shared by PR publish (pr-firmware-links.yml), PR delete (pr-dev-cleanup.yml), and dev-build.yml. Delete has priority (`cancel-in-progress: true`); publishes wait (`cancel-in-progress: false`). |
| D7 | **Manifest always uploaded last**: the firmware asset is uploaded first, then the manifest (`--clobber`). A failed manifest upload leaves the previous manifest live, referencing the previous manifest's assets. |
| D8 | **PR builds**: x4pro only (lean-device bias; the owner's device). |

## 4. Mechanism

### 4.1 Publish (pr-firmware-links.yml, after the comment update)

1. Resolve the PR (already done) and the PR author login. If the login contains
   "bot" (case-insensitive), skip publishing silently — no asset, no manifest
   change.
2. Download the `firmware-x4pro.bin` artifact from the CI run.
3. Rename to `xerif-pr-<number>-<sha7>-x4pro.bin` and upload to the `dev` release
   with `--clobber`.
4. Regenerate `manifest.json` from the current release assets: develop builds
   (newest 3, `xerif-dev-*` prefix) + PR builds (newest 6, `xerif-pr-*` prefix),
   dedupe by name, newest first, cap 4096 bytes. Upload the manifest last.

### 4.2 Delete (new `.github/workflows/pr-dev-cleanup.yml`)

- Trigger: `pull_request` closed (any reason).
- Delete `xerif-pr-<number>-*` assets from the `dev` release.
- Regenerate `manifest.json` from the current release assets (same shared
  generation code path), dropping the PR's entries.
- Manifest uploaded last.

### 4.3 dev-build.yml (develop merge publish)

- Joins the same concurrency group `dev-release-manifest`.
- The manifest step keeps PR entries: `keep = 3` applies to develop builds only;
  PR entries (`xerif-pr-*` prefix) are retained up to the N=6 window.

### 4.4 Manifest generation (shared)

The generation logic must be identical in all three workflows; it is
implemented once as `.github/workflows/generate_dev_manifest.py`, invoked by all
three. The manifest is rebuilt from the current release assets each time
(develop builds + PR builds, newest first, cap 4096 bytes), so the device list
always reflects the prerelease's actual assets. The dev-build.yml manifest step's `keep = 3` cap
applies to develop builds only; PR entries are kept separately up to the N=6
window.

## 5. Failure modes

- **Publish cancelled by a delete**: the PR's old manifest entry is gone
  (the delete removed it), and the new asset is not uploaded. The next push to
  that PR regenerates it. The dev channel stays consistent.
- **Delete cancelled by a publish**: the PR's firmware is removed by the delete;
  the publish then re-adds it. The PR is already closed — the next push to the
  PR is impossible, so this is the accepted state: the asset stays until the
  next dev-build publish prunes it. (Acceptable; the PR is closed.)
- **Publish cancelled by a dev-build**: the PR's firmware is not uploaded; the
  next push to the PR regenerates it. The dev channel stays consistent.
- **Manifest over 4096 bytes**: the publisher fails loudly (the publish step
  exits non-zero) before the manifest is uploaded — the previous manifest stays
  live. The device never sees a truncated manifest.

## 6. Consequences

- The device's Dev channel UI lists develop builds + PR builds together. The
  owner can install a candidate patch from the device.
- No device-side changes: `DevOtaClient` needs nothing.
- The bot filter covers dependabot-style bots and GitHub Actions bots.
- Fork PRs: the publish workflow runs on the base repo (write permissions work),
  so fork PRs are also published. The delete lane triggers on the base repo for
  every `pull_request.closed` event — fork PRs included — and keys the asset
  name on the base repo's PR number, so forked PR firmware is removed on close
  as well.

## 7. Verification plan

- Host tests: none (CI workflow only). Verification is on-device: the dev
  channel UI lists develop builds + PR builds; the PR's firmware is installable
  and removed when the PR closes.
- The manifest is verified against the 4096-byte cap with PR entries.

## 8. Amendment (2026-11-09): carried PR entries keep their stamped version

The develop publish lane (dev-build.yml) regenerates the manifest from the
release's assets and never passes `--pr-new`/`--pr-branch`, so every carried
PR entry lost its `version` field on the next develop publish — the picker
degraded the row to the raw asset name. Carried `xerif-pr-*` entries now
retain the version the PR lane stamped for the same asset (the asset name
embeds PR number + head sha, so the prior-manifest match is exact);
`xerif-dev-*` entries still always re-derive from
`git_branch.py --print-dev-version` so a stale identity can never ride
along. Also (2026-11-09, no "detached" stamps): the derivation resolves the
branch from the CI override or refs pointing at the commit and degrades to
"unknown" — a stamped version must never carry a detached-HEAD label. See
the dev-OTA design doc §17 for the picker-side consent binding.
