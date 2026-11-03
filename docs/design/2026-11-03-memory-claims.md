# Design: Memory-claim gate — suspend background consumers for memory-heavy operations

Date: 2026-11-03
Author: Antoine Aflalo (Belphemur) via analysis agent
Status: design only — this PR deliberately contains NO firmware code changes; a
follow-up implementation PR owns the code once this table is signed off.
Origin: community report on the X4 ("Every time I try to connect it says 'Font
Installation Failed, Memory Error'") and the owner directive (verbatim): "We
should disable bluetooth, font loading too when triggering the font managing or
any other background that take memory."
Related: `docs/design/2026-11-02-dram-optimization.md` (§1 baselines, §8 worker
reserve — cited as [DRAM]), `docs/design/2026-10-26-x3-wifi-connect-abort.md`
Addendum 3 (the heap pre-flight + verdict vocabulary), PR #275/#267 (NimBLE
pool trims), PR #279 (persistent worker stack).

## 0. The failure

Both font-management screens coordinate the same allocation burst: a manifest
buffer for the whole catalog, a string arena, the file table, and a JSON DOM,
all while the Wi-Fi working set and the TLS session are live.

| screen | env class | failure sites | what it takes |
|---|---|---|---|
| `TtfFontDownloadActivity` (native TTF boards: x4pro, x4c, `CROSSPOINT_TTF_READER=1`) | S3 + PSRAM | `src/activities/settings/TtfFontDownloadActivity.cpp:264-268` (manifest buffer OOM), `:272-275` (TLS floor), `:387-401` (string arena / file table OOM), `:703-707` (per-transfer TLS floor) | `MAX_TTF_MANIFEST_BYTES = 98304` (PSRAM via `poolMalloc`, `:255/:263`), string arena (`:387`), file table (`:396`), families vector (`:403`), `JsonDocument` DOM (`:296-311`) — the arena/file table/DOM use `operator new` (`lib/Memory/Memory.h:45-54`), so on the S3 core every request ≥ 4,096 B also prefers PSRAM (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`); on C3 they are all DRAM |
| `FontDownloadActivity` (classic C3 class: `default`/`slim`/`gh_release`, `.cpfont` path) | C3, no PSRAM | `src/activities/settings/FontDownloadActivity.cpp:189-194` (manifest buffer OOM), `:201-204` (TLS floor), `:566-577` (per-transfer) | `MAX_MANIFEST_BYTES = 65536` (`:171`), string arena + file table + registry rescan (`:176-180`) |

The buffers differ per board only in *where* they land: `poolMakeBytes`
(`lib/Memory/Memory.h:129`) draws from PSRAM on `BOARD_HAS_PSRAM` builds and
from DRAM otherwise. The TLS transfer itself is already guarded by
`HttpDownloader::heapAvailableForTransfer()`
(`src/network/HttpDownloader.cpp:393-401`) against the `tlsheap` floors
(`src/util/TlsHeapGuard.h`: free ≥ 40,000 B, largest block ≥ 20,000 B).

Two independent causes produce the same `STR_MEMORY_ERROR` screen:

1. **A background consumer was holding DRAM when the burst started** — the
   Bluetooth radio and the rebuildable font caches. This is what the owner
   directive targets, and what this design fixes.
2. **The burst is structurally larger than the board's free heap can ever
   host** — on the C3 class specifically, `FontDownloadActivity` asks DRAM for
   a 64 KB manifest buffer while Wi-Fi (~45 KB) and TLS (~40 KB + record
   buffer) are up. No amount of suspension can create that much heap at once
   (§7, honest statement).

The design below builds the mechanism for cause 1, detects cause 2 honestly
instead of thrashing, and records cause 2's structural remedies as follow-ups.

## 1. Requirements

- The device must never feel dead: every suspension step is bounded, feedback
  rides the screens that already exist, and `Back` stays live during any wait
  (the existing transfer input pump already guarantees this:
  `src/MappedInputManager.cpp:85`).
- No new dependencies, no dynamic allocation in the gate itself.
- Do **not** touch the BLE gate constants (`kStackBytes`/`kStartFreeBytes`,
  `lib/BlePageTurner/src/RadioPolicy.h:44-45`) or any regression pin from
  #275/#279 ([DRAM] §6 item 6).
- The gate is host-unit-testable like `webgate`
  (`test/web_server_heap_gate/WebServerHeapGateTest.cpp`) — pure core, device
  measurements injected by the caller.

## 2. Consumer inventory

Categories: **A — already released** (no work), **B — held but suspendable**
(the claim's levers), **C — held and NOT suspendable** (never touched).

Measured numbers are device-measured where the source says so; else they are
derived (D) from the constants they name. Per-board columns: X3/X4-classic =
C3 (`default` env), X4 Pro = S3 + PSRAM (`x4pro` env).

| # | consumer | X3-classic | X4 Pro | suspend / release API | category | restore semantics |
|---|---|---|---|---|---|---|
| 1 | **Bluetooth radio (NimBLE)** — host stack + pools + start task | Take measured 50,644–51,188 B on X3 before the #267 trims; derived post-trim take `kStackBytes = 43,508` B (`lib/BlePageTurner/src/RadioPolicy.h:44`, comment block derives 51,188 − 7,680 verified heap savings) | Measured 46,508 B of internal RAM + 45,056 B largest-block drop on 13/10 (`RadioPolicy.h:33-35`); the host lives in PSRAM (`FREEINK_BLE_STACK_IN_PSRAM`) but the *take* still lands in internal RAM | `bleturner::status()` to observe, stop via the existing screen-change hook, restore `bleturner::switchOn()` (`lib/BlePageTurner/src/Runtime.cpp:679`, `:687`) | **B** — but mostly *already released* at these screens (see note below) | Only if `SETTINGS.ble.enabled` AND the radio was up when the claim began; a refused restart is fine (the radio verdicts gate it, and the next reader/home visit re-arms it — `Runtime.cpp:430-440`) |
| 2 | **FontCacheManager decompressed caches** (`FontDecompressor` page slots × 8 + hot group; `lib/EpdFont/FontDecompressor.h:10-15, 55-63`) | tens of KB (per-slot page buffer + glyphs table; the runtime `Stats.pageBufferBytes` counter reports the real number per session) | same allocation sizes, but every `malloc()` ≥ 4,096 B lands in PSRAM (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`, [DRAM] §2.1 correction) → ~0 DRAM | `FontCacheManager::releaseSdFontCaches()` (`lib/GfxRenderer/FontCacheManager.cpp:28`) | **B**, and already called ad-hoc at 5 of the failure sites (`TtfFontDownloadActivity.cpp:257-259, :696-699`; `FontDownloadActivity.cpp:173-175`) | Lazy — the prewarm pass rebuilds on the next render (`PrewarmScope`), no explicit refill |
| 3 | **SdCardFont resident caches** (glyph/kern arenas, CJK fallback tables, ligature tables — classic class only) | up to tens of KB while a `.cpfont` was used this session | compiled out on TTF boards (`lib/GfxRenderer/FontCacheManager.cpp:31-33`) | same `releaseSdFontCaches()` → `SdCardFont::releaseResidentCaches()` (`lib/EpdFont/SdCardFont.cpp:256-267`) | **B** | Lazy (on-demand miss callback re-arms the glyph loader) |
| 4 | **Web server / file transfer** (`CrossPointWebServerActivity`, its own heap pre-flight `src/util/WebServerHeapGate.h`) | separate activity — cannot coexist with the font screens | same | n/a | **A** — not concurrent | n/a |
| 5 | **Reader-session reservations**: `FibpPrefetchWorker` reserved stack 32.8 KB DRAM ([DRAM] §8.2-8.4), ZIP index `unordered_map` ~115 B/entry (~56 KB at 465 entries, [DRAM] §8.5), inflate ring 32 KB (`lib/InflateReader/InflateReader.cpp:14-37`, PSRAM-pinned on S3 by #279; `lib/ZipFile/ZipFile.cpp:673` is the only streaming init in reading) | N/A (bitmap class: no worker, no TTF) | held **only while a book is open** | reader destructor releases them (`src/activities/reader/EpubReaderActivity.cpp:318-350`) | **C** — the font screens are only reachable from Home→Settings, where the reader is closed; the one conflict is in-reader network operations (§6) | n/a |
| 6 | **Hint-probe task** ~57 KB (`src/activities/reader/BookFontLoader.cpp:350`-era constant [DRAM] §1.4) | N/A | reading-session only | — | **A** — never concurrent with the font screens | — |
| 7 | **ProgressManager worker** (6,144 B stack `src/ProgressManager.cpp:88`, records + semaphores, begun at boot `src/main.cpp:939`) | same | same | none | **C** — small, latency-bearing saves; suspension buys nothing | — |
| 8 | **Plugin runtime** (event delivery; the catalog already streams JSON/XML — `src/activities/plugins/PluginCatalogActivity.cpp:820, :920, :961`) | plugin delivery can bring Wi-Fi up on the sleep path (`src/main.cpp:618-637`) | same | n/a | **A** — no persistent allocations to claim (post-e8299b5f chunked streaming) | — |
| 9 | **Wi-Fi working set** (modem + lwIP + mDNS; reserve constant 45,000 B `src/util/WebServerHeapGate.h:27`) | connected STA session costs real DRAM | same, internal RAM | n/a | **C** — it *is* the claimed operation's requirement, suspending it cancels the work | n/a |
| 10 | **`FontDownloadActivity` registry** — `fontInstaller_.refreshRegistry()` frees and reallocates every family entry (`FontDownloadActivity.cpp:176-180`) | fragmentation source, not a holder | — | already sequenced before the buffers | **A** | — |

**Note on consumer 1 (Bluetooth) — already released in the common case.**
`ActivityManager` runs `bleturner::beforeScreenChange(0, nextIsReader)` before
every screen replace (`src/activities/ActivityManager.cpp:163-171`),
suspends the radio when the next screen is not a reader, and defers the pop
until the stop completes. So by the time a font screen paints, the radio is
normally down. The residual gaps are real but narrow:

- the S3 profile keeps the radio up *at Home* by design
  (`homeStartLargest != 0`, `RadioPolicy.h:70`), so Home→Settings→Fonts can
  still race a Home-started radio into the claim window;
- a `ReplaceForSleep`-adjacent pending action skips the hook
  (`ActivityManager.cpp:165`);
- a start still in flight needs `bleturner::settleStart()` before it can be
  stopped (`lib/BlePageTurner/src/Runtime.cpp:600-611`).

The claim must therefore treat the radio as "maybe up", observe
`bleturner::status()`, stop it when needed, and record whether it was up for
restore. The X3-class radio additionally never starts at Home
(`kX3Profile.homeStartLargest = 0`), so on the classic class the claim is
mostly a no-op for consumer 1 — worth having for correctness, not the classic
class's rescuer.

## 3. Claim protocol — reuse, don't duplicate

The gate follows the exact vocabulary of the two existing guards and adds only
the suspension planning:

- **Floor criteria** — unchanged: `tlsheap::verdict()`
  (`src/util/TlsHeapGuard.h`, free ≥ 40,000 / largest ≥ 20,000) for the
  network stage. No new floor constants are introduced anywhere in this
  design; the [DRAM] §6 rule about re-pinning constants is honored by
  construction.
- **Verdicts** — the `webgate` triad, reused by name (`Proceed / Defrag /
  Fallback`, `src/util/WebServerHeapGate.h:13-17`) with one remapped middle
  meaning, because the heap-defrag-**reboot** rescue of the web-server gate
  does not belong inside a settings screen:

  ```cpp
  enum class Verdict : uint8_t {
    Proceed,   // floor met
    Defrag,    // floor not met, but a suspension lever remains — pull the
               // one `phase.next` names, re-measure, ask again (the twin
               // of webgate's "one more attempt may rescue it")
    Fallback,  // all levers pulled, floor still unmet — structurally short
  };
  ```

### 3.1 Pure core (host-testable), `src/util/MemoryClaimGate.h`

```cpp
namespace memclaim {

// A suspension lever, in the order the gate tries them (cheapest first).
enum class Step : uint8_t {
  FontCaches,  // releaseSdFontCaches(): synchronous, rebuildable, free
  Radio,       // BLE radio down: bounded (settleStart + suspend timeout)
};

struct StepCost { Step step; bool applied; uint32_t freeAfter; uint32_t maxBlockAfter; };

// Pure: decides whether the floor is met now, and if not, what to try next.
// `stepsTried` bitset of levers already pulled this claim.
struct Phase {
  Verdict verdict;
  Step next;      // valid only when verdict == Defrag (a lever remains)
};

// free/maxBlock/floors come straight from the caller's device measurement.
inline const Phase& plan(uint32_t free, uint32_t maxBlock, uint32_t freeFloor,
                         uint32_t maxBlockFloor, uint8_t stepsTried);

}  // namespace memclaim
```

(Exact signatures are for the implementation PR to fix; the property that
must survive is: no allocation, no device includes, tables inlined, and the
planner is a pure total function. The host test seeds the same
`free/largest` pairs the real screens log.)

### 3.2 Device flow (the recheck loop)

Integrated into the state machine the screens already have — the gate never
spins; each round of suspend→re-measure happens between two
`requestUpdateAndWait()` frames, so the panel always has a current frame and
`Back` is polled by the pump that already runs inside downloads
(`src/activities/settings/TtfFontDownloadActivity.cpp:676-690` progress
callback shows the pattern).

```text
state: LOADING_MANIFEST (existing)
  claim.begin();                      // records wasRadioUp = status().running
  loop:
    measure (free, maxBlock)
    plan(free, maxBlock, floors, stepsTried):
      Proceed  -> break, run the existing fetch path
      Defrag   -> render current frame once ("Freeing memory…" only if the
                  step will actually change something),
                  pull the step in phase.next (bounded:
                  releaseSdFontCaches is synchronous; radio stop =
                  bleturner::settleStart() + the runtime's bounded suspend),
                  stepsTried++, continue
      Fallback -> existing ERROR screen, STR_MEMORY_ERROR (unchanged UX)
  claim.end();                        // restore (see 3.3)
```

Boundedness per step: `releaseSdFontCaches()` is synchronous and immediate
(`lib/GfxRenderer/FontCacheManager.cpp:28-34`); the radio stop is already a
bounded path the screen-change hook drives every frame
(`ActivityManager.cpp:163-171` again) — the claim simply reuses it twice if a
stop is still settling.

### 3.3 Restore semantics

| consumer | restore rule |
|---|---|
| BLE radio | Restored **only if** it was running when the claim began **and** `SETTINGS.ble.enabled`. Restore = `bleturner::switchOn()` (`Runtime.cpp:679`); restart may refuse for memory — that refusal is *safe and silent*: the radio verdicts keep the radio down until the next reader/home visit re-arms it (`Runtime.cpp:430-455`), exactly the pre-existing behavior of an idle-off radio. Restoring is *after* the heavy allocation burst (on claim exit), never interleaved with it. |
| Font caches | Never explicitly refilled. `PrewarmScope` already rebuilds them lazily on the next render; instant refill would spend the heap we just recovered. |
| Wi-Fi / manifest buffers / DOM | Rely on the existing scopes (`claim.end` does not need to know about them). |

Restore is captured in a tiny RAII `Claim` object (or explicit
`claim.end()` in the same state machine block that currently frees
`clearManifest()`) so an early `return` on fetch failure still restores.

## 4. Where the gate lives

One shared helper — `src/util/MemoryClaimGate.h` for the pure core, with the
device adapter `src/util/MemoryClaimGateDevice` (or a static helper on the
same header) that owns the two real levers:

- `releaseFontCaches(renderer)` — the single helper that replaces every
  in-scope ad-hoc `renderer.getFontCacheManager()->releaseSdFontCaches()`
  call: the three font-screen groups (`TtfFontDownloadActivity.cpp:257-259,
  :696-699`; `FontDownloadActivity.cpp:173-175`) come under the claim, and
  the screens that already release for their own reasons keep their direct
  calls (`src/activities/network/CrossPointWebServerActivity.cpp:81, :358` —
  already reviewed, different heap model; `src/activities/CatalogActivity.cpp:184`;
  the reader/dictionary/sleep sites reach this helper only if a later round
  widens the claim to them).
- `radioSuspend()` / `radioRestore()` — thin wrappers over
  `bleturner::status()` / the screen-change stop / `switchOn()`.

**Call sites in this design's scope** (the ones the owner directive names,
"font managing or any other background that take memory"):

