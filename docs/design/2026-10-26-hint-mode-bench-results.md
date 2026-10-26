# Hint-mode bench results: can TTF hinting be increased? (2026-10-26)

Extends `2026-09-29-hint-stack-bench-results.md` from two modes (None/Light)
to all five `FtFont::HintingMode` values across both render paths (AA and
Mono/Crisp), and adds a second bench binary with the TrueType bytecode
interpreter compiled in. The question: can hinting quality be increased for
x4pro/x4c without breaking the 40 KB probe budget
(`kHintProbeStackBudgetBytes = 48 KB loop-task stack − 8 KB margin`)?

## How to regenerate

```sh
# Stock variant — what x4pro/x4c ship today (autohint + mono modules only)
cmake -S test -B build/test -DCROSSPOINT_BUILD_BENCH=ON
cmake --build build/test --target HintMemBench
./build/test/hint_mem_bench/HintMemBench

# Native variant — unlocks HintingMode::Default/Native (bytecode VM compiled in)
cmake -S test -B build/test -DCROSSPOINT_BUILD_BENCH=ON -DCROSSPOINT_BENCH_NATIVE_HINTING=ON
cmake --build build/test --target HintMemBenchNative
./build/test/hint_mem_bench/HintMemBenchNative
```

Families: Amazon Ember (TTF/glyf, the shipped reader face), DejaVu Sans and
Noto Sans Regular (TTF/glyf, instruction-heavy and instruction-light modern
fonts), Atkinson Hyperlegible Next (OTF/CFF, the Adobe engine). Point sizes
are the firmware's real selection (12/14/16/18 pt + SD clamp 8/72). The glyph
cache is disabled (`setGlyphCacheBudget(0)`) — the uncached worst case is what
the device probe measures. Render-path rows only; the layout path
(`ChapterLayout`/`FibpPrefetchWorker`) measured 0 B in every mode, as before.

## Results (host, 64-bit; max over all families and sizes)

### Stock build (interpreter compiled out — today's firmware config)

| hinting | AA steady | AA first | Mono steady | Mono first |
|---|---|---|---|---|
| TTF None    |  4592 |  4592 |  5400 |  5400 |
| TTF Default |  4592 |  4592 |  5400 |  5400 |
| TTF Light   | 14104 | 29544 | 14104 | 14104 |
| TTF Auto    | 14104 | 14104 | 14104 | 14104 |
| CFF None    | 29360 | 29360 | 29016 | 28992 |
| CFF Default | 29016 | 28992 | 29016 | 28992 |
| CFF Light   | 29016 | 28992 | 42584 | 58000 |
| CFF Auto    | 42584 | 42560 | 42584 | 42560 |

### Native build (`FREEINK_FONT_ENABLE_NATIVE_HINTING=1` — interpreter compiled in)

| hinting | AA steady | AA first | Mono steady | Mono first |
|---|---|---|---|---|
| TTF None    |  4592 |  4592 |  5400 |  5400 |
| TTF Default |  4592 |  4592 |  5400 |  5400 |
| TTF Native  |  4592 |  4592 |  5400 |  5400 |
| TTF Light   | 14120 | 29560 | 14120 | 14120 |
| TTF Auto    | 14120 | 14120 | 14120 | 14120 |
| CFF None    | 29032 | 29008 | 29032 | 29008 |
| CFF Default | 29032 | 29008 | 29032 | 29008 |
| CFF Native  | 29032 | 29008 | 29032 | 29008 |
| CFF Light   | 29032 | 29008 | 42600 | 58016 |
| CFF Auto    | 42600 | 42576 | 42600 | 42576 |

(`first` = first call of the sequence; `steady` = deepest call after it.
All numbers are the max across the three TTF / one CFF family; per-family
values are in the full bench output.)

## Why these numbers are trustworthy

The stock-build "Default == None" rows are genuinely degenerate (bytecode
compiled out), and the native-build equality is NOT. Three independent checks
establish that the bytecode VM actually executes in the native bench:

