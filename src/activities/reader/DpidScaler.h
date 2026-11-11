#pragma once

// DpidScaler — the TTF image pipeline's downscale + optional CLAHE local
// contrast (design docs/design/2026-11-09-ttf-image-dpid-clahe.md, §Contract
// 2-3; the reset4 rework). One implementation; three callers (SDK
// JPEG/PNG/progressive source rows, the app BMP row decoder, the GIF
// converter's first-frame rows).
//
// Fractional-coverage area average (always): every output patch accumulates
// its exact source coverage into the destination pixel (no per-row
// `sum/(x1-x0)` division, so nothing loses brightness).
//
// DPID (Nehab/Weber/Amend 2016; reference mergian/dpid `dpid.m`) pass 2 —
// re-accumulates each output pixel from its own source patch with weight
// f = |tent3x3(p) − p| (λ=1) times the patch coverage — is RANDOM-ACCESS
// ONLY (BMP row decoder). Streamed sources (JPEG/PNG/GIF) never run it: the
// pass walks the whole source band per destination pixel — measured on host
// at 934x1400 -> 440x782: 895.9M inner iterations, 3.0M (0.33%) contributing
// (~60 s on the X4 Pro's 240 MHz S3 owner soak 2026-11-10) — while pass 1
// carries the resolution.
//
// CLAHE (XteinkImageRefiner regime: cv2.createCLAHE clip≈2.0, tileGrid 8×8;
// Zuiderveld Graphics Gems IV algorithm): per-tile histograms, integer clip
// redistribution, bilinear blend of the 4 surrounding tile maps, in place on
// the destination image — after the resize, before ToneBake4's collect.
//
// Disabled-gate contract: the pictureDetail setting gates ONLY CLAHE, so
// "Sharp" is the CLAHE-disabled code path, byte-identical by construction.
//
// Integer fixed point only (Q16.16 edge math), fully deterministic. The full
// destination image lives in one call-scoped PSRAM block the caller hands in
// (see scratchBytes()) whenever pass 2 (random) or CLAHE needs it; streamed
// plans without CLAHE run bufferless and stream rows straight to the sink —
// the mode is fixed before the first row, never mid-decode.
//
// Host-testable: no Arduino/HAL includes; sources arrive through feedSourceRow
// or the random-access row provider, results leave through the emit callback.

#include <cstddef>
#include <cstdint>

#if defined(CROSSPOINT_TTF_READER)

namespace ttfimg {

class DpidScaler {
 public:
  // Destination row sink: returns false to abort (foreground takeover). The
  // row is valid only during the call.
  using EmitFn = bool (*)(void* user, int rowIdx, const uint8_t* dstRow);
  // Random-access source-row provider (BMP path: re-reads the resident PSRAM
  // item buffer). Must fill `out` with `plan.srcW` pixels. Rows are requested
  // strictly monotonically.
  using RowFn = bool (*)(void* user, int srcY, uint8_t* out);
  // Abort poll for the post-decode passes (pass-2 remainder, CLAHE, emit
  // walk). true = stop.
  using PollFn = bool (*)(void* user);

  struct Plan {
    int srcW = 0;
    int srcH = 0;
    int dstW = 0;
    int dstH = 0;
    bool clahe = false;   // pictureDetail == Enhanced
    bool detail = true;   // DPID λ=1 pass-2 request; streaming plans drop it
    bool random = false;  // source rows re-readable (pass 2 runs only here)
    // Small class (dst >= src on both axes) picks each destination pixel
    // from the nearest source pixel by center mapping instead of
    // box-averaging (blockiness design 2026-11-09): the divider-ornament
    // scale-ups render crisp line art instead of a blurred field. Costs
    // one srcW row buffer inside the scratch block; the degraded (no
    // block) ladder rung still streams the fractional box.
    bool nearest = false;
    // Caller override (2026-11-10 fidelity campaign): suppress the nearest
    // small class and resample with the fractional box instead. Set for a
    // progressive DC-fallback decode — an 1/8-scale thumbnail upscaled by
    // nearest-center picks is exactly the Lego-block defect. Such decodes
    // are degraded (never cached) — the override costs nothing else.
    bool bilinear = false;
  };

  DpidScaler() = default;

  // Builds the working plan for one decode: tiny images (dst >= src on both
  // axes) skip the detail pass and degenerate to plain sampling.
  static Plan plan(const Plan& dims);

  // PSRAM bytes one scratch block must carry for `p` (0 never happens — the
  // caller decides bufferless by passing a null block). The block is the
  // caller's lifetime (PoolBytes, acquired at decode start, released before
  // return — design §Decision log 6).
  static size_t scratchBytes(const Plan& p);

