#include "ZipFile.h"

#include <HalStorage.h>
#include <InflateStream.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <atomic>
#include <cstring>

// Profiling counters.
static std::atomic<uint32_t> g_load_all_stat_slims_count{0};
static std::atomic<uint32_t> g_fsfile_open_count{0};

struct ZipInflateCtx {
  HalFile* file = nullptr;
  size_t fileRemaining = 0;
  uint8_t* readBuf = nullptr;
  size_t readBufSize = 0;
};

namespace {
constexpr uint16_t ZIP_METHOD_STORED = 0;
constexpr uint16_t ZIP_METHOD_DEFLATED = 8;
// Central directory header excluding the filename, extra field, and comment.
constexpr size_t CENTRAL_HEADER_BYTES = 46;
constexpr uint32_t CENTRAL_HEADER_SIGNATURE = 0x02014b50;

uint16_t readLe16(const uint8_t* bytes) {
  uint16_t value;
  // memcpy to avoid unaligned access issues
  memcpy(&value, bytes, sizeof(value));
  return value;
}

uint32_t readLe32(const uint8_t* bytes) {
  uint32_t value;
  memcpy(&value, bytes, sizeof(value));
  return value;
}

// RAII zip: opens the zip if not already open, closes on destruction only if
// it performed the open.  Removes the wasOpen/close boilerplate from every method.
class ScopedOpenClose final {
 public:
  [[nodiscard]] explicit ScopedOpenClose(ZipFile& zf) : zf(zf), needsClose(!zf.isOpen()) {
    if (needsClose) ok = zf.open();
  }
  ~ScopedOpenClose() {
    if (needsClose && ok) zf.close();
  }
  ScopedOpenClose(const ScopedOpenClose&) = delete;
  ScopedOpenClose& operator=(const ScopedOpenClose&) = delete;
  ScopedOpenClose(ScopedOpenClose&&) = delete;
  ScopedOpenClose& operator=(ScopedOpenClose&&) = delete;
  explicit operator bool() const { return ok || !needsClose; }

 private:
  ZipFile& zf;
  bool needsClose = false;
  bool ok = true;  // true when zip was already open (no open() call needed)
};

size_t zipFillCallback(void* vctx, const uint8_t** data) {
  auto* ctx = static_cast<ZipInflateCtx*>(vctx);
  if (ctx->fileRemaining == 0) return 0;

  const size_t toRead = ctx->fileRemaining < ctx->readBufSize ? ctx->fileRemaining : ctx->readBufSize;
  const int result = ctx->file->read(ctx->readBuf, toRead);
  // HalFile::read() returns a negative int on error. Treat it as end-of-stream
  // rather than letting the negative-to-size_t conversion underflow fileRemaining
  // and report a huge bytesRead, which would have the inflate library read past
  // the end of readBuf.
  if (result < 0) {
    LOG_ERR("ZIP", "Failed to read compressed data: %d", result);
    return 0;
  }
  const size_t bytesRead = static_cast<size_t>(result);
  ctx->fileRemaining -= bytesRead;

  *data = ctx->readBuf;
  return bytesRead;
}
}  // namespace

bool ZipFile::loadAllFileStatSlims() {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  file.seek(zipDetails.centralDirOffset);

  uint32_t sig;
  char itemName[256];
  // Entries are emplaced directly into the PSRAM-resident FileStatSlimCache
  // (pool-allocated nodes + key strings) — never into a DRAM intermediate
  // that would transiently double the footprint at open.
  fileStatSlimCache.clear();
  fileStatSlimCache.reserve(zipDetails.totalEntries);

  while (file.available()) {
    file.read(&sig, 4);
    if (sig != 0x02014b50) break;  // End of list

    FileStatSlim fileStat = {};

    file.seekCur(6);
    file.read(&fileStat.method, 2);
    file.seekCur(8);
    file.read(&fileStat.compressedSize, 4);
    file.read(&fileStat.uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    file.read(&fileStat.localHeaderOffset, 4);

    if (nameLen < sizeof(itemName)) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';
      fileStatSlimCache.emplace(itemName, fileStat);
    } else {
      // Skip over oversized entry names to avoid writing past fixed buffer.
      file.seekCur(nameLen);
    }

    // Skip the rest of this entry (extra field + comment)
    file.seekCur(m + k);
  }

  // Set cursor to start of central directory for sequential access
  lastCentralDirPos = zipDetails.centralDirOffset;
  lastCentralDirPosValid = true;

  g_load_all_stat_slims_count++;

  return true;
}

