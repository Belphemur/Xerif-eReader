# TLS teardown between dev-picker list fetch and download — design + probe

Date: 2026-11-10
Branch: `wt/tls-teardown` (off origin/develop 04b39dfb1)
Status: probe round — one of the directive's two premises is contradicted by the
source; the probe measures the rest. No product change ships in this round.

## 0. Verdict up front

The owner directed: tear the TLS stack down after the dev picker's list fetch,
get the ~50 KB internal DRAM back, rebuild on the install press. Reading the
SDK and the picker flow before implementing shows the premise the change is
anchored on does not hold in code:

1. The picker **never holds a TLS context across its own states**. Every HTTPS
   exchange builds a fresh `freeink::SecureHttpClient` and destroys it at scope
   exit (§1). There is no retained object to tear down, so a "release the TLS
   stack after the list fetch" API would be dead code.
2. The asset download **does not reuse** the list fetch's connection — an
   independent client opens a fresh connection and the ~800 ms handshake on the
   install press is already paid today (§2). The directive's alternative
   branch ("new connection anyway → strict win") therefore also does not
   produce a win: teardown saves nothing because there is nothing resident
   between list and install.
3. The measured ~50 KB delta (Home 111,000 free / 69,620 maxBlock → picker
   61,004 / 31,732) **cannot come from TLS at rest** for the reason in (1).
   Candidates are the WiFi/lwIP connect baseline and the documented
   fragmentation attractor (`MaxAlloc=31732` recurs across reading sessions,
   `docs/design/2026-11-02-dram-optimization.md` §8.1). The probe in §5
   decomposes it with named serial lines before any further change is decided.

Per the brief's decision-log contract this round delivers the design + probe
and stops before implementation, because both of the brief's anticipated
design outcomes ("reuses the session" → stop; "new connection anyway" →
implement) rest on a retention that does not exist.

**Owner steer 2026-11-10 (recorded):** the owner reviewed this finding and
confirmed the stop: proceed with the §5 probe only; NO teardown API ships
(it would be dead code per finding 1). Implementing rounds resume only on the
owner's explicit go after the probe numbers land in a serial log.

## 1. Current lifetime of the TLS context (directive item a)

All firmware HTTPS traffic goes through `HttpDownloader`, which builds a
request-scoped client per call. There is no singleton, no member, and no
`static` `SecureHttpClient`/`SecureClient` anywhere in `src/` or `lib/` — every
occurrence is a function-local:

- `src/network/HttpDownloader.cpp:187` (options `fetchUrl`, one fresh client
  per redirect hop), `:235` (`postForm`), `:269` (`postToFile`),
  `downloadToFile` `:325`→`runGetSecure` `:363` (dev-OTA install path).
- `src/network/HttpDownloader.cpp:93` `runGetSecure` →
  `freeink::fetchResumable` builds a **new** `SecureHttpClient http;` per
  attempt/redirect hop (`freeink-sdk/libs/network/SecureNet/include/
  ResumableFetch.h:113`; on a 301/302 the loop re-enters with `--attempt`
  `ResumableFetch.h:184`, so every hop is a fresh object and a fresh
  handshake).

Inside a client, the TLS runtime is allocated at connect and freed at stop:

- `freeink-sdk/.../src/SecureClient.cpp:97` `wolfSSL_CTX_new(...)` per connect,
  `:114` `wolfSSL_new(ctx)` per connect (no session cache reuse, no
  `wolfSSL_set_session` call anywhere; each connect builds both from scratch).
- `freeink-sdk/.../src/SecureClient.cpp:269-278` `SecureClient::stop()`:
  `wolfSSL_free(_ssl)` + `wolfSSL_CTX_free(_ctx)` + `_transport.stop()`.
- The HTTP wrapper's destructor funnels there unconditionally:
  `SecureHttpClient.h:61` `~SecureHttpClient() { end(); }` → `end()` `:124` →
  `closeConnection()` `:486` (`_conn->stop()`).

