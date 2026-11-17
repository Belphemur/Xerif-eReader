# Brief — Fix manifest sort_key bug: date-string comparison vs mixed timezone formats

You work in `/home/balor/workspace/eink/crosspoint-x-reader` on `develop` (HEAD 488bc49ee).
The design doc `docs/design/2026-11-08-dev-ota-pr-firmware.md` is the contract: the manifest
is newest-first by asset date. The implementation does not match the contract — fix it.

## The bug (verified against the live dev release)

`generate_dev_manifest.py` `sort_key` (line ~67-71) sorts by the raw date **string**:

```python
def sort_key(candidate):
    # Newest first by asset date; undated assets sort last; name breaks ties
    name, _, date = candidate
    return (bool(date), date, name)
```

- `dev-build.yml:133` passes `--new-date "$(git show -s --format=%cI "${DEV_SHA}")"` —
  `%cI` gives the commit date in the **author's local timezone offset** (e.g. `-05:00`).
- The other dev/PR entries come from the release asset `created_at` which is UTC `Z` format.
- String comparison: `2026-11-08T12:52:30Z` vs `2026-11-08T07:54:08-05:00` — at the hour
  position `'1'` > `'0'`, so `12:52:30Z` sorts as newer than `07:54:08-05:00` even though
  `12:54:08Z` > `12:52:30Z` in actual time. The live manifest provably lists `xerif-dev-3b0dfb0c…`
  above `xerif-dev-488bc49e…` — wrong order.

## The fix (must do)

1. In `sort_key`, parse the date to a UTC datetime before comparing:
   `datetime.fromisoformat(date.replace('Z', '+00:00'))` (Python 3.7+ `fromisoformat` handles
   offsets; `Z` needs the replacement). Fallback for unparseable/empty dates: sort last
   (keep current `bool(date)` primary key, but with a zero-time for unparseable entries).
2. Sanity-check the ordering in a host test: a mixed-format fixture (`Z` + `-05:00` + `-00:00`)
   must sort by real instant, not string. The test is a pure Python check — add it to
   `tests/` (or wherever the existing host-test suite lives) if it exists; otherwise the
   same assertion as a documented inline comment in the helper is acceptable.
3. Verify the live manifest ordering: `gh api repos/Belphemur/Xerif-eReader/releases/tags/dev`
   and confirm the newest-first order is correct (or note the fix will only apply on the
   next dev-build run, since the manifest is a static artifact).

## Constraints

- TTF path only is NOT applicable here — this is a CI-workflow change, no firmware code.
- Push develop to origin after every commit (the owner directed this change lands
  directly on develop). Do NOT merge; never amend authorship; default/global git
  config only.
- Gates: the helper must run cleanly under `python3` locally. There is no device gate for
  this change — the workflow is YAML + Python, no firmware build.
- Design doc: the fix makes the implementation match the contract. No doc amendment needed
  unless you deviate from the contract.

## End condition

When the fix is committed, pushed, and verified, write the final report and end your reply
with the marker as the last line: MANIFEST_SORT_FIX_DONE