bool ZipFile::loadFileStatSlim(const char* filename, FileStatSlim* fileStat) {
  if (!fileStatSlimCache.empty()) {
    // PoolString key: the (rare) cache-hit probe allocates one short pool
    // string in PSRAM instead of DRAM.
    const auto it = fileStatSlimCache.find(PoolString{filename});
    if (it != fileStatSlimCache.end()) {
      *fileStat = it->second;
      return true;
    }
    return false;
  }

  if (lastLookupValid && lastLookupName == filename) {
    *fileStat = lastLookup;
    return true;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  // Phase 1: Try scanning from cursor position first
  const uint32_t startPos = lastCentralDirPosValid ? lastCentralDirPos : zipDetails.centralDirOffset;
  uint32_t entryStart = startPos;
  bool wrapped = false;
  file.seek(entryStart);

  const size_t targetLen = strlen(filename);
  uint8_t header[CENTRAL_HEADER_BYTES];
  char itemName[256];

  while (true) {
    // stop if we've wrapped and reached our start position
    if (wrapped && entryStart >= startPos) break;

    if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header)) ||
        readLe32(header) != CENTRAL_HEADER_SIGNATURE) {
      // End of central directory
      if (!wrapped && startPos != zipDetails.centralDirOffset) {
        // Wrap around to beginning
        entryStart = zipDetails.centralDirOffset;
        file.seek(entryStart);
        wrapped = true;
        continue;
      }
      break;
    }

    const uint16_t nameLen = readLe16(header + 28);
    const uint16_t extraLen = readLe16(header + 30);
    const uint16_t commentLen = readLe16(header + 32);
    // skip name, extra field + comment
    const uint32_t nextEntry = entryStart + sizeof(header) + nameLen + extraLen + commentLen;

    // Skip names that are too long or cannot match by length
    if (nameLen == targetLen && nameLen < sizeof(itemName)) {
      const int nameRead = file.read(itemName, nameLen);
      if (nameRead != static_cast<int>(nameLen)) {
        LOG_ERR("ZIP", "Failed to read entry name for %s: expected %u bytes, got %d", filename,
                static_cast<unsigned int>(nameLen), nameRead);
        break;
      }
      if (memcmp(itemName, filename, nameLen) == 0) {
        // Found it! Update cursor to next entry
        fileStat->method = readLe16(header + 10);
        fileStat->compressedSize = readLe32(header + 20);
        fileStat->uncompressedSize = readLe32(header + 24);
        fileStat->localHeaderOffset = readLe32(header + 42);
        lastCentralDirPos = nextEntry;
        lastCentralDirPosValid = true;
        lastLookupName.assign(filename, nameLen);
        lastLookup = *fileStat;
        lastLookupValid = true;
        return true;
      }
    }

    entryStart = nextEntry;
    file.seek(entryStart);
  }

  return false;
}

long ZipFile::getDataOffset(const FileStatSlim& fileStat) {
  const ScopedOpenClose zip{*this};
  if (!zip) return -1;

  constexpr auto localHeaderSize = 30;

  uint8_t pLocalHeader[localHeaderSize];
  const uint64_t fileOffset = fileStat.localHeaderOffset;

  file.seek(fileOffset);
  const size_t read = file.read(pLocalHeader, localHeaderSize);

  if (read != localHeaderSize) {
    LOG_ERR("ZIP", "Something went wrong reading the local header");
    return -1;
  }

  if (pLocalHeader[0] + (pLocalHeader[1] << 8) + (pLocalHeader[2] << 16) + (pLocalHeader[3] << 24) !=
      0x04034b50 /* ZIP local file header signature */) {
    LOG_ERR("ZIP", "Not a valid zip file header");
    return -1;
  }

  const uint16_t filenameLength = pLocalHeader[26] + (pLocalHeader[27] << 8);
  const uint16_t extraOffset = pLocalHeader[28] + (pLocalHeader[29] << 8);
  return fileOffset + localHeaderSize + filenameLength + extraOffset;
}

