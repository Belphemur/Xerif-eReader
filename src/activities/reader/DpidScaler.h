#pragma once

// DpidScaler — the TTF image pipeline's detail-preserving downscale + optional
// CLAHE local contrast (design docs/design/2026-11-09-ttf-image-dpid-clahe.md,
// §Contract 2-3, §Decision log). One implementation; three callers (SDK
// JPEG/PNG/progressive source rows, the app BMP row decoder, the GIF
// converter's first-frame rows).
//
// DPID (Nehab/Weber/Amend 2016; reference mergian/dpid `dpid.m`):
//   pass 1 — fractional-coverage area average of every output patch into a
//     full-destination image (fixes the per-row `sum/(x1-x0)` truncation the
//     old inline box filters had: no per-row division, so nothing loses
//     brightness);
//   pass 2 — re-accumulates each output pixel from its own source patch with
//     weight f = |tent3x3(p) − p| (λ=1; no pow) times the patch coverage, so
//     high-frequency content that box averaging smears away dominates the
//     output. Host-pinned: +41% stroke energy on hairline fields, λ=0 output
//     byte-identical to pass 1, no output pixel outside its source patch
//     [min,max] (weighted-average property), flat fields exact.
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
// (see scratchBytes()); PSRAM-starved callers skip the whole block and get
// the streaming λ=0 fractional box (still truncation-free) with no buffer at
// all — the mode is fixed before the first row, never mid-decode.
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
    bool detail = true;   // DPID λ=1; false => fractional λ=0 box
    bool random = false;  // source rows re-readable (no ring needed)
    // Small class (dst >= src on both axes) picks each destination pixel
    // from the nearest source pixel by center mapping instead of
    // box-averaging (blockiness design 2026-11-09): the divider-ornament
    // scale-ups render crisp line art instead of a blurred field. Costs
    // one srcW row buffer inside the scratch block; the degraded (no
    // block) ladder rung still streams the fractional box.
    bool nearest = false;
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
  // order. Runs pass 1 and (bufferful streaming detail) the interleaved
  // pass 2; returns false once the emit hook aborted.
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
  uint8_t* readRow(const int sy);

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
  uint8_t* dst_ = nullptr;         // dstW*dstH
  uint64_t* acc_ = nullptr;        // [dstW]
  uint64_t* wtot_ = nullptr;       // [dstW]
  uint8_t* hold_ = nullptr;        // dstW row, pass-2 flush scheme side buffer
  uint8_t* tent_ = nullptr;        // dstW row, tent values of the active row
  uint8_t* ring_ = nullptr;        // ringRows * srcW source rows (streaming)
  uint8_t* rowBuf_ = nullptr;      // srcW, random-access provider scratch
  uint8_t* near_ = nullptr;        // srcW, nearest-sample store (small class)
  int64_t nearBest_ = 0;           // overlap of the remembered nearest row
  uint16_t* claheHist_ = nullptr;  // [tiles][256]
  uint8_t* claheMap_ = nullptr;    // [tiles][256]
  int tilesX_ = 0, tilesY_ = 0, tileW_ = 0, tileH_ = 0;

  int curBand_ = -1;       // dst band under pass-1 accumulation
  int nextPass2_ = 0;      // first dst row not yet pass-2 emitted
  int lastRand_ = -1;      // random-access last requested row
  bool holdFull_ = false;  // hold row carries out(nextPass2_-1)
  bool finished_ = false;
  bool ok_ = false;

  size_t ringRows_ = 0;  // source rows the ring holds (streaming detail)
  static constexpr size_t kRingBytesCap = 128 * 1024;
};

}  // namespace ttfimg

#endif  // CROSSPOINT_TTF_READER
