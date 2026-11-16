#include "LogFileSink.h"

#include <cstring>
#include <mutex>

namespace logfile {

void LogFileSink::setEnabled(const bool on) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (on == enabled_.load(std::memory_order_relaxed)) return;
  if (on) {
    // Lazily opened on the first flush; a transiently absent card must not
    // block the enable itself (the first flush retry opens the file).
    consecutiveFailures_ = 0;
    enabled_.store(true, std::memory_order_relaxed);
    return;
  }
  // Disable path: flush what is pending while the sink is still enabled
  // (the buffer-full trigger, not the debounce — the card can be pulled
  // right after the toggle), then close the handle so the card can be
  // unmounted (deep sleep, USB handoff). A flush failure here drops the
  // residue with the handle: best effort at a card that is going away.
  flushLocked(0, /*debounceGate=*/false);
  enabled_.store(false, std::memory_order_relaxed);
  writer_.close();
  fileOpen_ = false;
}

void LogFileSink::append(const char* line, const unsigned long nowMs) {
  if (!enabled_.load(std::memory_order_relaxed) || line == nullptr) return;
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!enabled_.load(std::memory_order_relaxed)) return;

  const size_t len = strnlen(line, kMaxLineBytes);
  if (len == 0) return;

  // (a) buffer full: flush first (unconditional — this is the trigger) so
  // the overflowing line lands whole in a fresh buffer.
  if (pending_ + len + 1 > kBufferBytes) flushLocked(nowMs, /*debounceGate=*/false);
  if (pending_ + len + 1 > kBufferBytes) {
    // Line still does not fit (flush failed or line exceeds the buffer):
    // drop it whole rather than splitting a line across appends.
    ++droppedLines_;
    return;
  }
  if (pending_ == 0) firstPendingMs_ = nowMs;
  memcpy(buffer_ + pending_, line, len);
  pending_ += len;
  if (buffer_[pending_ - 1] != '\n') buffer_[pending_++] = '\n';
  hasPending_.store(true, std::memory_order_relaxed);

  // (b) debounce: at most one append per interval rides the log line that
  // crossed the interval; other callers' ticks stay zero-write.
  if (nowMs - firstPendingMs_ >= kDebounceMs) flushLocked(nowMs, /*debounceGate=*/true);
}

void LogFileSink::flush(const unsigned long nowMs) {
  if (!hasPending_.load(std::memory_order_acquire)) return;  // zero SD work when empty
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  flushLocked(nowMs, /*debounceGate=*/true);
}

size_t LogFileSink::pendingBytes() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return pending_;
}
// (mutex_ is mutable: the diagnostic accessor locks like the mutators.)

void LogFileSink::flushLocked(const unsigned long nowMs, const bool debounceGate) {
  if (pending_ == 0 || !enabled_.load(std::memory_order_relaxed)) {
    if (pending_ == 0) hasPending_.store(false, std::memory_order_relaxed);
    return;
  }
  // The debounce is a MINIMUM interval, not a target: a younger buffer waits
  // (a later tick or append flushes it); only the buffer-full trigger and the
  // disable path bypass it.
  if (debounceGate && nowMs - firstPendingMs_ < kDebounceMs) return;

  if (!fileOpen_) {
    if (!writer_.open()) {
      onFlushFailure();
      return;  // pending bytes stay buffered for the next flush attempt
    }
    fileOpen_ = true;
  }
  // Truncate-on-full at the cap: reachable only when the cap is actually
  // reached, and it restarts the file instead of churning renames.
  if (writer_.size() >= kFileCapBytes) {
    writer_.close();
    fileOpen_ = false;
    if (!writer_.reset() || !writer_.open()) {
      onFlushFailure();
      return;
    }
    fileOpen_ = true;
  }
  if (writer_.write(buffer_, pending_)) {
    pending_ = 0;  // the next append re-anchors the debounce window
    consecutiveFailures_ = 0;
    hasPending_.store(false, std::memory_order_relaxed);
    return;
  }
  onFlushFailure();
}

void LogFileSink::onFlushFailure() {
  // Keep the pending bytes (they are the owner's diagnostics) and retry on
  // later flushes; a persistently bad card reaches the auto-off quickly and
  // silently — no log recursion, serial/RTC ring unaffected.
  hasPending_.store(true, std::memory_order_relaxed);
  ++consecutiveFailures_;
  if (consecutiveFailures_ >= kMaxConsecutiveFailures) {
    // Auto-off: a bad card must not turn logging into a retry hammer.
    enabled_.store(false, std::memory_order_relaxed);
    hasPending_.store(false, std::memory_order_relaxed);
    pending_ = 0;
    writer_.close();
    fileOpen_ = false;
  }
}

}  // namespace logfile

#if defined(ESP_PLATFORM)

// Device wiring lives OUTSIDE the namespace above: the HalStorage include
// pulls C headers (newlib) that must never be parsed inside a namespace.
#include <HalStorage.h>

namespace {

// HalStorage-backed writer: the codebase's own SD append pattern
// (PluginEvents.cpp:223) — O_APPEND handle kept open between flushes, all
// access serialized by HalStorage's recursive StorageLock.
constexpr const char* kLogFilePath = "/.crosspoint/log.txt";

class HalStorageLogWriter final : public logfile::LogFileSink::Writer {
 public:
  bool open() override {
    Storage.ensureDirectoryExists("/.crosspoint");
    file_ = Storage.open(kLogFilePath, O_WRONLY | O_CREAT | O_APPEND);
    return static_cast<bool>(file_);
  }
  bool write(const char* data, const size_t len) override {
    if (!file_) return false;
    return file_.write(data, len) == len;
  }
  uint64_t size() override { return file_ ? file_.fileSize64() : 0; }
  bool reset() override { return Storage.remove(kLogFilePath); }
  void close() override {
    if (file_) file_.close();
    file_ = HalFile{};
  }

 private:
  HalFile file_;
};

HalStorageLogWriter deviceWriter;
logfile::LogFileSink sink(deviceWriter);

}  // namespace

namespace logfile {

void appendLogLine(const char* line) { sink.append(line, millis()); }
void setLogFileEnabled(const bool on) { sink.setEnabled(on); }
void tickLogFile() { sink.tick(millis()); }
void flushLogFile() { sink.flush(millis()); }
bool logFileEnabled() { return sink.enabled(); }

}  // namespace logfile

#endif  // ESP_PLATFORM
