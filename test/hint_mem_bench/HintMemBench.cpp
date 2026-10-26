// Hint-stack memory benchmark (opt-in: -DCROSSPOINT_BUILD_BENCH=ON).
//
// Answers, with numbers, the question BookFontLoader's hint-stack gate turns
// on: how deep does a REAL hinted render of a REAL face go, at the point
// sizes the firmware actually selects, on BOTH consumers the verdict is
// shared with (the 48 KB loop task and the 32 KB FIBP prefetch worker)?
//
// What runs here is the firmware's real code, not a lookalike harness:
//   - BookFontLoader.cpp loads the fixture families through the same
//     tryLoadFace path the device uses (stub HalStorage stands in for SD),
//   - the depth is measured across FontChain::rasterize()/advance()/kerning()
//     — the exact calls the reader and ChapterLayout make,
//   - the unhinted leg is produced by FtFont::setRenderOptions(HintingMode::None)
//     — the same call BookFontLoader::degradeHint() makes.
//
// The probe task body itself (BookFontLoader::probeHintStackSafety) is
// #if defined(ARDUINO) by design — it exists to read a FreeRTOS
// uxTaskGetStackHighWaterMark, which has no host equivalent — so the bench
// cannot execute it. The measurement below is the same measurement
// methodology that function relies on: IDF's prvTaskCheckFreeStackSpace
// (tasks.c) is a memory scan for the stack-fill byte, and paintStack()/
// depthBelow() here is the identical scan. Absolute numbers are 64-bit host
// and run somewhat above the 32-bit Xtensa device (see
// docs/design/2026-09-29-hint-stack-bench-results.md for the
// device-vs-host correspondence); the per-face ratios and the shape of the
// result are what the budget decision reads.
//
// Plain executable, NOT a ctest case: a depth assertion would be flaky
// across hosts (compiler/optimizer dependent) and would make CI a liar.
#include <pthread.h>
#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "BookFontLoader.h"
#include "FtFont.h"
#include "TestHeapHooks.h"
#include "TtfFont.h"

namespace {

// ── Painted-stack measurement (IDF prvTaskCheckFreeStackSpace equivalent) ──
constexpr uint64_t kMagic = 0xDEADBEEFCAFEF00DULL;
constexpr size_t kStackBytes = 8u * 1024 * 1024;
// Locals live below the frame pointer; start the paint 1 KB below it so the
// harness's own frame is never inside the painted region.
constexpr size_t kMarginBytes = 1024;

// Owns the painted-stack mapping so every exit path — the early returns below,
// and the std::exit() paths in measureSequence()/readFileOrDie() — releases it.
// A raw mmap() released by a single munmap() at the end of main() leaks on all
// of them.
class StackMapping {
 public:
  StackMapping()
      : ptr_(static_cast<uint8_t*>(
            mmap(nullptr, kStackBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0))) {
    if (ptr_ == MAP_FAILED) ptr_ = nullptr;
  }
  ~StackMapping() {
    if (ptr_ != nullptr) munmap(ptr_, kStackBytes);
  }
  StackMapping(const StackMapping&) = delete;
  StackMapping& operator=(const StackMapping&) = delete;

  bool valid() const { return ptr_ != nullptr; }
  uint8_t* get() const { return ptr_; }

