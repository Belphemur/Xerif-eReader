# Addendum 2026-11-09 — one Vocabulary Notebook (lookup history + saved definitions)

Owner decisions taken after device-side review of PR #306's settings surface.

## A1. Make it ONE feature that does BOTH jobs

Shipped §F4 built two parallel record paths on the same event:
`vocabularyHistoryEnabled` → `/Vocabulary.txt` (rich JSON: word, headword,
definition, timestamp, book/chapter/page) and `dictionaryHistoryEnabled` →
`/DictHistory.txt` (query ring). Two toggles, two files, two write sites,
near-identical labels ("Save Vocabulary Lookups" / "Save Dictionary
Lookups"), both default off, both in the Reader category. The owner's ruling:
**one feature, one switch, one store — the Vocabulary Notebook — and it must
serve both jobs**, not just one of them.

Both jobs, from one record set:

1. **Keep what you looked up** — the saved record (headword + definition +
   where you were reading), reviewable later on the device.
2. **Get back to a word fast** — recency list, newest first, tap to re-look-up.

The notebook record already carries everything for job 1. Job 2 needs only
`word`/`headword`/`timestamp` off the tail, so it is a *view* over the same
records — never a second store.

### Scope

- **One setting**: `vocabularyHistoryEnabled`, relabeled **Vocabulary
  Notebook**. `dictionaryHistoryEnabled` is deleted (field, Settings row,
  i18n key).
- **One store**: `/Vocabulary.txt` via `VocabularyStore` (already crash-safe:
  staged record writer, incomplete-tail recovery, `HalStorage::replaceFile`
  publication). `/DictHistory.txt` and `DictHistoryStore` are **deleted**. A
  stale file on existing cards is inert (nothing reads it) and is NOT
  back-filled record-by-record — the ring never held headwords or
  definitions, so a partial import would plant half-empty rows.
- **One record site**: the success branch in
  `DictionaryWordSelectActivity::performLookup` already calls
  `VocabularyStore::append`. Delete the `DictHistoryStore::record` calls at
  `DictionaryDefinitionActivity.cpp:589-591` and
  `DictionaryWordSelectActivity.cpp:527`.
- **Migration**: on settings load, if the retired `dictionaryHistoryEnabled`
  was set and the notebook toggle is clear, set the notebook toggle. Additive,
  idempotent; the retired key is ignored once absent.

### Blocker to solve first: the notebook is WRITE-ONLY

`VocabularyStore` exposes exactly one public method, `append`
(`VocabularyStore.h:18-25`). Nothing can read `/Vocabulary.txt` back — no
reader existed because nothing displayed the notebook. Add a bounded tail
reader:

- `struct NotebookEntry { std::string word; std::string headword; uint32_t timestamp; }`
- `VocabularyStore::loadRecent(std::vector<NotebookEntry>& out, size_t limit)`
  — read a bounded TAIL window (the file grows unbounded; never load it
  whole), split on record boundaries, parse only the three cheap fields,
  stop at `limit`. Append order means newest are last.
- `VocabularyStore::loadRecord(index, NotebookRecord& out)` for the full
  definition of the selected row (same bounded-window discipline).
- Partial/malformed trailing record → skip, keep the rest (same tolerance
  `recoverIncompleteRecord` shows on the write side).
- Failure (missing file, OOM, read error) → empty, log once; the UI shows
  "no lookups yet" rather than failing.
- Reuse the existing flat-JSON quoted-field scanner shape from
  `src/clippings/ClippingSync.cpp` instead of introducing a parser.

### Deletion (the one genuinely new storage hazard)

Deleting from an append-only crash-safe store must not become a
read-modify-write race or an in-place splice. Implement
`VocabularyStore::removeRecord(index)` / `clear()` as
**rewrite-without-the-record** through the same `replaceFile` publication path
the store already uses (stage temp file → replace). Interruption leaves either
the old file or the new one — never a half-spliced notebook. This is where
SOLID outranks KISS: the simpler in-place splice would corrupt the only copy.

## Decision log (coding-philosophy: DRY > SOLID > KISS)

| Decision | Winner | What the loser would have cost |
| --- | --- | --- |
| Keep `/Vocabulary.txt` as THE store; delete `DictHistoryStore` | **DRY** | Two records of one act, two writers, two migration stories, and a recents list that can disagree with the notebook it shadows. The SOLID-flavored "keep both, separated concerns" reads clean in isolation and duplicates the knowledge of what a lookup is. |
| Recents are a *tail-read view* over notebook records, not a second file | **DRY** | A parallel ring re-derives recency the append-only notebook already encodes in record order; dedupe/ordering would have to be re-implemented and kept consistent forever. |
| Delete = stage-to-temp + `replaceFile`, not in-place splice | **SOLID over KISS** | The simpler splice corrupts the only copy of user data on power loss mid-write; notebook contents are not recoverable from anywhere else. |
| Bounded tail window instead of reading the whole file | **SOLID over KISS** | The simplest read (`read all`) is unbounded in memory on a 380 KB-RAM baseline device; a long-lived notebook would fail the lookup that opened the list. |
| One toggle, one label | **KISS within DRY** | Two switches for one decision force the user to reason about an internal artifact split they cannot see. |
| Keep keyboard chips as a fast path, backed by the same reader | **KISS within DRY** | A second UI with its own data source; chips cost ~30 lines and serve the common case (touch, word just looked up) better than a scroll. |
| Shared `STR_IGNORE` NOT renamed (A2); new key for the clipping label | **DRY** | Renaming the shared key to serve one row would silently relabel the Home-button and power-button options that legitimately mean "Ignore". One key, one job. |
| A2 fix APPENDS `TOUCH_LP_CLIP = 3` instead of repurposing value 1 | **SOLID over KISS** | Repurposing the existing slot is the smaller diff but silently converts every user who chose "Ignore" into "open clip selection on long press"; the persisted index is append-only precisely so a stored value keeps its meaning. |

## A2. Add a real "Clipping" entry; `Ignore` keeps meaning nothing

`TOUCH_LP_IGNORE = 1` currently resolves to `TouchLongPressRoute::ClipSelection`
(`src/activities/reader/TouchLongPressRoute.h:25-26`, from upstream's
`d7fcc7ce` where value 1 *was* the clip behavior). Two problems:

- The screen long-press action has **no value that does nothing**, despite a
  menu entry that says "Ignore".
- On today's dev build "Ignore" *appears* inert only by accident: the clip gate
  fires, `startClipSelection` hits `if (!section) return` on TTF books and
  returns silently. PR #306 adds the TTF branch, so that same value would
  start opening the clip selector — a silent behavior change for everyone who
  picked "Ignore".

Fix — **append a new value, don't repurpose the old one** (the persisted uint8_t
is menu-position indexed and APPEND ONLY, `CrossPointSettings.h:254-261`):

