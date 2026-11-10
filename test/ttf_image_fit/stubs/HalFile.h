// Host-test stub of HalFile — backed by HalStorage's in-memory file map.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class HalStorage;

class HalFile {
 public:
  HalFile() = default;
  // Wired by HalStorage::openFileFor{Read,Write} to the fake map entry.
  std::string* data = nullptr;
  std::string openPath;  // set at open; remove-refusal bookkeeping key
  size_t cursor = 0;

  bool isOpen() const { return open_; }
  // Used by the HalStorage stub after a successful open.
  void markOpen(bool o) {
    open_ = o;
    cursor = 0;
  }
  uint64_t fileSize64() { return data ? data->size() : 0; }
  // Engine core calls size() (real HalFile API).
  size_t size() { return data ? data->size() : 0; }
  size_t position() const { return cursor; }
  bool seek(size_t pos) { return seek64(pos); }
  // GifCommon walks sub-blocks relative to the cursor.
  bool seekCur(int32_t delta) {
    const int64_t next = static_cast<int64_t>(cursor) + delta;
    if (next < 0) return false;
    return seek64(static_cast<uint64_t>(next));
  }
  bool seek64(uint64_t pos) {
    if (!data || pos > data->size()) return false;
    cursor = static_cast<size_t>(pos);
    return true;
  }
  int read(void* buf, size_t count) {
    if (failReadAtCall != 0 && ++readCallCount == failReadAtCall) return 0;
    if (!data || cursor > data->size()) return 0;
    // Subtraction form: no unsigned overflow before the bound decision.
    const size_t avail = data->size() - cursor;
    const size_t n = count <= avail ? count : avail;
    for (size_t i = 0; i < n; ++i) static_cast<char*>(buf)[i] = (*data)[cursor + i];
    cursor += n;
    return static_cast<int>(n);
  }
  int read() { return -1; }
  size_t write(const void* buf, size_t count) {
    if (testFailWrite) return 0;
    // Failure injection: write call #N fails when failWriteAtCall == N
    // (gray-temp mid-stream failure coverage).
    if (failWriteAtCall != 0 && ++writeCallCount == failWriteAtCall) return 0;
    if (!data || cursor > data->size()) return 0;
    const auto* b = static_cast<const char*>(buf);
    // Subtraction form for the span check, plus a wrap guard so the resize
    // argument cannot overflow: cursor + count must be representable.
    if (count > SIZE_MAX - cursor) return 0;
    if (count > data->size() - cursor) data->resize(cursor + count);
    for (size_t i = 0; i < count; ++i) (*data)[cursor + i] = b[i];
    cursor += count;
    return count;
  }
  size_t write(uint8_t b) { return write(&b, 1); }
  bool sync() {
    if (testFailSync) return false;  // injection point: sync stays open
    return open_;
  }
  bool close();  // defined in Stubs.cpp (releases the storage open-count)

  // Test control (defined in Stubs.cpp).
  static bool testFailClose;
  static bool testFailWrite;
  static bool testFailSync;
  static int failWriteAtCall;  // 0 = off; else the 1-based write call to fail
  static int writeCallCount;
  static int failReadAtCall;  // 0 = off; else the 1-based read call to fail
  static int readCallCount;

  // Production parity: the real Impl destructor closes the SdFat file under
  // the StorageLock (lib/hal/HalStorage.cpp:212), so leaving a positive
  // open-count behind on destruction would diverge from the device —
  // destroyed readers must not block a later remove/replace in the fake map.
  ~HalFile();

 private:
  bool open_ = false;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
};

void halFileClosed(HalFile& file);  // in Stubs.cpp (release the fake open-count)

// Out-of-line so the namespace-scope declaration above is visible to the body.
inline HalFile::~HalFile() {
  if (open_) {
    open_ = false;
    halFileClosed(*this);
  }
}
