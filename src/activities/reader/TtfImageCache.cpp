#include "TtfImageCache.h"

#if defined(CROSSPOINT_TTF_READER)

#include <cstdio>

namespace ttfimg {
bool ToneCacheWriter::begin(const char* p, const int w, const int h, const CacheIdentity& id) {
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
  // In-file identity header (see CacheIdentity): magic + bake revision
  // letter + identity-hash top 32 bits, then the dims the payload covers.
  uint8_t hdr[kCacheHeaderBytes];
  hdr[0] = kCacheMagic0;
  hdr[1] = kCacheMagic1;
  hdr[2] = static_cast<uint8_t>(id.letter);
  memcpy(hdr + 3, &id.hash32, sizeof(id.hash32));
  const uint16_t w16 = static_cast<uint16_t>(w);
  const uint16_t h16 = static_cast<uint16_t>(h);
  memcpy(hdr + 7, &w16, sizeof(w16));
  memcpy(hdr + 9, &h16, sizeof(h16));
  if (file.write(hdr, sizeof(hdr)) != static_cast<int>(sizeof(hdr))) {
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

bool readValidCacheHeader(HalFile& cacheFile, const int expectedWidth, const int expectedHeight,
                          const CacheIdentity& id, uint16_t& cachedWidth, uint16_t& cachedHeight) {
  uint8_t hdr[kCacheHeaderBytes];
  if (cacheFile.read(hdr, sizeof(hdr)) != static_cast<int>(sizeof(hdr))) return false;
  if (hdr[0] != kCacheMagic0 || hdr[1] != kCacheMagic1) return false;
  if (static_cast<char>(hdr[2]) != id.letter) return false;
  uint32_t hash32 = 0;
  memcpy(&hash32, hdr + 3, sizeof(hash32));  // little-endian target, written raw
  if (hash32 != id.hash32) return false;
  uint16_t w = 0, h = 0;
  memcpy(&w, hdr + 7, sizeof(w));
  memcpy(&h, hdr + 9, sizeof(h));
  cachedWidth = w;
  cachedHeight = h;
  const int widthDiff = abs(cachedWidth - expectedWidth);
  const int heightDiff = abs(cachedHeight - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) return false;
  const size_t bytesPerRow = (cachedWidth + 3) / 4;
  const size_t expectedSize = kCacheHeaderBytes + bytesPerRow * cachedHeight;
  return cacheFile.size() >= expectedSize;
}

uint64_t cacheHash64(const ImageSpec& spec) {
  // Identity: generation + href + placement. hrefExt (an overlong Page
  // href, paint-side only) IS the href for the identity.
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
  return hash;
}

char cacheRevisionLetter(const bool qualityTone, const bool clahe) { return qualityTone ? (clahe ? 'f' : 'e') : 'l'; }

CacheIdentity cacheIdentity(const ImageSpec& spec, const bool qualityTone, const bool clahe) {
  CacheIdentity id;
  id.letter = cacheRevisionLetter(qualityTone, clahe);
  id.hash32 = static_cast<uint32_t>(cacheHash64(spec) >> 32);
  return id;
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
  // same DPID/CLAHE stack — the 2026-11-09 soak regression fix; 'y' (Sharp;
  // 'v' died with it: the pre-blockiness DPID-texture bake reused it) and
  // 'z' (Enhanced; 'x' only ever shipped to test builds) invalidate every
  // cache written before the small-range guard + nearest/CLAHE emission fix
  // — review round 2026-11-10; 'm' (Sharp) and 'n' (Enhanced) are the
  // pipeline rework — streaming pass-2 removed, tent pre-blur removed,
  // exact-residue FS diffusion at weight 1.0, no-op-stretch skip, full-
  // fidelity progressive decode (reset4, 2026-11-10). 'o' (Sharp) and 'p'
  // (Enhanced) are the 2026-11-10 fidelity campaign — halo-free sharpen,
  // Sierra-Lite quantizer, decoder-trap fixes, and the in-file identity
  // header (11-byte CacheIdentity header replaces the bare dims word).
  // 'a' (Sharp) and 'b' (Enhanced) are the iteration-3 owner audit bake
  // (2026-11-11) — panel-calibrated levels {0,48,112,255} (midpoints
  // {28,78,182}), white snap 240, serpentine Atkinson 1/8 diffusion,
  // coring-6/darkFloor sharpen (both letters never used in this file's
  // history — swept). 'c' (Sharp) and 'd' (Enhanced) are the edge-aware
  // hybrid bake (amendment 7, 2026-11-11): 8/8 → 7/8 → 6/8 diffusion
  // strength by symmetric local gradient, the kEdgeBarrier cross-cliff
  // guard, smooth-region threshold dither, the causal prevRowMv context,
  // and the static gamma/contrast LUTs ('a'/'b' only ever shipped in the
  // merged PR #303 develop builds — swept). 'e' (Sharp) and 'f' (Enhanced)
  // are the legacy-BMP-parity bake (amendment 9, 2026-11-11): the exact
  // AtkinsonDitherer(originalThresholds=false) transfer — identity luma,
  // cutoffs {30,55,150}, targets {15,35,90,210}, non-serpentine 6/8 — and
  // the single-pass stream (no .g temp, no percentile stretch, no
  // post-resize sharpen), sweeping 'c'/'d' (never shipped outside PR #316
  // review builds).

  // Full 64-bit hash in the name: a truncated identity would let two records
  // of the same dimensions and generation share one file, and the header
  // check (dims only) would paint one image's pixels under the other's key.
  const uint64_t hash = cacheHash64(spec);
  const int n = snprintf(out, cap, "%s/i%c%08x-%016llx.pxc", dir, cacheRevisionLetter(qualityTone, clahe),
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
  const bool valid = readValidCacheHeader(cacheFile, spec.w, spec.h, cacheIdentity(spec, qualityTone, clahe),
                                          cachedWidth, cachedHeight);
  cacheFile.close();  // FAT refuses remove on an open handle
  if (!valid) {
    // A file under a live name whose content cannot match the key (older
    // bake, torn write): drop it so the next decode regenerates instead of
    // re-failing the open (defense-in-depth header, 2026-11-10).
    Storage.remove(path);
  }
  return valid;
}

}  // namespace ttfimg

#endif  // CROSSPOINT_TTF_READER
