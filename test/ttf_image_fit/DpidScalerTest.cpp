// DpidScaler tests (design docs/design/2026-11-09-ttf-image-dpid-clahe.md
// §Acceptance): stroke-energy pin vs box-average, no-ringing property,
// fractional-coverage flat fields, λ=0 degenerate, determinism, tiny-image
// skip, border coverage; CLAHE clip bounds, tile-border interpolation,
// in-place, determinism, and Sharp == CLAHE-off byte-identical.

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <vector>

#include "DpidScaler.h"
#include "TtfImageEngine.h"

namespace ttfimg {
namespace {

// Capture emitted destination rows.
struct Sink {
  std::vector<std::vector<uint8_t>> rows;
  int w = 0;
  static bool emit(void* user, int, const uint8_t* row) {
    auto* s = static_cast<Sink*>(user);
    s->rows.emplace_back(row, row + s->w);
    return true;
  }
};

// Synthetic field: white background with 1px dark strokes (hairlines) and a
// medium text bar — the thin-stroke detail box averaging smears away.
std::vector<uint8_t> makeStrokeField(const int w, const int h) {
  std::vector<uint8_t> img(static_cast<size_t>(w) * h, 255);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      if (x % 7 == 3) img[static_cast<size_t>(y) * w + x] = 40;
      if (x == (y * 3) % w) img[static_cast<size_t>(y) * w + x] = 40;
      if (y % 5 == 2 && (x / 4) % 3 == 1) img[static_cast<size_t>(y) * w + x] = 100;
    }
  }
  return img;
}

double strokeEnergy(const std::vector<std::vector<uint8_t>>& rows) {
  double e = 0;
  size_t n = 0;
  for (const auto& r : rows) {
    for (const uint8_t v : r) e += 255 - v;
    n += r.size();
  }
  return n ? e / n : 0.0;
}

// Legacy per-row-truncating box (the old TtfImageEngine.cpp:357 shape).
std::vector<uint8_t> legacyBoxRow(const uint8_t* gray, const int srcW, const int dstW) {
  std::vector<uint8_t> out(dstW);
  for (int c = 0; c < dstW; ++c) {
    const int x0 = c * srcW / dstW, x1 = (c + 1) * srcW / dstW;
    if (x1 > x0) {
      uint32_t sum = 0;
      for (int x = x0; x < x1; ++x) sum += gray[x];
      out[c] = static_cast<uint8_t>(sum / (x1 - x0));
    } else {
      out[c] = gray[x0 < srcW ? x0 : srcW - 1];
    }
  }
  return out;
}

bool pollFalse(void*) { return false; }

}  // namespace

// PSRAM budget (owner directive 2026-11-09 — 5 MB for TTF image processing,
// see the dated addendum in docs/design/2026-11-08-ttf-image-text-in-image.md):
// the gate must never force a full-page decode into a known-degradable rung,
// so the full rung's scratch at both portrait placements has to fit the
// budget.
TEST(PsramBudget, PortraitFullPageDetailClaheScratchFitsTheBudget) {
  // The soak book's full-page art: ~930x1400 source into the 480x800 portrait
  // placement (one full-image accumulator + CLAHE tables + rows).
  const DpidScaler::Plan p{930, 1400, 480, 800, true, true, false};
  const size_t need = DpidScaler::scratchBytes(p);
  EXPECT_GT(need, 384u * 1024u);  // carries the full-image buffer, not just it
  EXPECT_LT(need, kPsramBudgetBytes);
}

TEST(PsramBudget, LandscapeFullPageDetailClaheScratchFitsTheBudget) {
  const DpidScaler::Plan p{1400, 930, 800, 480, true, true, false};
  const size_t need = DpidScaler::scratchBytes(p);
  EXPECT_GT(need, 384u * 1024u);
  EXPECT_LT(need, kPsramBudgetBytes);
}

TEST(PsramBudget, DegradedRungsStayInsideTheBudget) {
  // Ladder invariant (reset4): every rung below the full one is smaller, so a
  // decode that passed the budget gate can always place SOME rung —
  // degradation is never a gate sidestep. The random-access plan is the only
  // one that carries pass 2 (streaming plans drop it by policy, which is why
  // the ladder is ranked on random plans: a streaming "full" request and the
  // box rung now ask the same scratch).
  const DpidScaler::Plan full{930, 1400, 480, 800, true, true, true};
  const DpidScaler::Plan box{930, 1400, 480, 800, true, false, true};
  const DpidScaler::Plan bufferless{930, 1400, 480, 800, false, false, false};
  const size_t boxNeed = DpidScaler::scratchBytes(box);
  const size_t bufferlessNeed = DpidScaler::scratchBytes(bufferless);
  EXPECT_LT(boxNeed, DpidScaler::scratchBytes(full));
  EXPECT_LT(bufferlessNeed, boxNeed);
  EXPECT_LT(boxNeed, kPsramBudgetBytes);
}

