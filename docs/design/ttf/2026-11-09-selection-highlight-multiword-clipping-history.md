# TTF Selection: Inverted Highlight, Multi-Word Selection, Clipping, Dictionary History

## Summary

Four related selection features, decided 2026-11-09:

1. **Proper highlight on the TTF path**: the selected word becomes a black box
   with the text inverted to white — same look as the legacy SD-font selector —
   instead of today's outline-only rectangle.
2. **Multi-word selection on the TTF path**: drag/button range selection with
   the legacy clip-selection interaction model (phrase lookup included).
3. **Clipping on the TTF path**: save, draw, and delete clippings, anchored by
   chapter character offsets so highlights survive font-size changes.
4. **Dictionary search history**: the last 50 successful lookups, persisted,
   reachable from the definition activity's search overlay.

Locked decisions (owner, 2026-11-09):

| Decision | Choice |
| --- | --- |
| Multi-word scope | Port `ClipSelectionActivity` to TTF via a pluggable data source (covers phrases AND clipping in one feature) |
| Highlight primitive | `GfxRenderer::invertRegion` in the firmware lib, shared by word highlight + saved-clip rendering |
| Char anchoring | Consumer-side codepoint walk in `buildTtfWordSelectData`, gated to LTR runs (no SDK change) |
| History | SD file ring, successful lookups only, separate `dictionaryHistoryEnabled` toggle |

## Current behaviour (verified)

### Selection surfaces

| Surface | File | TTF path today |
| --- | --- | --- |
| Single-word dictionary selector | `src/activities/reader/DictionaryWordSelectActivity.{h,cpp}` | Yes — prebuilt boxes via `buildTtfWordSelectData` (`TtfWordSelect.cpp:50-198`), page repaint through the reader hook (`EpubReaderActivity.cpp:1333-1385, 5139`) |
| Multi-word clip/lookup selector | `src/activities/reader/ClipSelectionActivity.{h,cpp}` | **No** — `startClipSelection` (`EpubReaderActivity.cpp:1412`) requires legacy `Page`s from `Section`; silently no-ops on the TTF path |

- Legacy highlight = `fillRect` black + `drawText` re-draw of the word
  (`DictionaryWordSelectActivity.cpp:737-739, 759-761`). TTF highlight =
  outline only, `drawRect` (cpp:729-735, 751-757) — engine glyphs cannot be
  re-drawn through the SD-font `drawText`.
- Differential repaint exists on both paths via the snapshot pair
  `readFramebufferRegion`/`writeFramebufferRegion`
  (`DictionaryWordSelectActivity.cpp:689-742, 784-821`).
- Selection is one logical word: `selectionGroup` joins fragments the layout
  split across lines/runs (`TtfWordSelect.cpp:157-166`,
  `DictionaryWordSelectActivity.cpp:96-100`).
- `ClipSelectionActivity` already carries the full multi-word model: drag
  selection (`WordSelectionInput`), `rangeStart`, 3-page window
  (`CLIP_SELECTION_PAGE_WINDOW`), `Mode::{Clip,Lookup,DefinitionLookup}`
  (`ClipSelectionActivity.h:20, 65`), per-page word boxes (`WordBox.pageOffset`,
  h:56), delete-existing-clipping-on-tap, and the action popup.

### Clipping data model

- `Clipping` (`src/ClippingStore.h:24-43`) stores spine/page/word-index ranges
  **and** `startOffset`/`endOffset` chapter char anchors.
- `clippingContainsWord` (`ClippingStore.h:116-126`) prefers the char-offset
  branch when both offsets are set — layout-signature-independent. A TTF clip
  anchored by char offsets survives font-size changes and relayout with no
  layout-signature work.
- `drawClippingHighlights` (`EpubReaderActivity.cpp:6268-6353`) re-draws saved
  clips over legacy pages by matching per-word `wordSourceRange`; gray fill is
  dithered (`fillHighlight`, cpp:6296-6305). `clippingAtPoint`
  (cpp:6234-6266) resolves delete-on-tap.

### Char anchoring availability