| operation | floor reused | today's guard | claim added |
|---|---|---|---|
| TTF font manifest + per-family download | `tlsheap` | `TtfFontDownloadActivity.cpp:272-275, :703-707` | yes |
| `.cpfont` font manifest + download (classic class) | `tlsheap` | `FontDownloadActivity.cpp:201-204, :573-577` | yes |
| Plugin catalog/install bundle | `tlsheap` | streaming parsers, no dedicated guard | yes, cheap (radio mostly down already) |
| OTA update check/download (`src/activities/settings/OtaUpdateActivity`) | `tlsheap` inside downloader | — | yes |
| Dictionary in the reader (`src/activities/reader/DictionaryDefinitionActivity.cpp:168-183`) | `tlsheap` + text size | already releases caches | **no** — see §6 |
| Clock sync tz-detect (`src/activities/settings/ClockSyncActivity.cpp:93-98`) | `tlsheap` | already | no (plain HTTP, own guard) |
| Web server / AP mode | `webgate` | own pre-flight | no — different heap model, out of scope |

## 5. UX rules

- **No blocking loops.** Every suspension step is bounded (synchronous release
  or the runtime's timed stop — the worst wait in the radio path is the
  3,000 ms build-release timeout, `lib/BlePageTurner/src/Runtime.cpp:37`) and
  the loop yields a frame per step, same as the download progress pump
  already does.
