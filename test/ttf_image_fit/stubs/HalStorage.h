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
  // Directory creation is a no-op on the flat in-memory map (the real mkdir
  // gates nothing the decode tests exercise).
  bool mkdir(const char* path, const bool pFlag = true) {
    (void)path;
    (void)pFlag;
    return true;
  }
  // Failure injection: the Nth openFileForWrite fails when set (0 = off).
  static int failOpenForWriteAtCall;
  static int openForWriteCallCount;
  static int failOpenForReadAtCall;
  static int openForReadCallCount;
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  bool replaceFile(const char* tmpPath, const char* path);

  // Test control: populate with path -> bytes. Empty map = all opens fail.
  std::map<std::string, std::string> files;
  // Open-handle counts per path: remove refuses while a handle is open
  // (FAT semantics — the real HAL cannot unlink an open file).
  std::map<std::string, int> openCounts;
  // Test fixture reset: clears the files AND the open-handle counts.
  void resetFiles() {
    files.clear();
    openCounts.clear();
  }
  // Failure injection: rename call #N fails when failOnRenameCall == N
  // (1-based; 0 = never fail). renameCallCount is public so tests reset it.
  int failOnRenameCall = 0;
  int renameCallCount = 0;

 private:
  HalStorage() = default;
  static HalStorage instance;
};

#define Storage HalStorage::getInstance()