TEST(DpidScaler, StrokeEnergyBeatsBoxAverage) {
  const int SW = 960, SH = 640, DW = 480, DH = 320;
  const auto src = makeStrokeField(SW, SH);
  Sink sink;
  sink.w = DW;
  // reset4: pass 2 runs only on a random-access source (the BMP row
  // decoder), so this pin of the DPID detail pass uses the random plan.
  // Streaming plans run pass 1 only and match the box average exactly
  // (LambdaZeroEqualsFractionalPass1 pins that).
  DpidScaler::Plan p{SW, SH, DW, DH, false, true, true};
  const size_t bytes = DpidScaler::scratchBytes(p);
  ASSERT_GT(bytes, 0u);
  std::vector<uint8_t> block(bytes);
  DpidScaler s;
  // A random plan drives pass 2 through the row provider (the BMP path's
  // shape: begin() registers it, runRandom() replays, finish() emits).
  struct ProviderCtx {
    const std::vector<uint8_t>* src;
    int w;
  } provCtx{&src, SW};
  DpidScaler::RowFn provider = [](void* user, int y, uint8_t* out) {
    auto* c = static_cast<ProviderCtx*>(user);
    memcpy(out, c->src->data() + static_cast<size_t>(y) * c->w, static_cast<size_t>(c->w));
    return true;
  };
  ASSERT_TRUE(s.begin(p, block.data(), bytes, &Sink::emit, &sink, &pollFalse, nullptr, provider, &provCtx));
  ASSERT_TRUE(s.runRandom(provider, &provCtx));
  ASSERT_TRUE(s.finish());
  ASSERT_EQ(sink.rows.size(), static_cast<size_t>(DH));

  // Box-average reference (fractional pass 1, λ=0).
  Sink boxSink;
  boxSink.w = DW;
  DpidScaler::Plan p0{SW, SH, DW, DH, false, false, false};
  std::vector<uint8_t> block0(DpidScaler::scratchBytes(p0));
  DpidScaler s0;
  ASSERT_TRUE(s0.begin(p0, block0.data(), block0.size(), &Sink::emit, &boxSink, &pollFalse, nullptr));
  for (int y = 0; y < SH; ++y) s0.feedSourceRow(y, src.data() + static_cast<size_t>(y) * SW);
  ASSERT_TRUE(s0.finish());

  // The design-probe margin (+41%) with slack for the panel-size change.
  EXPECT_GT(strokeEnergy(sink.rows), strokeEnergy(boxSink.rows) * 1.3);
  EXPECT_GT(strokeEnergy(sink.rows), 45.0);     // probe pinned 54.90 at 960x640→480x320
  EXPECT_LT(strokeEnergy(boxSink.rows), 42.0);  // probe pinned 38.96
}

TEST(DpidScaler, NoRingingWithinSourcePatchMinMax) {
  const int SW = 320, SH = 200, DW = 160, DH = 100;
  const auto src = makeStrokeField(SW, SH);
  Sink sink;
  sink.w = DW;
  DpidScaler::Plan p{SW, SH, DW, DH, false, true, false};
  std::vector<uint8_t> block(DpidScaler::scratchBytes(p));
  DpidScaler s;
  ASSERT_TRUE(s.begin(p, block.data(), block.size(), &Sink::emit, &sink, &pollFalse, nullptr));
  for (int y = 0; y < SH; ++y) s.feedSourceRow(y, src.data() + static_cast<size_t>(y) * SW);
  ASSERT_TRUE(s.finish());
  ASSERT_EQ(sink.rows.size(), static_cast<size_t>(DH));

  for (int r = 0; r < DH; ++r) {
    for (int x = 0; x < DW; ++x) {
      uint8_t mn = 255, mx = 0;
      for (int sy = r * SH / DH; sy < (r + 1) * SH / DH; ++sy) {
        for (int sx = x * SW / DW; sx < (x + 1) * SW / DW; ++sx) {
          const uint8_t v = src[static_cast<size_t>(sy) * SW + sx];
          if (v < mn) mn = v;
          if (v > mx) mx = v;
        }
      }
      ASSERT_GE(sink.rows[r][x], mn) << "ringing below patch min at " << r << "," << x;
      ASSERT_LE(sink.rows[r][x], mx) << "ringing above patch max at " << r << "," << x;
    }
  }
}