#if LOG_LEVEL >= 2
// Development-only inflate-failure forensics; compiles out of production
// firmware (LOG_LEVEL 0/1).
void ZipFile::logInflateFailure(const FileStatSlim& fileStat, const long dataOffset) {
  // The read cursor shows how far the fill callback got before the inflater
  // gave up; useful to tell genuine truncation from a mid-stream error.
  const uint64_t fileSize = file.size();
  const uint64_t dataEnd = static_cast<uint64_t>(dataOffset) + fileStat.compressedSize;
  LOG_ERR("ZIP", "Inflate failed; compressed=%u uncompressed=%u span=[%ld, %llu) fileSize=%llu readCursor=%llu%s",
          fileStat.compressedSize, fileStat.uncompressedSize, dataOffset, static_cast<unsigned long long>(dataEnd),
          static_cast<unsigned long long>(fileSize), static_cast<unsigned long long>(file.position()),
          dataEnd > fileSize ? "; DATA EXTENDS PAST EOF (truncated file on SD)" : "; span fully inside file");
}
#endif

bool ZipFile::loadZipDetails() {
  if (zipDetails.isSet) {
    return true;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  const size_t fileSize = file.size();
  if (fileSize < 22) {
    LOG_ERR("ZIP", "File too small to be a valid zip");
    return false;  // Minimum EOCD size is 22 bytes
  }

  // We scan the last 1KB (or the whole file if smaller) for the EOCD signature
  // 0x06054b50 is stored as 0x50, 0x4b, 0x05, 0x06 in little-endian
  const int scanRange = fileSize > 1024 ? 1024 : fileSize;
  const auto buffer = static_cast<uint8_t*>(malloc(scanRange));
  if (!buffer) {
    LOG_ERR("ZIP", "Failed to allocate memory for EOCD scan buffer");
    return false;
  }

  file.seek(fileSize - scanRange);
  file.read(buffer, scanRange);

  // Scan backwards for the signature
  int foundOffset = -1;
  for (int i = scanRange - 22; i >= 0; i--) {
    constexpr uint32_t signature = 0x06054b50;
    if (*reinterpret_cast<uint32_t*>(&buffer[i]) == signature) {
      foundOffset = i;
      break;
    }
  }

  if (foundOffset == -1) {
    LOG_ERR("ZIP", "EOCD signature not found in zip file");
    free(buffer);
    return false;
  }

  // Now extract the values we need from the EOCD record
  // Relative positions within EOCD:
  // Offset 10: Total number of entries (2 bytes)
  // Offset 16: Offset of start of central directory with respect to the starting disk number (4 bytes)
  zipDetails.totalEntries = *reinterpret_cast<uint16_t*>(&buffer[foundOffset + 10]);
  zipDetails.centralDirOffset = *reinterpret_cast<uint32_t*>(&buffer[foundOffset + 16]);
  zipDetails.isSet = true;

  free(buffer);
  return true;
}

bool ZipFile::open() {
  if (!Storage.openFileForRead("ZIP", filePath, file)) {
    return false;
  }
  g_fsfile_open_count++;
  return true;
}

bool ZipFile::close() {
  if (file) {
    // Explicit close() required: member variable persists beyond function scope
    file.close();
  }
  lastCentralDirPos = 0;
  lastCentralDirPosValid = false;
  // the archive may change before the next open
  lastLookupValid = false;
  return true;
}

bool ZipFile::getInflatedFileSize(const char* filename, size_t* size) {
  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) {
    return false;
  }

  *size = static_cast<size_t>(fileStat.uncompressedSize);
  return true;
}