1. **Bitmap checksums** (temporary bench instrumentation, since removed): in
   the native build, `rasterize('g', 25)` under Default and Native produces a
   different bitmap than under None (sum 2693332739993138456 vs
   14103762388647521804); in the stock build Default's checksum equals None's.
2. **Outline probes** (plain FreeType, overlay defines on): with
   `FT_LOAD_TARGET_NORMAL | FT_LOAD_NO_AUTOHINT` and
   `FT_Property_Set("truetype", "interpreter-version", 40)`, the glyph outline
   points move relative to `FT_LOAD_NO_HINTING` (e.g. DejaVu 'g' point
   (598,776) → (598,768)); the same probe with the overlay defines absent
   shows no movement — confirming the overlay's
   `#if FREEINK_FONT_ENABLE_NATIVE_HINTING` gate around
   `TT_CONFIG_OPTION_BYTECODE_INTERPRETER` (freetype-config overlay,
   ftoption.h ~line 705).
3. **Depth equality despite execution**: the interpreter's exec context,
   value stack, and call-recursion stack are heap-allocated FreeType objects;
   its C-stack delta stays below the rasterizer's peak, so the max render
   depth is byte-identical to the unhinted leg at every size.

The stock build's CFF rows are unchanged from the 2026-09-29 doc within
measurement noise; the Adobe CFF engine runs regardless of hinting mode, so
CFF depth is a property of the engine, not of the mode.

## Device calibration

From `2026-09-29-hint-stack-bench-results.md` (measured on device, not
derived): Amazon Ember TTF device probe 15988 B ≈ host steady 14104 B; CFF
device probe 28364 B ≈ host 29016 B. So:

- **TTF: device ≈ host steady.** The Light AA first-call transient (29.5 KB)
  does not appear on device — the autohinter's blue-zone build happens during
  face init, before the probe measures.
- **CFF: device ≈ host, no discount.** A CFF row at or above the 40 KB budget
  line trips the probe gate on device.

## Findings

1. **TTF bytecode hinting is stack-free.** Default/Native peak render depth
   equals the unhinted peak (4592 B AA / 5400 B Mono host; ≈ 5-6 KB device)
   while measurably executing the font's own instructions. The historical
   "TT bytecode has very deep stack frames" fear (freetype-config overlay
   comment, FtFont.h docs) conflated the interpreter's heap footprint with
   C-stack depth.
2. **Light → Auto is stack-free on TTF** (identical 14.1 KB — the same
   autohinter engine, only the render-target mode differs) but buys little:
   at AA 14-22 px on e-ink, full grid-fit mainly trades advance fidelity for
   stem uniformity.
3. **Auto on CFF breaks the budget**: 42584-42600 B host ≈ device. The probe
   gate would fail-close those faces to None, *losing* hinting. CFF must stay
   on Light/Default.
4. **CFF Mono first-call transient is 58 KB** (Light Mono `first`, both
   builds) — above both the 40 KB probe budget and the 48 KB loop-task stack,
   if it holds on device. The init probe rasterizes in AA, so this path is
   never probed today; the Crisp render path would hit it cold on the first
   CFF glyph render. Needs on-device measurement (see Open items).
5. **The bench had a latent crash**: `printRow()`'s format string declared 7
   `%s` for 6 string arguments, so `pointSize` (uint8_t) was consumed as a
   `char*` and the bench segfaulted after its header (exit 139) — present
   since the bench was written; found via an ASAN build
   (`__strlen_avx2` on address 0xc). Fixed.

## Recommendation

For x4pro/x4c:

