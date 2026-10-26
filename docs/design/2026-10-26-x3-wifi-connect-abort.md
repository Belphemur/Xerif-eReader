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