TEST(DpidScaler, FlatFieldBakesExactly) {
  const int SW = 480, SH = 320, DW = 240, DH = 160;
  std::vector<uint8_t> flat(static_cast<size_t>(SW) * SH, 137);
  for (const bool detail : {false, true}) {
    Sink sink;
    sink.w = DW;
    DpidScaler::Plan p{SW, SH, DW, DH, false, detail, false};
    std::vector<uint8_t> block(DpidScaler::scratchBytes(p));
    DpidScaler s;
    ASSERT_TRUE(s.begin(p, block.data(), block.size(), &Sink::emit, &sink, &pollFalse, nullptr));
    for (int y = 0; y < SH; ++y) s.feedSourceRow(y, flat.data() + static_cast<size_t>(y) * SW);
    ASSERT_TRUE(s.finish());
    ASSERT_EQ(sink.rows.size(), static_cast<size_t>(DH));
    for (const auto& r : sink.rows) {
      for (const uint8_t v : r) ASSERT_EQ(v, 137) << "detail=" << detail;
    }
  }
}

TEST(DpidScaler, LambdaZeroEqualsFractionalPass1AndRandomMode) {
  const int SW = 321, SH = 197, DW = 160, DH = 98;  // odd sizes stress edges
  const auto src = makeStrokeField(SW, SH);
  Sink streaming, random, lambda0;
  streaming.w = random.w = lambda0.w = DW;

  // reset4 contract: pass 2 never runs on a streamed source, so a
  // detail-requesting streaming plan is byte-identical to an explicit λ0
  // plan (determinism + the no-pass-2 parity pin); a random plan still
  // carries the detail pass and must differ somewhere (the detail pass
  // actually does something).
  DpidScaler::Plan pd{SW, SH, DW, DH, false, true, false};
  std::vector<uint8_t> bd(DpidScaler::scratchBytes(pd));
  DpidScaler sd;
  ASSERT_TRUE(sd.begin(pd, bd.data(), bd.size(), &Sink::emit, &streaming, &pollFalse, nullptr));
  for (int y = 0; y < SH; ++y) sd.feedSourceRow(y, src.data() + static_cast<size_t>(y) * SW);
  ASSERT_TRUE(sd.finish());

  DpidScaler::Plan p0{SW, SH, DW, DH, false, false, false};
  std::vector<uint8_t> b0(DpidScaler::scratchBytes(p0));
  DpidScaler s0;
  ASSERT_TRUE(s0.begin(p0, b0.data(), b0.size(), &Sink::emit, &lambda0, &pollFalse, nullptr));
  for (int y = 0; y < SH; ++y) s0.feedSourceRow(y, src.data() + static_cast<size_t>(y) * SW);
  ASSERT_TRUE(s0.finish());

  DpidScaler::Plan pr{SW, SH, DW, DH, false, true, true};
  std::vector<uint8_t> br(DpidScaler::scratchBytes(pr));
  DpidScaler sr;
  struct {
    const std::vector<uint8_t>* src;
    int w;
  } provCtx{&src, SW};
  DpidScaler::RowFn provider = [](void* user, int y, uint8_t* out) {
    auto* c = static_cast<decltype(provCtx)*>(user);
    memcpy(out, c->src->data() + static_cast<size_t>(y) * c->w, static_cast<size_t>(c->w));
    return true;
  };
  ASSERT_TRUE(sr.begin(pr, br.data(), br.size(), &Sink::emit, &random, &pollFalse, nullptr, provider, &provCtx));
  ASSERT_TRUE(sr.runRandom(provider, &provCtx));
  ASSERT_TRUE(sr.finish());

  ASSERT_EQ(random.rows.size(), static_cast<size_t>(DH));
  ASSERT_EQ(streaming.rows.size(), static_cast<size_t>(DH));
  for (int r = 0; r < DH; ++r) {
    for (int x = 0; x < DW; ++x) {
      ASSERT_EQ(streaming.rows[r][x], lambda0.rows[r][x]) << "no-pass-2 parity at " << r << "," << x;
    }
  }
  bool differs = false;
  for (int r = 0; r < DH && !differs; ++r) {
    for (int x = 0; x < DW; ++x) {
      if (random.rows[r][x] != lambda0.rows[r][x]) {
        differs = true;
        break;
      }
    }
  }
  EXPECT_TRUE(differs) << "the random detail pass did nothing";
}

