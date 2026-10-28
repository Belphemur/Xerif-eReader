# X3 WiFi-connect / clock-sync abort — OOM-class hardening

Date: 2026-10-26
Status: Proposed (defensive hardening; the exact thrower is not yet pinpointed — see
"What /crash_report.txt would confirm or refute")
Affected boards: X3 (ESP32-C3) profile class; written against v2.11.0 sources
at `origin/develop` 08fd9d05.

## 1. Reported symptom

A v2.11.0 X3 user, on a fresh install of the fork, gets a deterministic
"System Crash — abort() was called at PC 0x422c2db1 on core 0" every time the
reader connects to WiFi (phone hotspot), and again from Settings > System >
Clock > Sync now. A `/crash_report.txt` is written to the SD card; the reporter
has not yet retrieved it, so this document designs the hardening without it.

## 2. Decode evidence

Orchestrator-decoded facts, cited to the v2.11.0 tree:

- `0x422c2db1` resolves (gh_release v2.11.0 ELF, C3) to
  `__cxxabiv1::__terminate` (libstdc++ `eh_terminate.cc:45`) — the
  `std::terminate` → `abort()` landing. The project builds with
  `-fno-exceptions`, which bounds the throw sites in the project's OWN sources
  to two classes that can land there:
  1. a failed `operator new` — an uncaught `std::bad_alloc` thrown from
     libstdc++ (OOM), or
  2. `std::get<T>` on a variant holding a different alternative.
- The variant paths on the WiFi-connect chain were audited and are CLEAN:
  `WifiSelectionActivity::onComplete`
  (src/activities/network/WifiSelectionActivity.cpp:1120-1128) is the only
  completion site and always sets `WifiResult` when connected;
  `ActivityManager` normalizes unset results to cancelled; both
  `std::get<KeyboardResult>` call sites
  (WifiSelectionActivity.cpp:359, 383) are `isCancelled`-guarded.
  Conclusion: within the project's own sources the crash class is (a) — an
  OOM abort — and class (2) is ruled out on this chain. That boundary is
  not exhaustive: framework, third-party, and library-internal code (ESP-IDF,
  Arduino core, wolfSSL, libstdc++ itself) can throw or abort by other means,
  so OOM is the LEADING HYPOTHESIS (ranked H1 in §3), not an established fact,
  until the crash-report decode (§4) confirms it.
- Both reported symptoms share one code path. On `WL_CONNECTED`,
  `WifiSelectionActivity::checkConnectionStatus`
  (src/activities/network/WifiSelectionActivity.cpp:523-546) runs, and on the
  FIRST successful connect (fresh install ⇒ `SETTINGS.clockHasBeenSynced == 0`)
  calls `halClock.syncFromNTP()` and then `freeink::detectTimeZoneFromIp()`
  (lib/hal/HalTimeZone.cpp:109) — a wolfSSL TLS 1.3 HTTPS GET to
  `https://ipwho.is/` with fallback to `https://worldtimeapi.org/api/ip`.
  `ClockSyncActivity::runSync` (src/activities/settings/ClockSyncActivity.cpp:61)
  runs the same pair.
- If the device crashes before `SETTINGS.saveToFile()`
  (WifiSelectionActivity.cpp:538), `clockHasBeenSynced` stays 0, so every later
  connect re-runs the hook — matching "every time I try to connect it crashes".
- X3 = ESP32-C3: single core, ~380 KB usable RAM, no PSRAM, 792x528
  framebuffer (52 KB). Board profile `XTEINK_X3` at
  freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h:1030.
