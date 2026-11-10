# Soak findings — PR #306 build (head 14be43ba4), X4 Pro, 2026-11-10

Two numbered findings from the owner's first device soak, both reproducible,
both with file:line root causes identified from the two pasted serial logs.

## Finding 1 — Clip selection is a silent no-op on the TTF reader (all entry points)

**Repro** (log 1, 22:19:29): reader menu → Create Clipping; also confirmed via
long-press with `Long-press on Text = Clipping`. Nothing happens on screen.

**Log evidence**:

```
[22:19:29] [ERR] [CLIP] Cannot anchor clipping before the reader viewport is initialized
[22:19:30] [INF] [PRG] saveTtf(): spine=28 page=30/37 ...
[22:19:30] [DBG] [ERS] TTF prerendered page 31/36 (1 slice, 74 runs)
[22:19:32] [DBG] [GRS] ttf fast turn: page 31/36 displayed from prerender
[22:19:32] [INF] [PRG] saveTtf(): spine=28 page=31/37 ... changed=1
```

**Root cause (file:line)**: `startTtfClipSelection` keeps the legacy
`buildViewportWidth/Height` guard
(`src/activities/reader/EpubReaderActivity.cpp:1517-1521`):

```cpp
if (buildViewportWidth == 0 || buildViewportHeight == 0) {
  LOG_ERR("CLIP", "Cannot anchor clipping before the reader viewport is initialized");
  requestUpdate();
  return;  // <-- EVERY TTF clip selection dies here, silently to the user
}
```

But `buildViewportWidth/Height` are assigned **only inside the legacy
`renderBook()`** (`EpubReaderActivity.cpp:3295-3296`, computed from
`section` margins). `renderBookTtf()` (cpp:4063-4761) never touches them —
it lays out through the SDK `makeLayoutParams()`. On a TTF-rendered book
`renderBook()` never runs, so both members stay 0 → the TTF clip branch
always early-returns. The "readPage failed / OOM / no selectable words"
branches I flagged earlier were all downstream noise; the log shows the
actual terminator with the book rendering fine (`ttf fast turn: page 31/36
displayed from prerender` — the viewport is demonstrably fine, only the
member is unset).

**Note**: the same members are also *read* by other new TTF paths
(`ttfClippingAtPoint` layout signature at cpp:5394-5496 range,
`readerRenderSpecSignature(SETTINGS.readerRenderSpec(buildViewport…))` at
cpp:1434 / 1477 / 5496 / 5539 — the `startClipSelection` result-callback
signature and the saved-clip matching signature). Those reads yield
spec fields computed from a (0,0)-derived spec when the TTF branch set them
— verify during the fix whether the clip layout signature they produce is
consistent (it is passed to `ClippingStore::addClipping`'s
`layoutSignature`). If the TTF path's char anchors are authoritative
(they are — `clippingContainsWord` prefers offsets), the signature value
only needs to be *stable*, but it must not accidentally be 0-biased such
that legacy+TTF signatures collide or that voids the earlier claim.

