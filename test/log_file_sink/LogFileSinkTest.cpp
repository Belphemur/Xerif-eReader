// LogFileSinkTest — host suites for the SD debug log sink's state machine
// (lib/Logging/LogFileSink.cpp). The storage seam is an in-memory fake: the
// wear contract (zero writes on empty ticks, one large append per flush,
// truncate-on-full cap, auto-off after repeated failures) is exercised
// without an SD card.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "LogFileSink.h"

namespace {

// In-memory append file: records each write() call as one append
// transaction, so tests can assert "one large append per flush".
class FakeWriter final : public logfile::LogFileSink::Writer {
 public:
  bool open() override {
    if (failOpens > 0) {
      --failOpens;
      return false;
    }
    open_ = true;
    ++openCalls;
    return true;
  }
  size_t write(const char* data, const size_t len) override {
    if (failWrites > 0) {
      --failWrites;
      return 0;
    }
    if (!open_) return 0;
    // shortBy > 0 simulates a short write: the card accepts len - shortBy
    // bytes (0 when the write is shorter than the shortfall).
    const size_t accepted = shortBy > 0 ? (len > shortBy ? len - shortBy : 0) : len;
    if (accepted == 0) return 0;
    appends.emplace_back(data, accepted);
    size_ += accepted;
    return accepted;
  }
  uint64_t size() override { return size_; }
  bool reset() override {
    appends.clear();
    size_ = 0;
    ++resetCalls;
    return true;
  }
  void close() override {
    open_ = false;
    ++closeCalls;
  }

  unsigned failOpens = 0;
  unsigned failWrites = 0;
  size_t shortBy = 0;
  int openCalls = 0;
  int closeCalls = 0;
  int resetCalls = 0;
  uint64_t size_ = 0;
  bool open_ = false;
  std::vector<std::string> appends;

  std::string content() const {
    std::string out;
    for (const auto& a : appends) out += a;
    return out;
  }
};

struct SinkAndWriter {
  FakeWriter writer;
  logfile::LogFileSink sink{writer};

  void enable() { sink.setEnabled(true); }
};

constexpr unsigned long kDebounce = logfile::LogFileSink::kDebounceMs;

}  // namespace

// Disabled sink: every entry point is a no-op, no file is ever opened.
TEST(LogFileSinkTest, DisabledSinkNeverTouchesStorage) {
  SinkAndWriter s;
  s.sink.append("[1] [INF] [X] hello\n", 0);
  s.sink.tick(0);
  s.sink.flush(0);
  EXPECT_FALSE(s.sink.enabled());
  EXPECT_TRUE(s.writer.appends.empty());
  EXPECT_EQ(s.writer.openCalls, 0);
}

// Debounce: bytes coalesce into ONE append at the interval, not one write
// per line; the interval is a minimum (nothing pending = no writes at all).
TEST(LogFileSinkTest, DebounceCoalescesIntoOneAppend) {
  SinkAndWriter s;
  s.enable();
  s.sink.append("[1] [INF] [X] a\n", 100);
  s.sink.append("[2] [INF] [X] b\n", 1200);
  s.sink.tick(1900);  // before the interval: nothing out yet
  EXPECT_TRUE(s.writer.appends.empty());
  EXPECT_EQ(s.writer.openCalls, 0);  // lazy open: no file until a flush

  s.sink.tick(kDebounce + 100);
  ASSERT_EQ(s.writer.appends.size(), 1u);  // one large append per flush
  EXPECT_EQ(s.writer.appends[0], "[1] [INF] [X] a\n[2] [INF] [X] b\n");
  EXPECT_EQ(s.writer.content(), "[1] [INF] [X] a\n[2] [INF] [X] b\n");

  // Empty ticks never touch the card.
  const size_t writesBefore = s.writer.appends.size();
  s.sink.tick(kDebounce + 2000);
  s.sink.flush(kDebounce + 2100);
  EXPECT_EQ(s.writer.appends.size(), writesBefore);
}

// Buffer-full: the pending bytes go out before the overflowing line, and the
// overflowing line is not split. Lines are clamped to kMaxLineBytes (logPrintf's
// own MAX_ENTRY_LEN), so the buffer fills across several max-length lines.
TEST(LogFileSinkTest, BufferFullFlushesThenAppendsLine) {
  SinkAndWriter s;
  s.enable();
  // Four max-length lines: 1020 bytes, one short of the buffer.
  std::string three;
  for (int i = 0; i < 4; ++i) {
    const std::string line =
        "[" + std::to_string(i) + "] [INF] [X] " + std::string(logfile::LogFileSink::kMaxLineBytes - 16, 'x') + "\n";
    ASSERT_EQ(line.size(), logfile::LogFileSink::kMaxLineBytes - 1);
    s.sink.append(line.c_str(), 0);
    three += line;
  }
  EXPECT_TRUE(s.writer.appends.empty());  // still under the buffer, debounced
  // A fourth line overflows the buffer: flush-on-full fires (not debounced).
  const std::string fourth = "[3] [INF] [X] y\n";
  s.sink.append(fourth.c_str(), 100);
  ASSERT_GE(s.writer.appends.size(), 1u);
  EXPECT_EQ(s.writer.appends[0], three);
  // The overflowing line stays buffered whole (its own flush follows later).
  EXPECT_EQ(s.writer.content(), three);
  EXPECT_EQ(s.sink.pendingBytes(), fourth.size());
}

