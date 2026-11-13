# Design amendment — 2026-11-10: scope cut to highlight-only

**Supersedes** the clipping / vocabulary / dictionary-history scope of
`2026-11-09-selection-highlight-multiword-clipping-history.md`.

**Owner decision (2026-11-10):** delete clipping + vocabulary + lookup recording
from PR #306. Upstream `develop` keeps its own clipping/vocabulary code untouched;
this is a strip of OUR PR only. What remains is:

1. **F1 (kept)** — `GfxRenderer::invertRegion` (plane-aware: BW XOR, GRAYSCALE_DUAL
   both planes L→3−L), plane-pair-aware differential-move snapshot, and the
   long-press word-highlight parity work (TtfWordSelect geometry rounds,
   `InvertRegionsMerged`, `TouchLongPressRoute` wiring, review rounds 1–7).
2. **SDK touch fix (kept)** — freeink-sdk `27b3f29` (suppressed-lift one-shot drop
   in InputManager, already on SDK `origin/main`), carried by the gitlink.
3. **Soak-fix commits (kept)** — `c11b9ca79` viewport assignment + clip-failure
   toasts insofar as they touch the TTF render pass only (the clip-entry toast
   strings go if their only caller was the clipping route).
4. **DELETED** — clipping (ClipSelectionActivity changes, saved-clips,
   multi-word selection, ClippingStore char anchors), vocabulary (VocabularyStore,
   VocabularyListActivity, notebook), dictionary search history (DictHistoryStore,
   KeyboardEntryActivity history UI), and all their tests + design docs.
