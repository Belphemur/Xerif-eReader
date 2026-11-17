# Finish-Reading Prompt Modal (10% / 5% / 1%)

**Date:** 2026-10-20
**Status:** Accepted — design agreed with the owner; implementation delegated to pi.

## Summary

While reading, when the book progress first crosses 10%, then 5%, then 1%
remaining, show a small modal asking *"Did you finish the book?"*
"Keep reading" dismisses it; the dismissed prompt **never re-fires** for that
book (no later thresholds either — one latch per book).
"Yes, finished" sets progress to 100%, records the book as completed through
the existing completion path, and shows the **existing** achievement
(congratulation) screen. A reader setting (default ON) enables/disables the
prompt.

The feature reuses existing machinery end to end; the only genuinely new
pieces are the threshold detector, the modal activity, the per-book dismissal
latch, and one settings toggle.

## Motivation

Today the reader only asks about completion *reactively*: when the user leaves
the reader at the final page or ≥99.5% progress
(`goHomeOrShowCompletionAchievement()`,
`src/activities/reader/EpubReaderActivity.cpp:551-587`), a
`ConfirmationActivity` with `STR_MARK_FINISHED_PROMPT` offers to mark the book
finished. Readers who stop a few pages early — or who jump away from the last
chapter — never see it, and their stats show an unfinished book forever.

Proactively asking near the end ("almost there… did you finish?") catches those
readers at the moment the book actually ends for them, without changing the
existing 100% behaviour.

## Decisions (ADR records)

### ADR-1: Reuse the existing completion + achievement path — do not build a new congrats screen

The congrats screen already exists: `BookStatsActivity` with
`InitialPage::Achievement`
(`src/activities/reader/EpubReaderActivity.cpp:604-614`), reached when
`stats.isCompleted && stats.completionAchievementPending`. "Yes, finished" MUST
route through exactly the same calls the existing prompt confirmation uses:

- `setBookCompleted(true)` (`EpubReaderActivity.cpp:626`) — sets
  `isCompleted`/`completionAchievementPending`, stamps `finishedDate`, applies
  the `removeReadBooksFromRecents` and `moveFinishedToReadFolder` side effects.
- `saveFinishedProgress()` — persists the terminal progress record
  (`EpubReaderActivity.cpp:583` precedent) so 100% survives on disk.
- Then replace the reader with `BookStatsActivity` (Achievement page) exactly
  as `goHomeOrShowCompletionAchievement()` does at lines 604-614.

**Forbidden:** drawing a new congratulation frame, duplicating the
finished-book side effects, or bypassing `setBookCompleted()`.

### ADR-2: One dismissal latch, no re-fire — ever