So after any request returns — including both requests of
`dev_ota::fetchDevManifest` (`src/network/DevOtaClient.cpp:110-141`) — the
app-level TLS footprint is **zero bytes of retention**. The keep-alive reuse
that does exist (`SecureHttpClient.h:11-12`, `_reuse = true` default `:642`)
only reuses a connection for a *later request on the same client object*;
`HttpDownloader` never issues a second request on one object. Protocol-side
note: `HttpDownloader.cpp:113` already logs free heap / max block at request
start (`LOG_DBG`, so invisible at the owner's log level).

## 2. Does the install reuse the list fetch's TLS session? (directive item b)

**No.** `installFromManifestEntry` (`DevOtaClient.cpp:145`) downloads the asset
via `HttpDownloader::downloadToFile` (`DevOtaClient.cpp:177`,
`HttpDownloader.cpp:325`), which is a *different* `SecureHttpClient` object
from the two list-fetch ones — a fresh TCP connection and a fresh handshake are
structural, not a policy the code could toggle. The brief explicitly marked
"asset download reuses the same HTTP connection" as the wrong assumption to
check, and the source confirms: it is not reused.

Furthermore the money question in the directive ("if it reuses, teardown would
ADD one handshake") is moot in both directions:

- Keeping the stack alive across list → install would not need the
  ~800 ms handshake twice under the *current* code either — because the
  current code already re-handshakes on the install press (fresh client,
  fresh CTX, no session resumption). The observed 800/791/657 ms reconnects
  are today's cost, not something a teardown would newly introduce.
- Tearing down between list and install changes nothing: the install press
  opens a new connection regardless (and GitHub releases then redirect
  cross-origin to `objects.githubusercontent.com`, a second fresh hop even
  inside one fetch).

## 3. Where the measured ~50 KB can and cannot come from

Free heap totals only drop for allocations that are genuinely retained; the
picker's TLS stack is not retained (§1). The two identified postulates for the
measured delta:

1. **WiFi/lwIP connect baseline.** The Home measurement is taken with WiFi
   down; the picker measurement with WiFi up (the picker's first state is WiFi
   selection — `DevOtaActivity.cpp:79`, `onWifiSelectionComplete` `:96`).
   Connect cost covers WiFi task/event stacks, RX/TX frame buffers, DHCP, and
   lwIP. This is a *fixed cost of having the radio up*, which the install
   press still needs — it cannot be freed "between list and install" without
   full WiFi teardown/re-connect (§7).
2. **The fragmentation attractor `MaxAlloc=31732`.** The same maxBlock number
   recurs identically in deep reading sessions with the FIBP churn cycle
   (`docs/design/2026-11-02-dram-optimization.md` §8.1: ≤4 KB DRAM blocks
   pinch the freed hole and the heap settles at `MaxAlloc=31732`). The picker's
   claim machinery also runs through that documented pattern.
   `memclaim` lines already in the owner's log (`[DEVOTA] [MEM] claim begin/
   end`, `MemoryClaimGateDevice.h:73/:130`; `[MEM] Stage=` via `logMemAt`,
   `lib/Logging/Logging.h:105`) bracket the fetch and were the surface the
   owner quoted; the probe adds the finer decomposition below.

TLS *transient* peaks (both handshakes' key-share/session/record-buffer
spikes, and the ~17 KB record buffer when a peer ignores `HAVE_MAX_FRAGMENT`)
are real but return at each `stop()` — they never add to the resting delta.

## 4. (c) Boundary proposal — and its honest null result

Under the current source there is **no boundary to draw**: the "teardown after
list, rebuild on demand" ownership question has no subject. The picker flow
around the boundary is exactly:

```
WiFi select → onWifiSelectionComplete → state=CHECKING
            → fetchManifest()          — [MEM] claim begin/runCheap → 2 fetches (2-4 hops + handshakes) → all released
            → state=LIST               — resting state: zero TLS held, WiFi held
   (user picks) → promptInstall → beginInstall
            → runInstall()             — [MEM] claim begin/runCheap → downloadToFile (fresh handshake)
```

The only lever that actually returns resting bytes between LIST and the press
is WiFi itself (§7/C1), which the install needs within seconds — its re-cost
(pure re-association + DHCP, no TLS) is 2-4 s, i.e. worse than the handshake it
would "save nothing" against since the handshake happens regardless.

## 5. Probe (deliverable; measurement only, no product port)

The probe adds `[TLS-PROBE]`-tagged `LOG_INF` lines so the owner's next serial
capture answers, per line, exactly one question. It is branch-local and is
meant to be dropped (or reduced to `LOG_DBG`) before any product merge.

| # | Log line | Answers |
| --- | --- | --- |
| P1 | `[DEVOTA] [TLS-PROBE] wifi-up free=%u maxBlock=%u` (as soon as WiFi selection reports success, before the claim) | How much of the Home→picker gap is the WiFi/lwIP connect baseline alone |
| P2 | `[HTTP] [TLS-PROBE] hop %d connect %ums free %u->%u maxBlock %u->%u url %s` (one per TCP/TLS hop, `HttpDownloader.cpp` `runGetSecure`) | Handshake cost per hop (the owner's 800/791/657 ms numbers, tied to request order), and the transient heap floor the handshake needs (before→after) |
| P3 | `[DEVOTA] [TLS-PROBE] release-json %ums free %u->%u maxBlock %u->%u amt=%zu` and `manifest …` (per `fetchUrl` inside `fetchDevManifest`) | Whether anything survives *within* the list-taking fetch sequence (free before→after each call), plus per-call elapsed |
| P4 | `[DEVOTA] [TLS-PROBE] asset-download %ums free %u->%u maxBlock %u->%u` (inside `installFromManifestEntry` around `downloadToFile`) | Same for the install press |
| P5 | `[DEVOTA] [TLS-PROBE] list-enter free=%u maxBlock=%u` and one `"list-settle+3s"` line 3 s later (in `DevOtaActivity`) | The resting LIST state twice: what (if anything) decays after the last TCP close (PCBs, TIME_WAIT, deferred lwIP free) |
| P6 | `[MEM] Stage=home` / `Stage=devota_enter` / `Stage=devota_list` / `Stage=devota_settle` (the existing `logMemAt` format, new call sites in `HomeActivity::onEnter`, `DevOtaActivity::onEnter`/list-enter/settle; `Stage=boot` already exists at `src/main.cpp:948`) | The owner's per-phase allocation table — boot / Home / picker enter / list fetched / settled after — in one uniform format (free+maxBlock+PSRAM+core), directly comparable to `Stage=book_open` |

Signal to read for (asserted by §1, falsifiable by the probe): P1 should show a
~25-45 KB free drop vs Home (WiFi baseline), P3 should show free **return to
the P1 figure** at each fetch end minus DRAM churn noise, and P5's two samples
should agree within noise. If free at `list-enter` does NOT return to the P1
level, then a genuine retainer exists that contradicts §1 — the probe pinpoints
which call boundary leaks it, and the teardown design reopens with a real
object to release.

Compile/behavior discipline: P2 is implemented by calling
`SecureHttpClient::connect()` (public, `SecureHttpClient.h:129`) inside the
`configure` callback that `fetchResumable` already invokes per hop on a fresh
client, passing the same cancel-flag abort callback `runGetSecure` already
hands to `fetchResumable`. `ensureConnected` then skips re-connect on the GET,
so the request path keeps exactly today's retry/abort behavior — pure
measurement.

## 6. Decision record (directive item e)

- **Winner: evidence-first KISS.** Do not implement a "TLS teardown" API: the
  source shows no retained TLS object to release, so the API could not be
  verified non-vacuously (any test asserting "the stack is gone after the list
  fetch" would pass equally with the API deleted — the exact vacuity standard
  this repo rejects). Shipping it would cost a permanent owner-owned API field,
  call sites in three activities, and a false "we returned 50 KB" story while
  the actual delta comes from the WiFi baseline that cannot go away.
- **Loser: implement-the-directive-as-premised.** A `SecureClient`/`SecureHttp
  Client teardown hook exercised from `DevOtaActivity` after the list fetch:
  the hook would be a no-op by construction (§1), the "rebuild" path already
  exists as every call building and destroying its own stack, and the change
  would carry review/maintenance weight forever for zero observable effect.
  Owner-signed 2026-11-10: this loser stays the loser — no impl rounds without
  the owner's explicit go after the probe numbers land.
- The DRY angle the owner asked to be weighed explicitly: the *policy* ("one
  funnel builds and releases TLS per request") already exists as the single
  implementation site (`HttpDownloader`), so "teardown on demand" duplicates an
  invariant the code already guarantees — DRY argues against even a cheap
  duplicate.

## 7. If resting bytes must come back from the picker anyway

Options for the owner, none of which is "TLS teardown" (it has no object):

- **C1 — WiFi teardown after list, re-connect on install press.** Frees the
  real resting cost (the P1 figure). Cost: full re-association + DHCP, ~2-4 s,
  plus a THIRD transition (scan/select is already two). Materially worse UX
  than today's ~800 ms handshake, and the handshake cost happens either way.
- **C2 — Reduce the WiFi baseline itself** (esp_wifi buffer tuning) so all
  WiFi activities (font download, KOReader auth, web server) get the same
  bytes back — a device-level knob, not a picker-local one.
- **C3 — Accept.** The install press is not heap-critical:
  `HttpDownloader::heapAvailableForTransfer` (`HttpDownloader.cpp:393`) gates
  at 40,000 free / 20,000 maxBlock, satisfied at `61,004/31,732` for both the
  download and the flash run.

## 8. Graduation (directive item f)

Nothing graduates. `ClockSync`/font download/KOReader auth already share the
same per-request property (they all go through `HttpDownloader` or
`pluginhttp`'s optional caller-held session, `src/util/PluginHttp.h` bounded
transport section — that one is the legitimately session-holding pattern and
is opt-in per caller). A picker-first proof would have been the gate for a
cross-activity reach; the picker shows no retained object, so the reach has
nothing to reach with.

## 9. This round's deliverables

1. This document (design + probe spec + decision record).
2. The probe (`[TLS-PROBE]`) in `HttpDownloader.cpp`, `DevOtaClient.cpp`,
   `DevOtaActivity.cpp` (+ the P6 `Stage=` call sites in
   `HomeActivity`/`DevOtaActivity`) — compile-verified under both x4pro device
   envs and the host suite; never flashed by the agent. Lines the owner runs
   on-device: boot → `Stage=boot`, Home → `Stage=home`, picker open →
   `Stage=devota_enter` + `wifi-up`, list fetched → hop/connect lines +
   `release-json`/`manifest` + `list-enter` + `Stage=devota_list`, 3 s later
   → `list-settle+3s` + `Stage=devota_settle`, install press →
   `asset-download`. No teardown step exists (§4): the "after-teardown" table
   row is covered by the settle samples.
3. No API, no activity change, no submodule bump.

Gates on the branch tip before push: `pio run -e x4pro` and
`pio run -e x4pro-gh_release`, full host ctest, `clang-format-21 --dry-run
-Werror` on touched files, and `pio check -e cppcheck --fail-on-defect
low/medium/high` after `mkdir -p .cppcheck-build` (0 defects required).