 private:
  uint8_t* ptr_;
};

uint8_t* g_stack = nullptr;
const char* t_frameTop = nullptr;
size_t g_first = 0;
size_t g_steady = 0;

void paintStack(const char* top) {
  // Plain memset: paintStack's own frame sits ABOVE the painted region's top
  // edge (kMarginBytes below the trampoline frame), so no helper frame lands
  // inside the region being painted.
  const size_t bytes = reinterpret_cast<uintptr_t>(top) - kMarginBytes - reinterpret_cast<uintptr_t>(g_stack);
  std::memset(g_stack, 0, bytes);
  for (size_t i = 0; i + sizeof(kMagic) <= bytes; i += sizeof(kMagic)) {
    std::memcpy(g_stack + i, &kMagic, sizeof(kMagic));
  }
}

// Deepest byte written below `top` since the last paintStack(): the first
// non-magic word scanning up from the stack base. Same algorithm as
// prvTaskCheckFreeStackSpace, hence the same units (bytes).
size_t depthBelow(const char* top) {
  const volatile uint64_t* w = reinterpret_cast<const volatile uint64_t*>(g_stack);
  const size_t words = (reinterpret_cast<uintptr_t>(top) - kMarginBytes - reinterpret_cast<uintptr_t>(g_stack)) / 8;
  for (size_t i = 0; i < words; ++i) {
    if (w[i] != static_cast<volatile uint64_t>(kMagic)) {
      const size_t hit = reinterpret_cast<uintptr_t>(&w[i]) - reinterpret_cast<uintptr_t>(g_stack);
      const size_t depth = (reinterpret_cast<uintptr_t>(top) - reinterpret_cast<uintptr_t>(g_stack)) - hit;
      return depth > kMarginBytes ? depth - kMarginBytes : 0;
    }
  }
  return 0;
}

// ── Consumer call sets ────────────────────────────────────────────────────
// Render path (loop task, the only consumer that produces bitmaps).
// Layout path (FIBP worker / ChapterLayout): metrics only — the worker never
// rasterizes, so measuring its calls separately is the point of the exercise.
struct SequenceJob {
  freeink::font::FontChain* chain;
  const std::vector<uint32_t>* codepoints;
  uint16_t sizePx;
  bool renderPath;
};

// One painted-stack run that calls the consumer once per codepoint,
// repainting between calls. Records the depth of the FIRST call — the device
// probe's number — and the deepest call after it. The split matters: the
// first hinted render of a face pays the autohinter's one-time blue-zone
// setup, which later renders do not.
void runSequence(void* arg) {
  auto* job = static_cast<SequenceJob*>(arg);
  (void)job;
  for (size_t i = 0; i < job->codepoints->size(); ++i) {
    paintStack(t_frameTop);
    __asm__ __volatile__("" ::: "memory");
    const uint32_t cp = (*job->codepoints)[i];
    if (job->renderPath) {
      // The reader's render path: resolve the covering face, then rasterize it
      // (FontChain is metrics-only; bitmaps come from its RasterFont faces).
      freeink::font::RasterFont* face = job->chain->fontFor(cp, 0);
      if (face != nullptr) (void)face->rasterize(cp, job->sizePx);
    } else {
      (void)job->chain->lineHeight(job->sizePx);
      (void)job->chain->ascent(job->sizePx);
      job->chain->advance(cp, job->sizePx, 0);
      job->chain->kerning(cp, (*job->codepoints)[(i + 1) % job->codepoints->size()], job->sizePx, 0);
    }
    __asm__ __volatile__("" ::: "memory");
    const size_t depth = depthBelow(t_frameTop);
    if (i == 0) {
      g_first = depth;
    } else if (depth > g_steady) {
      g_steady = depth;
    }
  }
}

// Result of one sequence run: the first call's depth and the deepest call
// after it (see runSequence).
struct SequenceResult {
  size_t firstGlyph;
  size_t steadyState;
};

// Runs the sequence on a fresh painted stack, so every reading is a
// whole-sequence peak — matching the device probe's virgin-task reading.
SequenceResult measureSequence(const SequenceJob& job) {
  g_first = 0;
  g_steady = 0;
  struct Ctx {
    const SequenceJob* job;
  } ctx{&job};
  auto trampoline = +[](void* p) -> void* {
    t_frameTop = reinterpret_cast<const char*>(__builtin_frame_address(0));
    runSequence(const_cast<SequenceJob*>(static_cast<Ctx*>(p)->job));
    __asm__ __volatile__("" ::: "memory");
    return nullptr;
  };
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstack(&attr, g_stack, kStackBytes);
  pthread_t thread;
  const int rc = pthread_create(&thread, &attr, trampoline, &ctx);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    std::fprintf(stderr, "hint_mem_bench: pthread_create failed (%d)\n", rc);
    std::exit(2);
  }
  pthread_join(thread, nullptr);
  return SequenceResult{g_first, g_steady};
}

// ── Fixture wiring (real faces from test/fixtures/fonts) ──────────────────
constexpr const char* kEmberRegular = TESTDATA_DIR "/fixtures/fonts/amazon-ember/Amazon_Ember_Regular.ttf";
constexpr const char* kAtkinsonRegular =
    TESTDATA_DIR "/fixtures/fonts/atkinson-hyperlegible-next/AtkinsonHyperlegibleNext-Regular.otf";
constexpr const char* kDejaVuSans =
    TESTDATA_DIR "/../freeink-sdk/libs/font/FreeInkFont/test/fixtures/fonts/DejaVuSans.ttf";