- **Feedback.** A device that was going to free memory shows it once: while a
  claim step is actually pulling a lever, the existing
  `LOADING_MANIFEST`/`DOWNLOADING` frame carries a short status line. No new
  activity, no popup system — the screens are already state machines with a
  render path per state.
- **Back is never locked out.** Already true in the download states (the
  transfer pump maps `Back` to `cancelRequested_`,
  `TtfFontDownloadActivity.cpp:677-685`); the claim loop inherits it because
  it runs in the same pump cadence.
- **Degrade, not crash.** When even full suspension cannot meet the floor, the
  screens take their existing `ERROR` + `STR_MEMORY_ERROR` branch. New (in the
  implementation PR): the branch logs a `[MEM] claim` line so the owner can
  tell cause 1 from cause 2 in a field log (`free/max_block` before and after,
  which levers were pulled).

## 6. Risks and trade-offs

- **Bluetooth churn.** Suspending a radio a user expects to be up adds a
  reconnect on restore (pairing intent unchanged — the saved opt-in and the
  peer table are untouched; `switchOn()` reuses the stored peer). Accept:
  these screens are minutes long and the radio is mostly down at them anyway;
  the alternative is the report's failure screen. The pairing-screen silent
  restart machinery (`Runtime.cpp:559-573`) is untouched, so #275's pins are
  unaffected.
