#include "TtfImageCache.h"

#if defined(CROSSPOINT_TTF_READER)

#include <cstdio>

namespace ttfimg {

// ── ToneBake4 adaptive two-phase API (design 2026-11-08 §2.1, amendment 2) ──

bool ToneBake4::beginCollect(const char* grayPath) {
  if (width <= 0 || !ok()) return false;
  memset(hist, 0, sizeof(hist));
  histCount = 0;
  adaptive = false;
  grayFailed = false;
  grayOpen = false;
  grayPathLen = 0;
  // Validate the snprintf result BEFORE recording the path: a truncated or
  // empty path must never be opened (the open would target a different file).
  const int pathLen = snprintf(grayPathBuf, sizeof(grayPathBuf), "%s", grayPath);
  if (pathLen <= 0 || pathLen >= static_cast<int>(sizeof(grayPathBuf))) {
    grayPathBuf[0] = '\0';
    grayPathLen = 0;
    return false;
  }
  grayPathLen = static_cast<size_t>(pathLen);
  if (!Storage.openFileForWrite("TIMGE", grayPathBuf, grayFile)) {
    LOG_ERR("TIMGE", "Gray temp open failed: %s", grayPathBuf);
    return false;
  }
  grayOpen = true;
  return true;
}

void ToneBake4::collectRow(const uint8_t* gray) {
  if (width <= 0 || gray == nullptr) return;
  // u32 bins: no saturation, so the percentile targets always compare
  // against the real counts (1 KB of state in the bake object, no heap).
  for (int x = 0; x < width; ++x) ++hist[gray[x]];
  histCount += static_cast<uint32_t>(width);
  if (grayOpen && !grayFailed) {  // sticky: one log, no writes after failure
    if (grayFile.write(gray, static_cast<uint32_t>(width)) != width) {
      LOG_ERR("TIMGE", "Gray temp write failed: %s", grayPathBuf);
      grayFailed = true;  // sticky: grayDone() refuses a short stream
    }
  }
}

bool ToneBake4::grayDone() {
  if (grayOpen) {
    if (!grayFile.close()) {
      // grayOpen stays true: the handle stays reachable so a later
      // grayDiscard() (engine callers invoke it on this path) can still
      // close it and remove the temp.
      LOG_ERR("TIMGE", "Gray temp close failed: %s", grayPathBuf);
      return false;
    }
    grayOpen = false;
  }
  if (grayFailed) {
    grayFailed = false;
    Storage.remove(grayPathBuf);
    return false;
  }
  return true;
}

void ToneBake4::beginLegacyFallback() {
  // Storage-failure paint fallback (review round 1): drop whatever partial
  // histogram phase 1 managed and bake with the deterministic degenerate map
  // (fixed contrast 1.3, gamma off) — the image still paints without any
  // cache or gray temp.
  memset(hist, 0, sizeof(hist));
  histCount = 0;
  beginRemap();
}

void ToneBake4::grayDiscard() {
  if (grayOpen) {
    grayOpen = false;
    grayFile.close();
  }
  if (grayPathLen != 0) Storage.remove(grayPathBuf);
}

void ToneBake4::beginRemap() {
  // Percentile clip (integer): the smallest lo with cum(lo) >= ceil-ish(N·1%)
  // and the smallest hi with cum(hi) >= N·99%. A single flat histogram (hi <=
  // lo) has no range to stretch — fall back to the fixed contrast 1.3 map
  // with the gamma stage off.
  const uint32_t n = histCount;
  int lo = 0;
  int hi = 255;
  if (n > 0) {
    const uint32_t loTarget = n * kClipNum / kClipDen > 0 ? n * kClipNum / kClipDen : 1;
    const uint32_t hiTarget = n * (kClipDen - kClipNum) / kClipDen;
    uint32_t cum = 0;
    lo = 255;
    for (int v = 0; v < 256; ++v) {
      cum += hist[v];
      if (cum >= loTarget) {
        lo = v;
        break;
      }
    }
    cum = 0;
    for (int v = 0; v < 256; ++v) {
      cum += hist[v];
      if (cum >= hiTarget) {
        hi = v;
        break;
      }
    }
  }
  if (n == 0 || hi <= lo) {
    // Degenerate (flat scan, or an empty collection): fixed contrast, no
    // gamma — the doc §2.1 fallback.
    for (int i = 0; i < 256; ++i) {
      int c = (26 * i - 765) / 20;
      if (c < 0) c = 0;
      if (c > 255) c = 255;
      mapLut[i] = static_cast<uint8_t>(c);
    }
  } else {
    const int range = hi - lo;
    for (int i = 0; i < 256; ++i) {
      int s = (i - lo) * 255 / range;
      if (s < 0) s = 0;
      if (s > 255) s = 255;
      // Gamma stage defaults OFF (§2.2): the stretch already uses the full
      // range; the panel's absolute-gray waveform owns perceptual matching.
      if (kGammaEnabled) s = gammaLut[s];
      mapLut[i] = static_cast<uint8_t>(s);
    }
  }
  adaptive = true;
  // Phase 1 did not diffuse: reset the FS error state and the serpentine.
  if (err != nullptr && errNext != nullptr) {
    memset(err.get(), 0, (width + 2) * sizeof(int16_t));
    memset(errNext.get(), 0, (width + 2) * sizeof(int16_t));
  }
  reverse = false;
  row = 0;
}

bool ToneCacheWriter::begin(const char* p, const int w, const int h) {
  snprintf(path, sizeof(path), "%s", p);
  width = w;
  height = h;
  bytesPerRow = (w + 3) / 4;
  writtenRows = 0;
  ok = false;
  if (w <= 0 || h <= 0 || bytesPerRow <= 0 || w > 0xFFFF || h > 0xFFFF) return false;
  if (!Storage.openFileForWrite("TIMGE", path, file)) {
    LOG_ERR("TIMGE", "Cache open failed: %s", path);
    return false;
  }
  const uint16_t w16 = static_cast<uint16_t>(w);
  const uint16_t h16 = static_cast<uint16_t>(h);
  if (file.write(&w16, 2) != 2 || file.write(&h16, 2) != 2) {
    abort();
    return false;
  }
  ok = true;
  return true;
}

bool ToneCacheWriter::writeRow(const uint8_t* toneValues) {
  if (!ok) return false;
  uint8_t row[256];  // widest shipped panel row: (800+3)/4 = 201 bytes
  if (bytesPerRow > static_cast<int>(sizeof(row))) {
    abort();
    return false;
  }
  memset(row, 0, static_cast<size_t>(bytesPerRow));
  for (int x = 0; x < width; ++x) {
    const int shift = 6 - (x & 3) * 2;  // MSB first within byte
    row[x >> 2] |= static_cast<uint8_t>((toneValues[x] & 0x03) << shift);
  }
  if (file.write(row, static_cast<uint32_t>(bytesPerRow)) != static_cast<int>(bytesPerRow)) {
    LOG_ERR("TIMGE", "Cache write error row %d: %s", writtenRows, path);
    abort();
    return false;
  }
  ++writtenRows;
  return true;
}

bool ToneCacheWriter::finalize() {
  if (!ok) {
    abort();
    return false;
  }
  if (writtenRows != height) {
    LOG_ERR("TIMGE", "Cache incomplete: %s (%d/%d rows)", path, writtenRows, height);
    abort();
    return false;
  }
  if (!file.close()) {
    ok = false;
    Storage.remove(path);
    LOG_ERR("TIMGE", "Cache close failed: %s", path);
    return false;
  }
  ok = false;
  return true;
}

void ToneCacheWriter::abort() {
  if (file.isOpen()) file.close();
  if (path[0] != '\0') Storage.remove(path);
  ok = false;
}

bool readValidCacheHeader(HalFile& cacheFile, const int expectedWidth, const int expectedHeight, uint16_t& cachedWidth,
                          uint16_t& cachedHeight) {
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) return false;
  const int widthDiff = abs(cachedWidth - expectedWidth);
  const int heightDiff = abs(cachedHeight - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) return false;
  const size_t bytesPerRow = (cachedWidth + 3) / 4;
  const size_t expectedSize = 4 + bytesPerRow * cachedHeight;
  return cacheFile.size() >= expectedSize;
}