constexpr const char* kNotoSansRegular =
    TESTDATA_DIR "/../lib/EpdFont/builtinFonts/source/NotoSans/NotoSans-Regular.ttf";

std::string readFileOrDie(const char* path) {
  // ifstream: the handle closes on every path, including the early exits.
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "hint_mem_bench: cannot open fixture %s\n", path);
    std::exit(2);
  }
  std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (bytes.empty()) {
    std::fprintf(stderr, "hint_mem_bench: short read on %s\n", path);
    std::exit(2);
  }
  return bytes;
}

// Seed the stub HalStorage the way BookFontLoaderTest does, then build the
// manifest through the loader's host-test seams (editFamily +
// setFamilyCountForTest — the same seam BookFontLoaderTest uses, because the
// SD directory walk needs a directory-iterating HalFile the stub does not
// model). The manifest rows are exactly what the device's scanFonts writes:
// one face per style, full SD path, byte size and mtime.
struct SeededFace {
  const char* path;
  const char* fixture;
  uint8_t styleFlags;
};
struct SeededFamily {
  const char* name;
  const char* format;
  SeededFace faces[4];
};

const SeededFamily kSeededFamilies[] = {
    {"Amazon Ember",
     "TTF (glyf)",
     {{"/fonts/Amazon Ember/Amazon_Ember_Regular.ttf", kEmberRegular, 0u /*StyleNone*/},
      {"/fonts/Amazon Ember/Amazon_Ember_Bold.ttf", kEmberRegular, 1u /*StyleBold*/}}},
    {"Atkinson",
     "OTF (CFF)",
     {{"/fonts/Atkinson/AtkinsonRegular.otf", kAtkinsonRegular, 0u},
      {"/fonts/Atkinson/AtkinsonBold.otf", kAtkinsonRegular, 1u}}},
    {"DejaVu Sans",
     "TTF (glyf)",
     {{"/fonts/DejaVu/DejaVuSans.ttf", kDejaVuSans, 0u},
      {"/fonts/DejaVu/DejaVuSans.ttf", kDejaVuSans, 1u /*StyleBold*/}}},
    {"Noto Sans",
     "TTF (glyf)",
     {{"/fonts/NotoSans/NotoSans-Regular.ttf", kNotoSansRegular, 0u},
      {"/fonts/NotoSans/NotoSans-Regular.ttf", kNotoSansRegular, 1u /*StyleBold*/}}},
};

// Publishes ONE manifest record per family (faces[] + faceCount), the shape
// scanFonts() writes on device — a record per face would leave each with only
// one style and the chain would never cover Bold.
void seedFamily(const SeededFamily& fam, freeink::book::BookFontLoader& loader) {
  freeink::book::FamilyInfo& info = loader.editFamily(0);
  info = freeink::book::FamilyInfo{};
  std::snprintf(info.name, sizeof(info.name), "%s", fam.name);
  uint8_t count = 0;
  for (const SeededFace& face : fam.faces) {
    if (face.path == nullptr) break;  // unused style slots stay zero-initialized
    Storage.files[face.path] = readFileOrDie(face.fixture);
    for (size_t slash = std::string(face.path).find('/', 1); slash != std::string::npos;
         slash = std::string(face.path).find('/', slash + 1)) {
      Storage.dirs.insert(std::string(face.path).substr(0, slash));
    }
    freeink::book::FontFaceInfo& faceInfo = info.faces[count];
    std::snprintf(faceInfo.file, sizeof(faceInfo.file), "%s", face.path);
    faceInfo.styleFlags = face.styleFlags;
    faceInfo.fileSize = static_cast<uint32_t>(Storage.files[face.path].size());
    faceInfo.mtime = 0;
    faceInfo.faceIndex = 0;
    ++count;
  }
  info.faceCount = count;
  loader.setFamilyCountForTest(1);
}

// The firmware's real point-size selection: BUILTIN_READER_POINT_SIZES
// (src/ReaderFontSizes.h) for built-in families, TTF_FONT_POINT_SIZE_MIN/MAX
// (8..72) for SD families; px = round(pt * 150 / 72) as on device.
constexpr uint8_t kReaderPointSizes[] = {12, 14, 16, 18};  // built-in defaults
constexpr uint8_t kSdPointSizeMin = 8;
constexpr uint8_t kSdPointSizeMax = 72;
constexpr float kDeviceDpi = 150.0f;