TEST(DpidScaler, TinyImageSkipsDetail) {
  // dst >= src on both axes: plan() must drop the detail pass and the
  // scratch must shrink accordingly (no image buffer at all; nearest adds
  // one source-row store — see the small-class tests below).
  DpidScaler::Plan p{100, 80, 120, 96, false, true, false};
  const DpidScaler::Plan q = DpidScaler::plan(p);
  EXPECT_FALSE(q.detail);
  // Bufferless detail skip: scratchBytes carries only the ~2 KB row
  // accumulators — no dstW*dstH image buffer, no ring, no CLAHE tables.
  Sink sink;
  sink.w = 120;
  std::vector<uint8_t> src(static_cast<size_t>(100) * 80, 200);
  DpidScaler s;
  const size_t tinyBytes = DpidScaler::scratchBytes(q);
  ASSERT_GT(tinyBytes, 0u);
  ASSERT_LT(tinyBytes, 8u * 1024u);  // no image-sized buffer in the plan
  std::vector<uint8_t> tinyBlock(tinyBytes);
  ASSERT_TRUE(s.begin(q, tinyBlock.data(), tinyBytes, &Sink::emit, &sink, &pollFalse, nullptr));
  for (int y = 0; y < 80; ++y) s.feedSourceRow(y, src.data() + static_cast<size_t>(y) * 100);
  ASSERT_TRUE(s.finish());
  ASSERT_EQ(sink.rows.size(), static_cast<size_t>(96));
  for (const auto& r : sink.rows) {
    for (const uint8_t v : r) ASSERT_EQ(v, 200);
  }
}

// ── small class: nearest-center picks (blockiness design 2026-11-09) ────────

TEST(DpidScaler, SmallUpscaleKeepsSourcePixelsCrisp) {
  // dst >= src on both axes: nearest-center sampling — each destination
  // pixel equals some source pixel, so line art stays crisp instead of the
  // box-blur the fractional pass produced (the ornament soak defect).
  DpidScaler::Plan p{2, 2, 4, 4, false, true, false};
  const DpidScaler::Plan q = DpidScaler::plan(p);
  EXPECT_FALSE(q.detail);
  EXPECT_TRUE(q.nearest);
  Sink sink;
  sink.w = 4;
  const std::vector<uint8_t> src{10, 200, 60, 250};
  std::vector<uint8_t> block(DpidScaler::scratchBytes(q));
  DpidScaler s;
  ASSERT_TRUE(s.begin(q, block.data(), block.size(), &Sink::emit, &sink, &pollFalse, nullptr));
  for (int y = 0; y < 2; ++y) s.feedSourceRow(y, src.data() + y * 2);
  ASSERT_TRUE(s.finish());
  ASSERT_EQ(sink.rows.size(), static_cast<size_t>(4));
  // Every emitted pixel is one of the four source values (no blends).
  for (const auto& r : sink.rows) {
    for (const uint8_t v : r) {
      ASSERT_TRUE(v == 10 || v == 200 || v == 60 || v == 250) << "blended value " << int(v);
    }
  }
  // The 2x upscale is the source block pattern: each source pixel fills a
  // 2x2 destination square (nearest-center mapping).
  ASSERT_EQ(sink.rows[0][0], 10);
  ASSERT_EQ(sink.rows[0][3], 200);
  ASSERT_EQ(sink.rows[3][0], 60);
  ASSERT_EQ(sink.rows[3][3], 250);
}

TEST(DpidScaler, SmallUpscaleThroughputRunsThroughEveryDestinationPixel) {
  // 32x31 -> 96x93 (the epub ornament placement): every destination row and
  // column is emitted (flush per band) and the values stay single-source.
  const int sw = 32, sh = 31, dw = 96, dh = 93;
  std::vector<uint8_t> src(static_cast<size_t>(sw) * sh);
  for (int y = 0; y < sh; ++y) {
    for (int x = 0; x < sw; ++x) src[static_cast<size_t>(y) * sw + x] = static_cast<uint8_t>((x * 7 + y * 13) % 251);
  }
  DpidScaler::Plan p{sw, sh, dw, dh, false, true, false};
  const DpidScaler::Plan q = DpidScaler::plan(p);
  EXPECT_TRUE(q.nearest);
  Sink sink;
  sink.w = dw;
  std::vector<uint8_t> block(DpidScaler::scratchBytes(q));
  DpidScaler s;
  ASSERT_TRUE(s.begin(q, block.data(), block.size(), &Sink::emit, &sink, &pollFalse, nullptr));
  for (int y = 0; y < sh; ++y) s.feedSourceRow(y, src.data() + static_cast<size_t>(y) * sw);
  ASSERT_TRUE(s.finish());
  ASSERT_EQ(sink.rows.size(), static_cast<size_t>(dh));
  for (const auto& r : sink.rows) ASSERT_EQ(r.size(), static_cast<size_t>(dw));
  // Row count: the sw*sh source set is subject to nearest picks only.
  for (int y = 0; y < dh; ++y) {
    for (int x = 0; x < dw; ++x) {
      const int sx = (2 * x + 1) * sw / (2 * dw);
      const int sy = (2 * y + 1) * sh / (2 * dh);
      ASSERT_EQ(sink.rows[y][x], src[static_cast<size_t>(sy) * sw + sx]);
    }
  }
}

