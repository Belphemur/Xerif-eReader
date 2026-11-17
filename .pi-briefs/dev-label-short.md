# Job: dev-label-short — dev rows show the commit id, not the full version string

## Symptom (owner report, photo PXL_20261114_185845272.jpg, device running f4de183ba)

The Dev-channel picker's dev rows show the FULL manifest version string
(`2.16.1-dev-develop-f4de183b`, 27 chars) in body font, and the compact local
date is then DROPPED by the fit check in
`src/activities/settings/DevOtaActivity.cpp:648-657` (the code explicitly
documents: 28-char label + 132 px date cannot both fit a 440 px portrait row —
"the version wins").

Owner decision (2026-11-14): **for dev entries only, show the commit id
instead of the full version string**, so the date fits. This is DISPLAY-ONLY —
the manifest, the asset names, and the image stamps are unchanged.

## Deliverable — on THIS worktree branch `wt/dev-label-short` (base: origin/develop `f4de183ba`)

1. **Display-shape change in `dev_ota`**: for a dev (non-PR) entry whose
   version parses as a stamp (reuse `parseDevStamp` — DRY, no second grammar),
   `entryLabel` should emit the SHORT display form: base + commit id. Keep it
   compact and unambiguous: **`2.16.1-f4de183b`** — base + `'-'` + sha, NO
   `-dev` segment anywhere (owner correction 2026-11-14: "Actually the display
   should be 2.16.1-f4de183b in your example. No -dev"). The full stamp
   `2.16.1-dev-develop-f4de183b` shows as `2.16.1-f4de183b`; the
   `-dev-<branch>` middle part is dropped entirely and the display label never
   emits any `-dev` token. If `base` alone is
   still too wide with the date, the sha alone wins. Do NOT touch what the
   slot chooser (`slotRowValue`) shows on the Firmware Slots screen — its
   shapes are governed by §20 and its own fit logic.
   - PR entries keep their existing `pr <num> <sha>` badge — unchanged.
   - Unrecognized/unstamped entries keep today's verbatim fallback.
2. **Picker date fit**: with the shortened label, the portrait row now fits
   the right-aligned date (`DevOtaActivity.cpp:648` drops the date only when
   it does not fit). Verify the arithmetic with the real UI_12 metrics in a
   short note in the report (label ~20 chars + date ~10 chars must fit 440 px
   usable); no code change should be needed in DevOtaActivity, but if the
   `+4` gap check still trips, reduce the label further (sha-only) rather
   than touching the render site.
3. **Tests** in `test/ota_slots/DevOtaPolicyTest.cpp`:
   a. entryLabel on a develop entry with full stamp version now yields the
      short display form `<base>-<sha>` (e.g. `2.16.1-f4de183b`) with NO
      `-dev` token and no branch segment;
   b. the short form still fits alongside a 10-char date at realistic widths
      (pin with the injected TextMeasure stubs the suite already uses);
   c. PR entries still produce `pr <num> <sha>`;
   d. unstamped/garbage versions still print verbatim;
   e. slotRowValue behavior is UNCHANGED (pin one existing test or add an
      explicit regression pin).
4. **ADR**: add a short amendment to
   `docs/design/2026-11-07-dev-ota-slots.md` (next §number) recording: dev
   rows display base+sha (display-only), reason = the date must fit on
   portrait; full stamp remains the About/stamp identity (§20 unchanged).

## Gates (ACCEPTANCE — a TaskUpdate without a commit behind it is a FAILED round)

- One GPG-signed commit per logical change (`git commit -S`; default/global
  git config only; never set user.name/email/amend authorship; no
  Co-authored-by).
- `mkdir -p .cppcheck-build && pio check -e cppcheck --fail-on-defect low
  --fail-on-defect medium --fail-on-defect high` PASSED on the EXACT pushed tip.
- Full ctest green (report the count).
- `pio run -e x4pro` SUCCESS.
- Re-fetch origin/develop right before pushing; push to
  `origin wt/dev-label-short` ONLY. Commit BEFORE any TaskUpdate.

## AUTO-MERGE authorized by the owner (2026-11-14, standing instruction)

Once CI is green on the pushed branch tip: open the PR to develop with a
changelog-quality title, then squash-merge (`gh pr merge --squash`) without
waiting for the owner. Re-point nothing (no SDK change). The dev lane
publishes the fixed builds automatically.

## Final report + marker

Write `/tmp/pi_dev_label_short_final_report.md`: symptom, decision, display
shape chosen, commits (sha + subject), gate evidence, PR number + merged sha,
fit arithmetic note, and the device-verification step. End with the marker
DEV_LABEL_SHORT_DONE.
