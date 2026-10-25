# Design: richer cover sleep/shutdown progress line

Date: 2026-10-24
Status: approved for implementation
Follows: `docs/design/2026-10-23-cover-sleep-progress.md` (merged as 68078aca)

## Goal

On the cover-based sleep and shutdown screens (when `sleepScreenCoverShowProgress`
is On), the current single `NN%` line becomes a richer reading-status line and
sits slightly higher:

```
42% • ~2h 15m • 134 pages left
```

- percent — as shipped
- estimated time left (`~2h 15m` / `~45m` / `<1m`) — hours+minutes, Kindle-style
- whole-book pages left

Segments whose data is unknown are dropped (graceful degrade):
`42% • ~2h 15m` → `42% • 134 pages left` → `42%`.

## Grounded in the reading-stats subsystem (no new estimation logic)

Everything needed already exists — this feature only re-uses it:

| Need | Existing mechanism |
|------|--------------------|
| Time-left value | `BookReadingStats::estimatedTimeLeftSeconds` (v8 bytes 69–72, `0` = unavailable), computed at reader exit by `EpubReaderActivity::onExit` via `estimateBookTimeLeftSeconds(stats, globalStats, remainingPages, currentPageWordsOnPage)` (`EpubReaderActivity.cpp:440`) |
| Pages-left value | `remainingPages` computed in the SAME exit block (`EpubReaderActivity.cpp:434-441`): `bookPagesEstimate = chapterPages * bookSize / chapterBytes`, `remainingPages = (1 - progress) * bookPagesEstimate` — currently used only for the time estimate and then discarded |
| Duration formatting | `formatCompactReadingDuration` (`ReadingStatsUtils.cpp:330`) → `<1m` / `45m` / `2h 15m` / `3h` |
| Home-card precedent | `formatHomeProgressLine` (`ReadingStatsUtils.cpp:562`) renders `N% • ~Xh Ym` from the persisted record — the same presentation the user approved for the home card ("we did it for the theme") |

**Hard rule honored: zero reading-stats binary-format changes.** No record bump,
no new decode path. The stats record already persists the time-left; we surface
it, we don't re-store it.

## Data flow — extend the PR #249 cache pattern

The review campaign on #249 established the contract: the sleep/shutdown render
path must not read SD or allocate. So the two new values ride the same
`APP_STATE` cache the percent already uses.

### 1. `CrossPointState` gains two fields (`src/CrossPointState.h`)

```cpp
uint32_t openBookTimeLeftSeconds = 0;  // 0 = unknown (matches record sentinel)
int16_t  openBookPagesLeft      = -1;  // -1 = unknown
```

Serialized in `toJson`/`fromJson` next to `openBookProgressPercent`
(`CrossPointState.cpp:61`). They are saved/restored together with the percent,
so a reboot restores a consistent triple.

### 2. Seeding — `EpubReaderActivity::onExit` stats block

Inside the existing block that computes `remainingPages` and `timeLeft`
(`EpubReaderActivity.cpp:429-447`, same guard: `bookSize > 0 && chapterBytes >
0 && chapterPages > 0 && percent valid`), seed both fields:

```cpp
APP_STATE.openBookPagesLeft = static_cast<int16_t>(
    remainingPages > INT16_MAX ? INT16_MAX : remainingPages);
if (timeLeft) APP_STATE.openBookTimeLeftSeconds = *timeLeft;
```

Notes:
- Only seed `timeLeft` when the estimate exists (`nullopt` → leave previous/0).
  The percent seeding in the same block stays as-is.
- `ReaderActivity::onExit` (the generic seeding site at `ReaderActivity.cpp:126`)
  does NOT touch the new fields — `ScreenshotInfo` has no time/pages data, and
  writing a fabricated unknown there would clobber a good EPUB-seeded value on
  the TXT path. Unknown stays unknown for readers without stats.
- XTC/TXT: no time/pages computation exists → fields stay unknown → those
  segments never render (percent-only line, same as today).

### 3. Reset sites (mirror the percent cache exactly)

