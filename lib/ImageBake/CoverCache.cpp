#include "CoverCache.h"

#include <Logging.h>

#include <cstdio>
#include <cstring>

namespace imagebake {

namespace {

// Copy `src`'s whole contents onto the already-open `dst` in bounded
// chunks (the book-cache streamer size). The source is opened here so the
// caller's handles never multiply.
bool appendFileContents(HalFile& dst, const char* srcPath) {
  HalFile src;
  if (!Storage.openFileForRead("CC", srcPath, src)) return false;
  constexpr size_t kChunk = 4096;
  uint8_t buf[kChunk];
  size_t remaining = src.size();
  while (remaining > 0) {
    const size_t want = remaining < kChunk ? remaining : kChunk;
    if (src.read(buf, want) != static_cast<int>(want) || dst.write(buf, want) != want) return false;
    remaining -= want;
  }
  return true;
}

void writeLE32(uint8_t* p, const uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void writeLE16(uint8_t* p, const uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

bool readDirectory(HalFile& f, CoverSectionEntry (&entries)[kCoverSectionCount]) {
  uint8_t dir[kCoverDirectoryBytes];
  if (f.read(dir, sizeof(dir)) != static_cast<int>(sizeof(dir))) return false;
  for (size_t i = 0; i < kCoverSectionCount; ++i) {
    const uint8_t* p = dir + i * kCoverSectionEntryBytes;
    uint32_t off = 0, len = 0;
    uint16_t w = 0, h = 0;
    memcpy(&off, p, sizeof(off));      // little-endian target, written raw
    memcpy(&len, p + 4, sizeof(len));  //
    memcpy(&w, p + 8, sizeof(w));      //
    memcpy(&h, p + 10, sizeof(h));     //
    entries[i] = CoverSectionEntry{off, len, w, h};
  }
  return true;
}

// Parse <basename>/cover_c<cropChar>_<thumbHeight>.pxc. false = the name is
// not the combined-cache shape (an identity check cannot be recomputed).
bool parseCoverCacheName(const char* path, int& thumbHeight, bool& cropped) {
  const char* base = strrchr(path, '/');
  base = base != nullptr ? base + 1 : path;
  if (strncmp(base, "cover_c", 7) != 0) return false;
  const char c = base[7];
  if (c != 'k' && c != 'f') return false;
  cropped = c == 'k';
  if (base[8] != '_') return false;
  int h = 0;
  bool any = false;
  for (const char* p = base + 9; *p != '\0' && *p != '.'; ++p) {
    if (*p < '0' || *p > '9') return false;
    if (h > 100000) return false;  // overflow guard before the multiply
    h = h * 10 + (*p - '0');
    any = true;
  }
  if (!any || h <= 0 || h > 0xFFFF) return false;
  thumbHeight = h;
  return true;
}

// Full validation of a combined cache file against (thumbHeight, cropped).
// `f` is open, positioned anywhere; leaves it seeked past the directory on
// success. `outSection` receives the requested entry.
bool validateCoverCache(HalFile& f, const int thumbHeight, const bool cropped, const CoverSection section,
                        CoverSectionEntry& outSection) {
  if (!f.seek(0)) return false;
  uint8_t hdr[kCacheHeaderBytes];
  if (f.read(hdr, sizeof(hdr)) != static_cast<int>(sizeof(hdr))) return false;
  if (hdr[0] != kCacheMagic0 || hdr[1] != kCacheMagic1) return false;
  if (static_cast<char>(hdr[2]) != kCoverCacheLetter) return false;
  uint32_t hash32 = 0;
  memcpy(&hash32, hdr + 3, sizeof(hash32));
  if (hash32 != coverCacheHash32(thumbHeight, cropped)) return false;

  CoverSectionEntry entries[kCoverSectionCount];
  if (!readDirectory(f, entries)) return false;

  const size_t idx = section == CoverSection::Thumb ? 0 : 1;
  const CoverSectionEntry& e = entries[idx];
  if (e.w == 0 || e.h == 0 || e.length == 0) return false;
  if (e.offset < kCoverPrefixBytes) return false;
  if (static_cast<uint64_t>(e.offset) + e.length > f.size()) return false;
  if (section == CoverSection::Thumb && e.h != static_cast<uint16_t>(thumbHeight)) return false;  // height keying
  outSection = e;
  return true;
}

}  // namespace

uint32_t coverCacheHash32(const int thumbHeight, const bool cropped) {
  const char cropChar = cropped ? 'k' : 'f';
  uint64_t hash = 14695981039346656037ull;
  const uint32_t fields[3] = {static_cast<uint32_t>(kCoverCacheLetter), static_cast<uint32_t>(thumbHeight),
                              static_cast<uint32_t>(cropChar)};
  for (const uint32_t v : fields) {
    hash ^= static_cast<uint64_t>(v);
    hash *= 1099511628211ull;
  }
  return static_cast<uint32_t>(hash >> 32);
}

bool coverCachePath(const char* dir, const int thumbHeight, const bool cropped, char* out, const size_t cap) {
  const int n = snprintf(out, cap, "%s/cover_c%c_%d.pxc", dir, cropped ? 'k' : 'f', thumbHeight);
  return n > 0 && static_cast<size_t>(n) < cap;
}

bool coverCacheTemplate(const char* dir, const bool cropped, char* out, const size_t cap) {
  const int n = snprintf(out, cap, "%s/cover_c%c_[HEIGHT].pxc", dir, cropped ? 'k' : 'f');
  return n > 0 && static_cast<size_t>(n) < cap;
}

bool openCoverBitmap(const char* moduleName, const char* path, const int thumbHeight, const CoverSection section,
                     HalFile& out) {
  HalFile probe;
  if (!Storage.openFileForRead(moduleName, path, probe)) return false;

  // Kind probe: a legacy standalone BMP (GIF-covered books keep one, design
  // decision 3) opens whole-file; anything else must validate as a combined
  // cover cache.
  uint8_t magic[2];
  if (probe.seek(0) && probe.read(magic, sizeof(magic)) == static_cast<int>(sizeof(magic)) &&
      (magic[0] != kCacheMagic0 || magic[1] != kCacheMagic1)) {
    // Legacy BMP: hand out the probe handle rewound to the start.
    probe.seek(0);
    out = std::move(probe);
    return true;
  }

  int nameHeight = 0;
  bool nameCropped = false;
  if (!parseCoverCacheName(path, nameHeight, nameCropped)) return false;
  // The name's height segment is the thumb identity; the open request's
  // thumbHeight must agree with it (theme substitution supplies both from
  // the same source).
  if (thumbHeight > 0 && nameHeight != thumbHeight) return false;

  CoverSectionEntry entry;
  if (!validateCoverCache(probe, nameHeight, nameCropped, section, entry)) return false;
  probe.close();
  // Sub-file view over the section: the Bitmap reader parses the BMP from
  // section-local zero (Bitmap::parseHeaders seeks absolute 0 — the view
  // owns the base-offset translation).
  return Storage.openFileSubview(moduleName, path, entry.offset, entry.length, out);
}

bool coverBitmapIsValid(const char* path, const int thumbHeight, const CoverSection section) {
  HalFile probe;
  if (!Storage.openFileForRead("CC", path, probe)) return false;
  uint8_t magic[2];
  if (!probe.seek(0) || probe.read(magic, sizeof(magic)) != static_cast<int>(sizeof(magic))) return false;
  if (magic[0] != kCacheMagic0 || magic[1] != kCacheMagic1) return true;  // legacy standalone BMP
  int nameHeight = 0;
  bool nameCropped = false;
  if (!parseCoverCacheName(path, nameHeight, nameCropped)) return false;
  if (thumbHeight > 0 && nameHeight != thumbHeight) return false;
  CoverSectionEntry entry;
  return validateCoverCache(probe, nameHeight, nameCropped, section, entry);
}

bool assembleCoverCache(const char* finalPath, const char* thumbTmpPath, const uint16_t thumbW, const uint16_t thumbH,
                        const char* coverTmpPath, const uint16_t coverW, const uint16_t coverH, const int thumbHeight,
                        const bool cropped) {
  uint32_t thumbLen = 0;
  uint32_t coverLen = 0;
  {
    HalFile f;
    if (Storage.openFileForRead("CC", thumbTmpPath, f) && f.isOpen()) {
      thumbLen = static_cast<uint32_t>(f.size());
      f.close();
    }
    if (Storage.openFileForRead("CC", coverTmpPath, f) && f.isOpen()) {
      coverLen = static_cast<uint32_t>(f.size());
      f.close();
    }
  }
  if (thumbLen == 0 || coverLen == 0) {
    LOG_ERR("CC", "Cover section temps missing: %s", finalPath);
    return false;
  }

  char tmpPath[280];
  if (snprintf(tmpPath, sizeof(tmpPath), "%s.t", finalPath) >= static_cast<int>(sizeof(tmpPath))) return false;

  const CoverSectionEntry thumbEntry{kCoverPrefixBytes, thumbLen, thumbW, thumbH};
  const CoverSectionEntry coverEntry{static_cast<uint32_t>(kCoverPrefixBytes + thumbLen), coverLen, coverW, coverH};

  bool ok = false;
  {
    HalFile out;
    if (!Storage.openFileForWrite("CC", tmpPath, out)) {
      LOG_ERR("CC", "Cover cache create failed: %s", tmpPath);
      return false;
    }
    ok = true;
    uint8_t hdr[kCacheHeaderBytes];
    hdr[0] = kCacheMagic0;
    hdr[1] = kCacheMagic1;
    hdr[2] = static_cast<uint8_t>(kCoverCacheLetter);
    const uint32_t hash32 = coverCacheHash32(thumbHeight, cropped);
    memcpy(hdr + 3, &hash32, sizeof(hash32));
    // Identity-header dims carry the cover section geometry (the largest
    // payload); the directory is the per-section authority.
    memcpy(hdr + 7, &coverW, sizeof(coverW));
    memcpy(hdr + 9, &coverH, sizeof(coverH));
    ok = ok && out.write(hdr, sizeof(hdr)) == sizeof(hdr);

    uint8_t dir[kCoverDirectoryBytes];
    for (size_t i = 0; i < kCoverSectionCount; ++i) {
      const CoverSectionEntry& e = i == 0 ? thumbEntry : coverEntry;
      uint8_t* p = dir + i * kCoverSectionEntryBytes;
      writeLE32(p, e.offset);
      writeLE32(p + 4, e.length);
      writeLE16(p + 8, e.w);
      writeLE16(p + 10, e.h);
    }
    ok = ok && out.write(dir, sizeof(dir)) == sizeof(dir);
    ok = ok && appendFileContents(out, thumbTmpPath);
    ok = ok && appendFileContents(out, coverTmpPath);
    ok = ok && out.close();
  }
  if (!ok) {
    Storage.remove(tmpPath);
    LOG_ERR("CC", "Cover cache assembly failed: %s", finalPath);
    return false;
  }
  if (!Storage.replaceFile(tmpPath, finalPath)) {
    Storage.remove(tmpPath);
    LOG_ERR("CC", "Cover cache publish failed: %s", finalPath);
    return false;
  }
  Storage.remove(thumbTmpPath);
  Storage.remove(coverTmpPath);
  LOG_INF("CC", "Cover cache assembled: %s (thumb %ux%u %u B, cover %ux%u %u B)", finalPath, thumbW, thumbH, thumbLen,
          coverW, coverH, coverLen);
  return true;
}

}  // namespace imagebake
