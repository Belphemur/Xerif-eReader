# Reading statistics

Xerif tracks how you read: a per-book stats screen, device-wide totals, and
a set of history screens you open from the home screen's **Reading Stats**
entry.

The card UI comes from
[CrossInk](https://github.com/uxjulia/CrossInk) by
[@uxjulia](https://github.com/uxjulia), and the Reading Rhythm, Reading
Achievement, and Finished Books screens come from
[YACP](https://github.com/Sichroteph/YACP) by
[@Sichroteph](https://github.com/Sichroteph) — both ported with permission
and full credit. Xerif inherits this subsystem from
[CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader).

| | |
|---|---|
| ![Reading Stats screen](/media/yacp/reading-stats.png) | ![Reading Rhythm screen](/media/yacp/reading-rhythm.png) |
| ![Reading Achievement screen](/media/yacp/reading-achievement.png) | ![Finished Books screen](/media/yacp/finished-books.png) |

## What is tracked

- **Per-book and global stats** — sessions, reading time, pages turned,
  progress, reading speed, average session, reading streak, books read.
- **Reading speed (WPM)** — a trimmed-mean tracker: a 15-sample window with
  the two fastest and two slowest samples dropped, so one sprint or one
  distraction doesn't skew your average. Values outside 80–900 WPM are
  clamped.
- **Average session** — the same idea over a 10-sample window of session
  lengths.
- **Reading Rhythm** — a daily intensity grid over the last 91 days, weekly
  reading-time bars, and a 12-month reading-days chart, all from real
  minutes-per-day history.
- **Finished Books** — a month-by-month history of completed books with
  dates and reading time.
- **Reading Achievement** — finishing a new book opens a celebration screen
  with the book's reading time, sessions, favourite reading period, and
  your device totals.

## Finishing a book

Crossing the real end of a book completes it automatically. Leaving while
on the final page (or at about 100%) asks "Mark as Finished?" once — a
decline is remembered until you read further, so a jump to the end or some
trailing material never silently finishes a book.

## Fixing dates

On a book's stats screen you can edit its start and finish dates (and its
completed state) directly.