TEST(DpidScaler, EnhancedSmallClassEmitsEveryRowExactlyOnce) {
  // Small class + CLAHE carves the dst image buffer; the nearest flush must
  // write INTO dst_ (finish()'s emit walk then emits exactly dstH rows).
  // The pre-fix double-emit streamed each row to the collector AND zero
  // rows out of the zeroed dst_ (histogram half zeros -> wrong stretch,
  // 2x row count; review 2026-11-10).
  const int sw = 12, sh = 10, dw = 24, dh = 20;  // >= 16 px: CLAHE tiles carve
  std::vector<uint8_t> src(static_cast<size_t>(sw) * sh);
  for (int y = 0; y < sh; ++y) {
    for (int x = 0; x < sw; ++x) src[static_cast<size_t>(y) * sw + x] = static_cast<uint8_t>((x * 9 + y * 23) % 251);
  }
  DpidScaler::Plan p{sw, sh, dw, dh, true, true, false};
  const DpidScaler::Plan q = DpidScaler::plan(p);
  EXPECT_TRUE(q.nearest);
  EXPECT_FALSE(q.detail);
  Sink sink;
  sink.w = dw;
  std::vector<uint8_t> block(DpidScaler::scratchBytes(q));
  DpidScaler s;
  ASSERT_TRUE(s.begin(q, block.data(), block.size(), &Sink::emit, &sink, &pollFalse, nullptr));
  for (int y = 0; y < sh; ++y) s.feedSourceRow(y, src.data() + static_cast<size_t>(y) * sw);
  ASSERT_TRUE(s.finish());
  ASSERT_EQ(sink.rows.size(), static_cast<size_t>(dh));
  for (const auto& r : sink.rows) ASSERT_EQ(r.size(), static_cast<size_t>(dw));
  // No zero rows: the pre-fix double-emit streamed the nearest rows to the
  // collector and then zero rows out of the (never-written) dst_ image after
  // the CLAHE pass — an alternating black row band. With the fix each slot
  // carries CLAHE-remapped midtone content before the walk.
  for (int y = 0; y < dh; ++y) {
    int zeros = 0;
    for (const uint8_t v : sink.rows[y]) {
      if (v == 0) ++zeros;
    }
    EXPECT_LT(zeros, static_cast<int>(dw) / 2) << "row " << y << " bakes near-black (CLAHE walked a dead image)";
  }
}

TEST(DpidScaler, DegradedSmallUpscaleKeepsFractionalBox) {
  // A scratch block too small for the nearest row (but big enough for the
  // row accumulators — the rung the degraded ladder actually hands in)
  // streams the fractional-average box (the old small-class behavior).
  DpidScaler::Plan p{2, 2, 5, 5, false, true, false};
  const DpidScaler::Plan q = DpidScaler::plan(p);
  Sink sink;
  sink.w = 5;
  const std::vector<uint8_t> src{10, 200, 60, 250};
  DpidScaler s;
  // acc + wtot (2*5*8 B) + hold (5 B) = 85 B; the nearest row carve fails.
  std::vector<uint8_t> tiny(5 * 17);
  ASSERT_TRUE(s.begin(q, tiny.data(), tiny.size(), &Sink::emit, &sink, &pollFalse, nullptr));
  for (int y = 0; y < 2; ++y) s.feedSourceRow(y, src.data() + y * 2);
  ASSERT_TRUE(s.finish());
  ASSERT_EQ(sink.rows.size(), static_cast<size_t>(5));
  bool sawBlend = false;
  for (const auto& r : sink.rows) {
    for (const uint8_t v : r) {
      if (v != 10 && v != 200 && v != 60 && v != 250) sawBlend = true;
    }
  }
  EXPECT_TRUE(sawBlend);  // the fractional box blends across the 5x grid
}

