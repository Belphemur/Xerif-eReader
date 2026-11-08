// Host-test stub definitions for SdCardCacheStorage's HAL dependencies.
#include <utility>

#include "HalFile.h"
#include "HalStorage.h"

void halFileClosed(HalFile& file);  // defined below (open-count release)

bool HalFile::close() {
  if (!open_) return false;
  if (testFailClose) return false;  // injection point: close stays open
  open_ = false;
  halFileClosed(*this);
  return true;
}

bool HalFile::testFailClose = false;
bool HalFile::testFailWrite = false;
bool HalFile::testFailSync = false;
int HalFile::failWriteAtCall = 0;
int HalFile::writeCallCount = 0;
int HalFile::failReadAtCall = 0;
int HalFile::readCallCount = 0;
int HalStorage::failOpenForWriteAtCall = 0;
int HalStorage::openForWriteCallCount = 0;
int HalStorage::failOpenForReadAtCall = 0;
int HalStorage::openForReadCallCount = 0;

HalStorage HalStorage::instance;

bool HalStorage::ensureDirectoryExists(const char* path) {
  (void)path;  // in-memory map needs no directories
  return true;
}

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  (void)moduleName;
  // Failure injection: read-open call #N fails when failOpenForReadAtCall == N
  // (phase-2 gray re-open failure coverage).
  if (failOpenForReadAtCall != 0 && ++openForReadCallCount == failOpenForReadAtCall) return false;
  auto it = files.find(path);
  if (it == files.end()) return false;  // missing file = failed open
  file.data = &it->second;
  file.markOpen(true);
  file.openPath = path;
  ++openCounts[path];
  return true;
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  (void)moduleName;
  // Failure injection: open call #N fails when failOpenForWriteAtCall == N
  // (gray-temp open-failure coverage).
  if (failOpenForWriteAtCall != 0 && ++openForWriteCallCount == failOpenForWriteAtCall) return false;
  // openFileForWrite truncates, matching the real HAL contract.
  file.data = &files[path];
  file.data->clear();
  file.markOpen(true);
  file.openPath = path;
  ++openCounts[path];
  return true;
}

bool HalStorage::exists(const char* path) { return files.count(path) != 0; }

bool HalStorage::remove(const char* path) {
  // FAT semantics: an open handle blocks the unlink (FR_DENIED) — pinning
  // this here caught the gray-temp remove-while-read-handle-open leak.
  const auto open = openCounts.find(path);
  if (open != openCounts.end() && open->second > 0) return false;
  auto it = files.find(path);
  if (it == files.end()) return false;
  files.erase(it);
  openCounts.erase(path);
  return true;
}

bool HalStorage::rename(const char* oldPath, const char* newPath) {
  ++renameCallCount;
  if (failOnRenameCall != 0 && renameCallCount == failOnRenameCall) return false;
  auto it = files.find(oldPath);
  if (it == files.end()) return false;
  if (files.count(newPath) != 0) return false;  // SdFat: no overwrite
  files[newPath] = std::move(it->second);
  files.erase(it);
  openCounts.erase(oldPath);
  return true;
}

// HalFile::close's open-count release (HalFile.h cannot see HalStorage).
void halFileClosed(HalFile& file) {
  auto& storage = HalStorage::getInstance();
  const auto it = storage.openCounts.find(file.openPath);
  if (it != storage.openCounts.end() && it->second > 0) --it->second;
}

bool HalStorage::replaceFile(const char* tmpPath, const char* path) {
  if (!exists(tmpPath)) return false;  // never touch the destination without a replacement
  return (!exists(path) || remove(path)) && rename(tmpPath, path);
}