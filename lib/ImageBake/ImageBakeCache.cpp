#include "ImageBakeCache.h"

#include <Logging.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace imagebake {

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

bool BandCacheWriter::begin(const char* cachePath, const int w, const int h, const int ox, const int oy,
                            const int maxBlockDstRows, const CacheIdentity& id) {
  width = w;
  height = h;
  originX = ox;
  originY = oy;
  bytesPerRow = (w + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
  bandStart = 0;
  flushedRows = 0;
  ok = false;

  int wantRows = maxBlockDstRows + 2;
  if (wantRows < MIN_BAND_ROWS) wantRows = MIN_BAND_ROWS;
  if (wantRows > h) wantRows = h;

  size_t maxRowsByMem = MAX_BAND_BYTES / (size_t)bytesPerRow;
  if (maxRowsByMem < 1) maxRowsByMem = 1;
  if ((size_t)wantRows > maxRowsByMem) wantRows = (int)maxRowsByMem;

  // A single decode block must fit inside the band, otherwise streaming would
  // drop rows. This only fails for pathological upscales that could not be
  // cached at all; fall back to the no-cache path.
  if (wantRows < maxBlockDstRows) {
    LOG_ERR("IMG", "Cache band too small (%d < %d rows) for %dx%d", wantRows, maxBlockDstRows, w, h);
    return false;
  }
  bandRows = wantRows;

  const size_t bufSize = (size_t)(bandRows + 1) * bytesPerRow;  // +1 spare zero row
  // PoolBytes places the band in PSRAM on PSRAM boards, DRAM elsewhere.
  buffer.reset(static_cast<uint8_t*>(poolMalloc(bufSize)));
  if (!buffer) {
    LOG_ERR("IMG", "OOM cache band: %u bytes", (unsigned)bufSize);
    return false;
  }
  memset(buffer.get(), 0, bufSize);
  zeroRow = buffer.get() + (size_t)bandRows * bytesPerRow;

  if (!Storage.openFileForWrite("IMG", cachePath, file)) {
    LOG_ERR("IMG", "Failed to open cache file for writing: %s", cachePath);
    buffer.reset();
    zeroRow = nullptr;
    return false;
  }
  cachePathStr = cachePath;

  // In-file identity header (ToneCacheWriter's exact format): a legacy
  // headerless cache file fails the magic check at open and regenerates.
  uint8_t hdr[kCacheHeaderBytes];
  hdr[0] = kCacheMagic0;
  hdr[1] = kCacheMagic1;
  hdr[2] = static_cast<uint8_t>(id.letter);
  memcpy(hdr + 3, &id.hash32, sizeof(id.hash32));
  const uint16_t w16 = (uint16_t)w;
  const uint16_t h16 = (uint16_t)h;
  memcpy(hdr + 7, &w16, sizeof(w16));
  memcpy(hdr + 9, &h16, sizeof(h16));
  if (file.write(hdr, sizeof(hdr)) != sizeof(hdr)) {
    LOG_ERR("IMG", "Failed to write cache header: %s", cachePath);
    abort();
    return false;
  }

  LOG_DBG("IMG", "Cache stream started: %s (%dx%d, band %d rows)", cachePath, w, h, bandRows);
  ok = true;
  return true;
}

bool BandCacheWriter::advanceTo(int newTopRow) {
  if (!ok) return false;
  if (newTopRow <= bandStart) return true;
  if (newTopRow > height) newTopRow = height;

  for (int r = bandStart; r < newTopRow; ++r) {
    const int idx = r - bandStart;
    const uint8_t* rowPtr = (idx < bandRows) ? (buffer.get() + (size_t)idx * bytesPerRow) : zeroRow;
    if (file.write(rowPtr, (size_t)bytesPerRow) != (size_t)bytesPerRow) {
      LOG_ERR("IMG", "Cache write error at row %d", r);
      ok = false;
      return false;
    }
  }
  flushedRows = newTopRow;
  bandStart = newTopRow;
  memset(buffer.get(), 0, (size_t)bandRows * bytesPerRow);  // fresh band (gaps stay black)
  return true;
}

bool BandCacheWriter::finalize() {
  if (!ok) {
    abort();
    return false;
  }
  for (int r = flushedRows; r < height; ++r) {
    const int idx = r - bandStart;
    const uint8_t* rowPtr = (idx >= 0 && idx < bandRows) ? (buffer.get() + (size_t)idx * bytesPerRow) : zeroRow;
    if (file.write(rowPtr, (size_t)bytesPerRow) != (size_t)bytesPerRow) {
      LOG_ERR("IMG", "Cache write error at row %d", r);
      abort();
      return false;
    }
  }
  if (!file.close()) {
    LOG_ERR("IMG", "Failed to close cache: %s", cachePathStr.c_str());
    abort();
    return false;
  }
  LOG_DBG("IMG", "Cache written: %s (%dx%d, %u bytes)", cachePathStr.c_str(), width, height,
          static_cast<unsigned>(kCacheHeaderBytes + bytesPerRow * height));
  ok = false;  // file handed off; nothing left to clean up
  return true;
}

void BandCacheWriter::abort() {
  if (file.isOpen()) file.close();
  if (!cachePathStr.empty()) {
    Storage.remove(cachePathStr.c_str());
  }
  ok = false;
}

BandCacheWriter::~BandCacheWriter() {
  if (file.isOpen()) {
    // The file is still open, so neither finalize() nor abort() ran, or a
    // mid-stream write failed (advanceTo() cleared ok but left the file
    // open). Drop the partial cache so we leave no corrupt file behind.
    abort();
  }
  // PoolBytes frees the band (heap_caps_free on PSRAM builds, free elsewhere).
}

}  // namespace imagebake