- **TTF faces: raise hinting to `HintingMode::Default` (or `Native`) with
  `interpreterVersion = 40`**, keeping `HintingMode::Light` for CFF faces.
  This is the best-quality option (the font's own instructions) at ~zero
  additional render-stack cost, and the existing per-face probe gate remains
  the safety net. Implementation requires `FREEINK_FONT_ENABLE_NATIVE_HINTING=1`
  in the x4pro/x4c `platformio.ini` envs and a format-aware choice in
  `BookFontLoader`'s `kRenderOptions`.
- **CFF faces: keep Light.** Do not raise; Auto fails the budget.
- The Mono/Crisp path gains the same way: bytecode under
  `FT_LOAD_TARGET_MONO` performs full grid-fitting (measured: distinct
  bitmap, unchanged peak depth), which is what 1-bit rendering wants.

## Open items (before merging the switch)

1. **Flash + heap cost of the interpreter** on device: pio build size delta
   for x4pro with/without `FREEINK_FONT_ENABLE_NATIVE_HINTING`, and the
   per-size exec-context heap allocation. Stack was the fear; flash/heap are
   the real budgets to verify.
2. **CFF Mono 58 KB transient on device**: measure the Crisp-path first-glyph
   depth on hardware (serial probe). If it reproduces, Crisp CFF faces either
   need the probe extended to the mono path or a deliberate degradation.
3. **Quality verification** on the X4 Pro: render the same page under Light
   vs Default/Native (AA and Crisp) and compare — hinting quality is not
   measurable on the host bench.
4. Interpreter-version choice: v40 ("minimal" ClearType-style) vs v35
   (classic). `TT_CONFIG_OPTION_SUBPIXEL_HINTING` is compiled in with the
   same flag; `RenderOptions.interpreterVersion` selects at runtime.

## Adopted (2026-10-26)

The TTF recommendation is implemented; CFF stays on Light per Finding 3.

- **`platformio.ini`** — `FREEINK_FONT_ENABLE_NATIVE_HINTING=1` added to the
  six envs of the two S3 TTF boards: `x4pro`, `x4pro_profile`, `x4c`,
  `x4pro-gh_release`, `x4pro-gh_release_rc`, `x4c-gh_release` (the release/RC
  variants are the same hardware, so they ship the same configuration;
  `x4c-gh_release_rc` carries no TTF block and is untouched). All other envs
  keep the stock build: without the define the loader's Default request
  compiles out and every face keeps today's Light behavior.
- **`src/BookFontLoader`** — format-aware request, `requestedRenderOptions(slot)`:
  a glyf face (detected from the sfnt table directory — `'glyf'` vs
  `'CFF '/'CFF2'` — by the new `sfntOutlineFormat`, sharing the TTC/table-dir
  walk extracted from `validateSfntBytes`) requests `HintingMode::Default`
  with the RenderOptions-default `interpreterVersion = 40`; CFF and
  undetected faces keep `Light`. The request is applied at face load
  (resident and streamed paths — streamed faces without a cached prefix stay
  undetected and keep Light) and on the probe-retry restore. The probe gate
  (`probeHintStackSafety`), the degrade funnel, and the verdict cache are
  untouched; the effective per-slot mode still folds into the FIBP
  fingerprint tag, so existing caches re-index once on this firmware's first
  font load.
- **Host parity** — `add_font_backend_tests()` in `test/CMakeLists.txt` now
  defines `FREEINK_FONT_ENABLE_NATIVE_HINTING=1` for every FreeType-backend
  test variant, matching the shipped x4pro/x4c configuration. The
  `BookFontLoaderHinting` expectations were updated to the format-aware
  policy: a loaded glyf face sits at `Default` (the host funnel previously
  degraded it to `None` because neither mono nor autohint was compiled in —
  now only mono is refused); the retry tests pin the verdict key against the
  requested-options fingerprint, which no longer equals the failed attempt's
  degraded tag.
- **Bench re-verification** — both binaries re-run clean (stock 813 rows /
  native 1002 rows, exit 0); the native build's TTF Default rows remain
  4592 B AA / 5400 B Mono, and a temporary checksum probe confirmed the
  Default bitmap differs from None's only in the native build (sums
  2693332739993138456 vs 14103762388647521804, exactly the values recorded
  above; stock build: identical, bytecode compiled out). The probe was
  removed after verification.

Credit: Antoine Aflalo (Belphemur), Hermes Agent.