**Fix direction** (design-addendum level): on the TTF path the viewport the
layout actually used comes from `makeLayoutParams` (it exists today as
`params`); either (a) set `buildViewportWidth/Height` in the TTF render pass
using the same `getOrientedViewableTRBL`+`screenMargin` arithmetic the
legacy path uses (it is reader-level state, not legacy-only), or (b) gate
the TTF branch on the params' own viewport instead of the legacy members —
pick (a) for the DRY win: one meaning ("the viewport the current render was
built for"), one writer per render path, no parallel member.

**Secondary UX (same finding, one commit)**: the four silent failure gates
in `startTtfClipSelection` (readPage fail with empty window, OOM, no
selectable words, this viewport gate) should surface a toast
(`STR_CLIPPING_FAILED` exists) instead of only serial logs. Buttons/UI must
never feel dead — a silent no-op is exactly that.

## Finding 2 — Dictionary: definition opens on page 1 then flips to page 2 with no input

**Repro** (log 2, 22:20:15-17): long-press a word in the reader → selector
opens → definition shows page 1 → **without any further user input** renders
page 2 (`currentPage=1`, cpp log `drawBody plain-text page=1` at 22:20:17).

**Log evidence**:

```
[22:20:15] [ACT] Entering activity: DictionaryWordSelect
[22:20:15] [DICT] Long-press lookup: raw='upon' ...     (lookup runs inside the long-press tick)
[22:20:15] [DICT] Selector render start: ... popup=1
[22:20:16] [ACT] Entering activity: DictionaryDefinition
[22:20:16] [DICT] render: ... currentPage=0 ...
[22:20:16] [ERR] [GFX] No glyph for codepoint 601 ... (IPA glyphs missing from the SD font — cosmetic, note)
[22:20:17] [DICT] render: pages=0 ... currentPage=1 ...   <-- NO touch events in between (no [UIRT]/routed lines 22:20:16→17)
```

**Root cause (file:line)**: an input-ownership leak across the activity
transition — the lift of the long-press contact is consumed by the
*definition* as a tap = page turn.

Mechanism, exact:

1. The reader consumes the long-press
   (`MappedInputManager::wasScreenLongPress` →
   `InputManager::suppressTouchContact()`,
   `src/MappedInputManager.cpp:507-516`) while the finger is still down.
2. The selector opens, lookup runs (~1.5 s SD work), the definition is
   pushed — **all mid-contact**.
3. The user lifts. The poll task raises `kEvTouchReleased` into
   `touchOneShots_` (`freeink-sdk/libs/hardware/InputManager/src/InputManager.cpp:2107-2108`)
   and sets the suppression latch.
4. `serviceTouch()` clears the suppression latch on "the first fully-idle
   frame" — i.e. the frame **after** the release frame
   (`InputManager.cpp:1700-1706`: `if (!touchPressed && !touchReleasedEvent)
   touchSuppressed = false;`). Released-one-shots are durable in async mode
   (`consumeTouchFrame()` exchange, cpp:386-394) — they are *not* tied to the
   latch's lifetime.
5. The definition's loop
   (`DictionaryDefinitionActivity.cpp:780-799`,
   `ReaderUtils::detectTouchPageTurn` → `wasScreenTapped`) consumes that
   durable released one-shot **after** the latch cleared → the lift
   classifies as a TAP (stationary, within release slop) in the tap-enabled
   next-page zone → `currentPage++` → page 2. Matches the user's description
   exactly ("shows page 1 then directly go to page 2"), and matches the
   no-input log gap.

The suppression design covers "the rest of the contact" only while the latch
is alive; a *consumer transition inside the contact* — the buyer of the
release one-shot being a different activity than the suppressor — breaks the
invariant. This is a SOLID hole (input one-shot outlives its suppression),
not an activity bug: any activity pushed mid-contact is exposed.

**Fix direction**: close the gap at the InputManager
(consume-on-check path, `InputManager.cpp`): a released one-shot raised on a
suppressed contact must either (a) keep holding the latch until the one-shot
is consumed (`touchSuppressed` should not clear while
`poppedTouch_`'s durable released bit is still outstanding), or (b) clear the
durable released one-shot when the suppression latch clears (the contact is
over; the lift was already owned by the suppressor). (b) is one line and
semantically exact; (a) is broader.短期内 simplest correct: in
`serviceTouch()`'s idle-frame clear branch, also clear the pending
`kEvTouchReleased`/`kEvTouchPressed` bits if the contact was suppressed
(the suppressor already consumed its edge — the lift has no owner).

**Adjacent-path check required (owner's standing rule)**: the same leak
reaches every "activity opened by a mid-contact gesture" — dictionary
selector's own tap-to-lookup, clip selector opened by long-press (its
first-tap delete-on-tap guard!), ClipSelection drag start, and the
definition's search overlay. The fix is at the input layer precisely so all
of them are covered; list them in the commit body.

## Non-finding (noted, do not fix now)

- `[ERR] [GFX] No glyph for codepoint 601/712/593/720/859` — IPA glyphs
  (`ə ˈ ɝ ː ↑`) missing from the selected SD font in the plain-text
  definition path. Cosmetic; renders as gaps. Worth a fixture-font check in
  a later pass, not this PR.
- `[ERR] [VOCAB] Could not save successful dictionary lookup` at 22:20:16 —
  needs correlation: it fires right before the definition enters; check
  whether it is OOM (heap is tight there: Min later shows 16 KB) or the
  notebook toggle being off. Do not let this block the two findings.

## Severity

Both findings are soak-blocking: Finding 1 makes the clip feature entirely
unreachable on the TTF path (regression vs. the legacy reader), Finding 2
corrupts the definition reading position on every long-press lookup.
Neither is a merge blocker for develop-side work, but **PR #306 must not be
squash-merged until Finding 1 and Finding 2 have fix commits** on the branch.

## Suggested split

1. Fix 1 (viewport member) — small, one guard, device-verifiable instantly.
2. Fix 2 (InputManager release/suppression) — SDK-side, needs the adjacent-
   path list above; the semantic one-liner (b) with a host test in the SDK's
   InputManager suite.
3. UX: toast on failed clip entries (same commit as Fix 1).