uint16_t pointSizeToPx(uint8_t pt) {
  return static_cast<uint16_t>((static_cast<float>(pt) * kDeviceDpi / 72.0f) + 0.5f);
}

// The probe's own stress set (BookFontLoader.cpp kHintProbeCodepoints) plus a
// realistic page mix, so the bench can show whether the choice of glyphs
// moves the peak at all.
constexpr uint32_t kStressCodepoints[] = {'A', 'g', 'M', '@', 0x00C6u, 0x2019u};
const char* kPageText =
    "The quick brown fox jumps over the lazy dog while the wicked and warped "
    "heart of the court conspires; naive cafe, d'accord, quoted dashes and "
    "ellipses plus 0123456789 and MMMMMMMMMM @@@ gggg WWWW.";

std::vector<uint32_t> decodePageText() {
  std::vector<uint32_t> out;
  out.reserve(160);
  const char* p = kPageText;
  while (*p != '\0') {
    const auto c = static_cast<unsigned char>(*p);
    if (c < 0x80) {
      out.push_back(c);
      p += 1;
    } else if ((c & 0xE0) == 0xC0 && p[1] != '\0') {
      out.push_back(static_cast<uint32_t>(((c & 0x1F) << 6) | (p[1] & 0x3F)));
      p += 2;
    } else if ((c & 0xF0) == 0xE0 && p[1] != '\0' && p[2] != '\0') {
      out.push_back(static_cast<uint32_t>(((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F)));
      p += 3;
    } else {
      // Truncated or malformed sequence: emit the fallback and consume the
      // lead byte only, so the loop can never step past the terminator.
      out.push_back('?');
      p += 1;
    }
  }
  return out;
}

void setChainHinting(freeink::font::FontChain* chain, freeink::font::FtFont::HintingMode mode, bool monochrome) {
  // Same call degradeHint() makes; fontFor() hands back the face the render
  // path would actually draw with, so the options land on the real face.
  for (const uint32_t cp : kStressCodepoints) {
    auto* face = dynamic_cast<freeink::font::FtFont*>(chain->fontFor(cp, 0));
    if (face == nullptr) continue;
    freeink::font::FtFont::RenderOptions options{};
    options.hinting = mode;
    options.interpreterVersion = 40;
    options.monochrome = monochrome;
    face->setRenderOptions(options);
    // Measure the uncached worst case: the device's P1 glyph cache makes
    // repeat glyphs cheaper, but the FIRST render of a face is what must fit.
    face->setGlyphCacheBudget(0);
  }
}

struct Row {
  const char* family;
  const char* format;   // TTF (glyf) / OTF (CFF)
  const char* callSet;  // render / layout
  const char* glyphSet;
  const char* hinting;
  const char* renderMode;  // AA / Mono
  uint8_t pointSize;
  uint16_t sizePx;
  size_t firstGlyph;
  size_t steadyState;
};

void printRow(const Row& r) {
  std::printf("| %s | %s | %s | %s | %s | %s | %2u | %3u | %6zu | %6zu |\n", r.family, r.format, r.callSet, r.glyphSet,
              r.hinting, r.renderMode, r.pointSize, r.sizePx, r.firstGlyph, r.steadyState);
}

}  // namespace

