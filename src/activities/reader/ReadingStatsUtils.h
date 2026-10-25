#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

struct BookReadingStats;
struct GlobalReadingStats;

constexpr size_t READING_TIME_BUCKET_COUNT = 4;
constexpr uint32_t MIN_GLOBAL_PACE_PAGE_TURNS = 50;
constexpr size_t READING_DAY_OF_WEEK_COUNT = 7;
constexpr size_t READING_HISTORY_DAYS = 730;
constexpr size_t READING_HISTORY_BYTES = (READING_HISTORY_DAYS + 7) / 8;
// Real minutes per day for the Reading Rhythm charts (global v6+). Index 0 is
// the history anchor day; larger indexes are progressively older days.
constexpr size_t READING_MINUTE_HISTORY_DAYS = 91;
constexpr uint16_t READING_MINUTES_PER_DAY = 1440;

constexpr size_t WPM_WINDOW_SIZE = 15;
constexpr size_t WPM_TRIM_COUNT = 2;
constexpr uint16_t WPM_HARD_CAP = 900;  // discard samples above this: not a plausible reading speed
constexpr uint16_t WPM_FLOOR = 80;      // clamp slower samples to this
constexpr uint16_t CALIBRATED_WORDS_PER_PAGE = 220;

constexpr size_t SESSION_WINDOW_SIZE = 10;
constexpr size_t SESSION_TRIM_COUNT = 2;
constexpr uint32_t SESSION_MIN_SECONDS = 30;  // shorter sessions are not recorded
// A session with fewer completed forward page turns than this is treated as
// "book left open" and never enters the Avg Session window.
constexpr size_t SESSION_MIN_PAGE_TURNS = 2;
// The Avg Session cell switches from the all-time arithmetic mean to the
// window's (partial) result once this many sessions are recorded.
constexpr size_t SESSION_MIN_DISPLAY_SAMPLES = 4;

enum class ReadingTimeBucket : uint8_t { Morning = 0, Afternoon, Evening, Night };

// Rolling 15-sample reading-speed window (words per minute) with a trimmed
// mean: the two fastest and two slowest samples are dropped so a single
// anomalous page (a glance away, an unusually easy page) doesn't skew the
// estimate. Samples are contiguous in [0, count); the window wraps only once
// full, so the circular buffer is never read with a gap.
struct WpmWindow {
  std::array<uint16_t, WPM_WINDOW_SIZE> samples{};
  uint16_t avg = 0;   // trimmed mean, 0 until the first sample
  uint8_t pos = 0;    // next insertion slot
  uint8_t count = 0;  // valid samples, 0..WPM_WINDOW_SIZE

  void record(uint32_t seconds, uint16_t wordsOnPage);
  uint16_t trimmedMean() const;
  // Clamps count/pos loaded from untrusted binary data (samples[pos] must stay
  // in bounds) and recomputes the average from the window.
  void normalize();
  void clear() {
    samples.fill(0);
    avg = 0;
    pos = 0;
    count = 0;
  }
  bool full() const { return count >= WPM_WINDOW_SIZE; }
};

// Rolling 10-sample session-duration window (seconds) with a trimmed mean:
// the two longest and two shortest sessions are dropped so a single marathon
// (or an aborted glance) doesn't skew the average — the Kindle-style session
// algorithm, mirroring WpmWindow. Samples are contiguous in [0, count); the
// window wraps only once full.
struct SessionWindow {
  std::array<uint16_t, SESSION_WINDOW_SIZE> samples{};
  uint16_t avg = 0;   // trimmed mean in seconds, 0 until the first sample
  uint8_t pos = 0;    // next insertion slot
  uint8_t count = 0;  // valid samples, 0..SESSION_WINDOW_SIZE

  void record(uint32_t seconds);
  uint16_t trimmedMean() const;
  // Same post-load invariant as WpmWindow::normalize().
  void normalize();
  void clear() {
    samples.fill(0);
    avg = 0;
    pos = 0;
    count = 0;
  }
  bool full() const { return count >= SESSION_WINDOW_SIZE; }
};

struct ReadingStatsDate {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;

  bool isValid() const;
  void clear();
};

struct ReadingStatsDateTime {
  ReadingStatsDate date;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;

  bool isValid() const;
};

bool isLeapYear(uint16_t year);
uint8_t daysInMonth(uint16_t year, uint8_t month);
bool isValidReadingStatsDate(const ReadingStatsDate& date);
int compareReadingStatsDate(const ReadingStatsDate& lhs, const ReadingStatsDate& rhs);
void addDaysToReadingStatsDate(ReadingStatsDate& date, int delta);
void addSecondsToReadingStatsDateTime(ReadingStatsDateTime& dt, uint32_t seconds);
uint32_t readingStatsDayIndex(const ReadingStatsDate& date);
bool readingStatsDateFromDayIndex(uint32_t dayIndex, ReadingStatsDate& outDate);
uint8_t readingStatsDayOfWeekIndex(const ReadingStatsDate& date);  // Monday = 0
ReadingTimeBucket readingTimeBucketForHour(uint8_t hour);
bool getCurrentLocalReadingStatsDateTime(ReadingStatsDateTime& outDateTime);

