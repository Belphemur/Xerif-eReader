# Design: DRAM reduction analysis — X3-class first, trickle to X4 Pro

Date: 2026-11-02
Author: Antoine Aflalo (Belphemur) via analysis agent
Status: proposed — analysis only, no code changes in this job
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
| x4pro | Phase 3 TTF scratch 192 KB | Open known "large chapter" book. Host tests pass; device shows no `Arena::alloc` failure in log. |
| default | Phase 1 FontCacheManager 128 | Render mixed-script page (e.g. Russian + English). Page renders correctly; no infinite prewarm loop. |
| default | Phase 2 loop stack measurement | Instrumented build logs HWM after every heavy path. Stable HWM < new stack size − 2 KB margin. |
| default | Phase 6 sleep-frame RLE | Sleep 10×. `sleep_frame.bin` size < 20 KB each time. Resume shows correct frame. Legacy uncompressed frame loads after OTA rollback. |
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