int main() {
  const StackMapping stack;
  if (!stack.valid()) {
    std::fprintf(stderr, "hint_mem_bench: mmap failed\n");
    return 2;
  }
  g_stack = stack.get();
  // PSRAM tier like the x4pro (the only tier that carries real faces): 8 MB
  // PSRAM with a large contiguous block, so fixtures load resident instead of
  // tripping the DRAM-tier 128 KB cap.
  const HalMemory::HeapStats psram{8u * 1024 * 1024, 8u * 1024 * 1024, 8u * 1024 * 1024, 7u * 1024 * 1024};
  testSetPsramHeap(psram);
  testSetFreeHeap(320 * 1024, 320 * 1024);

  const std::vector<uint32_t> stress(std::begin(kStressCodepoints), std::end(kStressCodepoints));
  const std::vector<uint32_t> page = decodePageText();

  // All hinting modes defined in FtFont::HintingMode, in probe-relevant order.
  // The bench runs every mode; those not compiled into this FreeType build
  // (Native without FREEINK_FONT_ENABLE_NATIVE_HINTING) silently degrade to the
  // closest equivalent — see the header note printed below.
  const struct {
    const char* name;
    freeink::font::FtFont::HintingMode mode;
  } kModes[] = {
      {"None", freeink::font::FtFont::HintingMode::None},     {"Light", freeink::font::FtFont::HintingMode::Light},
      {"Auto", freeink::font::FtFont::HintingMode::Auto},     {"Default", freeink::font::FtFont::HintingMode::Default},
      {"Native", freeink::font::FtFont::HintingMode::Native},
  };
  const char* modeNames[] = {"AA", "Mono"};

  std::printf("# Hint-stack depth bench (host, 64-bit; see docs/design/ for device correspondence)\n\n");
  std::printf("Depth = deepest byte reached below a fresh stack top (bytes), the same\n");
  std::printf("measurement IDF's uxTaskGetStackHighWaterMark performs on device.\n");
  std::printf("first = first call of the sequence (the probe's number); steady = deepest\n");
  std::printf("call after it. Glyph cache disabled: uncached worst case.\n\n");
  std::printf("Hinting modes:\n");
  std::printf("  None    — FT_LOAD_NO_HINTING|NO_AUTOHINT (no hinting, no phantom rounding)\n");
  std::printf("  Light   — FT_LOAD_TARGET_LIGHT (auto-hinter, light grid-fit — current TTF default)\n");
  std::printf("  Auto    — FT_LOAD_FORCE_AUTOHINT|TARGET_NORMAL (auto-hinter, full grid-fit)\n");
  std::printf("  Default — FT_LOAD_NO_AUTOHINT (native bytecode v40 if compiled; see note)\n");
  std::printf("  Native  — FT_LOAD_NO_AUTOHINT + interpreter-version 40 (bytecode VM)\n");
  std::printf("Render modes:\n");
  std::printf("  AA     — 8-bit grayscale coverage (Smooth render path)\n");
  std::printf("  Mono   — 1-bit coverage (Crisp render path, the device's mono target)\n\n");

  std::printf("| family | format | call set | glyph set | hinting | mode | pt | px | first | steady |\n");
  std::printf("|---|---|---|---|---|---|---|---|---|---|\n");

  // Track TTF render depth maxima for the summary (only for render path, AA mode).
  struct MaxDepth {
    size_t light = 0, autos = 0, none = 0, native = 0, def = 0;
  };
  MaxDepth ttfMaxAA, ttfMaxMono;
#if FREEINK_FONT_ENABLE_NATIVE_HINTING
  constexpr bool kNativeHintingAvailable = true;
#else
  constexpr bool kNativeHintingAvailable = false;
#endif

  for (const SeededFamily& fam : kSeededFamilies) {
    // A fresh loader per family keeps the measurement independent of any
    // earlier family's residency, matching the device probe's per-face task.
    freeink::book::BookFontLoader loader;
    seedFamily(fam, loader);
    loader.selectFamily(fam.name);
    loader.ensureLoaded();
    freeink::font::FontChain* chain = loader.getReaderFont();
    if (chain == nullptr) {
      std::fprintf(stderr, "hint_mem_bench: no reader chain for %s\n", fam.name);
      return 2;
    }
    // Guard against silently measuring the builtin bitmap fallback: the whole
    // point is the real faces' depth.
    if (dynamic_cast<freeink::font::FtFont*>(chain->fontFor('A', 0)) == nullptr) {
      std::fprintf(stderr, "hint_mem_bench: %s did not load an FT face (builtin fallback in use)\n", fam.name);
      return 2;
    }
    // The kModes loop below handles Native mode availability via
    // kNativeHintingAvailable (compile-time). No runtime setRenderOptions
    // probe is needed — Native mode with no bytecode interpreter would
    // produce a degenerate FT_LOAD_NO_AUTOHINT load that can crash the
    // FreeType rasterizer, so we never call it.

    for (const auto& hm : kModes) {
      // Skip Native mode when the bytecode interpreter is not compiled in.
      if (hm.mode == freeink::font::FtFont::HintingMode::Native && !kNativeHintingAvailable) continue;
      for (int mono = 0; mono < 2; ++mono) {
        setChainHinting(chain, hm.mode, mono != 0);
        const char* rmode = modeNames[mono];
        for (int renderPath = 1; renderPath >= 0; --renderPath) {
          for (const uint8_t pt : kReaderPointSizes) {
            const uint16_t px = pointSizeToPx(pt);
            for (int set = 0; set < 2; ++set) {
              const std::vector<uint32_t>& cps = set ? page : stress;
              const SequenceResult r = measureSequence(SequenceJob{chain, &cps, px, renderPath != 0});
              printRow(Row{fam.name, fam.format, renderPath ? "render (rasterize)" : "layout (advance/kern)",
                           set ? "page mix" : "probe stress", hm.name, rmode, pt, px, r.firstGlyph, r.steadyState});
              // Track TTF render maxima
              if (renderPath && std::strcmp(fam.format, "TTF (glyf)") == 0) {
                size_t peak = r.steadyState;
                if (r.firstGlyph > peak) peak = r.firstGlyph;
                if (mono == 0) {
                  if (hm.mode == freeink::font::FtFont::HintingMode::Light && peak > ttfMaxAA.light)
                    ttfMaxAA.light = peak;
                  if (hm.mode == freeink::font::FtFont::HintingMode::Auto && peak > ttfMaxAA.autos)
                    ttfMaxAA.autos = peak;
                  if (hm.mode == freeink::font::FtFont::HintingMode::None && peak > ttfMaxAA.none) ttfMaxAA.none = peak;
                  if (hm.mode == freeink::font::FtFont::HintingMode::Native && peak > ttfMaxAA.native)
                    ttfMaxAA.native = peak;
                  if (hm.mode == freeink::font::FtFont::HintingMode::Default && peak > ttfMaxAA.def)
                    ttfMaxAA.def = peak;
                } else {
                  if (hm.mode == freeink::font::FtFont::HintingMode::Light && peak > ttfMaxMono.light)
                    ttfMaxMono.light = peak;
                  if (hm.mode == freeink::font::FtFont::HintingMode::Auto && peak > ttfMaxMono.autos)
                    ttfMaxMono.autos = peak;
                  if (hm.mode == freeink::font::FtFont::HintingMode::None && peak > ttfMaxMono.none)
                    ttfMaxMono.none = peak;
                  if (hm.mode == freeink::font::FtFont::HintingMode::Native && peak > ttfMaxMono.native)
                    ttfMaxMono.native = peak;
                  if (hm.mode == freeink::font::FtFont::HintingMode::Default && peak > ttfMaxMono.def)
                    ttfMaxMono.def = peak;
                }
              }
            }
          }
          // SD-family extremes: the reader's clamp range is 8..72 pt.
          for (const uint8_t pt : {kSdPointSizeMin, kSdPointSizeMax}) {
            const uint16_t px = pointSizeToPx(pt);
            for (int set = 0; set < 2; ++set) {
              const std::vector<uint32_t>& cps = set ? page : stress;
              const SequenceResult r = measureSequence(SequenceJob{chain, &cps, px, renderPath != 0});
              printRow(Row{fam.name, fam.format, renderPath ? "render (rasterize)" : "layout (advance/kern)",
                           set ? "page mix" : "probe stress", hm.name, rmode, pt, px, r.firstGlyph, r.steadyState});
              if (renderPath && std::strcmp(fam.format, "TTF (glyf)") == 0) {
                size_t peak = r.steadyState;
                if (r.firstGlyph > peak) peak = r.firstGlyph;
                if (mono == 0) {
                  if (hm.mode == freeink::font::FtFont::HintingMode::Light && peak > ttfMaxAA.light)
                    ttfMaxAA.light = peak;
                  if (hm.mode == freeink::font::FtFont::HintingMode::Auto && peak > ttfMaxAA.autos)
                    ttfMaxAA.autos = peak;
                  if (hm.mode == freeink::font::FtFont::HintingMode::None && peak > ttfMaxAA.none) ttfMaxAA.none = peak;
                  if (hm.mode == freeink::font::FtFont::HintingMode::Native && peak > ttfMaxAA.native)
                    ttfMaxAA.native = peak;
                  if (hm.mode == freeink::font::FtFont::HintingMode::Default && peak > ttfMaxAA.def)
                    ttfMaxAA.def = peak;
                } else {
                  if (hm.mode == freeink::font::FtFont::HintingMode::Light && peak > ttfMaxMono.light)
                    ttfMaxMono.light = peak;
                  if (hm.mode == freeink::font::FtFont::HintingMode::Auto && peak > ttfMaxMono.autos)
                    ttfMaxMono.autos = peak;
                  if (hm.mode == freeink::font::FtFont::HintingMode::None && peak > ttfMaxMono.none)
                    ttfMaxMono.none = peak;
                  if (hm.mode == freeink::font::FtFont::HintingMode::Native && peak > ttfMaxMono.native)
                    ttfMaxMono.native = peak;
                  if (hm.mode == freeink::font::FtFont::HintingMode::Default && peak > ttfMaxMono.def)
                    ttfMaxMono.def = peak;
                }
              }
            }
          }
        }
      }
    }
  }

  // ── Summary ──────────────────────────────────────────────────────────────
  // The budget from docs/design/2026-09-29-hint-stack-bench-results.md:
  //   kHintProbeStackBudgetBytes = 40 KB (48 KB loop-task consumer — 8 KB margin)
  // Host numbers run ~1.5-2× above the 32-bit Xtensa device; the ratios and
  // the shape of the result are what the budget decision reads.
  constexpr size_t kBudget = 40 * 1024;
  std::printf("\n## Summary: peak render depth vs %zu KB device budget\n\n", kBudget / 1024);
  std::printf("Max render depth across all families/sizes (host, 64-bit). Calibration\n");
  std::printf("(docs/design/2026-09-29-hint-stack-bench-results.md): TTF device probe ~=\n");
  std::printf("host STEADY (the first-call autohinter transient does not appear on\n");
  std::printf("device: blue zones build during face init); CFF device ~= host, no\n");
  std::printf("discount. A CFF row at or above the budget line trips the probe gate on\n");
  std::printf("device.\n\n");
  std::printf("| hinting | AA (grayscale) | Mono (Crisp) | vs budget |\n");
  std::printf("|---|---|---|---|\n");
  auto budgetPct = [](size_t depth) -> const char* {
    if (depth == 0) return "-";
    static char buf[3][32];
    static int idx = 0;
    idx = (idx + 1) % 3;
    std::snprintf(buf[idx], sizeof(buf[idx]), "%zu KB (%.0f%%)", depth / 1024, (double)depth / (double)kBudget * 100.0);
    return buf[idx];
  };
  std::printf("| None    | %14zu B | %13zu B | %s | %s |\n", ttfMaxAA.none, ttfMaxMono.none, budgetPct(ttfMaxAA.none),
              budgetPct(ttfMaxMono.none));
  std::printf("| Light   | %14zu B | %13zu B | %s | %s |\n", ttfMaxAA.light, ttfMaxMono.light,
              budgetPct(ttfMaxAA.light), budgetPct(ttfMaxMono.light));
  std::printf("| Auto    | %14zu B | %13zu B | %s | %s |\n", ttfMaxAA.autos, ttfMaxMono.autos,
              budgetPct(ttfMaxAA.autos), budgetPct(ttfMaxMono.autos));
  std::printf("| Default | %14zu B | %13zu B | %s | %s |\n", ttfMaxAA.def, ttfMaxMono.def, budgetPct(ttfMaxAA.def),
              budgetPct(ttfMaxMono.def));
  std::printf("| Native  | %14zu B | %13zu B | %s | %s |\n", ttfMaxAA.native, ttfMaxMono.native,
              budgetPct(ttfMaxAA.native), budgetPct(ttfMaxMono.native));
  std::printf("\n");
  if (!kNativeHintingAvailable) {
    std::printf("NOTE: Native hinting NOT compiled in (no FREEINK_FONT_ENABLE_NATIVE_HINTING).\n");
    std::printf("  Default and Native modes degrade to FT_LOAD_NO_AUTOHINT without bytecode VM.\n");
    std::printf("  On device, Default/Native require the bytecode interpreter — those rows show\n");
    std::printf("  the fallback (no bytecode execution) depths, not the bytecode VM depth.\n");
  } else {
    std::printf("Bytecode interpreter IS compiled in — Native rows reflect real bytecode VM depth.\n");
  }
  std::printf("\nLayout path (FIBP worker / ChapterLayout) uses FT_LOAD_NO_HINTING|FAST_ONLY\n");
  std::printf("for advances — measured depth is ~0 regardless of hinting mode.\n");
  std::printf("Only the render path (loop task, 48 KB stack on ESP32-S3) pays the hint depth.\n");

  return 0;
}