int ZipFile::fillUncompressedSizes(std::deque<SizeTarget>& targets, std::deque<uint32_t>& sizes) {
  if (targets.empty()) {
    return 0;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return 0;

  if (!loadZipDetails()) return 0;

  file.seek(zipDetails.centralDirOffset);

  int matched = 0;
  const int targetCount = static_cast<int>(targets.size());
  uint32_t sig;
  char itemName[256];

  while (file.available()) {
    file.read(&sig, 4);
    if (sig != 0x02014b50) break;

    file.seekCur(6);
    uint16_t method;
    file.read(&method, 2);
    file.seekCur(8);
    uint32_t compressedSize, uncompressedSize;
    file.read(&compressedSize, 4);
    file.read(&uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    uint32_t localHeaderOffset;
    file.read(&localHeaderOffset, 4);

    if (nameLen < 256) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';

      uint64_t hash = fnvHash64(itemName, nameLen);
      SizeTarget key = {hash, nameLen, 0};

      auto it = std::lower_bound(targets.begin(), targets.end(), key, [](const SizeTarget& a, const SizeTarget& b) {
        return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
      });

      while (it != targets.end() && it->hash == hash && it->len == nameLen) {
        if (it->index < sizes.size()) {
          sizes[it->index] = uncompressedSize;
          matched++;
        }
        ++it;
      }

      if (matched >= targetCount) {
        break;
      }
    } else {
      file.seekCur(nameLen);
    }

    file.seekCur(m + k);
  }

  return matched;
}

uint8_t* ZipFile::readFileToMemory(const char* filename, size_t* size, const bool trailingNullByte) {
  const ScopedOpenClose zip{*this};
  if (!zip) return nullptr;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return nullptr;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return nullptr;

  file.seek(fileOffset);

  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;
  const auto dataSize = trailingNullByte ? inflatedDataSize + 1 : inflatedDataSize;
  const auto data = static_cast<uint8_t*>(poolMalloc(dataSize));
  if (data == nullptr) {
    LOG_ERR("ZIP", "Failed to allocate memory for output buffer (%zu bytes)", dataSize);
    return nullptr;
  }

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const size_t dataRead = file.read(data, inflatedDataSize);

    if (dataRead != inflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read data");
      poolFree(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else if (fileStat.method == ZIP_METHOD_DEFLATED) {
    auto* fileReadBuffer = static_cast<uint8_t*>(malloc(1024));
    if (!fileReadBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer");
      poolFree(data);
      return nullptr;
    }

    ZipInflateCtx ctx;
    ctx.file = &file;
    ctx.fileRemaining = deflatedDataSize;
    ctx.readBuf = fileReadBuffer;
    ctx.readBufSize = 1024;

    // One-shot mode: `data` holds the entire output, so back-references
    // resolve inside it and no 32KB window is allocated.
    InflateStream inflate;
    if (!inflate.init(false)) {
      LOG_ERR("ZIP", "Failed to init inflate stream for %s", filename);
      free(fileReadBuffer);
      poolFree(data);
      return nullptr;
    }
    inflate.setFill(zipFillCallback, &ctx);

    if (!inflate.read(data, inflatedDataSize)) {
      LOG_ERR("ZIP", "Failed to inflate file");
#if LOG_LEVEL >= 2
      // Development-only forensics: SD raw-read CRC, DRAM one-shot A/B and
      // PSRAM probe. Compiles out of production firmware.
      logInflateFailure(fileStat, fileOffset);
      // Raw-read integrity check: re-read the compressed bytes straight from
      // the SD card (bypassing tinfl) and CRC32 them. Host reference for the
      // same file/span is part0035= a9f4ef0b / part0013= fd3de71b. A mismatch
      // below means the SD read itself returned wrong bytes for the entry,
      // before any decompressor ever touched them.
      file.seek(fileOffset);
      uint32_t crc = 0xFFFFFFFFu;
      size_t remain = deflatedDataSize;
      uint8_t firstWord[4] = {0, 0, 0, 0};
      size_t firstN = 0;
      while (remain > 0) {
        const size_t take = remain < ctx.readBufSize ? remain : ctx.readBufSize;
        const size_t got = file.read(ctx.readBuf, take);
        if (got == 0) break;
        for (size_t i = 0; i < got; i++) {
          if (firstN < sizeof(firstWord)) firstWord[firstN++] = ctx.readBuf[i];
          crc ^= ctx.readBuf[i];
          for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        remain -= got;
      }
      LOG_ERR("ZIP", "RAWREAD: crc=%08x read=%zu/%zu first4=%02x%02x%02x%02x", ~crc, deflatedDataSize - remain,
              static_cast<size_t>(deflatedDataSize), firstWord[0], firstWord[1], firstWord[2], firstWord[3]);
      // setSource decoy: re-read the whole entry contiguously into DRAM and
      // inflate with the input back-ended by memory (no SD fill callbacks).
      // Host reference succeeds; if this fails on-device, tinfl itself is the
      // divergence (codegen/state memory), not the SD/fill plumbing.
      if (deflatedDataSize <= 64 * 1024) {
        auto* dramIn = static_cast<uint8_t*>(malloc(deflatedDataSize));
        auto* dramOut = static_cast<uint8_t*>(malloc(inflatedDataSize));
        if (dramIn && dramOut) {
          file.seek(fileOffset);
          const size_t got = file.read(dramIn, deflatedDataSize);
          InflateStream mem;
          bool memOk = false;
          if (got == deflatedDataSize) {
            memOk = mem.init(false);
            if (memOk) {
              mem.setSource(dramIn, deflatedDataSize);
              memOk = mem.read(dramOut, inflatedDataSize);
            }
          }
          LOG_ERR("ZIP", "A/B: setSource-mem one-shot=%d (got=%zu/%u)", memOk, got,
                  static_cast<unsigned>(deflatedDataSize));
        } else {
          LOG_ERR("ZIP", "A/B: setSource decoy OOM in=%p out=%p", static_cast<void*>(dramIn),
                  static_cast<void*>(dramOut));
        }
        free(dramIn);
        free(dramOut);
      }
#ifdef BOARD_HAS_PSRAM
      // Inflate failed with the one-shot output in PSRAM (poolMalloc). Probe
      // three variables in one shot: (1) does the same stream succeed when the
      // output lives in DRAM, (2) do PSRAM and DRAM outputs diverge, (3) does
      // PSRAM itself read back what it was written. One of these isolates the
      // failure so the deflate-vs-PSRAM layer question is settled definitively.
      if (inflatedDataSize <= 128 * 1024) {
        auto* dramOut = static_cast<uint8_t*>(malloc(inflatedDataSize));
        if (dramOut) {
          file.seek(fileOffset);
          ZipInflateCtx dramCtx;
          dramCtx.file = &file;
          dramCtx.fileRemaining = deflatedDataSize;
          dramCtx.readBuf = fileReadBuffer;
          dramCtx.readBufSize = 1024;
          InflateStream dramInflate;
          bool dramOk = dramInflate.init(false);
          if (dramOk) {
            dramInflate.setFill(zipFillCallback, &dramCtx);
            dramOk = dramInflate.read(dramOut, inflatedDataSize);
          }
          const size_t cmpLen = inflatedDataSize < 32 ? inflatedDataSize : 32;
          size_t match = 0;
          for (size_t i = 0; i < cmpLen; i++) {
            if (dramOut[i] == data[i]) match++;
          }
          LOG_ERR("ZIP", "A/B: DRAM-one-shot=%d matchVPSRAM=%u/%u", dramOk, static_cast<unsigned>(match),
                  static_cast<unsigned>(cmpLen));
          free(dramOut);
        }
      }
      {
        uint8_t* probe = static_cast<uint8_t*>(poolMalloc(4096));
        if (probe) {
          for (size_t i = 0; i < 4096; i++) probe[i] = static_cast<uint8_t>(i * 31);
          size_t bad = 0;
          for (size_t i = 0; i < 4096; i++) {
            if (probe[i] != static_cast<uint8_t>(i * 31)) bad++;
          }
          LOG_ERR("ZIP", "A/B: PSRAM rw probe mismatches=%u/4096", static_cast<unsigned>(bad));
          poolFree(probe);
        }
      }
#endif  // BOARD_HAS_PSRAM
#endif  // LOG_LEVEL >= 2
      free(fileReadBuffer);
      poolFree(data);
      return nullptr;
    }
    free(fileReadBuffer);

    // Continue out of block with data set
  } else {
    LOG_ERR("ZIP", "Unsupported compression method");
    poolFree(data);
    return nullptr;
  }

  if (trailingNullByte) data[inflatedDataSize] = '\0';
  if (size) *size = inflatedDataSize;
  return data;
}

bool ZipFile::readFileToStream(const char* filename, Print& out, const size_t chunkSize, const bool allowEarlyStop) {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return false;

  return readFileToStream(fileStat, out, chunkSize, allowEarlyStop);
}

bool ZipFile::readFileToStream(const FileStatSlim& fileStat, Print& out, const size_t chunkSize,
                               const bool allowEarlyStop) {
  // If the cache already holds a matching entry we are using the in-memory stat;
  // otherwise the caller must have opened the ZIP themselves. Either way, do not
  // re-scan the central directory: use getDataOffset() directly with the provided stat.
  if (!file) {
    LOG_ERR("ZIP", "readFileToStream(FileStatSlim) called without an open ZIP");
    return false;
  }

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return false;

  file.seek(fileOffset);
  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const auto buffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!buffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for buffer");
      return false;
    }

    size_t remaining = inflatedDataSize;
    while (remaining > 0) {
      const size_t dataRead = file.read(buffer, remaining < chunkSize ? remaining : chunkSize);
      if (dataRead == 0) {
        LOG_ERR("ZIP", "Could not read more bytes");
        free(buffer);
        return false;
      }

      if (out.write(buffer, dataRead) != dataRead) {
        free(buffer);
        if (allowEarlyStop) return true;  // sink has what it needs
        LOG_ERR("ZIP", "Failed to write all output bytes to stream");
        return false;
      }
      remaining -= dataRead;
    }

    free(buffer);
    return true;
  }

  if (fileStat.method == ZIP_METHOD_DEFLATED) {
    auto* fileReadBuffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!fileReadBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer");
      return false;
    }

    auto* outputBuffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!outputBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for output buffer");
      free(fileReadBuffer);
      return false;
    }

    ZipInflateCtx ctx;
    ctx.file = &file;
    ctx.fileRemaining = deflatedDataSize;
    ctx.readBuf = fileReadBuffer;
    ctx.readBufSize = chunkSize;

    InflateStream inflate;
    if (!inflate.init(true)) {
      LOG_ERR("ZIP", "Failed to init inflate stream (FileStatSlim, inflatedSize=%zu)", inflatedDataSize);
      free(outputBuffer);
      free(fileReadBuffer);
      return false;
    }
    inflate.setFill(zipFillCallback, &ctx);

    bool success = false;
    size_t totalProduced = 0;

    while (true) {
      size_t produced;
      const InflateStream::Status status = inflate.readAtMost(outputBuffer, chunkSize, &produced);

      totalProduced += produced;
      if (totalProduced > static_cast<size_t>(inflatedDataSize)) {
        LOG_ERR("ZIP", "Decompressed size exceeds expected (%zu > %zu)", totalProduced,
                static_cast<size_t>(inflatedDataSize));
#if LOG_LEVEL >= 2
        logInflateFailure(fileStat, fileOffset);
#endif
        break;
      }

      if (produced > 0) {
        if (out.write(outputBuffer, produced) != produced) {
          if (allowEarlyStop) {
            success = true;  // sink has what it needs
          } else {
            LOG_ERR("ZIP", "Failed to write all output bytes to stream");
          }
          break;
        }
      }

      if (status == InflateStream::Status::Done) {
        if (totalProduced != static_cast<size_t>(inflatedDataSize)) {
          LOG_ERR("ZIP", "Decompressed size mismatch (expected %zu, got %zu)", static_cast<size_t>(inflatedDataSize),
                  totalProduced);
#if LOG_LEVEL >= 2
          logInflateFailure(fileStat, fileOffset);
#endif
          break;
        }
        LOG_DBG("ZIP", "Decompressed %d bytes into %d bytes", deflatedDataSize, inflatedDataSize);
        success = true;
        break;
      }

      if (status == InflateStream::Status::Error) {
        LOG_ERR("ZIP", "Decompression failed");
#if LOG_LEVEL >= 2
        logInflateFailure(fileStat, fileOffset);
#endif
        break;
      }
      // InflateStream::Status::Ok: output buffer full, continue
    }

    free(outputBuffer);
    free(fileReadBuffer);
    return success;  // inflate destructor frees the decompressor state + window
  }

  LOG_ERR("ZIP", "Unsupported compression method");
  return false;
}