```cpp
enum TOUCH_LONG_PRESS_ACTION {
  TOUCH_LP_DICTIONARY = 0,
  TOUCH_LP_IGNORE = 1,      // no consumer: long press is inert
  TOUCH_LP_FOOTNOTE = 2,
  TOUCH_LP_CLIP = 3,        // NEW — appended, never renumber
  TOUCH_LONG_PRESS_ACTION_COUNT
};
```

- Menu row gains a fourth option, **Clipping**, in that appended position.
- `TouchLongPressRoute` gains a `None` member; `resolveTouchLongPressRoute`
  maps `3 → ClipSelection`, `1 → None`, `0 → Dictionary`, `2 → Footnote`.
- Both consumers already gate on the resolved route, so `None` matches neither
  gate, `wasScreenLongPress()` is never read, and "Ignore" is a true no-op.
- Extend the existing host test `test/touch_long_press_route/TouchLongPressRouteTest.cpp`
  for the new mapping, and the `static_assert` in
  `EpubReaderActivity.cpp:2065-2067` for the appended index.
- New string key `STR_CLIPPING`-equivalent for the option label. Do NOT rename
  `STR_IGNORE` — shared by `SettingsList.h:186` and `HomeButtonSettings.h:10`,
  where "Ignore" is exactly right (one key, one job).

No migration needed: existing installs keep whatever index they had; only
users who explicitly pick "Clipping" get clip selection on long press.

## A3. Notebook UI — list (the missing half)

New `VocabularyListActivity` fed by `VocabularyStore::loadRecent`:

- Scrollable, newest first; row = `word` + `headword` (headword omitted when
  identical).
- Activate → re-run that lookup via the existing `DictionarySearchResult`
  path (the definition activity already consumes it).
- Long-press / confirm dialog → delete one entry (via `removeRecord`); menu
  action → clear all (via `clear()`).
- Buttons AND touch — this is the gap the chips left:
  `drawRecentChips` returns early without touch
  (`KeyboardEntryActivity.cpp:441`).
- Entry point: reader menu beside `VIEW_CLIPPINGS`
  (`EpubReaderMenuActivity.cpp:84-85`), gated on the toggle and a non-empty
  notebook.
- Chips stay, now reading `loadRecent` instead of `DictHistoryStore::load`.

## Coverage after this addendum

| Behaviour | Before | After |
| --- | --- | --- |
| Lookup settings toggles | 2 (confusable) | 1 (Vocabulary Notebook) |
| Stores on SD | `/Vocabulary.txt` + `/DictHistory.txt` | `/Vocabulary.txt` |
| Keep word + definition + book context | via vocabulary toggle only | always (same feature) |
| Recents source | `DictHistoryStore` ring | notebook tail reader |
| Recents UI | keyboard chips, touch-only, 8 of 50 | chips + scrollable list, buttons and touch, all entries |
| Delete / clear history | impossible | delete one / clear all |
| `Long-press on Text` options | Dictionary / Ignore / Footnotes (Ignore silently routes to clip) | Dictionary / Ignore (true no-op) / Footnotes / **Clipping** |

## Verification

- Host tests: tail reader (multi-record, partial trailing record, `>limit`
  window, missing/empty file, records longer than the scan window);
  `removeRecord`/`clear` preserve every other record byte-for-byte and leave
  a recoverable state on simulated write failure; settings migration from the
  retired key; chips/list share one reader.
- Deleted-symbol sweep: `DictHistoryStore`, `dictionaryHistoryEnabled`,
  `STR_SAVE_DICTIONARY_HISTORY`, `/DictHistory.txt` → zero remaining
  references (src, tests, docs).
- Device: toggle on → look up 3 words → open the notebook from the reader
  menu → re-lookup from a row → delete one → clear all → `/Vocabulary.txt`
  matches. `Long-press on Text` = **Clipping** → long press opens the clip
  selector on a TTF book; = **Ignore** → long press does nothing at all
  (no selector, no page turn, no dictionary); = Dictionary / Footnotes keep
  their current behavior. Confirm a settings file written by the previous
  build keeps its stored index (no renumber).