User decision (2026-10-20): once the modal is answered "Keep reading" (or
dismissed via Back / tap-outside), **no further threshold prompts are shown for
that book**. The 5% and 1% modals do not appear after a 10% dismissal. The
latch is per book and permanent (until the book's completion state is reset).

Storage follows the existing per-book one-shot precedent
`stats.completionPromptDismissedAtHundred`
(`EpubReaderActivity.cpp:563-576`, reset in `setBookCompleted()` at line 631):
add a `finishPromptDismissed` flag to `BookReadingStats`, persisted via
`stats.save(epub->getCachePath())` — no progress.bin format change, no sidecar
file. "Yes, finished" does not need to set the latch: completion state itself
prevents any further prompt.

### ADR-3: The setting gates only the new threshold modals

New toggle `finishReadingPrompt` (default **1** = enabled), following the
`moveFinishedToReadFolder` pattern (`src/CrossPointSettings.h:428`, registered
in `src/SettingsList.cpp:234` as a `SettingInfo::Toggle` under
`STR_CAT_READER`). New I18n key for the label.

- Setting OFF → the 10/5/1 modals never appear. The **existing** at-100%
  `STR_MARK_FINISHED_PROMPT` confirmation and the End-of-Book screen
  (`EndOfBookOptions.cpp`, `STR_END_OF_BOOK`) are untouched by this setting —
  they predate the feature and are gated by their own logic.
- Setting changes take effect on the next page turn; no live-reload of an open
  modal is needed.

### ADR-4: Modal UI — shared OptionPopup chrome, drawn over the live page

Same chrome family as `UiSliderDialog`
(`src/components/UiSliderDialog.h`): bordered popup panel centered at ¾ screen
width, small bold centered title caption, large centered hero readout
(NOTOSANS_18 slot), body-text question line, and either a two-button pill row
(touch boards) or two centered hint lines (button boards). Drawn over the
current page — no full-page clear, same as the slider dialog. Tap outside /
Back = dismiss (latch set, no completion).

```
┌──────────────────────────────────────┐   ← reader page dimmed behind
│   ┌───────────────────────────────┐  │
│   │  ALMOST THERE                 │  │  ← title caption (small, bold, centered)
│   │        10%                    │  │  ← hero readout (large sans, centered)
│   │   left to read                │  │
│   │  Did you finish the book?     │  │  ← body question (theme.bodyText)
│   │ ┌──────────┐    ┌──────────┐  │  │
│   │ │  Yes,    │    │  Keep    │  │  │  ← touch: option-button pills
│   │ │ finished │    │ reading  │  │  │
│   │ └──────────┘    └──────────┘  │  │
│   └───────────────────────────────┘  │
└──────────────────────────────────────┘
```

Button-board variant replaces the button row with hint lines (UiSliderDialog
hint-line pattern, `UiSliderDialog.h:125-137`):

```
│   │  Front: Keep reading          │  │  ← hint line 1 (smallText, centered)
│   │  Side:  Yes, I finished       │  │  ← hint line 2
```

Readout escalates across firings: `10%` → `5%` → `1%` (only one is ever shown
per book given ADR-2's latch; the escalation exists for a book that is
re-opened after the latch state was reset by a completed-state change).

New I18n keys (translation workflow — generated files, English source in
`lib/I18n/I18nStrings.*`): `STR_ALMOST_THERE`, `STR_DID_YOU_FINISH`,
`STR_YES_FINISHED`, `STR_KEEP_READING`, plus the settings label. The "left to
read" suffix may fold into the readout string to keep translation flexible.

### ADR-5: Threshold detection — on page turn, using the existing progress computation

Evaluate after each committed page turn (where
`computeBookProgressPercent()` is already available,
`EpubReaderActivity.cpp:557`) and after a jump that lands in the zone:

- Fire when `computeBookProgressPercent(...) >= 90` (equivalently ≤10%
  remaining) and the modal was never answered for this book and the setting is
  on and `!stats.isCompleted`.
- The readout shows the **crossed** band's value (10/5/1) chosen as the
  highest threshold not yet crossed; with the single latch (ADR-2) each book
  sees at most one firing.
- Only fire on forward progress: a backward jump re-entering the zone after a
  dismissal does not re-fire (latch); a backward jump *before* any dismissal
  does not fire either — firing resumes when progress moves forward again.
- Do not fire while any overlay/other modal is open, while the End-of-Book
  screen is active (`endOfBookMenuActive()`), or during a page-turn render
  window — queue the prompt for the next settled frame (standing rule: UI must
  never feel dead; modal opening must not race the render lock).

### ADR-6: Interaction with the existing end-of-book flow

- If the modal is open and the user keeps reading past the true final page, the
  normal `atEndOfBook` handling (`EpubReaderActivity.cpp:1435-1455`,
  `goHomeOrShowCompletionAchievement()`) proceeds unchanged; close the modal
  first.
- The modal is an *additional* trigger of the same completion path, not a
  replacement: the existing 100% `ConfirmationActivity` prompt
  (`EpubReaderActivity.cpp:565`) stays as is.

## Alternatives considered

1. **Re-firing escalating prompts (10 → 5 → 1) per book.** Rejected by owner:
   nagging; one dismissal silences the book (ADR-2).
2. **Sidecar file / progress.bin format bump for the latch.** Rejected:
   `BookReadingStats` already persists per-book flags through
   `stats.save(epub->getCachePath())`; a new format version invites cache
   invalidation (see `.agents/rules/generated-files-cache.md` versioning rules).
3. **Gating the existing 100% prompt with the new setting.** Rejected: changes
   long-standing behaviour under a differently-named setting; scope discipline
   (SCOPE.md) — the setting describes only the new proactive modal.
4. **New congratulation screen.** Rejected by owner: the achievement page
   already exists and must keep being used (ADR-1).

## Verification

- Host tests: threshold detector as a pure helper (progress percent →
  {none, fire 10/5/1, latched}) in the existing host test style
  (`test/`, build only the touched suites).
- Manual on device: open a book near the end, page to the 10% boundary → modal
  appears once; "Keep reading" → page on past 5% and 1% → no modal; reopen the
  book → still no modal; toggle setting off → no modal at any boundary.
- "Yes, finished" → progress 100% in the library, book moved to `/Read/` if
  that setting is on, achievement screen shown once
  (`completionAchievementPending` cleared after display).
- `pio run -e x4pro` clean; no untracked `platformio.local.ini`; `gh` resolves
  against `Belphemur/Xerif-eReader`.