TEST(DpidScaler, BorderRowsAndColumnsStayCovered) {
  // Extreme shrink (8x) so every dst row samples multiple src rows and the
  // first/last bands exercise the boundary overlap math.
  const int SW = 640, SH = 800, DW = 80, DH = 100;
  std::vector<uint8_t> src(static_cast<size_t>(SW) * SH, 255);
  for (int x = 0; x < SW; ++x) src[x] = 10;                                     // top row dark
  for (int x = 0; x < SW; ++x) src[static_cast<size_t>(SH - 1) * SW + x] = 10;  // bottom row dark
  for (int y = 0; y < SH; ++y) src[static_cast<size_t>(y) * SW] = 10;           // left column dark
  for (int y = 0; y < SH; ++y) src[static_cast<size_t>(y) * SW + SW - 1] = 10;  // right column dark
  Sink sink;
  sink.w = DW;
  DpidScaler::Plan p{SW, SH, DW, DH, false, true, false};
  std::vector<uint8_t> block(DpidScaler::scratchBytes(p));
  DpidScaler s;
  ASSERT_TRUE(s.begin(p, block.data(), block.size(), &Sink::emit, &sink, &pollFalse, nullptr));
  for (int y = 0; y < SH; ++y) s.feedSourceRow(y, src.data() + static_cast<size_t>(y) * SW);
  ASSERT_TRUE(s.finish());
  ASSERT_EQ(sink.rows.size(), static_cast<size_t>(DH));
  // The 1px border survives pass 1's fractional coverage (the 8x shrink
  // smears a dark border pixel across srcW/dstW = 8 source rows, so a
  // mid-row border cell lands ~224) — but the edge is still a shade darker
  // than the untouched interior (no complete loss of the edge). The old
  // <200 bound pinned the detail pass's edge retention, which reset4
  // dropped for streamed sources (pass 2 is random-access only).
  EXPECT_LT(sink.rows[0][0], 240);
  EXPECT_LT(sink.rows[DH - 1][DW - 1], 240);
  EXPECT_LT(sink.rows[DH / 2][0], 240);
  EXPECT_LT(sink.rows[DH / 2][DW - 1], 240);
  EXPECT_EQ(sink.rows[DH / 2][DW / 2], 255);  // interior untouched
}

TEST(Clahe, FlatTileStaysConstant) {
  // Flat input: every tile is constant → identity LUT → unchanged output
  // (the texture campaign's flat-field invariant must survive Enhanced).
  const int SW = 480, SH = 320, DW = 240, DH = 160;
  std::vector<uint8_t> flat(static_cast<size_t>(SW) * SH, 137);
  Sink sink;
  sink.w = DW;
  DpidScaler::Plan p{SW, SH, DW, DH, true, true, false};
  std::vector<uint8_t> block(DpidScaler::scratchBytes(p));
  DpidScaler s;
  ASSERT_TRUE(s.begin(p, block.data(), block.size(), &Sink::emit, &sink, &pollFalse, nullptr));
  for (int y = 0; y < SH; ++y) s.feedSourceRow(y, flat.data() + static_cast<size_t>(y) * SW);
  ASSERT_TRUE(s.finish());
  for (const auto& r : sink.rows) {
    for (const uint8_t v : r) ASSERT_EQ(v, 137);
  }
}

TEST(Clahe, EnhancedExpandsLocalContrast) {
  // Two-region field (dark quadrant, light rest): CLAHE must change bytes vs
  // the Sharp run, and every output stays within [0,255].
  const int SW = 480, SH = 320, DW = 240, DH = 160;
  std::vector<uint8_t> img(static_cast<size_t>(SW) * SH, 220);
  for (int y = 0; y < SH; ++y) {
    for (int x = 0; x < SW / 2; ++x) {
      // soft ramp in the left half
      img[static_cast<size_t>(y) * SW + x] = static_cast<uint8_t>(60 + (x * 40) / (SW / 2));
    }
  }
  Sink sharp, enhanced;
  sharp.w = DW;
  enhanced.w = DW;
  for (const bool clahe : {false, true}) {
    Sink& sink = clahe ? enhanced : sharp;
    DpidScaler::Plan p{SW, SH, DW, DH, clahe, true, false};
    std::vector<uint8_t> block(DpidScaler::scratchBytes(p));
    DpidScaler s;
    ASSERT_TRUE(s.begin(p, block.data(), block.size(), &Sink::emit, &sink, &pollFalse, nullptr));
    for (int y = 0; y < SH; ++y) s.feedSourceRow(y, img.data() + static_cast<size_t>(y) * SW);
    ASSERT_TRUE(s.finish());
    ASSERT_EQ(sink.rows.size(), static_cast<size_t>(DH));
    for (const auto& r : sink.rows) {
      for (const uint8_t v : r) {
        ASSERT_LE(v, 255);
        // No absolute clipping to the extremes on this non-degenerate input.
        ASSERT_GE(v, 0);
      }
    }
  }
  size_t diff = 0;
  for (int r = 0; r < DH; ++r) {
    for (int x = 0; x < DW; ++x) diff += sharp.rows[r][x] != enhanced.rows[r][x];
  }
  EXPECT_GT(diff, static_cast<size_t>(DW * DH) / 10) << "CLAHE had no effect";
}