  // Attaches the scratch block + hooks. The emit hook fires per destination
  // row (collectRow) — needed at begin() because bufferless streaming emits
  // rows directly at band completion. provider/providerUser are required for
  // random-access plans. false rejects unusable dims / undersized block; the
  // caller falls back to a bufferless plan.
  bool begin(const Plan& p, uint8_t* block, size_t bytes, EmitFn emit, void* emitUser, PollFn poll, void* pollUser,
             RowFn provider = nullptr, void* providerUser = nullptr);

  // Streaming source mode: one completed source row, strictly in raster
  // order. Pass 1 only (reset4: the O(band x srcW) detail walk never runs
  // on streamed rows); the emit hook aborts via its own false return.
  void feedSourceRow(int srcY, const uint8_t* gray);

  // Random-access source mode: pass 1 over `provider` (fills plan.srcW
  // pixels per call), pass 2 runs at finish().
  bool runRandom(RowFn provider, void* providerUser);

  // Flushes pass 1, completes pass 2, applies CLAHE when planned, and emits
  // every destination row. Returns false on abort / unusable state.
  bool finish();

  // True once begin() succeeded with a buffer; bufferless streaming calls
  // feedSourceRow() untouched and finish() emits pass-1 rows directly.
  bool buffered() const;
  bool needsFinish() const;  // rows may still be pending

  // Placed plan dims (valid after begin()): the source width the ROW feeds
  // must match, and the destination width the sinks receive.
  int srcWidth() const { return p_.srcW; }
  int dstWidth() const { return p_.dstW; }
  const Plan& placedPlan() const { return p_; }

 private:
  // Pass-1 area average + band bookkeeping (probe-validated fixed point).
  void accumulateRow(const int y, const uint8_t* gray);
  void flushBand();

  // Pass-2 emission for dst row m (in-place flush scheme: out(m) -> hold
  // row, previous hold -> slot m-1 once tent(m) consumed D[m-1]).
  void pass2Row(const int m);
  uint8_t* readRow(const int sy);  // false on a failed provider row
  // Reads the pass-2 vertical patch rows once (random mode). Returns the
  // row count covered, or -1 when the copy must be skipped (no buffer).
  int copyPass2Band(const int64_t y0q, const int64_t y1q);

  // CLAHE on the resident destination image (in place).
  bool clahePass();

  Plan p_ = {};
  EmitFn emit_ = nullptr;
  void* emitUser_ = nullptr;
  PollFn poll_ = nullptr;
  void* pollUser_ = nullptr;
  RowFn provider_ = nullptr;
  void* providerUser_ = nullptr;
  bool aborted_ = false;

  // carved views (forward-aligned, host arithmetic only)
  uint8_t* dst_ = nullptr;     // dstW*dstH
  uint64_t* acc_ = nullptr;    // [dstW]
  uint64_t* wtot_ = nullptr;   // [dstW]
  uint8_t* hold_ = nullptr;    // dstW row, pass-2 flush scheme side buffer
  uint8_t* tent_ = nullptr;    // dstW row, tent values of the active row
  uint8_t* rowBuf_ = nullptr;  // srcW, random-access provider scratch
  uint8_t* near_ = nullptr;    // srcW, nearest-sample store (small class)
  // Pass-2 band copy (random mode): the vertical patch read ONCE per output
  // row instead of once per column — readRow's single-row cache refetches
  // every patch row for each dst column (BMP rows are seeks; minutes-long
  // stalls). Best-effort carve; null falls back to the per-column readRow path.
  uint8_t* p2band_ = nullptr;  // p2bandRows_ * srcW rows of the band
  uint32_t* p2wy_ = nullptr;   // [p2bandRows_] vertical coverage weights
  int p2bandRows_ = 0;
  // Random source-provider failure (sticky): a failed row read must fail
  // the decode instead of baking stale rowBuf_ bytes into a valid-looking
  // cache (truncated BMP corruption guard).
  bool readFailed_ = false;
  int64_t nearBest_ = 0;           // overlap of the remembered nearest row
  uint16_t* claheHist_ = nullptr;  // [tiles][256]
  uint8_t* claheMap_ = nullptr;    // [tiles][256]
  int tilesX_ = 0, tilesY_ = 0, tileW_ = 0, tileH_ = 0;

  int curBand_ = -1;                                       // dst band under pass-1 accumulation
  int lastRand_ = -1;                                      // random-access last requested row
  static constexpr size_t kPass2BandBytesCap = 64 * 1024;  // band copy bound
  bool holdFull_ = false;                                  // hold row carries out(dstH-1)
  bool finished_ = false;
  bool ok_ = false;
};

}  // namespace ttfimg

#endif  // CROSSPOINT_TTF_READER
