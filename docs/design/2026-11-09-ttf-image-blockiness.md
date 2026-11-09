# TTF image blockiness — bake-side rebuild of the texture/dither contract

Date: 2026-11-09 · Scope: TTF reader image path only (`CROSSPOINT_TTF_READER`)
· Amends: 2026-11-08-ttf-image-texture-quality.md (contract 2/3/4), 2026-11-08
-ttf-image-contrast-and-fit.md (§2 remap constants), 2026-11-09-ttf-image
-dpid-clahe.md (upscale sampling).

## 1. Evidence from the 2026-11-09 owner soak (post-`9d9494ed4`, Sharp mode)

Full-page illustrations render blocky with flat stepped tone bands — worse
than the pre-adapter rendering (which was cropped, but showed source pixels
1:1), WITHOUT CLAHE: "Removing clahe won't change anything this is without
clahe this is blocky too". Blacks render too deep. Enhanced vs Sharp: no
visible difference. The small ornament glyph resizes badly.

Host stage split (probe /tmp/pi_blockiness/probe, findings
/tmp/pi_blockiness_findings.md with image evidence under /tmp/pi_blockiness/):

- Stage B (DpidScaler out, 8-bit) is clean on every epub illustration fixture.
- Stage C (ToneBake4 out) carries the entire artifact set: flat stepped
  plateaus, moth-eaten erosion of dark fields, ragged level fringes, salt
  speckle on midgrays, solid black blobs.

The blockiness is 100% bake-side. The 2026-11-08 texture-quality changes
(diffusion 0.8→1.0, flat-run snap, stroke anchor, ±8 threshold noise) were
validated on synthetic gradients; on real print art they are the regression.

## 2. Mechanisms (each pinned on host with image + metric evidence)

1. **FS over-diffusion at ~0.5× downscale.** The Destronger mechanism: at
   1.0 the serpentine kernel dumps the full quantization error of every pixel;
   at the 2×-reduced sampling the transition band between two 4-level shelves
   is 1–2 px wide, so each row cell receives a large neighbor error and the
   shelf transitions break into large opposing clusters (moth-eaten edges and
   stepped tone bands). Metric over the 10 fixture corpus: salt-isolated
   4013 → 1164 (−71%), adjacent hard steps 58k → 42k at 0.8.
2. **Flat-run snap + stroke anchor** (texture contract): freeze gradients into
   plateaus and force mapped ≤42 to solid level 0 — the "blacks too deep"
   blob class (epilogue page, falling-girl leggings).
3. **±8 threshold noise**: metrically neutral on real art at the 0.8 weight
   (salt 1164 vs 1173);, and host A/B shows it is what protects the
   SYNTHETIC suite once snap/anchor are gone (flat-40 dropout 92.4% →
   100% solid at amp ≥ 12, gradient row delta 13.2 → 3.3 at amp 8/12+).
   Amp 12 triples the real-art isolated-speck count (420 → 1053), so amp
   8 stays; flat-40's ~8% constant-field dropout is the 's'-era behavior
   the owner accepted on device 2026-11-08 and is re-pinned as dominance.
4. **Percentile 1%/99% stretch**: on soft-shadow pages the 1% floor sits at
   gray ~62, crushing every softer shadow below it to level 0 (00008
   mapLut: 30..61 → 0) while stretching 62..255 to full scale — deepened
   blacks and saturated highlights. 0.5%/99.5% keeps the midtone structure
   (crush metric 9.86% → 8.65%) and still reaches true black/white where the
   art contains them.
5. **Upscale sampling**: the small class (dst ≥ src on both axes) box-blurs
   in pass 1, so ornamentis arrive soft and then bake with salt speckle.
   Nearest-center sampling keeps line art crisp.

## 3. Contract (this change)

1. Bake constants: `kDiffusionNum` 2048 (0.8); `kClipNum`/`kClipDen` 5/1000
   (0.5%/99.5% clip, winsorized at the same bucket boundaries).
2. **Removals** (texture contract 2, 3): the flat-run snap branch and the
   thin-stroke anchor branch (TtfImageCache.h toneDir). The threshold
   noise (texture §2.3, ±8) STAYS: at the new 0.8 weight it is what keeps
   the synthetic suite's guarantees (see §2.3) and it is metrically
   neutral on real art; flat-40's constant-field dropout is re-pinned as
   ≥90% level-0 dominance (the 's'-era behavior, owner-accepted on-device
   2026-11-08).
3. Serpentine scanning stays (kernel mirrored per row direction); the
   exact-mirror suite pin bakes with a per-instance noiseAmp=0 (the x-keyed
   offset is not mirror-symmetric).
4. **Nearest small class** (dpid-clahe amendment): when dst ≥ src on both
   axes the scaler samples each destination pixel from the nearest source
   pixel by center mapping (one srcW row buffer, <1 KB for ornament sizes,
   zero extra pass-2/CLAHE state). Degenerate identity at 1:1 is unchanged.
   The degraded-rung ladder (no scratch) falls back to the old fractional
   box behavior.
5. Tone cache identity: 'w' → 'x' (the baked bytes change; the shipped
   chain is q/r/s/t/v/w → x).
6. No new heap class on device: the bake constants are compile-time; the
   scaler's nearest row lives in the existing call-scoped PSRAM block; no
   new allocations, no I/O change. The 5 MB PSRAM budget policy is untouched.

## 4. Rejected, with evidence

- Tent pre-blur of the destination rows before the bake (probe variant
  `pb08`): visually close to 0.8 alone, no metric gain on the corpus, and it
  needs a 3-row ring in the emit path — cost without measured payoff. Kept
  as a probe lever (/tmp/pi_blockiness/probe/v/v_pb08) if the owner asks
  for smoother edges after soak.
- Identity remap (no stretch): visibly washed out — the flat look the
  2026-11-08 contrast campaign fixed on device. (Probe re-check 2026-11-09:
  r_identity matches the shipped bake exactly on the crush metric, so the
  0.5% clip is the sole crush lever, kept for its midtone retention.)
- Fixed contrast 1.3 remap (no adaptive): no crush win and loses the
  per-image range the contrast campaign ships.
- Conservative 2% clip: more punch but measurably more crushed midtones.
- Screentone pre-blur / halftone handling: no screentone lattice exists at
  panel scale on these scans (stage A clean).

## 5. Tests (host, test/ttf_image_fit)

- Bake: diffusion-0.8 weight pin (kernel math /20), remap clip 0.5%/99.5%
  (MidtoneStaysMidtone recomputed lo=1/hi=254), texture suite re-pinned:
  flat-40 dominance ≥90% level 0, stroke survival without the anchor at
  0.8, serpentine exact-mirror pin bakes noise-free (per-instance
  noiseAmp), gradient band/oscillation pins unchanged (they hold at
  0.8+noise).
- Scaler: small-class nearest pins (2× crisp pattern with every emitted
  value a source value; 32×31 → 96×93 ornament takes exact nearest-center
  picks; a small block without nearest-room falls back to the fractional
  box), plus the existing box/λ0 pins.
- Fit/coverage suite unchanged (placement invariants untouched).
