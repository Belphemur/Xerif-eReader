// Host-test stub of HalStorage.h — in-memory filesystem with rename/remove
// semantics matching SdFat (rename does not overwrite) and per-call failure
// injection so tests can exercise the rotate/restore paths of endWrite().
#pragma once

#include <map>
#include <string>

#include "HalFile.h"

// Minimal String stand-in (HalStorage's openFileForRead overload set uses it).
using String = std::string;

class HalStorage {
 public:
  static HalStorage& getInstance() { return instance; }

  bool ensureDirectoryExists(const char* path);
  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const String& path, HalFile& file) {
    return openFileForRead(moduleName, path.c_str(), file);
  }
  bool openFileForWrite(const char* moduleName, const char* path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
    return openFileForWrite(moduleName, path.c_str(), file);
  }
  bool exists(const char* path);
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  bool replaceFile(const char* tmpPath, const char* path);

  // Test control: populate with path -> bytes. Empty map = all opens fail.
  std::map<std::string, std::string> files;
  // Failure injection: rename call #N fails when failOnRenameCall == N
  // (1-based; 0 = never fail). renameCallCount is public so tests reset it.
  int failOnRenameCall = 0;
  int renameCallCount = 0;

 private:
  HalStorage() = default;
  static HalStorage instance;
};

#define Storage HalStorage::getInstance()