- **Announced vs silent.** Announce only when a lever actually moves; at the
  common path (radio already down, caches already released) the claim costs
  two measurements and no frame — silent, invisible, free.
- **Font-cache refill latency.** Zero extra: the caches were already
  released-and-lazily-rebuilt at these screens today; the claim only
  guarantees it happens *before* the burst instead of next to it.
- **Peer returns mid-download.** A remote pressing a key during a font
  download: the radio is down, the press is dropped (Home screen behavior:
  `Runtime.cpp:662-665`); same as today after an idle-off.
- **Dictionary and other in-reader downloads stay out of scope.** The reader
  session holds the worker's reserved 32.8 KB stack ([DRAM] §8.2) and the ZIP
  index; cancelling the worker to free the stack would re-expose exactly the
  fragmentation storm §8.1 fixed, and the ZIP index is not suspendable without
  a redesign ([DRAM] §8.5 — owner decision pending). The dictionary screens
  already release font caches and bound their own text buffer
  (`DictionaryDefinitionActivity.cpp:168-183`); their claim is the reader's,
  not this gate's.
- **The gate never lies with a big floor number.** It reuses `tlsheap`'s
  floors; nothing in this design duplicates or widens them, and nothing it
  releases can be *less* than what the existing ad-hoc releases already
  recovered.