- `ReaderActivity::onEnter` book-change reset (`ReaderActivity.cpp:73`):
  also `openBookTimeLeftSeconds = 0; openBookPagesLeft = -1;`
- `main.cpp` path clear (`main.cpp:1202`): same two-line reset.
- Rename sites (`FileBrowserActivity.cpp:504`, `EpubReaderActivity.cpp:287`)
  intentionally keep the values — same book, estimates stay valid (same
  rationale the review accepted for the percent).

### 4. Render-time fallback (pre-upgrade state only)

Cold cache (`timeLeft == 0`) with a non-empty `openEpubPath`: load the book's
`BookReadingStats` record (`<cachePath>/stats_v8.bin`, 135 B fixed record — no
`Epub` allocation, orders of magnitude cheaper than `loadBookProgress`) and use
its `estimatedTimeLeftSeconds`. This mirrors the percent's `loadBookProgress`
fallback role for state saved before the cache existed. If the load fails or
the field is 0, the segment is omitted. Pages-left has no persisted source
before the upgrade → simply unknown.

The fallback may run once per sleep render (same accepted cost shape as the
percent fallback); it never runs when the cache is warm.

## Display contract

### Placement — "slightly higher"

- Sleep screen: `pageHeight - 20` → **`pageHeight - 32`** (one UI_10 line of
  clearance above the bottom edge).
- Shutdown framed: unchanged, `frameY + frameH + 8` (between frame and caption).
- Shutdown no-cover fallback: `pageHeight - 20` → `pageHeight - 32` (consistency
  with the sleep screen).

### Line format (single centered line, `UI_10_FONT_ID`, black)

New pure formatter in `ReadingStatsUtils` (host-testable, same home as
`formatHomeProgressLine`):

```cpp
// Sleep/shutdown info line: "42% • ~2h 15m • 134 pages left".
// Segments with unknown data are dropped; percent always leads when known.
void formatSleepProgressLine(int percent, uint32_t timeLeftSeconds,
                             int pagesLeft, char* buf, size_t len);
```

- Composes: percent (`%d%%`), `formatCompactReadingDuration` for time, and a
  new `STR_SLEEP_PAGES_LEFT_FMT: "%lu pages left"`.