`PageTextRun` (freeink-sdk `libs/book/FreeInkBook/include/layout/ChapterLayout.h`)
already carries `charStart` (chapter codepoint of the run's first character) and
`charLen` (chapter codepoints covered). `TtfWordBox` (`TtfWordSelect.h:27-39`)
has **no** char fields — only page-local text offsets. Caveat documented in the
run header: re-encoded run text does not map 1:1 onto the chapter for soft
hyphens, baked ligature pairs, and RTL runs (stored in visual order).

### Framebuffer / render modes

- `GfxRenderer` render modes: `BW | GRAYSCALE_LSB | GRAYSCALE_MSB |
  GRAYSCALE_DUAL` (`lib/GfxRenderer/GfxRenderer.h:36`). Reader page renders run
  `GRAYSCALE_DUAL` (one walk fills both plane buffers,
  `EpubReaderActivity.cpp:4961, 5865, 6075`); LSB/MSB passes exist for strip
  targets.
- `readFramebufferRegion`/`writeFramebufferRegion`
  (`GfxRenderer.h:370-371`, impl cpp:1998/2016) operate on **1bpp panel memory**
  (`mem.w / 8` byte rows) in the currently active plane. There is no region
  invert primitive; `invertScreen` (cpp:1952-1956) is whole-screen only.
- Bitwise-NOT of packed pixels equals level inversion for both 1bpp (0↔1) and
  the 2-bit gray packing (level L → 3−L is the complement of both plane bits).

### Dictionary history

- `DictionaryDefinitionActivity::LookupState.history`
  (`DictionaryDefinitionActivity.h:87-95`) is a 16-entry **in-RAM** back-stack
  for the current lookup chain — nothing persisted, lost on exit.
- `VocabularyStore::append` (`src/vocabulary/VocabularyStore.h`) exports
  successful lookups to `/Vocabulary.txt` — an export, not a re-lookup history.
  It is called from `DictionaryWordSelectActivity::performLookup`
  (cpp:518-521), the single success branch both paths and direct lookups flow
  through.

## Design

### F1 — `GfxRenderer::invertRegion` + TTF inverted highlight

**Contract.** `void invertRegion(int x, int y, int w, int h) const;` — screen
coordinates, clipped like `fillRect`. Semantics: every pixel in the region
takes the composite-inverse of its current level (1bpp: bit flip; gray: level
L → 3−L). Implementation mirrors `readFramebufferRegion`'s
`screenRectToAlignedMemRect` walk, XOR-ing the bound plane buffer(s) for the
active `RenderMode`:

- `BW`: XOR the framebuffer region.
- `GRAYSCALE_DUAL`: XOR **both** plane buffers (complementing the 2-bit field).
- `GRAYSCALE_LSB`/`MSB` (strip passes): not used by selection surfaces — the
  selector never renders inside a `beginStripTarget` band. Assert/ignore.

**Highlight rendering.** In `DictionaryWordSelectActivity`, the TTF branches
(`drawHighlightWithSnapshot` cpp:729-735, `drawHighlightRange` cpp:751-757)
replace `drawRect` with `invertRegion` per segment box — same +2 px padding and
screen clipping as today. The legacy branch keeps `fillRect` + `drawText` (it
already produces the requested look).

**Differential repaint.** Unchanged flow: restore the snapshot region, invert
the new box. The snapshot must round-trip the same plane(s) the invert touches:
in `GRAYSCALE_DUAL` the snapshot read/write must cover both planes (two
`SNAPSHOT_CAPACITY` buffers, or read/write per plane). Word boxes are small
(<4 KB per plane), so the cost is bounded. Without this the move-repaint
restores one plane and inverts both — a one-frame ghost. Verify the active-plane
selection of `readFramebufferRegion` against `setRenderMode` during
implementation (does it follow the active plane, or is a plane parameter
needed?).

Edge case: a word over a saved-clip dither inverts the dither pattern too —
visually acceptable, noted, not special-cased.

### F2 — `ClipSelectionActivity` on the TTF path (multi-word + clip)

**Data-source abstraction**, mirroring the pattern
`DictionaryWordSelectActivity` already uses for its TTF ctor
(`PageRenderFn`, h:43-46):

- New ctor: `ClipSelectionActivity(..., std::vector<TtfWordSelectData>
  perPageData, TtfPageRenderFn renderPage, ...)`. `TtfPageRenderFn` =
  `void (*)(void* ctx, GfxRenderer&, uint16_t pageOffset)` — the existing
  `renderTtfSelectorPage` (`EpubReaderActivity.cpp:5139`) gains the offset
  parameter (legacy path keeps its current ctor untouched).
- `extractWords()` gains a TTF branch mapping per-page `TtfWordBox`es into the
  existing flat `WordBox` array (`pageOffset` field already exists, h:56);
  rows are per-page `rowCount` continuations. Group fragments by
  `selectionGroup` per page via `DictionarySelection::groupTokens`, same as
  `extractWordsTtf` (`DictionaryWordSelectActivity.cpp:231-275`).

**Reader routing.** `startClipSelection` gains a TTF branch before the legacy
body: for the `CLIP_SELECTION_PAGE_WINDOW` window, `ttf_->readPage(spine,
pageNumber + offset)` + `buildTtfWordSelectData` per page (footnotes not
needed), then the new ctor. `currentSpineIndex`/`ttfPage` substitute
`section->currentPage`; chapter page count comes from the TTF runtime's mirror
(`cachedChapterTotalPageCount`), used by `ClippingResult` handling and the
post-selection page jump.

**Actions** (popup already exists):

- `Clip` → `ClippingStore::addClipping(...)` with char offsets from F3
  (`startOffset` = first token's `charStart`, `endOffset` = last token's
  `charStart + charLen`), book/author/chapter metadata identical to the legacy
  callback (`EpubReaderActivity.cpp:1461-1465`).
- `Lookup` → `ClippingResult::Action::Lookup` flows into the existing
  direct-lookup path. `openDictionaryWordSelect` needs a `lookupText`
  parameter on its TTF branch (today only the legacy ctor receives it,
  `EpubReaderActivity.cpp:1526`); the TTF ctor already accepts `lookupText`
  (`DictionaryWordSelectActivity.h:48-62`).
- `Bookmark` → `addBookmark()` is reader-level, works unchanged.

**Delete-on-tap.** Port `clippingAtPoint` to a TTF variant that walks the
tokenized page boxes and their char ranges instead of `PageLine`/TextBlock
words. Entry condition stays `initialX >= 0 && CLIPPINGS.hasClippings()`.

**Long-press routing.** `EpubReaderActivity.cpp:1860-1864` routes
`TouchLongPressRoute::ClipSelection` to `startClipSelection` — with the TTF
branch in place this route stops being a silent no-op on the TTF path. No
setting changes.

### F3 — Chapter char anchors for TTF tokens

Extend `TtfWordBox` with `uint32_t charStart; uint16_t charLen;` (chapter
codepoints of the **trimmed** token; `UINT32_MAX`/0 when unavailable).

Computation in `buildTtfWordSelectData` (`TtfWordSelect.cpp`): walk the run's
`text` decoding codepoints (the `decodeCp` helper already exists, cpp:24-46),
counting from `run.charStart`. Byte→codepoint within the run must be decoded,
never assumed 1 byte/char.

Gating per the run header's re-encoding warning:

- **LTR runs**: anchors computed. Ligatures: verify during implementation that
  ligature substitution bakes glyphs but keeps both codepoints in run `text`
  (expected — the substitution is at the glyph/render layer). If a divergence
  is found, drift is bounded to the affected run; log once, not per token.
- **RTL runs** (stored visual order): no anchor (`charStart = UINT32_MAX`) —
  `clippingContainsWord`'s offset branch skips them and the clip degrades to
  text-only (viewable in the clipping list, not re-highlighted). RTL books are
  rare on this device; re-anchoring is a future SDK-side change if ever needed.

Consumers:

- Clip save (F2) — offsets into `ClippingStore`.
- Saved-clip page highlights: a TTF variant of `drawClippingHighlights`
  tokenizing the page (cheap passes: boundary scan only, no font metrics) and
  matching via `clippingContainsWord(..., sourceStart, sourceEnd)` from the
  box anchors. Early-out on `!CLIPPINGS.hasClippings()` / no chapter clips, as
  the legacy variant does (cpp:6274-6278). Fill via `invertRegion` (matching
  the new highlight look) — or the dithered gray fill if the owner prefers the
  legacy clip aesthetic; default **invert** for coherence with F1, revisit on
  device soak.
- TTF `clippingAtPoint` (F2 delete-on-tap) — same anchors.

### F4 — Dictionary search history

**Store.** New `src/DictHistoryStore.{h,cpp}`:

- Ring of the last 50 queries (`constexpr size_t DICT_HISTORY_MAX = 50`),
  file `/DictHistory.txt` on SD, one query per line (queries are single tokens
  or keyboard-entered phrases — no embedded newlines).
- `record(query)`: move-to-front dedupe (a re-looked-up word jumps to the top),
  cap at 50, rewrite via temp-file + rename so a power loss mid-write cannot
  truncate the history.
- `load(out)`: parse into a caller-owned vector for the UI.
- No NVS: bounded size (<1 KB), same storage family as `VocabularyStore`, and
  the SD write happens on an interaction (a lookup), not a render tick.

**Recording point.** `DictionaryWordSelectActivity::performLookup` success
branch, next to the existing `VocabularyStore::append` call (cpp:518-521) —
records the effective query (`lookupText` if set, else trimmed). Covers: button
navigation, touch lookup, and direct lookups (phrase lookups from F2). Gated by
the new setting; failures never block the lookup (same rule as Vocabulary).

**Setting.** `dictionaryHistoryEnabled` in `CrossPointSettings`, surfaced in
Settings near the vocabulary-history row (follow the existing
`vocabularyHistoryEnabled` wiring: setting field, `SettingsList` row, i18n
strings via the translation workflow).

**UI.** The definition activity's search overlay gains a "Recent" list of the
stored queries (most recent first). Selecting an entry performs that lookup
(reusing the overlay's existing submit path). The overlay is shared by legacy
and TTF paths, so the feature lands on both for free. Empty store → no recent
section (no dead UI).

## Coverage matrix (parity after this work)

| Behaviour | Legacy | TTF |
| --- | --- | --- |
| Single-word dictionary select | ✓ | ✓ (existing) |
| Highlight = black box + white text | ✓ (`fillRect`+`drawText`) | ✓ (`invertRegion`) |
| Differential highlight move | ✓ | ✓ (plane-aware snapshot) |
| Multi-word drag/button range | ✓ | ✓ (ported selector) |
| 3-page selection window | ✓ | ✓ (`ttf_->readPage` window) |
| Phrase lookup (Lookup action) | ✓ | ✓ (TTF `lookupText` branch) |
| Save clipping | ✓ | ✓ (char-anchored) |
| Highlight survives font-size change | ✓ (char-offset branch) | ✓ (char offsets) |
| Saved clips drawn on pages | ✓ | ✓ (LTR-anchored; RTL clips text-only) |
| Delete clip by tapping it | ✓ | ✓ (ported `clippingAtPoint`) |
| Bookmark action | ✓ | ✓ (reader-level) |
| Dictionary search history | ✓ | ✓ (shared definition activity) |

## Risks

1. **DUAL-plane snapshot/invert interplay** (F1): if `readFramebufferRegion`
   follows the active plane, the move-repaint needs per-plane snapshots in
   `GRAYSCALE_DUAL`. Resolve against `setRenderMode` internals first; a missed
   plane shows as a one-frame ghost on highlight moves.
2. **Ligature/RTL char drift** (F3): bounded per run; RTL tokens unanchored by
   design. Verify the ligature-preserves-codepoints assumption during
   implementation with a ligature-heavy sample (e.g. "fi" in a serif font).
3. **SD write on lookup success** (F4): same latency class as the existing
   `VocabularyStore::append`; keep synchronous, measure on device soak.
4. **Selector memory budget**: the TTF clip selector holds 3 pages of
   `TtfWordSelectData` (pool-backed PSRAM on the X4 Pro) — same order as the
   dictionary selector's single page ×3. Watch the PSRAM pool on soak.
5. **Search overlay space**: the recents list must not crowd the keyboard on
   the X4 Pro's portrait; cap visible recents (e.g. 7 rows) and scroll.

## Verification plan

- **Host tests** (ctest): token char-anchor walk (ASCII, 2-byte and 3-byte
  UTF-8, soft-hyphen run, LTR/RTL gating), history ring (dedupe, cap 50,
  temp-rename crash safety), clipping char-range matching through
  `clippingContainsWord`. Extend every host stub that compiles the touched
  sources when the `GfxRenderer` surface grows (per repo rule — all stubs, not
  just one).
- **Build gates**: `pio run -e x4pro` + `-e default`, clang-format, cppcheck on
  touched files.
- **Device soak (X4 Pro)**:
  1. Long-press a word → black box, white text (BW and AA-gray render modes).
  2. Move the selection with buttons/touch → no ghosting (differential path).
  3. Drag multi-word → Lookup opens the phrase; Bookmark; Clip.
  4. Change font size after clipping → highlight still matches (char anchors).
  5. Tap a saved highlight → clipping deleted.
  6. Look up several words → recents appear in the search overlay, newest
     first, re-lookup jumps to top; toggle off → no recording.
  7. Buttons never feel dead during selector renders (queued-press coalescing
     requirement holds).

## Implementation breakdown (pi campaign order)

1. `GfxRenderer::invertRegion` + plane handling (foundation; independent).
2. `TtfWordBox` char anchors in `buildTtfWordSelectData` + tests (independent).
3. `ClipSelectionActivity` TTF source + `startClipSelection` routing +
   actions (depends on 2).
4. Saved-clip highlights + TTF `clippingAtPoint` (depends on 2, 3).
5. `DictHistoryStore` + setting + search-overlay recents (independent).

## Addendum — implementation findings (2026-11-09, from the campaign's
## first two milestones)

Two assumptions in this doc were checked against code reality during
implementation. Neither blocks the locked decisions; both residuals are
bounded and are corrected in the shipped implementation, not silently.

**F1 — “GRAYSCALE_DUAL: XOR both plane buffers” is not a true level
inversion.** `GfxRenderer` owns no plane storage: the gray planes are
caller-owned flag-mask scratches, and the composed planes live in the
panel driver (`Uc8279X4Driver::copyGrayscaleLsb/Msb` compose `p0 =
base | lsb`, `p1 = p0 ^ msb`, streamed inverted — driver cpp ~636-715).
XOR-ing the *flag masks* is not the same as complementing the *composed*
plane pair: e.g. a light-gray (tone 1) pixel on a black base is bucket
(1,0); flipped masks (0,1) recompose to bucket (1,1) = black, not the
level-complement dark-gray. A mask-level XOR is a wash, not an inversion.
Since every selection surface renders in BW (the dictionary/clip selectors
paint their page with the BW base pass), `invertRegion` ships with exactly
the contract the selection surfaces need: BW → framebuffer flip; DUAL strip
target bound → both flag scratches flip (future-safe, no current caller);
no strip target → framebuffer flip (the snapshot pair's memory);
GRAYSCALE_LSB/MSB → refuse + error log (the two-pass plane walks forbid
external draws). The doc's §F1 DUAL language should be read with this
caveat; delivering the composed-plane inversion inside GfxRenderer would
require a driver-level primitive and it was not owed by any surface here.

**F3 — ligature substitution does NOT keep both codepoints in run text.**
`ChapterLayout::decodeShaped` folds ligature chains (ff→FB00, FB00+i/l →
FB03/FB04) and lam-alef pairs (FEF5..FEFC) into a single codepoint before
emitSeg re-encodes the run (ChapterLayout.cpp decodeShaped + “Re-encode:
ligatures bake in” comment), and soft hyphens drop entirely. The shipped
consumer-side walk therefore weights each decoded run codepoint by the
fold arity the active font could have produced (queried through
`FontChain::ligature`/`covers` — the same fold gates the layout uses);
raw source occurrences of the same codepoints and dropped soft hyphens
drift the anchor inside the affected run only (≤1 line, log-once rule —
see TtfWordSelect.cpp `chapterCpWeight`).