// flushAndClose: the sleep/power-off entry — a young buffer drains WITHOUT
// the debounce gate, the handle is released, and the sink stays enabled.
TEST(LogFileSinkTest, FlushAndCloseDrainsYoungBufferAndReleasesHandle) {
  SinkAndWriter s;
  s.enable();
  s.sink.append("[1] [INF] [X] young\n", /*nowMs=*/0);
  EXPECT_EQ(s.writer.appends.size(), 0u);  // still inside the debounce window

  s.sink.flushAndClose();
  ASSERT_EQ(s.writer.appends.size(), 1u);
  EXPECT_EQ(s.writer.appends[0], "[1] [INF] [X] young\n");
  EXPECT_EQ(s.writer.closeCalls, 1);
  EXPECT_EQ(s.sink.pendingBytes(), 0u);
  EXPECT_TRUE(s.sink.enabled());  // the setting survives; next flush reopens

  // The next flush reopens lazily and appends without re-writing the prefix
  // (the fresh append re-anchors the debounce window, so the flush waits a
  // full interval — the debounce is a MINIMUM).
  s.sink.append("[2] [INF] [X] after\n", kDebounce);
  s.sink.flush(2 * kDebounce);
  ASSERT_EQ(s.writer.appends.size(), 2u);
  EXPECT_EQ(s.writer.appends[1], "[2] [INF] [X] after\n");
  EXPECT_EQ(s.writer.content(), "[1] [INF] [X] young\n[2] [INF] [X] after\n");
}

// Sleep-boundary contract: a line appended after the close cannot be written
// (the card is unmounted by then, so the lazy reopen fails) — the sink must
// keep the bytes buffered instead of dropping them, and a later drain writes
// them whole. This is what lets the firmware drain again after its last log
// emission (enterDeepSleep) and still capture the late lines.
TEST(LogFileSinkTest, AppendAfterCloseStaysBufferedUntilNextDrain) {
  SinkAndWriter s;
  s.enable();
  s.sink.append("[1] [INF] [X] before\n", /*nowMs=*/0);
  s.sink.flushAndClose();
  ASSERT_EQ(s.writer.appends.size(), 1u);
  EXPECT_EQ(s.writer.closeCalls, 1);

  // The card is gone: the reopen fails and the line stays pending.
  s.writer.failOpens = 1;
  s.sink.append("[2] [INF] [X] late\n", kDebounce);
  EXPECT_EQ(s.writer.appends.size(), 1u);
  EXPECT_EQ(s.sink.pendingBytes(), 19u);  // "[2] ..." whole line buffered, not dropped

  // Storage is back (next boot or a re-mounted card): the drain writes it.
  s.writer.failOpens = 0;
  s.sink.flushAndClose();
  ASSERT_EQ(s.writer.appends.size(), 2u);
  EXPECT_EQ(s.writer.appends[1], "[2] [INF] [X] late\n");
  EXPECT_EQ(s.writer.content(), "[1] [INF] [X] before\n[2] [INF] [X] late\n");
  EXPECT_EQ(s.sink.pendingBytes(), 0u);
  EXPECT_TRUE(s.sink.enabled());  // the setting still survives the loss window
}

// No buffered bytes and no open handle: flushAndClose issues zero SD work.
TEST(LogFileSinkTest, FlushAndCloseEmptyBufferTouchesNothing) {
  SinkAndWriter s;
  s.sink.flushAndClose();  // disabled
  EXPECT_EQ(s.writer.closeCalls, 0);
  EXPECT_EQ(s.writer.openCalls, 0);

  s.enable();
  s.sink.flushAndClose();  // enabled, empty (lazy open has not run yet)
  EXPECT_EQ(s.writer.closeCalls, 0);
  EXPECT_EQ(s.writer.openCalls, 0);
}

// A short write keeps only the unwritten suffix: the next flush appends the
// remainder without replaying the bytes the card already took.
TEST(LogFileSinkTest, ShortWriteRetainsUnwrittenSuffix) {
  SinkAndWriter s;
  s.enable();
  s.sink.append("aa\n", /*nowMs=*/0);
  s.sink.append("bb\n", /*nowMs=*/0);
  s.writer.shortBy = 2;  // the card takes "aa\nb", chokes on the last 2 bytes
  s.sink.flush(kDebounce);
  ASSERT_EQ(s.writer.appends.size(), 1u);
  EXPECT_EQ(s.writer.appends[0], "aa\nb");
  EXPECT_EQ(s.sink.pendingBytes(), 2u);  // "b\n" stays buffered

  s.writer.shortBy = 0;
  s.sink.flush(2 * kDebounce);
  ASSERT_EQ(s.writer.appends.size(), 2u);
  EXPECT_EQ(s.writer.appends[1], "b\n");
  EXPECT_EQ(s.writer.content(), "aa\nbb\n");  // no duplication, no loss
  EXPECT_EQ(s.sink.pendingBytes(), 0u);
}

