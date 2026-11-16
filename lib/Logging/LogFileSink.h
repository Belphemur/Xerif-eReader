#pragma once

// LogFileSink — optional SD-card file sink for the logPrintf stream (owner
// soak diagnostics: the stuck-indexing capture needs retrievable logs, and
// serial is not always attached).
//
// Every formatted logPrintf line (same timestamp/level/origin prefix the
// serial and RTC ring get) can be appended to /.crosspoint/log.txt when the
// owner enables the feature in Settings → System. Disabled by default: the
// append fast path is one relaxed atomic load.
//
// Wear contract (owner rule — the SD card must not be worn by logging):
//   - The 2 s flush debounce is a MINIMUM interval. A tick with nothing
//     pending issues zero SD operations (the check is an atomic load).
//   - One large append per flush: the whole line buffer goes out in a single
//     sequential write (no seeks, no headers, no rotate-by-rename churn).
//   - When the file reaches kFileCapBytes it restarts empty at the next
//     flush (truncate-on-full); that can only happen once per cap of real
//     logged bytes.
//   - Repeated write failures disable the sink automatically so a bad card
//     cannot turn logging into a retry hammer. Sink failures never log (no
//     recursion); serial and the RTC ring keep working.
//
// Host-testable: the state machine takes a Writer seam (device wires
// HalStorage; tests inject an in-memory fake) and a caller-supplied
// millisecond clock, so flushing behavior is deterministic off-device.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace logfile {

class LogFileSink {
 public:
  // Sequential append-only storage behind the sink. open() may be retried
  // after a failure (the next flush attempts it again); size() backs the
  // truncate-on-full cap.
  class Writer {
   public:
    virtual ~Writer() = default;
    virtual bool open() = 0;                               // open (or reopen) for append
    virtual bool write(const char* data, size_t len) = 0;  // one sequential append
    virtual uint64_t size() = 0;                           // bytes currently in the file
    virtual bool reset() = 0;                              // discard the file, start empty
    virtual void close() = 0;
  };

  static constexpr size_t kBufferBytes = 1024;                   // one large append per flush
  static constexpr size_t kMaxLineBytes = 256;                   // logPrintf's MAX_ENTRY_LEN
  static constexpr uint32_t kDebounceMs = 2000;                  // flush interval MINIMUM
  static constexpr uint64_t kFileCapBytes = 2u * 1024u * 1024u;  // 1-4 MiB allowed
  static constexpr unsigned kMaxConsecutiveFailures = 3;         // then auto-off

  explicit LogFileSink(Writer& writer) : writer_(writer) {}

  // Enables the sink (opens the file lazily on the first flush) or disables
  // it (flushes pending bytes, closes the file). Idempotent.
  void setEnabled(bool on);

  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

  // Buffers one formatted log line ('\n' is appended when missing). The
  // caller supplies the current time; the flush triggers are buffer-full
  // (unconditional) and the debounce (no more than one append per
  // kDebounceMs). A line longer than the buffer is dropped (counted, never
  // half-written).
  void append(const char* line, unsigned long nowMs);

  // Writes pending bytes once the debounce allows (single sequential
  // append). Cheap no-op when nothing is pending: the flush hooks (debounce
  // tick, sleep entry, USB handoff, disable) all issue zero SD operations on
  // an empty buffer, and no more than one append per kDebounceMs otherwise.
  void flush(unsigned long nowMs);

  // Debounce check for the main loop. Zero SD work while nothing is pending.
  void tick(unsigned long nowMs) { flush(nowMs); }

  // Diagnostics for tests.
  size_t pendingBytes() const;
  unsigned droppedLines() const { return droppedLines_; }

 private:
  void flushLocked(unsigned long nowMs, bool debounceGate);
  void onFlushFailure();

  Writer& writer_;
  mutable std::recursive_mutex mutex_;
  std::atomic<bool> enabled_{false};
  std::atomic<bool> hasPending_{false};  // lock-free fast path for append/tick
  char buffer_[kBufferBytes] = {};
  size_t pending_ = 0;                // guarded by mutex_
  unsigned long firstPendingMs_ = 0;  // debounce anchor (first pending byte)
  bool fileOpen_ = false;             // writer_ holds an open file
  unsigned consecutiveFailures_ = 0;
  unsigned droppedLines_ = 0;
};

#if defined(ESP_PLATFORM)
// Device wiring (Arduino build only; host tests drive LogFileSink directly).
void appendLogLine(const char* line);
void setLogFileEnabled(bool on);
void tickLogFile();
void flushLogFile();
bool logFileEnabled();
#endif

}  // namespace logfile