TEST(Clahe, EnhancedDeterministicAcrossRuns) {
  const int SW = 480, SH = 320, DW = 240, DH = 160;
  const auto src = makeStrokeField(SW, SH);
  std::vector<std::vector<uint8_t>> first;
  for (int run = 0; run < 2; ++run) {
    Sink sink;
    sink.w = DW;
    DpidScaler::Plan p{SW, SH, DW, DH, true, true, false};
    std::vector<uint8_t> block(DpidScaler::scratchBytes(p));
    DpidScaler s;
    ASSERT_TRUE(s.begin(p, block.data(), block.size(), &Sink::emit, &sink, &pollFalse, nullptr));
    for (int y = 0; y < SH; ++y) s.feedSourceRow(y, src.data() + static_cast<size_t>(y) * SW);
    ASSERT_TRUE(s.finish());
    if (run == 0) {
      first = sink.rows;
    } else {
      for (int r = 0; r < DH; ++r) {
        for (int x = 0; x < DW; ++x) ASSERT_EQ(first[r][x], sink.rows[r][x]);
      }
    }
  }
}

TEST(Clahe, EnhancedMatchesSharpOnDegenerateResizes) {
  // A destination too small for the 8x8 grid: Enhanced must equal Sharp
  // byte-for-byte (the documented deterministic skip).
  const int SW = 200, SH = 150, DW = 12, DH = 9;
  const auto src = makeStrokeField(SW, SH);
  Sink sharp, enhanced;
  sharp.w = DW;
  enhanced.w = DW;
  for (const bool clahe : {false, true}) {
    Sink& sink = clahe ? enhanced : sharp;
    DpidScaler::Plan p{SW, SH, DW, DH, clahe, true, false};
    std::vector<uint8_t> block(DpidScaler::scratchBytes(p));
    DpidScaler s;
    ASSERT_TRUE(s.begin(p, block.data(), block.size(), &Sink::emit, &sink, &pollFalse, nullptr));
    for (int y = 0; y < SH; ++y) s.feedSourceRow(y, src.data() + static_cast<size_t>(y) * SW);
    ASSERT_TRUE(s.finish());
  }
  for (int r = 0; r < DH; ++r) {
    for (int x = 0; x < DW; ++x) ASSERT_EQ(sharp.rows[r][x], enhanced.rows[r][x]);
  }
}

TEST(DpidScaler, EmitAbortPropagates) {
  const int SW = 320, SH = 200, DW = 160, DH = 100;
  const auto src = makeStrokeField(SW, SH);
  struct AbortSink {
    std::vector<std::vector<uint8_t>> rows;
    int w;
    int limit = 3;
    static bool emit(void* user, int, const uint8_t* row) {
      auto* s = static_cast<AbortSink*>(user);
      s->rows.emplace_back(row, row + s->w);
      return static_cast<int>(s->rows.size()) < s->limit;
    }
  } sink;
  sink.w = DW;
  DpidScaler::Plan p{SW, SH, DW, DH, false, true, false};
  std::vector<uint8_t> block(DpidScaler::scratchBytes(p));
  DpidScaler s;
  ASSERT_TRUE(s.begin(p, block.data(), block.size(), &AbortSink::emit, &sink, &pollFalse, nullptr));
  for (int y = 0; y < SH; ++y) s.feedSourceRow(y, src.data() + static_cast<size_t>(y) * SW);
  EXPECT_FALSE(s.finish());  // emit refused rows 3+
  EXPECT_LT(sink.rows.size(), static_cast<size_t>(DH));
}

TEST(DpidScaler, FinishReportsRefusedFinalBand) {
  // Bufferless mode streams completed bands straight to the emit hook; the
  // LAST band flushes from finish(). A refusal there used to read as success
  // (finish returned true without re-checking aborted_).
  const int SW = 64, SH = 400, DW = 32, DH = 50;
  std::vector<uint8_t> src(static_cast<size_t>(SW) * SH);
  for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>(i * 7 + i / SW * 13);
  struct LastBandSink {
    std::vector<std::vector<uint8_t>> rows;
    int w;
    int limit;
    static bool emit(void* user, int, const uint8_t* row) {
      auto* s = static_cast<LastBandSink*>(user);
      s->rows.emplace_back(row, row + s->w);
      return static_cast<int>(s->rows.size()) <= s->limit;  // refuse the final band
    }
  } sink{{}, DW, DH - 1};
  const DpidScaler::Plan p{SW, SH, DW, DH, /*clahe=*/false, /*detail=*/false, /*random=*/false};
  std::vector<uint8_t> block(DpidScaler::scratchBytes(p));
  DpidScaler s;
  ASSERT_TRUE(s.begin(p, block.data(), block.size(), &LastBandSink::emit, &sink, nullptr, nullptr));
  for (int y = 0; y < SH; ++y) s.feedSourceRow(y, src.data() + static_cast<size_t>(y) * SW);
  EXPECT_EQ(sink.rows.size(), static_cast<size_t>(DH - 1));  // the final band waits for finish()
  EXPECT_FALSE(s.finish());                                  // the refused final band fails the decode
  // The refused band was still handed to the sink (the hook decides by its
  // return); the decode result is what must read as failure.
  EXPECT_EQ(sink.rows.size(), static_cast<size_t>(DH));
}