- This abort class has extensive history on tight boards:
  - platformio.ini:128-140 (MEMFIX-PORT): without SP ECC, wolfSSL
    `WOLFSSL_SMALL_STACK` heap-allocates `FP_MAX_BITS`-sized temporaries and
    "OOMs (MP_MEM) at the ~50KB free heap a reading session leaves". (SP ECC is
    now on, but the 50 KB-tight-heap baseline stands.)
  - src/activities/reader/EpubReaderActivity.cpp:2841: "X3: abort() in a page
    restore at free 22,676 B" — the C3 heap in reader-exit states can be below
    even 25 KB free.
  - src/activities/network/CrossPointWebServerActivity.cpp:66-96, 142-150:
    X3 web-server activation aborts on fragmented heaps; mitigated there by
    reboot-once into a pristine heap plus SD-font cache release before the big
    allocations.
  - lib/Memory/Memory.h `makeUniqueNoThrow` — the sanctioned no-throw idiom,
    not available inside libstdc++ itself.

### Pre-existing heap guards (the idiom this change mirrors)

- src/network/HttpDownloader.h:30-35: `MIN_TLS_FREE_HEAP = 40000`,
  `MIN_TLS_MAX_ALLOC = 20000` — "Below this the session or its ~17KB record
  buffer fails mid-stream (wolfSSL MEMORY_E) — or an interior allocation
  abort()s the device."
- src/util/DictHtmlPages.cpp:28-29, 218: 40 KB free / 20 KB max-alloc before
  styled dictionary pages.
- src/SdCardFontSystem.cpp:304-307: 48 KB max-alloc before a font-variant load.
- src/activities/CatalogActivity.cpp:185-187 and
  src/activities/browser/OpdsBookBrowserActivity.cpp:904 apply the same shape
  before TLS-adjacent work.

## 3. Ranked hypotheses

### H1 (most likely): OOM abort inside `detectTimeZoneFromIp()`

`SecureHttpClient` runs the same wolfSSL TLS 1.3 stack as `HttpDownloader`
(freeink-sdk/libs/network/SecureNet), so it carries the same demand profile the
repo already measured for TLS: the handshake working set, and ~17 KB-class
record buffers when a peer ignores `HAVE_MAX_FRAGMENT` (HttpDownloader.h:31-33).
`ipwho.is`/`worldtimeapi.org` are arbitrary third-party hosts — nothing
guarantees they honour the fragment-size extension. The parse step adds an
ArduinoJson document for the response body on top.

On a fresh install the connect runs from a state where the X3 heap is exactly
the "~50 KB free after a reading session" baseline of the MEMFIX comment — and
`CrossPointWebServerActivity` proves the web path already treats that state as
abort-likely. The clock-sync entry (`ClockSyncActivity`, launched directly from
Settings or the reader popup at EpubReaderActivity.cpp:2697) never gets the
web-path's reboot-into-pristine-heap treatment, so the TLS tz-detect runs on the
current heap whatever its state.

### H2: OOM elsewhere on the connect chain

