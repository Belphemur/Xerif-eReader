# Brief — Fix publish lane artifact rename: `mv artifact/firmware-x4pro.bin` fails

You work in `/home/balor/workspace/eink/crosspoint-x-reader` on develop (HEAD 44960a493).
The PR-firmware publish lane failed on the live PR #303 CI completion. Root cause is
verified — one line.

## The bug (file:line evidence)

`.github/workflows/pr-firmware-links.yml` line 224:

```yaml
mv artifact/firmware-x4pro.bin "${asset}"
```

The CI workflow uploads the artifact with `name: ${{ matrix.asset }}` = `firmware-x4pro.bin`,
but the file inside the artifact is `firmware.bin` (ci.yml:238: `path: .pio/build/${{ matrix.env }}/firmware.bin`).
After `gh run download --name firmware-x4pro.bin --dir artifact`, the downloaded file is at
`artifact/firmware.bin` — NOT `artifact/firmware-x4pro.bin`. The `mv` fails with
`mv: cannot stat 'artifact/firmware-x4pro.bin': No such file or directory` and the job exits 1.

Live failure: `gh run view 42484829255 --repo Belphemur/Xerif-eReader --log-failed`
(job `publish-pr-firmware`, PR_NUMBER=303, SHORT_SHA=50bbb33).

## The fix (must do)

1. `.github/workflows/pr-firmware-links.yml` line 224: change to
   `mv artifact/firmware.bin "${asset}"`.
2. Commit message: `ci(dev-ota): download the x4pro artifact's internal firmware.bin path, not the artifact name`
3. Push develop directly (the owner directed this workflow change lands directly on develop).

## Verify live (must do — the fix is verifiable on the release)

After the fix lands on develop:

1. Rerun the CI run for PR #303's head `50bbb3354` (run 42483936293) — this re-triggers
   the workflow_run completed event and the publish lane with the fixed code:
   `gh run rerun 42483936293 --repo Belphemur/Xerif-eReader`
2. Wait for the CI run to complete, then confirm the publish lane run succeeded.
3. Confirm the dev release gained the PR asset `xerif-pr-303-50bbb33-x4pro.bin` and
   `manifest.json` lists it (PR entries are `xerif-pr-*` — the PR id is in the name).
4. Confirm the manifest still has the 3 dev entries + the new PR entry, newest-first.

## Constraints

- This is a CI-workflow change, no firmware code.
- Do NOT merge; never amend authorship; default/global git config only.
- Push develop directly after verification.
- If the manifest generation or dev-build lane is affected, the design doc
  docs/design/2026-11-08-dev-ota-pr-firmware.md is the contract — amend in lockstep if needed.

## End condition

When the fix is committed, pushed, verified live on the release, write the final report
and end your reply with the marker as the last line: PR_FIRMWARE_MVFIX_DONE