- Separator: the same `" • "` used by `STR_PROGRESS_LINE_FMT`.
- Unknown handling: `percent < 0` → whole line empty (caller skips the draw,
  preserving today's no-percent-no-draw rule); `timeLeftSeconds == 0` → drop
  the time segment; `pagesLeft < 0` → drop the pages segment.
- Buffer: 48 bytes is sufficient for the longest realistic line
  (`100% • ~999h 59m • 32767 pages left` = 34 chars).

`SleepActivity` replaces `drawReadingProgressPercent` with
`drawReadingProgressLine(renderer, percent, timeLeftSeconds, pagesLeft, y = -1)`
and the payload (`percent`, `timeLeft`, `pagesLeft`) flows through
`renderBitmapSleepScreen` and `renderShutdownImageFramed` where `progressPercent`
flows today (same three call sites: sleep cover path, framed shutdown,
no-cover shutdown fallback). Signature change is mechanical.

### Width discipline

The composite line must fit the narrowest supported panel at `UI_10_FONT_ID`.
Worst realistic line (`100% • ~99h 59m • 999 pages left`) ≈ 33 chars. If the
pi implementer finds overflow on any board's panel width, drop the
`pages left` segment on that width rather than shrinking the font — document
the finding in the PR.

## Pages-left semantics

"134 pages left" is an **estimate of device screen pages** — the same kind of
page the reader lays out (screen size, fonts, margins) — extrapolated for the
whole book. It is NOT the book's print/page-index numbering: an EPUB has no
fixed pages, and none of the book's page-map metadata (if any) is consulted.

Computed once at reader exit (`EpubReaderActivity::onExit`, the same block that
seeds `APP_STATE`):

```
bookPagesEstimate = chapterPages × bookSize / chapterBytes
remainingPages    = (1 − bookProgress) × bookPagesEstimate
```

- `chapterPages` = `section->estimatedTotalPages()` — renderer-laid-out pages of
  the **current chapter only**.
- `bookSize` / `chapterBytes` = byte share of the whole book vs the chapter, so
  the estimate assumes the book's bytes-per-page density matches the current
  chapter's. Image-heavy or dialogue-heavy chapters skew the extrapolation, and
  the value is re-estimated at every exit, so it can drift between chapters.
- `bookProgress` is byte-based, matching the percent segment of the line.
- Snapshot semantics: the value is written to `APP_STATE` and the stats record
  at exit (see `e0a0e811` for the exact refresh rules) and is not recomputed
  while the device sleeps.

Difference from upstream `crosspoint-reader`: upstream's sleep/cover screens
draw **no reading progress at all** (no percent, no time, no pages — its only
progress display is the status-bar percent while reading,
`statusBarBookProgressPercentage`). The cover progress line itself is a Xerif
addition (#249: percent + time), and pages-left is Xerif-only (#252).

## i18n

- `STR_SLEEP_PAGES_LEFT_FMT: "%lu pages left"` in `english.yaml` (printf style,
  `%lu` for the unsigned long page count), placed next to the
  `STR_SLEEP_COVER_*` block; `I18nKeys.h` regenerated via
  `scripts/gen_i18n.py` (file is git-ignored; only YAML is committed).
- Other languages fall back to English per the translation workflow.
- No new strings for time (reuse `formatCompactReadingDuration`'s literal
  `h`/`m` composition, exactly like the home card does — those units are
  intentionally untranslated compact forms).

## Settings

None. The existing `sleepScreenCoverShowProgress` toggle governs the whole
line, exactly as it governs the percent today.

## Out of scope

- Any reading-stats record/layout change (explicitly none — no version bump).
- Time/pages for XTC/TXT readers (no estimation source; percent-only line).
- Refreshing the estimates during reading (values are exit-time snapshots,
  same freshness the home card already shows).

## Implementation plan (pi tasks)

1. **[stats-utils]** `formatSleepProgressLine` in `ReadingStatsUtils.{h,cpp}`
   + `STR_SLEEP_PAGES_LEFT_FMT` in `english.yaml` (+ regenerated
   `I18nKeys.h`) + host tests in the ReadingStatsUtilsTest suite covering:
   all segments present, each segment dropped, percent-only, percent<0 →
   empty, buffer-truncation safety.
2. **[state+seed]** `CrossPointState` fields + serialization; seeding in the
   `EpubReaderActivity::onExit` stats block; resets at `ReaderActivity::onEnter`
   and `main.cpp` clear; render fallback via `BookReadingStats` load in
   `SleepActivity`.
3. **[draw]** `drawReadingProgressLine` replacing the percent helper; payload
   threaded through `renderBitmapSleepScreen` / `renderShutdownImageFramed` /
   call sites; placements per the contract (−32 bottom lines, framed
   unchanged).
4. **[verify+PR]** `./bin/clang-format-fix` (no `-g`) + verify;
   host test suite full pass; `pio run -e x4pro` AND `pio run -e default`;
   push, open **draft** PR with title `feat: richer cover sleep progress line`
   (Title Check requires conventional format).

## Verification checklist

- [ ] Host suite full pass (333+ tests, incl. the new formatter tests).
- [ ] `pio run -e x4pro` and `pio run -e default` green (fresh `.pio/build`).
- [ ] clang-format clean (`clang-format-21 --dry-run -Werror` on touched files).
- [ ] No `stats_v8.bin` / global-record byte changes (diff shows no
      `BookReadingStats.cpp` decode constants touched).
- [ ] Owner test checklist: cover sleep with book (all 3 segments), toggle Off,
      no-book case, COVER_TRANSPARENT overlay + plain-cover fallback, inverted
      filter, framed shutdown + no-cover fallback, non-cover modes, XTC book
      (percent-only), reboot persistence of the cached triple.