`WL_CONNECTED` also triggers: DHCP/framework-internal allocations after the
status flips, `trustedtime::startSync()` (WifiSelectionActivity.cpp:550), the
SD write in `WIFI_STORE.setLastConnectedSsid` (RenderLock'd), and the plugin
outbox drain that follows. Any of these can hit the same `operator new →
std::terminate` landing. The web-server precedent
(CrossPointWebServerActivity.cpp:66-96) shows X3 aborts on fragmented heaps in
far cheaper code paths.

### H3 (residual): variant/`std::get` misuse

Audited and ruled out above (§2). Retained only as a residual in case the crash
report's stack window lands somewhere neither H1 nor H2 covers.

## 4. What /crash_report.txt would confirm or refute

The report captures a 1 KB stack window plus the last log lines. This change
makes the tail decisive:

| Crash-report observation | Reading |
| --- | --- |
| Last line `CLK: NTP sync: free=… maxblock=…` then nothing | Crash during `halClock.syncFromNTP()` (SNTP is UDP/small — unexpected; would push H2). |
| Last line `CLK: tz detect: free=… maxblock=…` with free ≥ 40000, then nothing, no `HalTimeZone: Zone via` | Crash inside the TLS tz-detect WITH the floor satisfied — the guard did not fire. Would mean the 40 KB floor is too low for this path and must be raised (or SecureHttpClient's peak measured on-device). |
| Last line `CLK: tz detect skipped: low heap (free=… maxblock=…)` | Guard fired as designed; the connect continued. No crash expected — if a crash still follows, it is H2. |
| Last line `WIFI: …` / `CLK: NTP sync done ok=…` with heap values, crash before any tz line | H2 (connect chain) — the guard's breadcrumbs localize it even if the guard can't prevent it. |
| `HalTimeZone: Zone via …` as the last line, crash later | H2 with the tz-detect exonerated; look at `trustedtime`/plugin drain next. |

The reporter still owes `/crash_report.txt`; decoding it against a local
v2.11.0 `gh_release` ELF will name the thrower. Until then this PR is honestly
scoped as hardening: it removes the OOM-abort class from the connect/sync path
(H1) and instrumented the rest of the chain (H2) so any future report is
self-locating. It does NOT claim to fix THE crash.

## 5. Guard design

One pure, host-testable decision function mirrors the existing guards' style:

- `src/util/TlsHeapGuard.h` — `tlsheap::verdict(freeHeap, maxAllocHeap)`
  returning `Proceed`/`Skip` against `kMinFreeHeap = 40'000` and
  `kMinLargestBlock = 20'000` (constexpr, no Arduino includes).
- `HttpDownloader::MIN_TLS_FREE_HEAP` / `MIN_TLS_MAX_ALLOC` are re-aliased to
  the same constants so the TLS floor has exactly one source of truth
  (`heapAvailableForTransfer()` delegates to the same verdict — behaviour
  unchanged).

Threshold justification: the tz-detect uses the device's own measured TLS
demand. `SecureHttpClient` (tz-detect) and `HttpDownloader` (downloads) are the
same wolfSSL TLS 1.3 stack on this device, so the 40 KB free / 20 KB
largest-block floor already validated for downloads transfers directly; the
tz-detect's extra ArduinoJson parse of a ~1 KB body fits inside that margin.
Two independent checks because the C3's failure mode is fragmentation: free
heap says there is room in total, max-alloc says the biggest contiguous chunk
can host the record buffer and handshake temporaries.

Call-site behaviour:

- `WifiSelectionActivity::checkConnectionStatus`: breadcrumb `LOG_INF("CLK", …)`
  with free/max-alloc before `syncFromNTP()` and before the tz-detect; when the
  verdict is `Skip`, log the skip with the measured values and skip only the
  tz-detect — the NTP sync and `SETTINGS.saveToFile()` still run, so
  `clockHasBeenSynced` persists and the NTP half of the hook does not re-fire
  on every connect. The skip also sets the persistent `clockTzPending` flag,
  and the next connect re-runs ONLY the tz-detect (no NTP re-sync) once the
  heap passes the floor, so a first-time user is not stranded on UTC. The flag
  clears when a detect is actually attempted, valid or not: a reachable-but-
  failing geolocation service keeps the pre-PR one-shot semantics instead of
  turning every connect into an extra TLS session.
- `clockTzPending` is a new non-SettingsList settings key (absent = 0, no
  retry owed), saved next to the other manually persisted clock keys.
- `ClockSyncActivity::runSync`: same breadcrumbs and guard; on skip,
  `tzDetected_` stays false and the existing `STR_TIMEZONE_DETECT_FAILED`
  status line renders — the pre-existing tz-failure UX, unchanged. On skip it
  also sets `clockTzPending` so the automatic hook retries on a later connect;
  a manual detect attempt clears it.
- Heap breadcrumbs bracket the NTP and tz-detect calls in both sites (tag
  `CLK`/`WIFI`), at `LOG_INF` level so they survive `LOG_LEVEL=1` release
  builds into any future `/crash_report.txt` log tail.

Out of scope (explicit user-initiated TLS with their own failure UX): `KOSync`,
OPDS browsing, OTA downloads, `HttpDownloader` consumers — each already guards
or degrades via `heapAvailableForTransfer()`/catalog error UI, and a silent
skip there would hide a user action's failure rather than degrade an automatic
background step.

## 6. Caller audit (owner rule: every caller checked)

`grep -rn detectTimeZoneFromIp src/ lib/` (excluding HalTimeZone itself) shows
exactly two callers:

1. `src/activities/network/WifiSelectionActivity.cpp:531` (first-connect hook),
2. `src/activities/settings/ClockSyncActivity.cpp:81` (`runSync`).

Both are guarded by this change. `SecureHttpClient`'s other users (KOSync,
OPDS, OTA, web-server relay) are explicit user-initiated actions with their own
failure UX and are out of scope (§5).

## 7. Verification

- Host unit tests for `tlsheap::verdict` (pure function; boundary cases:
  exactly-at-floor, free-heap shortfall, fragmentation shortfall, both low).
- Firmware build `pio run -e x4pro` proves compilation; CI covers the heavier
  C3 (`default`/`gh_release`) environments.
- clang-format and cppcheck on the touched tree.
- Device verification remains the owner's: on the reporter's X3, connect to a
  hotspot and run Sync now; expected serial output shows the `CLK` heap
  breadcrumbs and either a tz-detect result or a `low heap` skip — never a
  reboot. Retrieving `/crash_report.txt` from the reporter's card remains
  required to name the original thrower.

## Addendum (2026-10-26, later): tz-detect switched to plain HTTP

Measured live on 2026-10-26 (all three endpoints probed from a host the same
day): the time-zone detect no longer opens any TLS session. The wolfSSL demand
analysis above (§3 H1) described the v2.11.0 code; the shipped firmware now
avoids that demand class on this path entirely:

| Service | URL | Response | Measured size |
| --- | --- | --- | --- |
| Primary | `http://ipwho.is/?fields=success,timezone` | nested (`timezone.id`/`offset`/`is_dst`) | 157 B (full response 918 B) |
| Fallback | `http://ip-api.com/json/?fields=timezone` | flat `{"timezone":"America/Toronto"}` | 30 B |

- `worldtimeapi.org` is removed: it was unreachable over HTTPS in two probes
  on 2026-10-26 (connection reset) and is a known-flaky service. Its parser and
  the `utc_offset` helper are deleted.
- `ipwho.is` keeps its nested response shape; `?fields=success,timezone` scopes
  the payload to the one block the parser reads (the `success` flag is
  explicitly requested because the scoped response omits it otherwise).
- `ip-api.com`'s flat response carries no offset or DST flag. The parser
  therefore records the id with neutral `offsetMin=0`/`isDst=false` — the
  existing AceTime resolution (`clockEffectiveOffsetMin()`, which computes the
  live offset from the id and folds DST) drives correctness, and the persisted
  cached offset stays only an offline fallback.
- ip-api's free tier is HTTP-only (HTTPS is a paid tier) and rate-limited to
  45 req/min — irrelevant for a once-per-device lookup.

Tamper mitigation: a plain-HTTP response can be modified in transit, so the
zone id is accepted only when it is shaped like an IANA id — 1–3 segments of
`[A-Za-z0-9_+-]` separated by `/`, no empty segments or leading/trailing
slash, capped at 39 characters (`isValidIanaZoneId`, host-tested in
`test/tz_detect_parse`). `clampOffsetMin` is unchanged. Documented trade-off:
a tampered response can yield a *wrong time zone* (plain HTTP is not
confidentiality-protected), not a security breach — the id feeds AceTime's
zone-name lookup and settings JSON only.

The TLS heap guard, `TlsHeapGuard.h`, the skip semantics, the `clockTzPending`
retry, and the heap breadcrumbs stay exactly as described above: other paths
still use TLS, and a genuinely fragmented heap can fail allocations outside
the transport, so the guard is now defense-in-depth rather than the primary
protection for this path. With no TLS session in the tz-detect, the peak
transient demand on this path drops from the ~25–40 KB TLS/ArduinoJson profile
to roughly the ArduinoJson document for a sub-kilobyte body (~1–2 KB class).

Earlier sections are left as written: §3 H1 and the §4 decode table remain the
record of what v2.11.0 shipped and what `/crash_report.txt` from an affected
device still has to confirm — this change does not claim to fix THE reported
crash until that decode names the thrower.

## Addendum 2 (2026-10-26): crash_report decoded — thrower was the web-server allocation

The reporter of the original issue attached `/crash_report.txt` (v2.11.0, X3).
Decoded against the preserved v2.11.0 `gh_release` ELF (§2 procedure), the panic
stack names the thrower — and it is **not** the tz-detect TLS path:

```
loop() (main.cpp:1561)
 → CrossPointWebServerActivity::onNetworkModeSelected lambda (CWA.cpp:201)
  → CrossPointWebServerActivity::onWifiSelectionComplete (CWA.cpp:229)
   → CrossPointWebServerActivity::startWebServer (CWA.cpp:310)
    → operator new  →  std::bad_alloc  →  std::terminate  →  abort() @ 0x422c2db1
```

The log tail confirms the tz-detect **failed gracefully** ("All time zone
services failed", `autoDetect: ok=0`) seconds before the abort — the two failed
HTTPS attempts each ran a partial wolfSSL handshake (~20–40 KB working set
allocated then freed), fragmenting the heap immediately before
`startWebServer()`'s `operator new`. On that fragmented heap the allocation
threw, and under `-fno-exceptions` the throw terminated the device. This
upgrades §3: H1's mechanism (OOM abort on the connect chain) is confirmed, but
the failing allocation was the web-server object, not the TLS fetch itself.

Fix shipped with this addendum: `startWebServer()` allocates the server with
`makeUniqueNoThrow` and routes allocation failure through the same
log-and-`onGoHome()` path as a `begin()` failure, with free/max-alloc heap
breadcrumbs in the error line. Combined with the HTTP switch above (no TLS
session on this path any more) the reported crash chain has neither its
fragmentation source nor its throwing allocation; a web server that still
cannot be allocated now degrades to a visible "go home" instead of a reboot.
`/crash_report.txt` files from v2.12.0+ will carry the `WEBACT` heap
breadcrumbs and can no longer abort at this site.

## Addendum 3 (2026-10-27): crash still reproducible in v2.12.1 — the throw
site moved *into* `begin()`, and the AP (hotspot/QR) path has no heap-defrag
reboot at all

Reporter dreysnaps posted a v2.12.1-dev `/crash_report.txt` on issue #258 (PC
`0x422cffcd`). Decoding it (log window is conclusive; PC not symbolized
— `b51b05b` is a CI build id, not a commit sha present in the repo):

- The v2.12.0 fix (Addendum 2) hardened only the *outer* allocation in
  `CrossPointWebServerActivity::startWebServer()` with `makeUniqueNoThrow`
  (`CrossPointWebServerActivity.cpp:319`). The reporter's first attempt
  behaved exactly as designed — it logged
  `LOG_ERR("Out of memory for web server ...")` (a87d4018) and returned to
  Home instead of rebooting — but the *second* attempt still aborted.
- Log tail shows the outer alloc **succeeded** then the heap collapsed:
  `[MEM] Free heap after WebServer allocation: 6784 bytes` →
  `[MEM] Free heap after route setup: 928 bytes` (30 route lambdas consumed
  the remainder) → `Starting WebSocket server on port 81...` →
  `abort() was called at PC 0x422cffcd`. The abort lands between the
  "Starting WebSocket server" and "WebSocket server started" traces, pinning it
  to the **next** throwing `new` inside `CrossPointWebServer::begin()`:
- `src/network/CrossPointWebServer.cpp:284` — `wsServer.reset(new WebSocketsServer(wsPort))`.
  With 928 bytes free, `operator new` throws → `std::bad_alloc` →
  `std::terminate` → `abort()` under `-fno-exceptions`.

### Remaining throwing allocations in `begin()` (all unguarded by `isRunning()`)

| File:Line | Allocation | AP-only? |
| --- | --- | --- |
| `CrossPointWebServer.cpp:176` | `new CrossPointHttpServer(port)` | no  |
| `CrossPointWebServer.cpp:277` | `new WebDAVHandler()` | no  |
| `CrossPointWebServer.cpp:284` | `new WebSocketsServer(wsPort)` | no  |
| `CrossPointWebServerActivity.cpp:288` | `new DNSServer()` (in `startAccessPoint`) | **yes** — AP/QR path |

None of these are reachable by the activity's `isRunning()` failure check —
they abort before it is evaluated (§5's "log-and-onGoHome" path only covers the
outer object).

### The AP/QR gap is worse than the STA path

The heap-defrag reboot at mode selection is **Join Network only**:
`CrossPointWebServerActivity::onNetworkModeSelected()` (`:149`) gates the call to
`silentRestartToJoinNetwork()` (`main.cpp:233`) on `JOIN_NETWORK`. The
Create Hotspot / QR flow never defrags; it runs `startAccessPoint()` (`:246`)
— including the throwing `new DNSServer()` at `:288` — on the current heap
straight out of the reader, where the e-ink framebuffer (52 KB on X3) + reading
session state leave the heap already fragmented. This is the path the reporter's
feature ("start the device in AP mode with a clear QR code for phone/computer to
send ebooks") actually exercises; Join Network was just his repro vehicle in
#258 because the two share the same `begin()` tail.

### Why the second attempt got further with *less* heap

The first attempt's graceful `onGoHome()` tore the outer web-server object but
left WiFi + lwIP + mDNS resident (STA isn't torn down until `onExit()`,
`:134-137`), so fragmentation deepened for the second attempt. The heap
accounting on X3 (no PSRAM; 146 KB free at boot per the new report) is
structural: WiFi (~45 KB per the `onEnter` comment at `:72`) + lwIP + mDNS +
NTP/tz + SD-font caches already consume the bulk before `begin()`, leaving the
web stack (http server + 30 route lambdas + WebDAV + WebSocket client array)
~40 KB short. This is not recoverable by retry alone.

## Design (Addendum 3) — one funnel, DRY over everything

Per the repo coding philosophy (DRY > SOLID > KISS), the "low heap → silent
restart → graceful fallback" knowledge must live in **one** place, not be
re-pecified per network mode or per checkpoint.

1. **One heap gate, called from every network entry to `begin()`.** Add
   `bool CrossPointWebServerActivity::ensureNetworkStackHeap(NetworkMode mode)`
   — the single copy of: the floor constants, the `ESP.getMaxAllocHeap()/
   getFreeHeap()` measurement, the breadcrumb log, the bounded-retry latch, the
   silent-restart arm, and the graceful-fallback decision. Call sites:
   `onNetworkModeSelected` (both `JOIN_NETWORK` and `CREATE_HOTSPOT`, replacing
   the `:149` mode-specific gate) and `startWebServer()` before `begin()` (the
   shared tail both modes reach — `:296` for AP via `startAccessPoint`, `:230`
   for STA via `onWifiSelectionComplete`).

2. **AP gets the same reboot as Join Network.** Generalize the mode check at
   `:149` to `mode == JOIN_NETWORK || mode == CREATE_HOTSPOT`, add a
   `SILENT_REBOOT_TARGET_HOTSPOT` to the existing RTC target table
   (`main.cpp:149-153`, bumped `TARGET_MAX`), dispatch it to a
   `goToFileTransfer(startInHotspot=true)` in the boot resolver
   (`main.cpp:1179-1188`), and replace the two booleans (`startInJoinNetwork`,
   and a new AP twin) with the boot target the activity is constructed under —
   one source of truth for "which mode to resume in" and "already rebooted once".
   Folding `silentRestartToJoinNetwork()` into `silentRestartTo(SILENT_REBOOT_TARGET_HOTSPOT, "hotspot")`
   also repairs its drift: the inline copy omits the `powerOffInProgress` guard
   that `silentRestartTo` has (`main.cpp:211` vs `main.cpp:234`).

3. **One failure path.** The preflight's graceful fallback reuses the existing
   `LOG_ERR` + `onGoHome()` path (`:320-326`, `:341+`); the throwing `new`s in
   `begin()` (`:176/277/284`) and `new DNSServer()` (`:288`) become
   `makeUniqueNoThrow` null-checks feeding that same path — no second error
   discipline. `begin()` reports failure via `isRunning()`; no-op on touch boards
   (`startInJoinNetwork`'s existing `hasTouch` precedent at `main.cpp:239`).

4. **Bounded retry.** The latch is one bool (`heapDefragRetried`) so a
   structurally-underprovisioned device degrades to Home with breadcrumbs
   instead of boot-looping — the load-bearing complexity the philosophy allows.
   Two named constants, not one (pitfall: equal values ≠ equal jobs): the
   mode-selection floor = `WEB_STACK_FLOOR + WIFI_WORKING_SET_RESERVE`
   (~45 KB on X3, the `onEnter` comment), the tail floor = `WEB_STACK_FLOOR`
   alone (~32-48 KB, mirroring the existing `HttpDownloader::MIN_TLS_FREE_HEAP`
   style at `src/util/HttpDownloader.h:30-35`).

### Decision log

- (DRY) Funnel over per-mode/per-checkpoint guards — the loser (separate STA
  and AP preflights, plus a Join-Network-only reboot) would duplicate the
  threshold, the latch, and the breadcrumbs. (SOLID) the missing
  `powerOffInProgress` guard in `silentRestartToJoinNetwork` is a race this
  unification closes for free.
- (KISS, pitfall exception) Two floor constants kept distinct: `WEB_STACK_FLOOR`
  and `WIFI_WORKING_SET_RESERVE` compute different things; collapsing them to one
  value would silently narrow the mode-selection check.
- (SOLID) Bounded latch over unbounded restart: correctness under structural
  shortage demands a terminal graceful fallback; KISS cannot excuse a boot loop.

### Out of scope (DEFER)

The symmetric `silentRestart()`-in-`onExit()` duplication across the other five
WiFi activities (`ClockSyncActivity.cpp:41`, `OtaUpdateActivity.cpp:124`,
`FontDownloadActivity.cpp:91`, `KOReaderAuthActivity.cpp:76`,
`TtfFontDownloadActivity.cpp:102`, `CalibreConnectActivity.cpp:61`) — different
futures, audited but not consolidated in this change.

## Verification (Addendum 3)

- Host unit test for the new `ensureNetworkStackHeap` verdict function
  (mirrors `tlsheap::verdict` host tests — boundary cases: at-floor, free-heap
  shortfall, fragmentation shortfall, both low, retry-latch exhausted).
- `pio run -e x4pro` + CI C3 build compile. clang-format/cppcheck on touched tree.
- Device check (owner): AP-mode File Transfer on an X3 from a reading session —
  serial shows `WEBACT` heap breadcrumbs + either a successful QR or a single
  visible graceful fallback; must not reboot mid-start. A second AP attempt
  from the same boot must not loop.
- The reporter's v2.12.1 repro re-run after the fix: expected first attempt
  graceful (breadcrumbs), second attempt succeeds on the defragged heap, or
  falls back to Home — never abort.
