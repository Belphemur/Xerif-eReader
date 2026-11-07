#include "TtfImageCache.h"

#if defined(CROSSPOINT_TTF_READER)

#include <cstdio>

namespace ttfimg {

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

bool cacheNameFor(const ImageSpec& spec, const char* dir, const bool qualityTone, char* out, const size_t cap) {
  // Identity: generation + href + placement (+ the tone policy, so the
  // quality revert switch never mixes bakes in one file).
  uint64_t hash = 14695981039346656037ull;
  for (const char* p = spec.href; *p != '\0'; ++p) {
    hash ^= static_cast<uint8_t>(*p);
    hash *= 1099511628211ull;
  }
  const uint32_t fields[4] = {static_cast<uint32_t>(spec.x), static_cast<uint32_t>(spec.y),
                              static_cast<uint32_t>(spec.w), static_cast<uint32_t>(spec.h)};
  for (const uint32_t v : fields) {
    hash ^= static_cast<uint64_t>(v);
    hash *= 1099511628211ull;
  }
  const int n = snprintf(out, cap, "%s/i%c%08x-%08x.pxc", dir, qualityTone ? 'q' : 'l',
                         static_cast<unsigned>(spec.generation), static_cast<unsigned>(hash & 0xFFFFFFFFull));
  return n > 0 && static_cast<size_t>(n) < cap;
}

bool hasValidCache(const ImageSpec& spec, const char* dir, const bool qualityTone) {
  char path[256];
  if (!cacheNameFor(spec, dir, qualityTone, path, sizeof(path))) return false;
  HalFile cacheFile;
  if (!Storage.openFileForRead("TIMGE", path, cacheFile)) return false;
  uint16_t cachedWidth = 0;
  uint16_t cachedHeight = 0;
  return readValidCacheHeader(cacheFile, spec.w, spec.h, cachedWidth, cachedHeight);
}

}  // namespace ttfimg

#endif  // CROSSPOINT_TTF_READER
