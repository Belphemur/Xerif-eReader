// Host-test stub of HalFile — backed by HalStorage's in-memory file map.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class HalStorage;

class HalFile {
 public:
  // Wired by HalStorage::openFileFor{Read,Write} to the fake map entry.
  std::string* data = nullptr;
  size_t cursor = 0;
  // Sub-file view (openFileSubview): seek(0) lands at viewBase, reads clamp
  // at viewBase + viewLen — the same translation the real HAL applies for
  // the combined cover cache's section reads.
  size_t viewBase = 0;
  size_t viewLen = 0;  // 0 = no view (whole file)

  bool hasView() const { return viewLen != 0; }
  bool setSubview(size_t offset, size_t length) {
    if (data == nullptr || offset > data->size() || length > data->size() - offset) return false;
    viewBase = offset;
    viewLen = length;
    cursor = offset;
    return true;
  }

  bool isOpen() const { return open_; }
  // Used by the HalStorage stub after a successful open.
  void markOpen(bool o) {
    open_ = o;
    cursor = 0;
  }
  uint64_t fileSize64() { return data ? data->size() : 0; }
  // Engine core calls size() (real HalFile API). A view reports its window.
  size_t size() {
    if (!data) return 0;
    return hasView() ? viewLen : data->size();
  }
  size_t position() const { return hasView() && cursor >= viewBase ? cursor - viewBase : cursor; }
  bool seek(size_t pos) { return seek64(pos); }
  bool seek64(uint64_t pos) {
    if (!data) return false;
    if (hasView()) {
      if (pos > viewLen) return false;
      cursor = viewBase + static_cast<size_t>(pos);
      return true;
    }
    if (pos > data->size()) return false;
    cursor = static_cast<size_t>(pos);
    return true;
  }
  bool seekCur(int64_t offset) {
    if (!data) return false;
    if (hasView()) {
      const int64_t rel = static_cast<int64_t>(cursor - viewBase) + offset;
      if (rel < 0 || static_cast<uint64_t>(rel) > viewLen) return false;
      cursor = viewBase + static_cast<size_t>(rel);
      return true;
    }
    const int64_t abs = static_cast<int64_t>(cursor) + offset;
    if (abs < 0 || static_cast<uint64_t>(abs) > data->size()) return false;
    cursor = static_cast<size_t>(abs);
    return true;
  }
  int read(void* buf, size_t count) {
    if (!data || cursor > data->size()) return 0;
    // Subtraction form: no unsigned overflow before the bound decision.
    size_t avail = data->size() - cursor;
    if (hasView()) {
      if (cursor >= viewBase + viewLen) return 0;  // the view's EOF
      const size_t viewAvail = viewBase + viewLen - cursor;
      if (viewAvail < avail) avail = viewAvail;
    }
    const size_t n = count <= avail ? count : avail;
    for (size_t i = 0; i < n; ++i) static_cast<char*>(buf)[i] = (*data)[cursor + i];
    cursor += n;
    return static_cast<int>(n);
  }
  int read() { return -1; }
  size_t write(const void* buf, size_t count) {
    if (testFailWrite) return 0;
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
  bool close() {
    if (!open_) return false;
    if (testFailClose) return false;  // injection point: close stays open
    open_ = false;
    return true;
  }

  // Test control (defined in Stubs.cpp).
  static bool testFailClose;
  static bool testFailWrite;
  static bool testFailSync;

 private:
  bool open_ = false;
};