## 7. Honest statement: does the gate fix the reported font OOM?

Per-board, using the measured numbers the repo already carries:

**X4 Pro (S3, `CROSSPOINT_TTF_READER=1`) — the screen in the report.**
Measured Home free 124,752 / largest 81,908; with the radio started at Home
the same measurement drops to free 78,244 / largest 36,852
(`RadioPolicy.h:33-35`). Wi-Fi up costs ~45,000 of that
(`WebServerHeapGate.h:27`); TLS floors at 40,000 free / 20,000 largest
(`TlsHeapGuard.h`); the manifest buffer (96 KB, `poolMalloc`), the JSON DOM
and the arena/file-table allocations all prefer PSRAM on the S3 core (every
request ≥ 4,096 B — §0 table), so the DRAM demand is Wi-Fi + TLS + small
runover arrays. The arithmetic:

- radio up at claim time → **78,244 free** vs ~45 KB Wi-Fi + 40 KB TLS floor
  ⇒ the per-transfer guard at `:703-707` and the TLS stage at `:272-275`
  fail exactly as reported. The claim (radio down) recovers **46,508 B +
  largest-block room** and turns the screen reproducibly green — this is the
  case the community report hit, and the gate **fixes it**.
- radio already down → free ~110-125 KB; the claim is a no-op by
  measurement (both levers already released), and the remaining failure mode
  is fragmentation or arena/file-table DRAM overruns (follow-up F1 frees the
  96 KB buffer earlier), which the claim cannot manufacture block room out
  of. There the `Fallback` branch keeps today's honest error instead of
  thrashing levers that no longer exist.

