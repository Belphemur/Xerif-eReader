// DpidScaler implementation (see DpidScaler.h). All math is the probe-validated
// integer fixed point from docs/design/2026-11-09-ttf-image-dpid-clahe.md. Unit
// rule the probe established: edge(i) is in SOURCE units (65536 per source
// pixel), so source-axis enumeration keeps cover(); the row->band direction is
// the inverse and uses bandOverlap() against band edges directly.

#include "DpidScaler.h"

#if defined(CROSSPOINT_TTF_READER)

#include <cstring>

namespace ttfimg {

namespace {

constexpr int64_t kQ16 = 65536;
constexpr int kClaheTiles = 8;      // tile grid (XteinkImageRefiner regime)
constexpr int kClaheBins = 256;     // 8-bit histogram bins
constexpr int kClaheQ4 = 16;        // per-axis bilinear weight fixed point (0..16)
constexpr int kClaheClipDiv = 128;  // per-bin cap divisor: area/128 == clip 2.0

// dst edge i in SOURCE-axis units.
inline int64_t edgeQ16(const int i, const int src, const int dst) {
  return (static_cast<int64_t>(i) << 16) * src / dst;
}

// Source pixels a dst patch [e0q, e1q) touches, with Q16 overlap weights.
// e0/e1 stay in source units; `n` bounds the source-axis index.
template <typename Fn>
void cover(int64_t e0, const int64_t e1, const int n, Fn fn) {
  if (e1 <= e0 || n <= 0) return;
  int p0 = static_cast<int>(e0 >> 16);
  int p1 = static_cast<int>((e1 - 1) >> 16);
  if (p0 < 0) p0 = 0;
  if (p1 >= n) p1 = n - 1;
  if (p1 < p0) return;
  for (int p = p0; p <= p1; ++p) {
    const int64_t a = static_cast<int64_t>(p) * kQ16;
    const int64_t hi = (a + kQ16) < e1 ? (a + kQ16) : e1;
    const int64_t lo = a > e0 ? a : e0;
    const int64_t w = hi - lo;
    if (w > 0) fn(p, static_cast<uint32_t>(w));
    if ((p + 1) * kQ16 >= e1) break;
  }
}

// Overlap of dst band b ([edge(b), edge(b+1)) in source units) with source
// span [y0q, y1q): the inverse direction — edge() is already source units.
inline int64_t bandOverlap(const int b, const int64_t y0q, const int64_t y1q, const int srcH, const int dstH) {
  if (b < 0 || b >= dstH) return 0;
  const int64_t b0 = edgeQ16(b, srcH, dstH);
  const int64_t b1 = edgeQ16(b + 1, srcH, dstH);
  const int64_t lo = b0 > y0q ? b0 : y0q;
  const int64_t hi = b1 < y1q ? b1 : y1q;
  return hi > lo ? hi - lo : 0;
}

inline int bandFirst(const int64_t y0q, const int srcH, const int dstH) {
  int b = static_cast<int>((y0q / kQ16) * dstH / srcH);
  if (b > 0) --b;
  return b;
}

inline int bandLast(const int64_t y1q, const int srcH, const int dstH) {
  int b = static_cast<int>((y1q / kQ16) * dstH / srcH);
  if (b >= dstH) b = dstH - 1;
  return b;
}

// Streaming plan policy (reset4): pass 2 never runs on a streamed source —
// the per-destination-pixel full-band walk measured 895.9M inner iterations
// at 934x1400 -> 440x782 on host (~60 s on the 240 MHz S3; owner soak
// 2026-11-10 showed exactly that window at rung detail). Random plans keep
// pass 2 (BMP row decoder; the source pattern is already resident, so the
// detail pass buys stroke energy without a decode in the loop). Applied ONCE
// at the scratch/begin seam so no streamed band boundary re-questions it.
static DpidScaler::Plan effectivePlan(DpidScaler::Plan p) {
  if (p.detail && !p.random) p.detail = false;
  return p;
}

// Per-axis tile-center blend params (Q4 weights, sum 16). `n` is the scaled
// pixel-center coordinate (16ths of a pixel: 16·coord + 8); tile centers sit
// at (2·tx+1)·size in the same units — spacing 2·size. Out-of-image
// positions pin the edge tile: the closest center carries weight 16.
inline void claheBlendParams(const int n, const int size, int& tA, int& tB, int& wA, int& wB) {
  const int spacing = 2 * size;
  const int rel = n - size;  // relative to center 0 at 1·size
  if (rel < 0) {
    tA = tB = 0;
    wA = kClaheQ4;
    wB = 0;
    return;
  }
  if (rel >= spacing * (kClaheTiles - 1)) {
    tA = tB = kClaheTiles - 1;
    wA = kClaheQ4;
    wB = 0;
    return;
  }
  const int t = rel / spacing;
  const int f = rel - t * spacing;  // 0..spacing-1, distance toward center t+1
  tA = t;
  wA = (spacing - f) * kClaheQ4 / spacing;
  wB = kClaheQ4 - wA;
  tB = t + 1;  // t < kClaheTiles-1 here, so t+1 <= kClaheTiles-1
}

}  // namespace

// ── plan / memory ───────────────────────────────────────────────────────────

DpidScaler::Plan DpidScaler::plan(const Plan& dimsIn) {
  Plan p = dimsIn;
  if (p.srcW <= 0 || p.srcH <= 0 || p.dstW <= 0 || p.dstH <= 0) {
    p.detail = false;
    p.clahe = false;
    p.nearest = false;
  } else if (p.dstW >= p.srcW && p.dstH >= p.srcH) {
    // Small class: no box/pass-2 averaging — nearest-center picks keep
    // line art crisp at ornament scale-ups (blockiness design 2026-11-09).
    p.detail = false;
    p.nearest = true;
  }
  return p;
}

size_t DpidScaler::scratchBytes(const Plan& planIn) {
  const Plan p = effectivePlan(plan(planIn));
  const size_t dstW = static_cast<size_t>(p.dstW);
  const bool tooSmallForClahe = p.clahe && (p.dstW / kClaheTiles < 2 || p.dstH / kClaheTiles < 2);
  const bool needsImage = p.detail || (p.clahe && !tooSmallForClahe);
  size_t bytes = dstW * 8 * 2 + dstW;                   // acc + wtot accumulators, hold row
  if (p.nearest) bytes += static_cast<size_t>(p.srcW);  // nearest-sample row
  if (needsImage) bytes += dstW * static_cast<size_t>(p.dstH);
  if (needsImage && p.detail) bytes += dstW;           // tent row
  if (p.random) bytes += static_cast<size_t>(p.srcW);  // provider row scratch
  if (p.detail && p.random) {
    // Pass-2 band copy: every source row of one output row's patch, read
    // exactly once — only requested when it fits the bound (the carve is
    // then guaranteed; a taller patch falls back to the per-column path).
    const size_t rows = static_cast<size_t>((p.srcH + p.dstH - 1) / p.dstH) + 1;
    const size_t bandBytes = rows * (static_cast<size_t>(p.srcW) + sizeof(uint32_t));
    if (bandBytes + bandBytes / 4 <= kPass2BandBytesCap) bytes += bandBytes;
  }
  if (p.clahe && !tooSmallForClahe) {
    bytes += static_cast<size_t>(kClaheTiles * kClaheTiles) * kClaheBins * 2;  // hist u16
    bytes += static_cast<size_t>(kClaheTiles * kClaheTiles) * kClaheBins;      // LUT maps
  }
  return bytes;
}

bool DpidScaler::begin(const Plan& planIn, uint8_t* block, const size_t bytes, EmitFn emit, void* emitUser, PollFn poll,
                       void* pollUser, RowFn provider, void* providerUser) {
  ok_ = false;
  p_ = effectivePlan(plan(planIn));
  aborted_ = false;
  emit_ = emit;
  emitUser_ = emitUser;
  poll_ = poll;
  pollUser_ = pollUser;
  provider_ = provider;
  providerUser_ = providerUser;
  if (p_.srcW <= 0 || p_.srcH <= 0 || p_.dstW <= 0 || p_.dstH <= 0) return false;
  if (emit_ == nullptr) return false;
  if ((p_.detail || p_.clahe) && block == nullptr) return false;

  const size_t dstW = static_cast<size_t>(p_.dstW);
  const bool tooSmallForClahe = p_.clahe && (p_.dstW / kClaheTiles < 2 || p_.dstH / kClaheTiles < 2);
  const bool needsImage = p_.detail || (p_.clahe && !tooSmallForClahe);
  if (needsImage) {
    if (block == nullptr) return false;
  }

  curBand_ = -1;
  nearBest_ = 0;
  lastRand_ = -1;
  holdFull_ = false;
  finished_ = false;

  size_t off = 0;
  auto carve = [&](const size_t n) -> void* {
    if (block == nullptr || n == 0 || off + n > bytes || off + n < off) return nullptr;
    void* out = block + off;
    off += n;
    return out;
  };
  acc_ = static_cast<uint64_t*>(carve(dstW * 8));
  wtot_ = static_cast<uint64_t*>(carve(dstW * 8));
  hold_ = static_cast<uint8_t*>(carve(dstW));
  dst_ = nullptr;
  tent_ = nullptr;
  rowBuf_ = nullptr;
  near_ = nullptr;
  p2band_ = nullptr;
  p2wy_ = nullptr;
  p2bandRows_ = 0;
  readFailed_ = false;
  if (needsImage) {
    dst_ = static_cast<uint8_t*>(carve(dstW * static_cast<size_t>(p_.dstH)));
    if (p_.detail) tent_ = static_cast<uint8_t*>(carve(dstW));
  }
  if (p_.random) rowBuf_ = static_cast<uint8_t*>(carve(static_cast<size_t>(p_.srcW)));
  if (p_.detail && p_.random) {
    const size_t rows = static_cast<size_t>((p_.srcH + p_.dstH - 1) / p_.dstH) + 1;
    const size_t bandBytes = rows * (static_cast<size_t>(p_.srcW) + sizeof(uint32_t));
    if (bandBytes + bandBytes / 4 <= kPass2BandBytesCap) {
      p2bandRows_ = static_cast<int>(rows);
      p2band_ = static_cast<uint8_t*>(carve(rows * static_cast<size_t>(p_.srcW)));
      p2wy_ = static_cast<uint32_t*>(carve(rows * sizeof(uint32_t)));
      if (p2band_ == nullptr || p2wy_ == nullptr) {
        // The provider row scratch above already carved; losing the band
        // copy only costs throughput (same carve-failure shape as near_).
        p2band_ = nullptr;
        p2wy_ = nullptr;
        p2bandRows_ = 0;
      }
    }
  }
  tilesX_ = tilesY_ = 0;
  tileW_ = tileH_ = 0;
  claheHist_ = nullptr;
  claheMap_ = nullptr;
  if (p_.clahe && !tooSmallForClahe) {
    tilesX_ = tilesY_ = kClaheTiles;
    tileW_ = p_.dstW / kClaheTiles;
    tileH_ = p_.dstH / kClaheTiles;
    claheHist_ = static_cast<uint16_t*>(carve(sizeof(uint16_t) * kClaheTiles * kClaheTiles * kClaheBins));
    claheMap_ = static_cast<uint8_t*>(carve(static_cast<size_t>(kClaheTiles * kClaheTiles) * kClaheBins));
  }
  if (p_.nearest) {
    near_ = static_cast<uint8_t*>(carve(static_cast<size_t>(p_.srcW)));
    if (near_ == nullptr) {
      // Degraded rung (no block): stream the fractional box like the old
      // small class instead of failing — the blockless ladder keeps working.
      p_.nearest = false;
    }
  }
  if (acc_ == nullptr || wtot_ == nullptr || hold_ == nullptr) return false;
  if (needsImage && (dst_ == nullptr || (p_.detail && tent_ == nullptr))) return false;
  if (p_.detail && p_.random && rowBuf_ == nullptr) return false;
  if (p_.clahe && tilesX_ != 0 && (claheHist_ == nullptr || claheMap_ == nullptr)) return false;

  memset(acc_, 0, dstW * sizeof(uint64_t));
  memset(wtot_, 0, dstW * sizeof(uint64_t));
  if (dst_ != nullptr) memset(dst_, 0, dstW * static_cast<size_t>(p_.dstH));
  ok_ = true;
  return true;
}

bool DpidScaler::buffered() const { return ok_ && dst_ != nullptr; }
bool DpidScaler::needsFinish() const { return ok_ && !finished_; }

// ── pass 1 ──────────────────────────────────────────────────────────────────

void DpidScaler::accumulateRow(const int y, const uint8_t* gray) {
  if (p_.nearest) {
    // Nearest sampling: a destination band spans at most one source row.
    // Remember the highest-overlap covering row (for a ≤1-row span that is
    // exactly the row containing the band center), pick the nearest column
    // at flush.
    const int64_t y0q = static_cast<int64_t>(y) * kQ16;
    const int64_t y1q = y0q + kQ16;
    const int bFirst = bandFirst(y0q, p_.srcH, p_.dstH);
    const int bLast = bandLast(y1q, p_.srcH, p_.dstH);
    for (int band = bFirst; band <= bLast; ++band) {
      const int64_t ov = bandOverlap(band, y0q, y1q, p_.srcH, p_.dstH);
      if (ov <= 0) continue;
      if (band != curBand_) {
        const int ended = curBand_;
        if (ended >= 0) flushBand();
        curBand_ = band;
        nearBest_ = ov;
        memcpy(near_, gray, static_cast<size_t>(p_.srcW));
      } else if (ov > nearBest_) {
        nearBest_ = ov;
        memcpy(near_, gray, static_cast<size_t>(p_.srcW));
      }
    }
    return;
  }
  const int64_t y0q = static_cast<int64_t>(y) * kQ16;
  const int64_t y1q = y0q + kQ16;
  const int bFirst = bandFirst(y0q, p_.srcH, p_.dstH);
  const int bLast = bandLast(y1q, p_.srcH, p_.dstH);
  for (int band = bFirst; band <= bLast; ++band) {
    const int64_t wv = bandOverlap(band, y0q, y1q, p_.srcH, p_.dstH);
    if (wv <= 0) continue;
    if (band != curBand_) {
      if (curBand_ >= 0) flushBand();
      curBand_ = band;
      memset(acc_, 0, sizeof(uint64_t) * static_cast<size_t>(p_.dstW));
      memset(wtot_, 0, sizeof(uint64_t) * static_cast<size_t>(p_.dstW));
    }
    for (int x = 0; x < p_.dstW; ++x) {
      cover(edgeQ16(x, p_.srcW, p_.dstW), edgeQ16(x + 1, p_.srcW, p_.dstW), p_.srcW,
            [&](const int px, const uint32_t wx) {
              const uint64_t cov = static_cast<uint64_t>(wv) * wx;
              acc_[x] += cov * gray[px];
              wtot_[x] += cov;
            });
    }
  }
}

void DpidScaler::flushBand() {
  // Nearest sampling (small class): each source row covering a band spanned
  // at most one band, so the remembered row is THE nearest source row; the
  // picked column is the nearest-center mapping. When the image buffer is
  // carved (Enhanced: pass 2 is skipped for nearest but CLAHE runs before
  // finish()'s emit walk) the row lands in dst_ so the cache stream sees it
  // exactly once; bufferless plans stream straight to the sink.
  if (p_.nearest && near_ != nullptr) {
    uint8_t* out = dst_ != nullptr ? dst_ + static_cast<size_t>(p_.dstW) * curBand_ : hold_;
    for (int x = 0; x < p_.dstW; ++x) {
      int sx = (2 * x + 1) * p_.srcW / (2 * p_.dstW);
      if (sx >= p_.srcW) sx = p_.srcW - 1;
      out[x] = near_[sx];
    }
    if (dst_ == nullptr && !emit_(emitUser_, curBand_, hold_)) aborted_ = true;
    curBand_ = -1;
    return;
  }
  // Bufferless mode: the completed band streams straight to the emit hook —
  // pass 2 and CLAHE can never run without the full-image buffer.
  if (dst_ == nullptr) {
    for (int x = 0; x < p_.dstW; ++x) {
      const uint64_t t = wtot_[x];
      uint64_t d = t ? (acc_[x] + (t >> 1)) / t : 0;
      if (d > 255) d = 255;
      hold_[x] = static_cast<uint8_t>(d);
    }
    if (!emit_(emitUser_, curBand_, hold_)) aborted_ = true;
    curBand_ = -1;
    return;
  }
  uint8_t* out = dst_ + static_cast<size_t>(p_.dstW) * curBand_;
  for (int x = 0; x < p_.dstW; ++x) {
    const uint64_t t = wtot_[x];
    uint64_t d = t ? (acc_[x] + (t >> 1)) / t : 0;
    if (d > 255) d = 255;
    out[x] = static_cast<uint8_t>(d);
  }
  curBand_ = -1;
}

// ── pass 2 (in-place flush scheme) ─────────────────────────────────────────

// Destination row m: tent over D rows m-1..m+1 (edge tiles omitted and the
// kernel renormalized — dpid.m does not replicate edges), then band m's
// source patch re-accumulated with the detail weight |tent − pixel| ×
// coverage. In-place sequence per row: tent(m) first (it still needs
// D(m-1)); then the hold row's previous content (out(m-1)) flushes into slot
// m-1 — that D value is dead the moment tent(m) ran; then the fresh result
// goes to hold. Outputs never share a slot with a live D value; finish()
// flushes the hold after the last row.
void DpidScaler::pass2Row(const int m) {
  static constexpr int KT[3][3] = {{1, 2, 1}, {2, 4, 2}, {1, 2, 1}};
  const size_t dstW = static_cast<size_t>(p_.dstW);

  // 1. tent(m, x) for the whole row.
  for (int x = 0; x < p_.dstW; ++x) {
    int sum = 0, wt = 0;
    for (int dy = -1; dy <= 1; ++dy) {
      const int yy = m + dy;
      if (yy < 0 || yy >= p_.dstH) continue;
      const uint8_t* row = dst_ + dstW * yy;
      for (int dx = -1; dx <= 1; ++dx) {
        const int xx = x + dx;
        if (xx < 0 || xx >= p_.dstW) continue;
        sum += KT[dy + 1][dx + 1] * row[xx];
        wt += KT[dy + 1][dx + 1];
      }
    }
    tent_[x] = static_cast<uint8_t>((sum + wt / 2) / wt);
  }
  // 2. flush the previous hold (out m-1) into slot m-1.
  if (holdFull_) memcpy(dst_ + dstW * (m - 1), hold_, dstW);
  // 3. re-accumulate band m's patch with the detail weight; result -> hold.
  // Random mode only (the streaming plan drops pass 2; reset4 policy). The
  // patch rows are fetched ONCE into the band copy (a per-column readRow
  // walk refetches every patch row for each dst column — BMP provider rows
  // are full row decodes); identical integer terms in a different visit
  // order stay bit-identical (fixed-point, no rounding).
  const int64_t y0q = edgeQ16(m, p_.srcH, p_.dstH);
  const int64_t y1q = edgeQ16(m + 1, p_.srcH, p_.dstH);
  const int bandRows = copyPass2Band(y0q, y1q);
  for (int x = 0; x < p_.dstW; ++x) {
    uint64_t a = 0, fSum = 0;
    const int64_t e0q = edgeQ16(x, p_.srcW, p_.dstW);
    const int64_t e1q = edgeQ16(x + 1, p_.srcW, p_.dstW);
    if (bandRows >= 0) {
      // Pre-fetched band: identical terms to the cover()/cover() walk, so
      // the uint64 sums stay bit-identical (visit order doesn't round).
      for (int b = 0; b < bandRows; ++b) {
        const uint8_t* srow = p2band_ + static_cast<size_t>(b) * p_.srcW;
        const uint64_t wy = p2wy_[b];
        for (int px = 0; px < p_.srcW; ++px) {
          const int64_t px0 = static_cast<int64_t>(px) * kQ16;
          const int64_t px1 = px0 + kQ16;
          const int64_t plo = px0 > e0q ? px0 : e0q;
          const int64_t phi = px1 < e1q ? px1 : e1q;
          const uint64_t cov = wy * static_cast<uint64_t>(phi > plo ? phi - plo : 0);
          if (cov == 0) continue;
          const uint8_t s = srow[px];
          const uint64_t f = static_cast<uint64_t>(tent_[x] > s ? tent_[x] - s : s - tent_[x]) * cov;
          a += f * s;
          fSum += f;
        }
      }
    } else {
      int sFirst = static_cast<int>(y0q >> 16);
      int sLast = static_cast<int>((y1q - 1) >> 16);
      if (sFirst < 0) sFirst = 0;
      if (sLast >= p_.srcH) sLast = p_.srcH - 1;
      for (int sy = sFirst; sy <= sLast; ++sy) {
        const int64_t ply0 = static_cast<int64_t>(sy) * kQ16;
        const int64_t ply1 = ply0 + kQ16;
        const int64_t ylo = ply0 > y0q ? ply0 : y0q;
        const int64_t yhi = ply1 < y1q ? ply1 : y1q;
        const uint64_t wy = static_cast<uint64_t>(yhi > ylo ? yhi - ylo : 0);
        if (wy == 0) continue;
        const uint8_t* srow = readRow(sy);
        for (int px = 0; px < p_.srcW; ++px) {
          const int64_t px0 = static_cast<int64_t>(px) * kQ16;
          const int64_t px1 = px0 + kQ16;
          const int64_t plo = px0 > e0q ? px0 : e0q;
          const int64_t phi = px1 < e1q ? px1 : e1q;
          const uint64_t cov = wy * static_cast<uint64_t>(phi > plo ? phi - plo : 0);
          if (cov == 0) continue;
          const uint8_t s = srow[px];
          const uint64_t f = static_cast<uint64_t>(tent_[x] > s ? tent_[x] - s : s - tent_[x]) * cov;
          a += f * s;
          fSum += f;
        }
      }
    }
    uint64_t o = fSum ? (a + (fSum >> 1)) / fSum : static_cast<uint64_t>((dst_ + dstW * m)[x]);
    if (o > 255) o = 255;
    hold_[x] = static_cast<uint8_t>(o);
  }
  holdFull_ = true;
}

uint8_t* DpidScaler::readRow(const int sy) {
  if (lastRand_ != sy) {
    if (!provider_(providerUser_, sy, rowBuf_)) readFailed_ = true;
    lastRand_ = sy;
  }
  return rowBuf_;
}

// Copies the pass-2 vertical patch rows of one output row into p2band_/
// p2wy_ so pass 2 reads each source row exactly once (see DpidScaler.h).
// Returns -1 when the buffer is unavailable (per-column readRow fallback).
int DpidScaler::copyPass2Band(const int64_t y0q, const int64_t y1q) {
  if (p2band_ == nullptr || p2wy_ == nullptr) return -1;
  int rows = 0;
  cover(y0q, y1q, p_.srcH, [&](const int sy, const uint32_t wy) {
    const uint8_t* srow = readRow(sy);
    memcpy(p2band_ + static_cast<size_t>(rows) * p_.srcW, srow, static_cast<size_t>(p_.srcW));
    p2wy_[rows] = wy;
    ++rows;
  });
  if (readFailed_) return -1;
  return rows;
}

// ── CLAHE (in place, 8x8 tile grid, clip 2.0 regime) ────────────────────────

bool DpidScaler::clahePass() {
  const size_t dstW = static_cast<size_t>(p_.dstW);
  // Tile histograms (exact cell boundaries; the per-pixel index uses the
  // same y·tiles/H, x·tiles/W mapping the LUT bounds reuse below).
  memset(claheHist_, 0, sizeof(uint16_t) * kClaheTiles * kClaheTiles * kClaheBins);
  for (int y = 0; y < p_.dstH; ++y) {
    const int ty = y * kClaheTiles / p_.dstH;
    for (int x = 0; x < p_.dstW; ++x) {
      const int tx = x * kClaheTiles / p_.dstW;
      ++claheHist_[(static_cast<size_t>(ty) * kClaheTiles + tx) * kClaheBins + dst_[dstW * y + x]];
    }
  }
  // Per-tile clip + redistribute + CDF -> LUT.
  for (int t = 0; t < kClaheTiles * kClaheTiles; ++t) {
    uint16_t* h = claheHist_ + static_cast<size_t>(t) * kClaheBins;
    uint8_t* map = claheMap_ + static_cast<size_t>(t) * kClaheBins;
    const int tX = t % kClaheTiles, tY = t / kClaheTiles;
    const int x0 = tX * p_.dstW / kClaheTiles, x1 = (tX + 1) * p_.dstW / kClaheTiles;
    const int y0 = tY * p_.dstH / kClaheTiles, y1 = (tY + 1) * p_.dstH / kClaheTiles;
    const int area = (y1 - y0) * (x1 - x0);
    if (area <= 0) {
      for (int i = 0; i < kClaheBins; ++i) map[i] = static_cast<uint8_t>(i);
      continue;
    }
    int limit = area / kClaheClipDiv;
    if (limit < 1) limit = 1;
    // Constant tile (all pixels one level): clip+redistribute would invent a
    // ramp and drift the level (probe: 137 -> 142); identity keeps the
    // texture campaign's flat-field invariant on flat artwork.
    int flatBin = -1;
    bool constant = true;
    for (int i = 0; i < kClaheBins; ++i) {
      if (h[i] == 0) continue;
      if (flatBin >= 0) {
        constant = false;
        break;
      }
      flatBin = i;
    }
    if (constant && flatBin >= 0) {
      for (int i = 0; i < kClaheBins; ++i) map[i] = static_cast<uint8_t>(i);
      continue;
    }
    // Redistribution can revive capped bins; bounded rounds keep the loop
    // deterministic (documented degradation, never unbounded).
    for (int round = 0; round < 4; ++round) {
      int excess = 0;
      for (int i = 0; i < kClaheBins; ++i) {
        if (h[i] > limit) {
          excess += h[i] - limit;
          h[i] = static_cast<uint16_t>(limit);
        }
      }
      if (excess == 0) break;
      const int add = excess / kClaheBins;
      const int rem = excess - add * kClaheBins;
      for (int i = 0; i < kClaheBins; ++i) {
        int v = h[i] + add;
        if (i < rem) ++v;
        if (v > 0xFFFF) v = 0xFFFF;
        h[i] = static_cast<uint16_t>(v);
      }
    }
    uint32_t cdf[kClaheBins];
    uint32_t total = 0;
    for (int i = 0; i < kClaheBins; ++i) {
      total += h[i];
      cdf[i] = total;
    }
    int i0 = 0;
    while (i0 < kClaheBins && cdf[i0] == 0) ++i0;
    if (i0 >= kClaheBins) {
      for (int i = 0; i < kClaheBins; ++i) map[i] = static_cast<uint8_t>(i);
      continue;
    }
    const uint32_t cdfMin = cdf[i0];
    const int64_t range = static_cast<int64_t>(total) - cdfMin;
    if (range <= 0) {
      // Degenerate single-level tile: identity keeps the tone true.
      for (int i = 0; i < kClaheBins; ++i) map[i] = static_cast<uint8_t>(i);
      continue;
    }
    for (int i = 0; i < kClaheBins; ++i) {
      int64_t o =
          i < i0 ? 0 : ((static_cast<int64_t>(cdf[i]) - static_cast<int64_t>(cdfMin)) * 255 + range / 2) / range;
      if (o < 0) o = 0;
      if (o > 255) o = 255;
      map[i] = static_cast<uint8_t>(o);
    }
  }
  // In-place rewrite: bilinear blend of the 4 surrounding tile centers,
  // horizontal then vertical at Q4 fixed point. Side maps share their tile-
  // row base, indexed by the column tile; the two vertical bases come from
  // the same center geometry as the column pairs.
  for (int y = 0; y < p_.dstH; ++y) {
    int tYA, tYB, wYA, wYB;
    claheBlendParams(y * 16 + 8, p_.dstH, tYA, tYB, wYA, wYB);
    const uint8_t* mapTop = claheMap_ + static_cast<size_t>(tYA) * kClaheTiles * kClaheBins;
    const uint8_t* mapBot = claheMap_ + static_cast<size_t>(tYB) * kClaheTiles * kClaheBins;
    uint8_t* row = dst_ + dstW * y;
    for (int x = 0; x < p_.dstW; ++x) {
      int tXA, tXB, wXA, wXB;
      claheBlendParams(x * 16 + 8, p_.dstW, tXA, tXB, wXA, wXB);
      const uint8_t g = row[x];
      const int hTop = (mapTop[tXA * kClaheBins + g] * wXA + mapTop[tXB * kClaheBins + g] * wXB + 8) / kClaheQ4;
      const int hBot = (mapBot[tXA * kClaheBins + g] * wXA + mapBot[tXB * kClaheBins + g] * wXB + 8) / kClaheQ4;
      int o = (hTop * wYA + hBot * wYB + 8) / kClaheQ4;
      if (o < 0) o = 0;
      if (o > 255) o = 255;
      row[x] = static_cast<uint8_t>(o);
    }
    if (poll_ != nullptr && poll_(pollUser_)) return false;
  }
  return true;
}

// ── driver ─────────────────────────────────────────────────────────────────

// Streaming mode: one completed source row in raster order. Pass 1 only.
void DpidScaler::feedSourceRow(const int y, const uint8_t* gray) {
  if (!ok_ || aborted_) return;
  accumulateRow(y, gray);
}

// Random-access mode: pass 1 over the provider; pass 2 runs at finish().
bool DpidScaler::runRandom(RowFn provider, void* providerUser) {
  if (!ok_ || aborted_ || provider == nullptr || !p_.random) return false;
  if (provider_ == nullptr) {
    provider_ = provider;
    providerUser_ = providerUser;
  }
  for (int y = 0; y < p_.srcH; ++y) {
    if (!provider_(providerUser_, y, rowBuf_)) {
      readFailed_ = true;
      return false;  // sticky: a failed row decodes garbage into the cache
    }
    lastRand_ = y;
    accumulateRow(y, rowBuf_);
    if (aborted_) return false;
  }
  return !aborted_ && !readFailed_;
}

bool DpidScaler::finish() {
  if (!ok_) return false;
  if (finished_) return true;
  finished_ = true;
  if (readFailed_) return false;  // provider row decode failed mid-run
  if (aborted_) return false;     // emit refusal / takeover: report via finish() too

  // Last pass-1 band + the random pass-2 rows it unblocks.
  if (curBand_ >= 0) flushBand();
  if (dst_ == nullptr) return true;  // bufferless: rows already streamed out
  // Random detail: every band flushed, so every pass-2 patch is resident;
  // row dstH-1 has no row below it, so it always emits.
  if (p_.detail && p_.random) {
    for (int m = 0; m < p_.dstH; ++m) {
      pass2Row(m);
      if (readFailed_) return false;  // provider rows died inside pass 2
    }
  }
  // Final hold flush — slot dstH-1's D value died at the last tent(m).
  if (holdFull_ && p_.detail) {
    memcpy(dst_ + static_cast<size_t>(p_.dstW) * (p_.dstH - 1), hold_, static_cast<size_t>(p_.dstW));
    holdFull_ = false;
  }
  // Enhanced: local contrast before the bake sees the rows.
  if (p_.clahe && tilesX_ != 0 && !clahePass()) return false;

  // Emit walk: rows feed ToneBake4's collectRow; false = Busy upstream.
  const size_t dstW = static_cast<size_t>(p_.dstW);
  for (int r = 0; r < p_.dstH; ++r) {
    if (poll_ != nullptr && poll_(pollUser_)) return false;
    if (!emit_(emitUser_, r, dst_ + dstW * r)) return false;
  }
  return true;
}

}  // namespace ttfimg

#endif  // CROSSPOINT_TTF_READER
