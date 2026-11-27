# TTF image predecode — next-chapter sighting (chapter-boundary warm caches)

Date: 2026-11-26 · Scope: TTF reader image predecode only (`CROSSPOINT_TTF_READER` && `BOARD_HAS_PSRAM`) · Parent/record of record: [2026-11-25-image-pipeline-adr.md](2026-11-25-image-pipeline-adr.md) (§2.6 predecode, §5.2 sighting)

Status: Accepted — to be implemented by supervised pi in a dedicated worktree; flips to implemented-in-PR-#N on merge.

## 1. Problem: the worker idles exactly at chapter boundaries

The predecode worker only sees images a **sighting pass** enqueues.
`EpubReaderActivity::ttfSightingPass()` (`EpubReaderActivity.cpp:5244`) walks the
current spine's committed pages, then crosses into the next spine through the
prefetch cache reader, up to `kSightPagesPerPaint` (12) page records per pass.
Three defects in how that walk terminates and re-arms leave the next chapter's
images unsighted until the user has already turned into it — the exact moment
the pipeline exists to protect:

- **A — Latched-early (the dominant mode).** When the walk reaches the end of
  the current spine and no prefetch reader for the next spine is open, the pass
  sets `done = true` (`EpubReaderActivity.cpp:5270-5271`) and the latch
  (`ttfImagesSightedSpine_`/`ttfImagesSightedGen_`, written at `:5305-5307`)
  marks the pair fully sighted. Every later pass early-returns at `:5249-5253`
  until the spine or generation changes. The next spine is never marked as
  pending, so when its cache **is** committed later (background build or FIBP
  worker), nothing re-walks it: its first image page is a cold 3.5–4 s
  synchronous decode on first view — the stall the ADR of record §2.6 removes.
- **B — Worker-commit blind.** With the FIBP prefetch worker active
  (`fibpBegun_`), `ttfPrefetchTick()` returns immediately
  (`EpubReaderActivity.cpp:5578-5580`) and the worker owns next-chapter
  builds. The session-completion sighting that exists for reader-built
  spines (`:5423-5432`: reopen the committed cache, sight, drop) has **no**
  worker-built equivalent: a worker commit for the next spine becomes visible
  only through `availablePageCount` (`TtfBookRuntime.cpp:188`), which serves
  the next spine solely through the prefetch reader — and nothing opens that
  reader for sighting after a worker commit. Even in the rare pass that was
  not latched (defect A fixed alone would not suffice), the next spine reads
  as zero pages.
- **C — Mid-chapter growth is current-spine only.** The `extended` re-arm
  (`:5250-5258`) tracks growth of the **current** spine's committed prefix
  only; the next-spine watermark does not exist, so next-spine pages laid out
  after the walk crossed (worker commits pages progressively) are skipped by
  the one bounded crossing pass.

Net effect on device: books whose chapters start with images pay the cold
decode on every chapter boundary; the worker, sized and scheduled for exactly
this lookahead, sits idle from the moment the current chapter is fully sighted
until the boundary turn.

## 2. Decision

**Sighting becomes a two-phase cursor with a per-spine watermark, and the
next-spine phase re-arms on every path that grows or commits the next
spine's cache.** Concretely:

1. **Two-phase latch.** Replace the single current-spine "sighted" latch with
   two watermarks keyed by `ttfGeneration`: the existing current-spine one
   (`ttfSightedPageCount_`) and a new next-spine watermark
   (spine index + generation + last fully-sighted page). The early-return at
   `:5249` fires only when **both** phases are complete for the current
   generation. A next-spine phase whose reader was absent stays **pending**,
   not done: each later pass retries exactly one `openPrefetch` probe (the
   runtime dedupes internally via `prefetchFor`, `TtfBookRuntime.cpp:405`) —
   an SD `exists()` + reader open when the cache has appeared, and no page
   re-walk. When the cache exists but the spine is image-free, the first
   successful walk marks the phase done and the probe cost stops.
2. **Resume from the watermark.** The next-spine walk resumes at its
   watermark page (mirroring the current-spine `extended` resume at
   `:5256-5258`), so progressive worker commits are sighted incrementally
   instead of being skipped (defect C).
3. **Commit pickup re-arms.** Every path that makes next-spine pages newly
   readable clears the next-spine watermark so the next pass re-walks from
   the watermark:
   - the session-completion branch (`:5420-5432`) — already sights, now also
     clears/resumes against the watermark;
   - the FIBP **worker** commit: detect the worker leaving a next spine (the
     committed-cache probe shape already used for claims,
     `:4225-4238`) and, from the idle build tick, open the committed cache
     reader for one bounded sighting pass and drop it — the same
     open/sight/drop shape as the session branch, never a held reader.
   The pickup is a one-shot per commit, logged with a bounded
   `[IPR] Next-spine sighting` line (spine + page range) so a soak log can
   separate pickup from hang.
4. **Queue discipline is unchanged.** Next-spine jobs ride the existing
   64-slot ring with its existing dedupe/refresh and reading-order overflow;
   they enqueue only after the current spine's phase is complete, so
   current-spine warm caches keep priority. No dequeue-order change, no
   capacity change.
5. **Budget unchanged.** `kSightPagesPerPaint = 12` still bounds every pass;
   the idle build tick keeps draining the cursor for fast-turn readers.

## 3. Non-goals

- No third-chapter lookahead: the ring and the SD cache set stay sized to
  current + next. A second lookahead multiplies worker time and SD writes for
  a boundary the user may never reach.
- No change to the legacy BITMAP path, the display routing, the bake, or the
  claim registry (§ Invariants of the ADR of record).
- GIF jobs still defer to first view (no memory-source GIF decode).
- No prefetch-reader retention change: the pickup opens/drops per commit and
  the normal per-paint crossing keeps its existing `prefetchFor` lifetime.

## 4. Acceptance

- `pio run -e x4pro` and `pio run -e default` green; cppcheck gate clean
  (`mkdir -p .cppcheck-build` first; inert stub members stay `static`).
- Host: if the watermark-cursor arithmetic extracts to a pure helper (as the
  paint-path geometry helpers did), pin it in `test/ttf_image_engine`;
  otherwise state that explicitly — no synthetic test standing in for device
  behavior this codebase cannot host-test.
- Device soak (the bar): open an image-bearing book whose **next chapter
  starts with an image**, stay in chapter 1 until it is fully read, then turn
  the boundary. `[IPR]` lines must show next-spine image jobs decoded (worker
  work + `Job done`) **before** the boundary turn, and the boundary image page
  must first-view within ~250 ms of a text-only turn at equal panel refresh
  (ADR of record §5 acceptance, now holding at boundaries).
- Regression guard: the current-chapter warm path (§5.2 soak baseline) must
  not lose latency or add paint-path cost; `[IPR]` job order still runs
  current-spine images first.

## 5. Consequences

- Positive: chapter-boundary image pages are warm — the last uncovered stall
  of the predecode design closes; the worker's utilization window extends
  from "current chapter only" to "current + next", matching the ring's
  documented sizing.
- Cost: one bounded `openPrefetch` probe per pass while the next spine has no
  cache (an SD stat; stops once the spine is sighted image-free), and one
  open/sight/drop per next-spine commit. Both bounded, both logged.
- Risk and mitigation: a next-spine sighting pass competes with the paint
  path only through the same 12-page budget as today; a worker commit cannot
  deadlock the pass (the pickup is one-shot from the idle tick, and the pass
  itself already tolerates a reader dropping mid-walk).