bool cacheNameFor(const ImageSpec& spec, const char* dir, const bool qualityTone, const bool clahe, char* out,
                  const size_t cap) {
  // Quality bake revision: bump when the bake algorithm changes so stale
  // caches re-decode. 'q' was the plain Floyd–Steinberg4 bake (2026-11-07);
  // 'r' was the fixed ToneBake4 map (2026-11-08 §5.2); 's' is the adaptive
  // two-phase bake (2026-11-08 §2, amendment 2); 't' is the texture bake
  // (full diffusion, threshold noise, flat-run snap, stroke anchor —
  // texture design §2.2–§2.4, 2026-11-08); 'v' is the DPID detail-
  // preserving resize feeding the same texture bake (DPID design
  // 2026-11-09; 'u' was burned by the reverted edge pass); 'w' adds the
  // CLAHE local-contrast pass on top (the Enhanced pictureDetail value —
  // the setting flips re-decode, never mix bakes); 'x' is the blockiness
  // bake (diffusion 0.8, snap/anchor removed, 0.5%/99.5% clip) over the
  // same DPID/CLAHE stack — the 2026-11-09 soak regression fix.
  // Identity: generation + href + placement (+ the tone policy, so the
  // quality revert switch never mixes bakes in one file). hrefExt (an
  // overlong Page href, paint-side only) IS the href for the identity.
  const char* const href = (spec.hrefExt != nullptr && spec.hrefExt[0] != '\0') ? spec.hrefExt : spec.href;
  uint64_t hash = 14695981039346656037ull;
  for (const char* p = href; *p != '\0'; ++p) {
    hash ^= static_cast<uint8_t>(*p);
    hash *= 1099511628211ull;
  }
  const uint32_t fields[4] = {static_cast<uint32_t>(spec.x), static_cast<uint32_t>(spec.y),
                              static_cast<uint32_t>(spec.w), static_cast<uint32_t>(spec.h)};
  for (const uint32_t v : fields) {
    hash ^= static_cast<uint64_t>(v);
    hash *= 1099511628211ull;
  }
  // Full 64-bit hash in the name: a truncated identity would let two records
  // of the same dimensions and generation share one file, and the header
  // check (dims only) would paint one image's pixels under the other's key.
  const int n = snprintf(out, cap, "%s/i%c%08x-%016llx.pxc", dir, qualityTone ? (clahe ? 'x' : 'v') : 'l',
                         static_cast<unsigned>(spec.generation), static_cast<unsigned long long>(hash));
  return n > 0 && static_cast<size_t>(n) < cap;
}

bool hasValidCache(const ImageSpec& spec, const char* dir, const bool qualityTone, const bool clahe) {
  char path[256];
  if (!cacheNameFor(spec, dir, qualityTone, clahe, path, sizeof(path))) return false;
  HalFile cacheFile;
  if (!Storage.openFileForRead("TIMGE", path, cacheFile)) return false;
  uint16_t cachedWidth = 0;
  uint16_t cachedHeight = 0;
  return readValidCacheHeader(cacheFile, spec.w, spec.h, cachedWidth, cachedHeight);
}

}  // namespace ttfimg

#endif  // CROSSPOINT_TTF_READER