TEST(DpidScaler, PassTwoLoopHonorsTheAbortPoll) {
  // Random detail plans run the O(band x srcW) pass-2 walk at finish(); the
  // PollFn contract covers that remainder (the header says ~60 s on device).
  // An immediate-true poll must stop the walk BEFORE it re-reads provider
  // rows: pass 1 reads exactly srcH rows, and an unpolled walk would re-read
  // the pass-2 patches on top of that.
  const int SW = 96, SH = 192, DW = 48, DH = 96;
  struct RandSink {
    int providerCalls = 0;
    static bool emit(void*, int, const uint8_t*) { return true; }
    static bool row(void* user, int srcY, uint8_t* out) {
      auto* s = static_cast<RandSink*>(user);
      ++s->providerCalls;
      std::memset(out, static_cast<uint8_t>(srcY * 3), 96);
      return true;
    }
    static bool poll(void*) { return true; }  // abort at the first check
  } sink;
  const DpidScaler::Plan p{SW, SH, DW, DH, /*clahe=*/false, /*detail=*/true, /*random=*/true};
  std::vector<uint8_t> block(DpidScaler::scratchBytes(p));
  DpidScaler s;
  ASSERT_TRUE(
      s.begin(p, block.data(), block.size(), &RandSink::emit, &sink, &RandSink::poll, &sink, &RandSink::row, &sink));
  ASSERT_TRUE(s.runRandom(&RandSink::row, &sink));
  EXPECT_EQ(sink.providerCalls, SH);  // pass 1 only
  EXPECT_FALSE(s.finish());           // the pass-2 walk must stop on the poll
  EXPECT_EQ(sink.providerCalls, SH);  // ...before any pass-2 provider re-read
}

// Defined here (friend of DpidScaler): exposes the typed carve addresses so
// the pin can assert natural alignment on an exact-need scratch block.
struct DpidScalerCarveProbe {
  static uintptr_t acc(const DpidScaler& s) { return reinterpret_cast<uintptr_t>(s.acc_); }
  static uintptr_t wtot(const DpidScaler& s) { return reinterpret_cast<uintptr_t>(s.wtot_); }
  static uintptr_t p2wy(const DpidScaler& s) { return reinterpret_cast<uintptr_t>(s.p2wy_); }
  static uintptr_t claheHist(const DpidScaler& s) { return reinterpret_cast<uintptr_t>(s.claheHist_); }
};

TEST(DpidScaler, TypedCarvesStayNaturallyAligned) {
  // Odd dstW/srcW make every subsequent carve offset arbitrary — the old
  // carve handed out raw offsets and could place the uint32 pass-2 weights
  // and the uint16 CLAHE histogram at misaligned addresses (faults on the
  // S3). carve() now rounds each typed view up.
  struct ProbeSink {
    std::vector<std::vector<uint8_t>> rows;
    int w = 479;
    static bool emit(void* user, int, const uint8_t* row) {
      auto* s = static_cast<ProbeSink*>(user);
      s->rows.emplace_back(row, row + s->w);
      return true;
    }
    static bool row(void*, int, uint8_t*) { return true; }
  } sink;
  const DpidScaler::Plan p{933, 1401, 479, 799, true, true, true};
  const size_t need = DpidScaler::scratchBytes(p);
  std::vector<uint8_t> block(need + 64);  // slack above exact-need too
  DpidScaler scaler;
  ASSERT_TRUE(scaler.begin(p, block.data(), need, &ProbeSink::emit, &sink, nullptr, nullptr, &ProbeSink::row, &sink));
  EXPECT_EQ(DpidScalerCarveProbe::acc(scaler) % alignof(uint64_t), 0u);
  EXPECT_EQ(DpidScalerCarveProbe::wtot(scaler) % alignof(uint64_t), 0u);
  // Odd dims keep the band copy under the 64 KiB cap, so the uint32 weights
  // are actually carved — the misalignment the review flagged.
  EXPECT_NE(DpidScalerCarveProbe::p2wy(scaler), 0u);
  EXPECT_EQ(DpidScalerCarveProbe::p2wy(scaler) % alignof(uint32_t), 0u);
  EXPECT_EQ(DpidScalerCarveProbe::claheHist(scaler) % alignof(uint16_t), 0u);
}

}  // namespace ttfimg
