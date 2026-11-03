# Design: DRAM reduction analysis — X3-class first, trickle to X4 Pro

Date: 2026-11-02
Author: Antoine Aflalo (Belphemur) via analysis agent
Status: implemented in PR #279 — Phase 0 (candidates 1–3) + Phase 1 (candidate 5) + loop HWM
instrumentation; Phase 2 constant is measurement-only. See §7 addendum.
Candidate 8 (AudioManager) was approved but is a no-op — see §7.5.
Related: PR #275 (NimBLE X3 trim), PR #278 (sdk digest), upstream e8299b5f (web-server memory fix)

## Executive summary

The Xteink X3 (ESP32-C3, ~380 KB usable SRAM, no PSRAM) is already at the
edge after PR #275 trimmed NimBLE.  The goal of this analysis is to find
**additional DRAM headroom** without simply pushing cost onto the SD card.
The X4 Pro (ESP32-S3, ~512 KB DRAM + 8 MB PSRAM) gets most savings
"for free" through shared code, and has two X4-Pro-only levers because it
has PSRAM.

**Key measured baselines**

| Metric | `default` (C3/X3) | `x4pro` (S3/X4 Pro) |
|---|---|---|
| `.dram0.data` | 21,613 B | 31,914 B |
| `.dram0.bss` | 77,784 B | 115,600 B |
| DRAM static total | **99,397 B** | **147,514 B** |
| `.iram0.text` (shares SRAM on C3) | 69,632 B | 108,288 B |
| SRAM static total (C3 shared pool) | **169,029 B** | n/a (IRAM separate on S3) |