**X3/X4-classic (C3, `default`) — the same error string on a board that
cannot always win.** Static SRAM budget after boot ([DRAM] §1.3-1.4): ~169 KB
static + 49 KB loop stack + 52 KB framebuffer leaves ~110 KB heap; the
measured X3 refusal sample sits at free 73,692 / largest 47,092 (pinned in
`lib/BlePageTurner/test/RuntimeTest.cpp:496`). On the C3 class the manifest
buffer is DRAM, and while Wi-Fi is connected the firmware needs ~45 KB of
free heap plus the TLS floor on top of the 64 KB buffer + arena: the
guard that already runs after the buffer is allocated
(`FontDownloadActivity.cpp:201-204`) fails with ~29 KB left
(73,692 − 45,000 Wi-Fi), below even the 40,000 B `tlsheap`
free floor, before the parse buffers are considered. Even the most pristine
possible state (~110 KB budget, both levers released) leaves ~110 − 45 (Wi-Fi)
− 40 (TLS floor) ≈ 25 KB against a 64 KB manifest. **The gate widens
the window and gives the error earlier and more gracefully, but it does not
make `.cpfont` font management structurally feasible on the no-PSRAM class;
that requires shrinking the manifest path itself** — follow-up F2 below,
which has in-repo precedent (the plugin catalog replaced whole-buffer parsing
with streaming parsers post-upstream e8299b5f,
`src/activities/plugins/PluginCatalogActivity.cpp:820, :920, :961`).

So: the implementation makes the *reported* X4 Pro failure go away and makes
every memory-heavy screen defend its floor consistently; it deliberately
refuses to promise the impossible on the classic class and says so on the
screen it always did.

## 8. Follow-ups (owner decisions, not in the gate)

- **F1 — release the manifest buffer before the arena is allocated
  (`TtfFontDownloadActivity`).** `manifestBuf` stays live until the
  function's scope closes (`:263` … end), although the DOM copied everything
  it kept at parse time (parse at `:296-311`), so the buffer sits across the
  `:387-401` arena/file-table allocations. Freeing it right after
  `deserializeJson` removes ~96 KB of PSRAM pressure exactly where the
  reported `:390/:399` failures occur — likely the smaller and earlier
  win for the TTF screen, independent of the claim gate.
- **F2 — classic-class manifest streaming.** Replace the 64 KB
  whole-buffer parse of `FontDownloadActivity.cpp:165-215` with the
  chunked/list-parser pattern the plugin catalog already uses. This is the
  structural fix the claim gate cannot provide (§7, C3 case).
- **F3 — measure, then tune.** `[MEM] claim` log lines (§5) across one
  real font install feed the owner the real per-board numbers this design
  marks as derived.

## 9. Verification plan (for the implementation PR)

| step | pass criteria |
|---|---|
| host `ctest` — new `test/memory_claim_gate/` | planner is total: every (free, maxBlock, floors, stepsTried) input maps to a verdict; no step suggested twice; `Proceed` claim at exactly the `tlsheap` floors |
| per-screen behavior | with the radio up and caches resident, the first claim frame's `[MEM]` log shows the pre/post lever deltas and the same free-heap arc the §7 arithmetic predicts |
| regression guards | `pio run -e default` + `-e x4pro` green; BLE regression pins from #275/#279 untouched (`RadioVerdictTest`, `RadioVerdictS3Test`, `RuntimeTest`); `git diff --exit-code` after format clean |
| device | owner runs one font install with BLE on and one with BLE off; both succeed on x4pro; classic-class installs still error honestly until F2 lands |