// Lightweight accessor for CrossPointSettings::shouldTrackReadingStats().
// Defined in ReadingStatsUtils.cpp so library code (e.g. lib/SQLite) can gate
// on the setting without including the heavy CrossPointSettings.h header.
bool readingStatsTrackingEnabled();

uint16_t readingSpanDaysInclusive(const ReadingStatsDate& start, const ReadingStatsDate& end);
uint16_t readingSpanDaysElapsed(const ReadingStatsDate& start, const ReadingStatsDate& end);
void formatReadingStatsShortDate(const ReadingStatsDate& date, char* buf, size_t len);
void formatReadingStatsMonthToken(const ReadingStatsDate& date, char* buf, size_t len);
void formatCompactReadingDuration(uint32_t seconds, char* buf, size_t len);

std::optional<uint32_t> resolveReadingPaceSecondsPerPage(const BookReadingStats& bookStats,
                                                         const GlobalReadingStats& globalStats,
                                                         uint16_t wordsOnPage = 0);
std::optional<uint32_t> estimateChapterTimeLeftSeconds(const BookReadingStats& bookStats,
                                                       const GlobalReadingStats& globalStats, uint16_t pagesRemaining,
                                                       uint16_t wordsOnPage = 0);
std::optional<uint32_t> estimateBookTimeLeftSeconds(const BookReadingStats& bookStats,
                                                    const GlobalReadingStats& globalStats,
                                                    uint32_t estimatedRemainingPages, uint16_t wordsOnPage = 0);
void formatChapterTimeLeft(uint32_t seconds, char* buf, size_t len);
// Avg-session display value with the legacy arithmetic-mean fallback: a
// non-empty session window wins (trimmed mean); otherwise the all-time
// totals produce the plain mean; with neither there is no value.
// Returns nullopt for the "-" sentinel cell.
std::optional<uint32_t> avgSessionSeconds(uint16_t windowAvg, uint8_t windowCount, uint64_t totalReadingSeconds,
                                          uint64_t sessionCount);
// One-line "N% • time-left" summary for the home Continue Reading card:
// unknown progress renders "-", a zero time-left estimate renders the
// unavailable marker instead of a duration.
void formatHomeProgressLine(const BookReadingStats& stats, char* buf, size_t len);
// Sleep/shutdown info line: "42% • ~2h 15m • 134 pages left".
// Segments with unknown data are dropped; percent always leads when known.
void formatSleepProgressLine(int percent, uint32_t timeLeftSeconds, int pagesLeft, char* buf, size_t len);
// CrossInk-style stat cell: decimal value, or the localized unavailable
// marker when the value is missing (no data / feature disabled).
std::string formatStatCell(uint32_t value, bool hasValue = true);

void recordReadingSpanIntoBuckets(std::array<uint32_t, READING_TIME_BUCKET_COUNT>& timeOfDaySeconds,
                                  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT>& dayOfWeekSeconds,
                                  const ReadingStatsDateTime& localStart, uint32_t seconds);
void markReadingHistoryDay(uint32_t& anchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& bits, uint32_t dayIndex);
void recordReadingSpanIntoHistory(uint32_t& anchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& bits,
                                  std::array<uint16_t, READING_MINUTE_HISTORY_DAYS>& dailyMinutes,
                                  const ReadingStatsDateTime& localStart, uint32_t seconds);
// Value for the given calendar day from the rolling 91-day minutes array;
// returns 0 when the day is absent or outside the window.
uint16_t readingMinutesForDay(uint32_t anchorDay, const std::array<uint16_t, READING_MINUTE_HISTORY_DAYS>& dailyMinutes,
                              uint32_t dayIndex);
void mergeReadingHistory(uint32_t& targetAnchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& targetBits,
                         uint32_t sourceAnchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& sourceBits);
uint16_t computeReadingHistoryLongestStreak(uint32_t anchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& bits);
uint16_t computeReadingHistoryCurrentStreak(uint32_t anchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& bits,
                                            const ReadingStatsDate* today);

// Lean, dependency-free database key for a book's reading stats.
// [Dead code since the binary-store migration: the store keys records by the
// cache path directly. Kept only because ReadingStatsUtilsTest still exercises
// it; remove together with that test when doing a cleanup pass.]
std::string bookStatsDbKey(const std::string& cachePath);