The X3 has ~169 KB of static SRAM before `malloc()` is called.  After the
~52 KB framebuffer and ~49 KB loop-task stack are allocated at boot, only
~160 KB remains for the heap, FreeRTOS tasks, and runtime pools.  The BLE
gate (`kStartFreeBytes = 76,276` after #275) already consumes most of that.

**Highest-impact candidates**
1. **Move InflateReader ring to PSRAM on X4 Pro** — 32,768 B, one line change.
2. **Enable `FREEINK_FB_PSRAM` for X4 Pro** — 48,000 B framebuffer to PSRAM.
3. **Right-size loop-task stack on X3** — potentially 10–20 KB, but requires
device HWM measurement (high risk).
4. **FIBP LRU PSRAM cache on X4 Pro** — owner-brainstormed idea; trades PSRAM
for SD wear.  X3 equivalent: not viable.

---

## 1. Measured baseline per environment

### 1.1 Environment mapping

| env | MCU | PSRAM | USB-MSC | reader path | notes |
|---|---|---|---|---|---|
| `default` | ESP32-C3 | none | no | bitmap (`EpdBook`) | baseline X3/X4 |
| `slim` | ESP32-C3 | none | no | bitmap | serial disabled |
| `x4pro` | ESP32-S3 | 8 MB octal | yes | native TTF (`TtfBook`) | prebuilt core |
| `x4pro_profile` | ESP32-S3 | 8 MB | yes | native TTF | + `BOOK_PROFILE` |
| `sticky` | ESP32-S3R8 | off | yes | bitmap | SMALL profile |

All C3 envs (`default`, `slim`, `gh_release`) extend `firmware_tuned_c3` and
carry the post-#275 NimBLE pool trims.

### 1.2 Static section sizes (from ELF)

Tooling: `riscv32-esp-elf-size -A` (C3) and `xtensa-esp32s3-elf-size -A` (S3)
on the firmware ELF produced by `pio run -e <env>`.

**`default` (C3/X3)**

| section | size | addr | note |
|---|---|---|---|
| `.dram0.data` | 21,613 B | 0x3fc9… | initialised globals |
| `.dram0.bss` | 77,784 B | 0x3fca… | zero-initialised globals |
| `.noinit` | 3 B | — | uninitialised |
| `.iram0.text` | 69,632 B | 0x4038… | instruction RAM (shares SRAM on C3) |
| `.flash.text` | 3,128,054 B | — | flash, not RAM |

**`x4pro` (S3/X4 Pro)**

| section | size | note |
|---|---|---|
| `.dram0.data` | 31,914 B | +10,301 B over C3 (USB-MSC structs, FT data) |
| `.dram0.bss` | 115,600 B | +37,816 B over C3 (TinyUSB buffers, coredump stack) |
| `.noinit` | 2 B | — |
| `.iram0.text` | 108,288 B | separate IRAM pool on S3, does not eat heap |
| `.flash.text` | 3,028,968 B | — |

The x4pro `.bss` delta of ~38 KB is almost entirely USB-MSC / TinyUSB:

| symbol | size | source |
|---|---|---|
| `ncm_epbuf` | 6,416 B | `lib/tinyusb` network ECM buffer |
| `_transfer_buf` | 4,096 B | TinyUSB bulk transfer scratch |
| `_mscd_epbuf` | 4,096 B | MSC endpoint buffer |
| `ep_out_sw_buf` | 3,104 B | TinyUSB software OUT buffer |
| `ep_in_sw_buf` | 3,104 B | TinyUSB software IN buffer |
| `_hidh_epbuf` | 2,688 B | HID host endpoint buffer |
| `s_coredump_stack` | 1,892 B | ESP-IDF coredump task stack |
| `_usbh_devices` | 837 B | USB host device table |
| `_hcd_data` | 544 B | host controller driver data |

These are bound to `FREEINK_CAP_USB_MSC=1` (x4pro env only).  Firmware-side
options to shrink them are limited — most sizes are hard-coded in the
TinyUSB/Arduino stack or ESP-IDF prebuilt libraries.

### 1.3 Top static DRAM contributors (`default` env, `.bss` only)

Extracted via `nm -S --size-sort -r firmware.elf | grep ' [bB] '`.

| symbol (demangled) | size | file / origin |
|---|---|---|
| `freeink::book::fontLoader` | 30,384 B | `src/BookFontLoader.h` — global `BookFontLoader` object |
| `g_cnxMgr` | 3,880 B | ESP-IDF `libnet80211.a` — WiFi connection manager |
| `freeink::BleKeyboardHost::instance` | 2,152 B | `BleKeyboardHost.cpp` — BLE singleton |
| `fontCacheManager` | 2,120 B | `lib/GfxRenderer/FontCacheManager.h` — scan codepoint table (512 × 4 B) + metadata |
| `xIsrStack` | 2,096 B | ESP-IDF FreeRTOs — interrupt stack |
| `packet.6` | 1,460 B | `libmDNS` send buffer |
| `s_wifi_nvs` | 1,308 B | ESP-IDF WiFi NVS cache |
| `SDCardManager::instance` | 1,200 B | `SDCardManager` singleton |
| `gWpaSm` | 1,052 B | `libwpa_supplicant.a` |
| `TxRxCxt` | 960 B | `libpp.a` — packet processor context |
| `ble_att_svr_prep_entry_mem` | 768 B | NimBLE — ATT prepare-write entry pool |
| `freeink::sZoneCache` | 760 B | `AceTime` zone cache |
| `FinishReadingPromptActivity::props` | 720 B | `EpubReaderActivity.cpp` — static prompt-screen props |
| `g_ic` | 696 B | ESP-IDF `ieee80211.o` |
| `gChmCxt` | 592 B | ESP-IDF `wl_chm.o` |
| `g_pm` | 544 B | ESP-IDF `pm.o` |
| `bus` | 532 B | Arduino I2C HAL |
| `gpio` | 520 B | `hal/HalGPIO.cpp` |
| `pins` | 352 B | Arduino periman |
| `__sf` | 312 B | newlib file stream table |

### 1.4 Task stacks (dynamic, allocated from heap at runtime)

| task | stack size | where created | X3-class | x4pro |
|---|---|---|---|---|
| Main loop (`loopTask`) | 49,152 B | `SET_LOOP_TASK_STACK_SIZE(49152)` in `main.cpp:881` | **DRAM** | **DRAM** |
| FIBP prefetch worker | 32,768 B | `FibpPrefetchWorker.cpp:356` (xTaskCreate) | N/A (no TTF) | DRAM |
| Hint probe task | ~57,344 B | `BookFontLoader.cpp:350` (xTaskCreatePinnedToCore) | DRAM | DRAM |
| ActivityManager render | 12,288 B | `ActivityManager.cpp` (render task) | DRAM | DRAM |
| ProgressManager worker | 6,144 B | `ProgressManager.cpp:88` | DRAM | DRAM |
| Audio manager | 8,192 B | `AudioManager.cpp:360` | DRAM* | DRAM* |
| InputManager async poll | 4,096 B | `InputManager.cpp:336` | DRAM | DRAM |
| BLE host task | 3,072 B | sdkconfig `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` | static .bss | static .bss |
| BLE start task | 3,072 B | `SdkRadio.cpp:100` | DRAM | DRAM |
| BLE conn task | 4,096 B | `BleKeyboardHost.cpp:615` | DRAM | DRAM |

*Audio manager is compiled in but never instantiated on X3/X4; its stack is
never allocated in practice.

**Conclusion from baseline:** the X3-class boot image already commits ~169 KB
static + ~49 KB loop stack + ~52 KB framebuffer = **~270 KB** before any book
is opened.  With ~380 KB total SRAM, that leaves ~110 KB heap.  The BLE gate
needs 76 KB free + 32 KB largest block = 108 KB.  Every additional runtime
allocation (inflate ring 32 KB, FIBP worker 32 KB, hint probe 57 KB) must be
sequenced carefully so they do not coexist at peak.

---

## 2. Runtime DRAM inventory

The map file only catches static allocations.  The following are **dynamically**
allocated and therefore invisible in `.bss`/`.data`, yet they compete for the
same DRAM on X3-class boards.

### 2.1 Framebuffer

- **X3**: 52,272 B (`792 × 528 ÷ 8`) — allocated in `FreeInkDisplay::begin()`
  via `allocFrameBufferStorage()` which calls `malloc()` because
  `FREEINK_FB_PSRAM` is off for X3-class boards.  Lines:
  `freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp:443-454`.
- **x4pro**: 48,000 B (`800 × 480 ÷ 8`) — **also allocated via `malloc()`**
  because `FREEINK_FB_PSRAM` is only auto-enabled for M5Paper and PaperMono
  (`BoardConfig.h:398`).  Confirmed by nm: no framebuffer symbol in `.bss`;
  it is heap-allocated at runtime.

### 2.2 Inflate / decompression rings

| consumer | size | allocator | path |
|---|---|---|---|
| `InflateReader` streaming ring | 32,768 B | `malloc()` | `lib/InflateReader/InflateReader.cpp:20` |
| `InflateStream` tinfl window | 32,768 B | `malloc()` | `lib/inflate/src/InflateStream.cpp:56` |
| `ZipFileCache` (content cache) | `sizeof(ZipFileCache)` | `heap_caps_malloc(MALLOC_CAP_SPIRAM)` on x4pro, `malloc()` on X3 | `lib/Epub/Epub.cpp:539` |
| `BuildContext` (chapter layout) | `sizeof(BuildContext)` | `heap_caps_malloc(MALLOC_CAP_SPIRAM)` on x4pro, `malloc()` on X3 | `lib/Epub/Epub/blocks/Section.cpp:410` |

**Note:** the InflateReader ring is the one consumer that is **not** routed
to PSRAM on x4pro.  Both InflateStream and the EPUB pipeline already use
`heap_caps_malloc(MALLOC_CAP_SPIRAM)` when `BOARD_HAS_PSRAM` is defined.
InflateReader uses plain `malloc()` unconditionally.

### 2.3 Font caches and arenas (runtime, not static)

All of the following go through `poolMakeBytes` / `PoolBytes`:

| pool | size per instance | when allocated | X3 (C3) | x4pro (S3) |
|---|---|---|---|---|
| Glyph arena (`kGlyphArenaBytes`) | 12/32/48 KB per face | face load | DRAM | **PSRAM** |
| Font file bytes (`fontPsramBytes_`) | up to 2 MB per face | face load | DRAM | **PSRAM** |
| Stream prefix (`kStreamPrefixBytes`) | 1 MB | streamed face load | DRAM | **PSRAM** |
| TtfBookRuntime scratch (`kScratchBytes`) | 256 KB | reader open | N/A (no TTF) | **PSRAM** |
| TtfBookRuntime book arena | 128 KB | reader open | N/A | **PSRAM** |
| TtfBookRuntime parse arena | 64 KB | reader open | N/A | **PSRAM** |
| TtfBookRuntime prescan | 64 KB | reader open | N/A | **PSRAM** |
| TtfBookRuntime prefetch | 48 KB | reader open | N/A | **PSRAM** |
| TtfBookRuntime cache arena | 48 KB | reader open | N/A | **PSRAM** |
| TextSettingsPreview scratch | 256 KB | preview relayout | N/A | **PSRAM** |
| EpdBookFont coverage scratch | on demand (`rasterize()`) | glyph raster | DRAM | **PSRAM** |

On X3-class, the bitmap reader (`EpdBook`) loads **up to 4 faces × 12 KB**
= 48 KB glyph arenas into DRAM when a book is open.  The font file bytes
(embedded or SD-loaded book fonts) are also in DRAM.

### 2.4 Web server / network buffers (post-e8299b5f)

Upstream commit e8299b5f (merged in this branch) already refactored plugin
loading to use chunked streaming instead of buffering the whole plugin JSON
in memory.  Remaining runtime buffers:

| buffer | size | where |
|---|---|---|
| Static GET/PUT chunk | 4,096 B | `CrossPointWebServer.cpp:842` (`makeUniqueNoThrow<uint8_t[]>`) |
| Upload state buffer | 4,096 B | `CrossPointWebServer.h:48` |
| Font upload buffer | 4,096 B | `CrossPointWebServer.h:145` |
| WebSocket message (max) | 15,360 B | `WebSockets.h:66` (`WEBSOCKETS_MAX_DATA_SIZE`) — per-message malloc |
| WolfSSL RX record (TLS) | 2,048 B | negotiated via `HAVE_MAX_FRAGMENT_LENGTH`; per-session heap |
| WolfSSL handshake temporaries | ~4–8 KB | `WOLFSSL_SMALL_STACK` routes large bignums to heap |

The web-server buffers are modest after e8299b5f; the 15 KB WebSocket ceiling
is the largest single transient allocation.

### 2.5 SDK-side pools (NimBLE, post-#275)

Verified from generated `sdkconfig.h` + `nimconfig.h`:

| pool | count | block | total | note |
|---|---|---|---|---|
| MSYS_1 | 4 | 256 B | 1,024 B | `os_msys_init.c` |
| MSYS_2 | 4 | 320 B | 1,280 B | `os_msys_init.c` |
| ACL from LL | 4 | 300 B | 1,200 B | `transport.c` |
| EVT (non-discardable) | 8 | 72 B | 576 B | `transport.c` |
| EVT (discardable) | 4 | 72 B | 288 B | `transport.c` |
| **Total** | — | — | **4,368 B** | — |

These are allocated inside `NimBLEDevice::init()` (`Runtime.cpp` probe window)
and stay resident while BLE is enabled.

---

## 3. Candidate table

For each candidate: file:line evidence, estimated saving, X4 Pro effect, SD
cost, risk, reversibility.  Ranked by (saving ÷ risk) subject to the SD-wear
constraint.

| # | candidate | file:line | X3 saving | x4pro saving | SD cost | risk | reversibility |
|---|---|---|---|---|---|---|---|
| 1 | **Route InflateReader ring to PSRAM** | `lib/InflateReader/InflateReader.cpp:20` | 0 (no PSRAM) | **32,768 B** | 0 | Low: fallback to DRAM on PSRAM OOM keeps safety | Revert 1 line |
| 2 | **Enable `FREEINK_FB_PSRAM` for x4pro** | `platformio.ini:521` (add `-DFREEINK_FB_PSRAM=1`) or `BoardConfig.h:398` | 0 | **48,000 B** | 0 | Med: PSRAM cache-miss latency during panel refresh; needs device soak | Revert build flag |
| 3 | **Shrink TtfBookRuntime `kScratchBytes`** | `src/activities/reader/TtfBookRuntime.h:48` | N/A (TTF not compiled) | **~64 KB** (256→192 KB) | 0 | Med: engine peaks ~152 KB; 192 KB gives 26 % headroom but needs host test coverage | Revert constant |
| 4 | **Right-size main loop stack on X3** | `src/main.cpp:881` | **~10–20 KB** (needs device HWM) | 0 (S3 has DRAM margin) | 0 | **High**: stack overflow = instant panic; must measure HWM on every path first | Revert constant |
| 5 | **Shrink `FontCacheManager` scan table** | `lib/GfxRenderer/FontCacheManager.h:65` | **~1.5 KB** (MAX_SCAN_CODEPOINTS 512→128) | ~1.5 KB | 0 | Low: rare pages with >128 distinct codepoints degrade to cache miss (single extra pass) | Revert constant |
| 6 | **Compress sleep-frame with RLE before SD write** | `src/main.cpp` / `SleepFrameHash.h` | **~26 KB** (52 KB → ~8 KB typical) SD write reduction | same SD reduction | ** writes still go to SD, just fewer bytes** | Low-Med: RLE encode/decode adds ~1 ms and a small stack buffer; no user-visible change | Revert encode/decode calls |
| 7 | **FIBP page cache in PSRAM (LRU), flush ≤5 % battery** | New module | N/A (no PSRAM) | **~200–500 KB** PSRAM used, **~50–200 KB/chapter SD writes saved** | **Reduced wear**: fewer FIBP file writes (quantified per chapter) | Med: power-loss window loses unflushed FIBP cache; resume rebuilds. Needs TTL / dirty tracking. | Remove module, restore direct SD writer |
| 8 | **Remove AudioManager from C3 envs** | `platformio.ini:295` (strip from `lib_deps` in `[base]` or C3-only override) | **~8 KB** stack + ~2 KB static BSS never allocated | ~8 KB | 0 | Low: no X3/X4 hardware has audio output; `AudioManager` is dead code on these boards | Re-add lib_deps |

### 3.1 Savings explanation

**Candidate 1 — InflateReader to PSRAM**
`InflateReader::init(true)` calls `malloc(INFLATE_DICT_SIZE)` where
`INFLATE_DICT_SIZE = RING_BYTES = 32768`
(`lib/InflateReader/InflateReader.cpp:20`,
`lib/InflateReader/InflateReader.h:24`).
On x4pro this ring lives in DRAM even though every other inflate path
(`InflateStream`, `ZipFileCache`, `BuildContext`) is already PSRAM-first.
Replacing `malloc` with `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` and
falling back to `malloc` on failure is the exact same pattern used in
`FreeInkDisplay::allocFrameBufferStorage()`.

**Candidate 2 — Framebuffer to PSRAM**
`freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp:443-454`
already contains the `#if FREEINK_FB_PSRAM` branch.  On x4pro the flag is
off, so `allocFrameBufferStorage()` falls through to `malloc()` and the
48 KB framebuffer sits in DRAM.  Enabling the flag routes it to
`heap_caps_malloc(MALLOC_CAP_SPIRAM)`.  Risk is display-refresh latency:
the panel controller does a large linear read of the framebuffer during
`displayBuffer()`.  PSRAM on ESP32-S3 is cacheable (32 KB cache line), so
linear reads hit cache well, but the first uncached burst is slower than
DRAM.  A device soak with the e-ink refresh scope is required to confirm no
visible lag.

**Candidate 3 — TtfBookRuntime scratch downsize**
`kScratchBytes = 256 * 1024` (`TtfBookRuntime.h:48`).  The design comment says
"STANDARD profile peaks ~152 KB".  The 256 KB value is a 68 % over-margin.
Dropping it to 192 KB (25 % margin over peak) would save 64 KB PSRAM on x4pro.
Because this is only compiled for `CROSSPOINT_TTF_READER=1` envs, X3-class is
unaffected.  Host tests already exercise `TtfBookRuntime` with the current
size; they must be re-run after the change.

**Candidate 4 — Loop stack right-sizing**
`SET_LOOP_TASK_STACK_SIZE(49152)` is the single largest task stack.
Historical context (`BookFontLoader.cpp:95` comment) says it was raised from
8 KB because the EPUB parser runs on the loop task and overflows it.
If the current worst-case HWM is, say, 38 KB, dropping to 40 KB or 44 KB
recovers 5–9 KB on X3.  **This must be measured** — not estimated — because
a stack overflow on the loop task is a non-recoverable panic.  The measurement
requires `uxTaskGetStackHighWaterMark(nullptr)` inserted at the deepest call
sites (book open, font preview, web server startup, BLE pairing) and a full
device exercise.

**Candidate 5 — FontCacheManager scan table**
`scanCodepoints_` is `uint32_t[512+1]` = 2,052 B, the dominant part of the
2,120 B `fontCacheManager` object.  Reducing `MAX_SCAN_CODEPOINTS` from 512
to 128 shrinks it to ~516 B, saving ~1.5 KB static DRAM on **both** X3 and
x4pro.  The scan pass collects unique codepoints touched during a render.
Pages with >128 distinct glyphs are rare but possible in multi-language text.
The degradation is a cache miss on the excess codepoints, causing a single
extra prewarm pass on the next page — negligible latency.

**Candidate 6 — Sleep-frame RLE**
The sleep frame is a full framebuffer (52 KB X3 / 48 KB x4pro) written to
`/.crosspoint/sleep_frame.bin` on every sleep transition.  It already carries
a 4-byte Adler-32 trailer (PR #275).  E-ink framebuffers are typically
<< 50 % entropy because of large white margins and text regions.  A simple
run-length encoder (RLE for 0x00 and 0xFF runs) can compress typical frames
to ~15–25 % of raw size, cutting the SD write from 52 KB to ~8–13 KB per
sleep.  This reduces SD wear without any DRAM change.  Decoding at resume is
a single streaming pass (~1 ms).  Because the file format already has a
trailer, a version bump in the first byte can distinguish compressed vs
legacy uncompressed frames.

**Candidate 7 — FIBP PSRAM LRU cache**
Owner-brainstormed idea: hold the FIBP (page layout cache) in PSRAM instead of
flushing every page to SD as a separate file in `<cache>/ficache/`.  On x4pro,
PSRAM is 8 MB while a full-book FIBP cache is typically 200–500 KB.  An LRU
keeps hot chapters in PSRAM and writes evicted pages to SD only when the
cache is full or when battery ≤ 5 %.  This saves SD wear (fewer erase cycles)
at the cost of PSRAM occupancy.  On **X3** there is no PSRAM; the equivalent
would be to hold the cache in DRAM, but that consumes 200–500 KB of the
already-tight 380 KB pool, which is **not viable**.  X3 answer: keep on SD,
but candidate 6 (sleep-frame RLE) can free up enough DRAM breathing room to
reduce the pressure that made this idea attractive.

**Candidate 8 — Remove AudioManager from C3 envs**
`AudioManager=symlink://freeink-sdk/libs/hardware/AudioManager` is listed in
`[base].lib_deps` (platformio.ini:295).  It compiles into every env, yet no
X3/X4 board has audio DAC or speaker hardware.  The `AudioManager::begin()`
and task are never called on these boards, so the runtime stack is never
allocated, but the code and static data (BSS + text) are still linked.
Removing it from C3 envs via a per-env `lib_ignore` or by moving the symlink
to a PSRAM-capable section saves a small amount of flash and eliminates the
8 KB stack potential, plus reduces build time.  Risk is zero if the library
is truly unused.

---

## 4. Phased plan

### Phase 0 — X4-Pro-only PSRAM moves (free wins, no SD cost)

1. **InflateReader ring → PSRAM** (candidate 1)
   - One-line change in `InflateReader.cpp:20`.
   - x4pro gets 32 KB DRAM back; X3 unchanged.
   - No re-pin needed because this is a runtime path, not a gate constant.

2. **Enable FREEINK_FB_PSRAM for x4pro** (candidate 2)
   - Add `-DFREEINK_FB_PSRAM=1` to `env:x4pro` build flags, or patch
     `BoardConfig.h:398` to include `FREEINK_DEVICE_X4PRO`.
   - x4pro gets 48 KB DRAM back; X3 unchanged.
   - **Device soak required**: verify e-ink refresh timing is unchanged.

3. **TtfBookRuntime scratch downsize** (candidate 3)
   - Change `kScratchBytes` 256→192 KB.
   - Host tests (`ctest`) and x4pro device soak with large-chapter book.

### Phase 1 — Shared-code shrink (trickles to both X3 and x4pro)

4. **FontCacheManager scan table** (candidate 5)
   - `MAX_SCAN_CODEPOINTS` 512→128.
   - Saves ~1.5 KB static DRAM on **both** envs.
   - Low risk; host render tests confirm no regression.

5. **AudioManager removal from C3** (candidate 8)
   - `lib_ignore = AudioManager` in `[firmware_tuned_c3]` or per C3 env.
   - Saves flash + removes dead stack potential.

### Phase 2 — X3-only high-risk, high-reward

6. **Loop stack right-size** (candidate 4)
   - Requires a separate *measurement* task first: instrument
     `uxTaskGetStackHighWaterMark` on the loop task after each deep call
     (book open, preview, font settings, web server start, BLE pairing).
   - Only after a stable HWM is known can the constant be lowered.
   - **Do not implement in the same PR as Phase 0/1.**

### Phase 3 — SD-wear tradeoffs (owner sign-off required)

7. **Sleep-frame RLE** (candidate 6)
   - Adds encode/decode to sleep/resume path.
   - Reduces SD write per sleep by ~30–40 KB.
   - No DRAM change; purely a wear-reduction feature.
   - Needs a format-version bump and backward-compat loader (legacy
     uncompressed frames must still load).

8. **FIBP PSRAM LRU cache** (candidate 7)
   - **x4pro only.**  X3 does not have the RAM.
   - Requires new cache module, dirty tracking, battery-fail flush, and
     eviction policy.
   - The SD-wear saving is significant (hundreds of KB per chapter), but the
     complexity is high.

---

## 5. Verification plan

### Build gates (implementer's responsibility)

| step | command | pass criteria |
|---|---|---|
| 1 | `pio run -e default` | SUCCESS (C3 baseline) |
| 2 | `pio run -e x4pro` | SUCCESS (S3 PSRAM baseline) |
| 3 | `pio run -e slim` | SUCCESS (C3 minimal) |
| 4 | `pio run -e x4pro_profile` | SUCCESS (S3 TTF profile) |
| 5 | host `ctest` | all green (no new failures) |
| 6 | `pio check -e cppcheck` | no new defects |
| 7 | `bin/clang-format-fix` + `git diff --exit-code` | format clean |

### Device soak (owner's responsibility)

| env | soak item | pass criteria |
|---|---|---|
| x4pro | Phase 0 framebuffer in PSRAM | Open book → page turns → sleep → wake. No visible refresh lag. Serial log shows no `heap_caps_malloc` DRAM fallback for framebuffer. |
| x4pro | Phase 0 InflateReader in PSRAM | Open large EPUB (>10 MB). `InflateReader` ring allocation succeeds. No OOM in serial log. |
| x4pro | Phase 0 (candidate 3) TTF scratch 192 KB | Open known "large chapter" book. Host tests pass; device shows no `Arena::alloc` failure in log. |
| default | Phase 1 (candidate 5) FontCacheManager 128 | Render mixed-script page (e.g. Russian + English). Page renders correctly; no infinite prewarm loop. |
| default | Phase 2 (candidate 4, measurement-only) loop stack | Instrumented build logs HWM after every heavy path. Stable HWM < new stack size − 2 KB margin. |
| default | Phase 3 (candidate 6) sleep-frame RLE | Sleep 10×. `sleep_frame.bin` size < 20 KB each time. Resume shows correct frame. Legacy uncompressed frame loads after OTA rollback. **Excluded from PR #279 — owner-gated follow-up (§6).** |
| default | BLE gate (regression check) | Settings → Bluetooth. `free=` ≥ `kStartFreeBytes` (76,276). Pair many-service remote. Page-turn. No reboot loop on second toggle. |

---

## 6. Decided-by-owner list

The following items **must not be implemented without explicit owner sign-off**:

1. **Candidate 2 (framebuffer in PSRAM)** — owner must flash-test x4pro and
   confirm e-ink refresh timing is acceptable.  If panel controller DMA reads
   stutter, this is rejected.
2. **Candidate 4 (loop stack right-sizing)** — requires a separate
   measurement-only build first.  Do not lower the constant until HWM data is
   collected on a real X3.
3. **Candidate 6 (sleep-frame RLE)** — changes the on-SD file format.
   Owner must accept the backward-compat loader and the risk that a power-off
   mid-write could corrupt the frame (same risk as today, but a compressed
   frame is harder to manually recover).
4. **Candidate 7 (FIBP PSRAM cache)** — significantly complex new module.
   Owner must approve the design, the dirty-flush policy, and the acceptance
   that power-loss before flush loses the cached layouts (rebuild on next open).
5. **Any proposal that increases SD usage** — the balance constraint says
   moving cost to SD is the last resort.  If a candidate requires more SD
   writes, it must quantify the extra bytes per action and the owner must
   sign off on the trade.
6. **No change to PR #275 BLE constants** — `kStackBytes`/`kStartFreeBytes`
   must only be re-pinned with the same measured discipline documented in
   `2026-10-30-ble-nimble-trim-for-x3.md`.

---

## 7. Implementation addendum (2026-11-02)

### 7.1 Owner scope decisions applied

- Approved: Phase 0 (candidates 1–3) + Phase 1 (candidates 5, 8) + loop HWM
  instrumentation (measurement only).
- Excluded from this PR: sleep-frame RLE (candidate 6), FIBP PSRAM LRU cache
  (candidate 7), loop-stack constant change (candidate 4 measurement-only).

### 7.2 Measured after-values vs baseline

| Metric | `default` before | `default` after | delta | `x4pro` before | `x4pro` after | delta |
|---|---|---|---|---|---|---|
| `.dram0.data` | 21,613 B | 21,613 B | 0 B | 31,914 B | 31,914 B | 0 B |
| `.dram0.bss` | 77,784 B | 76,248 B | **−1,536 B** | 115,600 B | 114,064 B | **−1,536 B** |
| DRAM static total | 99,397 B | 97,861 B | **−1,536 B** | 147,514 B | 145,978 B | **−1,536 B** |

The 1,536 B saving on both envs is from `MAX_SCAN_CODEPOINTS` 512→128
(Task 2). The remaining candidates are runtime-only and do not appear in
`.bss`/`.data`:

- InflateReader ring → PSRAM (Task 4): 32,768 B runtime DRAM saved on x4pro.
- Framebuffer → PSRAM (Task 5): 48,000 B runtime DRAM saved on x4pro.
- TtfBookRuntime scratch 256→192 KB (Task 6): 64 KB PSRAM saved on x4pro.

### 7.3 Deferred items

- **Phase 2: loop stack right-sizing** — HWM instrumentation (Task 1) prints
  measurements; the constant stays at 49,152 B until owner reviews device logs.
  The helper compiles out entirely unless the build enables
  `ENABLE_SERIAL_LOG` with `LOG_LEVEL >= 2`, so telemetry-less builds pay
  nothing for the stack scan.
- **Phase 3: sleep-frame RLE + FIBP PSRAM cache** — excluded per owner
  decision; require separate sign-off.

### 7.4 Commits

| commit | task | description |
|---|---|---|
| 83b32859 | Task 1 | feat(dram): add loop-task HWM instrumentation after heavy paths |
| 3e5db91e | Task 2 | feat(dram): shrink FontCacheManager scan table 512→128 |
| d84213e8 | Task 3/4 | feat(dram): route InflateReader ring to PSRAM on x4pro with DRAM fallback |
| 74283c7c | fix | fix(build): add missing esp_heap_caps.h include in InflateReader.cpp |
| df541cbd | Task 5 | feat(dram): enable FREEINK_FB_PSRAM for x4pro envs |
| 7d07c30b | Task 6 | feat(dram): downsize TtfBookRuntime scratch 256→192 KB |

### 7.5 Deviations from the approved scope

- **Candidate 8 (AudioManager removal) is a no-op.** The candidate's evidence
  (`platformio.ini:295`) was stale: `AudioManager` has never been an entry in
  any `platformio.ini` `lib_deps` section (verified across the file's history),
  so there is nothing to `lib_ignore` and no flash/static saving exists. The
  approval is recorded here as implemented-by-finding-nothing-to-remove; no
  code change ships for it.
- **Phase numbering in the §5 soak table** now cites the §4 phase plus the
  candidate number (the original rows used bare candidate numbers as phase
  labels, which clashed with §4's phase list).
- The loop-HWM helper is compile-gated to builds that emit `LOG_DBG`
  telemetry (`ENABLE_SERIAL_LOG` + `LOG_LEVEL >= 2`). Measurement builds opt
  in the same way they do for all `LOG_*` output; production and
  serial-disabled builds no longer pay the stack-scan cost.

## 8. Amendment of 2026-11-03 — persistent static stack for the FIBP worker

### 8.1 Soak Finding 1 (owner-approved fix, same PR #279)

Soaking the CI x4pro artifact with an image-heavy 177-spine TTF book
("Le ti-pou d'Amérique", `ttfGrayPath: images=1`) shows
`[PREF] Task respawn failed` ×5, each inside a `[MEM]` window with
`MaxAlloc=31732`. Chain (file:line at commit 79ae6af2):

1. `FibpPrefetchWorker.h:124` — the worker stack floor is `kStackBytes = 32 * 1024`
   (load-bearing: 24 KB overflowed on device, a long 93-page spine measured
   HWM 1,792 B ⇒ peak ≈ 22.8 KB; not touched here).
2. The worker self-exits after each one-spine plan
   (`FibpPrefetchWorker.cpp:322-329`), so FreeRTOS frees the 32 KB stack, and
   the next spine respawns the task (`notifyChapterProgress` → `ensureTask`,
   `FibpPrefetchWorker.cpp:315-331`).
3. The create/destroy cycle interleaves with ≤4 KB DRAM blocks from
   image-heavy pages (those stay in DRAM per `SPIRAM_MALLOC_ALWAYSINTERNAL=4096`)
   that pinch the freed hole. One cycle later the fragmentation settles at
   `MaxAlloc=31732 < 32768`, `xTaskCreatePinnedToCore`
   (`FibpPrefetchWorker.cpp:356`) fails, and prefetch is dead for the rest of
   the session — silently, exactly on the books that need it most.

### 8.2 Fix (decided by owner): allocate the task stack once, respawn reuses it

- `FibpPrefetchWorker` allocates the stack buffer ONCE, lazily at the first
  `ensureTask()` spawn: one DRAM block of `kStackBytes` via
  `heap_caps_aligned_alloc` with `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`
  (INTERNAL is mandatory — a FreeRTOS task stack can never live in PSRAM;
  see the fix comment for the alignment detail), plus an inline
  `StaticTask_t` member for the TCB. Both are held as members until
  `cancel()` (reader teardown).
- Spawn switched to `xTaskCreateStaticPinnedToCore(taskTrampoline,
  "fibpprefetch", kStackBytes, ...)` — stack depth stays in BYTES: ESP-IDF
  differs from vanilla FreeRTOS there (`idf_additions.h` task API docs;
  `portSTACK_TYPE` is `uint8_t` on the Xtensa port).
- Respawn reuses the same buffers; join semantics, the self-exit design, the
  `lastPrefetchFiredSpine_` dedup and the heap floors are unchanged.
- One correction beyond the original design sketch: a self-deleting task is
  NOT fully deleted at its join semaphore — `vTaskDelete(NULL)` parks the
  TCB on `xTasksWaitingTermination` until the idle task unlinks it, and
  recreating a task on the same buffers while that stale node is still
  linked would corrupt the new task. The respawn therefore also waits
  (bounded) until `xTaskGetHandle("fibpprefetch")` returns null (the name
  search covers the termination list; the worker name is unique in the
  system) before reusing the buffers — the exitedSem_ join itself is
  untouched.
- If the one-time allocation fails (`MaxAlloc < 32768` at first spawn; not
  observed — first spawn happens at book open with 57-69 KB free), the
  respawn fails with the same `LOG_ERR("PREF", ...)` as today; single
  failure path, no dynamic fallback.

### 8.3 Trade-off

32.8 KB DRAM is now held for the whole reading session instead of being
freed between plans. During reading nothing else requests >32 KB DRAM
(inflate ring, TTF scratch and image decode route to PSRAM), and the BLE
toggle is already gate-refused below ~50 KB free while reading — no gate
changes. X3-class envs do not compile this file (TTF-only), so they cannot
regress.

### 8.4 Follow-up soak (build 0e652a97, 2026-11-03) — reserve moved to reader entry

A second soak of the same image-heavy 177-spine book, opened FIRST after a
fresh boot, exposed a gap in §8.2's lazy allocation point. Log order:

- `[CPRO] open: free=110348 max_block=69620` — reader entry, plenty of
  contiguous DRAM for the 32 KB stack.
- `[ZIP] ZIP cache loaded: 465 entries` — the open path's DRAM index
  `unordered_map` lands before the worker ever spawns (§8.5).
- `MaxAlloc=31732` stays flat for the whole reading session.
- `[PREF] Worker armed` → `[ERR] [PREF] Stack alloc failed: 32768` ×6 —
  the first threshold trigger arrives AFTER the cache load, so the lazy
  §8.2 allocation finds no contiguous block. Deterministic across both
  soak logs; a 93-entry book loses only ~13 KB in the same window and never
  shows it.

Fix (owner-decided, same PR #279): `FibpPrefetchWorker::reserveStack()` —
public, idempotent, performs the §8.2 one-shot stack allocation — is called
from `EpubReaderActivity::loadBook()` at reader entry, before the EPUB open
loads anything. `spawnTask()` only uses the pre-reserved buffers afterwards;
its own allocation remains purely as a fallback for paths where the reader
never called `reserveStack()`. Reserve failure at entry logs once and keeps
prefetch disabled for the session (same shape as today, and the armies of
spawn failure logs disappear with it). The buffer is still freed by
`cancel()` at reader teardown — the held-memory window is the reading
session, unchanged from §8.3.

### 8.5 Newly-exposed finding: the ZIP index cache (next candidate, owner decision pending)

The reserve gets its room from the 69,620 B block at reader entry, but that
block is already the DRAM ceiling because of the open path itself: the EPUB
open builds a
`std::unordered_map<std::string, FileStatSlim>` in DRAM
(`lib/ZipFile/ZipFile.h:77,80`, filled by `loadAllFileStatSlims()` at
`lib/ZipFile/ZipFile.cpp:87`) and keeps it the whole reading session. Footprint
is entry-count dependent (~115 B/entry incl. heap nodes; ~56 KB at 465
entries — measured in the 2026-11-03 soak; the old "~5-20 KB" comment in
`lib/Epub/Epub.cpp` was stale and is corrected in this PR). For book-heavy
e.readers this map is the largest remaining runtime DRAM consumer on the X4
Pro reading path and the true fragmentation driver behind §8.1 and §8.4.
Redesigning it (slimmed value, swapped storage, or not caching everything
in DRAM) is logged as the NEXT candidate for this campaign — owner decision
pending, deliberately not touched in this PR.

### 8.6 Owner directive

Fix shipped in the same PR #279 as the DRAM-optimization work (direct human
instruction, 2026-11-03).

Review-round hardening (same PR, 2026-11-03): `cancel()` now waits (bounded,
50 ms) for the idle task's unlink of the self-deleted worker before freeing
the stack and letting the object die — the join token lands BEFORE
`vTaskDelete`, and the TCB the kernel unlinks is a member of this object, so
an immediate free/destroy was a narrow use-after-free on both; a timed-out
reap keeps the whole object alive (leak beats UAF). The spawner's reap probe
is now non-blocking: a spawn whose predecessor is still linked defers by one
trigger instead of a 50 ms render-path stall. The TCB residency
`static_assert` now compiles for real: it reads
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` (the sdkconfig name, 4096) instead of
a symbol no build defined. Second review pass: the inert stub gained a
`reserveStack()` no-op so rollback builds that set TTF without PSRAM+FT
still compile, and mid-session worker re-creation (family swap, takeover)
re-reserves at the `updateFibpWorker()` creation site — `loadBook()`'s
reserve died with the original object. Third pass: the worker object itself
is now allocated through `makeFibpWorkerInternal()`
(`heap_caps_aligned_alloc` with `MALLOC_CAP_INTERNAL` + placement new, custom
deleting deleter `FibpWorkerRawDelete`) — the size assert is a secondary
guard only, because the default allocator's internal preference falls back
to PSRAM on a fragmented heap (Memory.h), and a PSRAM-resident TCB is
invalid. The bounded `cancel()` reap wait (≤50 ms, correctness-mandatory
after a live worker) is accepted once-per-teardown, not copied to the render
path.