// Disable flushes what is pending and releases the file, so the card can be
// unmounted or the file read right after the toggle.
TEST(LogFileSinkTest, DisableFlushesPendingAndCloses) {
  SinkAndWriter s;
  s.enable();
  s.sink.append("[1] [INF] [X] pending\n", 0);
  EXPECT_TRUE(s.writer.appends.empty());
  s.sink.setEnabled(false);
  ASSERT_EQ(s.writer.appends.size(), 1u);
  EXPECT_EQ(s.writer.appends[0], "[1] [INF] [X] pending\n");
  EXPECT_EQ(s.writer.closeCalls, 1);
  EXPECT_FALSE(s.sink.enabled());
  EXPECT_EQ(s.sink.pendingBytes(), 0u);
}

// Wear: the file restarts (single reset, no rename churn) only when the cap
// is actually reached; the pending line is then written to the fresh file.
TEST(LogFileSinkTest, CapReachedRestartsFile) {
  SinkAndWriter s;
  s.enable();
  s.writer.size_ = logfile::LogFileSink::kFileCapBytes;  // the file is at the cap
  s.sink.append("[1] [INF] [X] over\n", 0);
  s.sink.flush(kDebounce);  // past the debounce: the flush runs
  EXPECT_EQ(s.writer.resetCalls, 1);
  ASSERT_EQ(s.writer.appends.size(), 1u);
  EXPECT_EQ(s.writer.appends[0], "[1] [INF] [X] over\n");
  EXPECT_EQ(s.writer.size_, sizeof("[1] [INF] [X] over\n") - 1);
}

// Repeated write failures disable the sink (auto-off) instead of retry-
// hammering a bad card; pending bytes are kept until then.
TEST(LogFileSinkTest, RepeatedWriteFailuresAutoDisable) {
  SinkAndWriter s;
  s.writer.failWrites = logfile::LogFileSink::kMaxConsecutiveFailures;
  s.enable();
  for (unsigned i = 0; i < logfile::LogFileSink::kMaxConsecutiveFailures; ++i) {
    s.sink.append("[1] [INF] [X] f\n", i * 2 * kDebounce);
    s.sink.flush(i * 2 * kDebounce + 1);
    EXPECT_EQ(s.sink.enabled(), i + 1 < logfile::LogFileSink::kMaxConsecutiveFailures);
  }
  EXPECT_EQ(s.sink.pendingBytes(), 0u);
  EXPECT_EQ(s.writer.appends.size(), 0u);
}

// Failed opens also converge to the auto-off; pending bytes are preserved
// until then (the owner's diagnostics are not lost on a transient absence).
TEST(LogFileSinkTest, FailedOpensRetryUntilAutoDisable) {
  SinkAndWriter s;
  s.writer.failOpens = logfile::LogFileSink::kMaxConsecutiveFailures;
  s.enable();
  s.sink.append("[1] [INF] [X] kept\n", 0);
  s.sink.flush(kDebounce);  // first open failure
  EXPECT_TRUE(s.sink.enabled());
  EXPECT_EQ(s.sink.pendingBytes(), sizeof("[1] [INF] [X] kept\n") - 1);
  s.sink.flush(3 * kDebounce);  // second open failure
  EXPECT_TRUE(s.sink.enabled());
  s.sink.flush(5 * kDebounce);  // third failure → auto-off
  EXPECT_FALSE(s.sink.enabled());
  EXPECT_EQ(s.sink.pendingBytes(), 0u);
}

// A max-length logPrintf line (MAX_ENTRY_LEN) buffers whole and goes out
// as its own flush transaction — never welded or split.
TEST(LogFileSinkTest, MaxLengthLineBuffersWhole) {
  SinkAndWriter s;
  s.enable();
  const std::string line = "[1] [INF] [X] " + std::string(logfile::LogFileSink::kMaxLineBytes - 20, 'z') + "\n";
  ASSERT_LT(line.size(), logfile::LogFileSink::kBufferBytes);
  s.sink.append(line.c_str(), 0);
  EXPECT_EQ(s.writer.appends.size(), 0u);  // buffered until the debounce
  s.sink.flush(kDebounce);
  ASSERT_EQ(s.writer.appends.size(), 1u);
  EXPECT_EQ(s.writer.appends[0], line);
}

// Append without a trailing newline gets one (the serial stream already
// terminates lines; the sink must not weld two lines together).
TEST(LogFileSinkTest, MissingNewlineIsAdded) {
  SinkAndWriter s;
  s.enable();
  s.sink.append("[1] [ERR] [X] no newline", 0);
  s.sink.append("[2] [INF] [X] next", kDebounce + 5);
  ASSERT_EQ(s.writer.appends.size(), 1u);
  EXPECT_EQ(s.writer.appends[0], "[1] [ERR] [X] no newline\n[2] [INF] [X] next\n");
}
