// Host-test stub definitions for SdCardCacheStorage's HAL dependencies.
#include <utility>

#include "HalFile.h"
#include "HalStorage.h"

bool HalFile::testFailClose = false;
bool HalFile::testFailWrite = false;
bool HalFile::testFailSync = false;

HalStorage HalStorage::instance;

bool HalStorage::ensureDirectoryExists(const char* path) {
  (void)path;  // in-memory map needs no directories
  return true;
}

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  (void)moduleName;
  auto it = files.find(path);
  if (it == files.end()) return false;  // missing file = failed open
  file.data = &it->second;
  file.markOpen(true);
  return true;
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  (void)moduleName;
  // openFileForWrite truncates, matching the real HAL contract.
  file.data = &files[path];
  file.data->clear();
  file.markOpen(true);
  return true;
}

bool HalStorage::exists(const char* path) { return files.count(path) != 0; }

bool HalStorage::remove(const char* path) {
  auto it = files.find(path);
  if (it == files.end()) return false;
  files.erase(it);
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
  return true;
}

bool HalStorage::replaceFile(const char* tmpPath, const char* path) {
  if (!exists(tmpPath)) return false;  // never touch the destination without a replacement
  return (!exists(path) || remove(path)) && rename(tmpPath, path);
}