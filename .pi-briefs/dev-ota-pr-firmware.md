# Brief — PR firmware on the rolling dev pre-release (direct-to-develop CI workflow change)

You work in `/home/balor/workspace/eink/crosspoint-x-reader` on branch `develop`,
HEAD `3b0dfb0cd`. This is CI infrastructure work ONLY.

## Read first, in full
1. `docs/design/2026-11-08-dev-ota-pr-firmware.md` (committed as 3b0dfb0cd) — this
   doc is THE contract. Every claim there has file evidence behind it.
2. `.github/workflows/pr-firmware-links.yml` (today's PR-attached-artifacts flow)
3. `.github/workflows/dev-build.yml` (today's dev channel publisher)
4. `.github/workflows/release.yml` (for reference)

## Scope: files under `.github/workflows/` only.
Never touch C++ code or the freeink-sdk submodule pointer at all.

## Tasks

### T1 — Extend pr-firmware-links.yml (publish lane)
After the existing comment-update step, add PR firmware publish:
- Gate on TWO conditions: (a) the PR author login does NOT contain the substring
  "bot" case-insensitively — else SKIP silently (no asset upload, no manifest
  write); (b) the CI run's conclusion is 'success'.
- Download the `firmware-x4pro.bin` artifact from the CI run, rename to
  `xerif-pr-<PR_NUMBER>-<shortSha7>-x4pro.bin`, upload to the `dev` prerelease
  with `--clobber` (replaces any prior same-name entry).
- Regenerate `manifest.json` from the release assets: newest 3 `xerif-dev-*` entries
  PLUS newest 6 `xerif-pr-*` entries, combined dedupe by name, newest first.
- Upload the manifest LAST (asset first, then manifest) — the atomic-publish
  invariant: a failed manifest upload leaves the previous manifest live.

### T2 — New workflow pr-dev-cleanup.yml (delete lane)
- Trigger: `pull_request` closed, `permissions: contents: write`.
- Delete `xerif-pr-<PR_NUMBER>-*` assets from the `dev` release.
- Regenerate `manifest.json` (same shared generation rules as T1).
- Manifest uploaded last.

### T3 — dev-build.yml (join concurrency group + PR keep cap)
- Join the shared repo-wide concurrency group `dev-release-manifest` with
  `cancel-in-progress: false` (publish waits; the delete lane cancels it).
- The manifest step keeps PR entries: `keep = 3` applies to develop builds only;
  PR entries (`xerif-pr-*`) are retained up to the N=6 window.
- The prune step keeps whatever the manifest names + `manifest.json` — PR assets
  that dropped out of the manifest window get pruned by the same keep-set.

### Shared generation logic
Both T1 and T2 (and dev-build.yml) regenerate manifest.json from the same rules.
If logic is shared, do it via a checked-in helper script referenced from the
workflows, or a single reusable workflow file — no duplicated logic.

## Hard requirements
- Worktree stays the main checkout on branch develop; commit directly to develop.
- Never merge. Never amend authorship. Default/global git config only.
- Commit per task (T1, T2, T3 separately), each commit message a conventional
  title mentioning the design doc.
- Report-first discipline: write the report at round start and after every commit.
- Marker when all three tasks are committed and verified: `DEV_OTA_PR_FIRMWARE_DONE`.
- Push develop to origin after every commit (the owner explicitly directed this
  change lands directly on develop). Never merge; never amend authorship.
- Gates before you consider a task done: every workflow file you touched must
  parse as valid YAML (`python3 -c "import yaml, sys; [yaml.safe_load(open(f)) for f in [...]]"`),
  and the design doc must be updated in lockstep if you deviate from it.
