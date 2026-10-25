#include "ReadingStatsUtils.h"

#include <HalClock.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <numeric>

#include "BookReadingStats.h"
#include "CrossPointSettings.h"
#include "GlobalReadingStats.h"

namespace {
constexpr const char* MONTH_NAMES[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

bool isBitSet(const std::array<uint8_t, READING_HISTORY_BYTES>& bits, const size_t bitIndex) {
  if (bitIndex >= READING_HISTORY_DAYS) {
    return false;
  }
  return (bits[bitIndex / 8] & static_cast<uint8_t>(1u << (bitIndex % 8))) != 0;
}

void setBit(std::array<uint8_t, READING_HISTORY_BYTES>& bits, const size_t bitIndex) {
  if (bitIndex >= READING_HISTORY_DAYS) {
    return;
  }
  bits[bitIndex / 8] |= static_cast<uint8_t>(1u << (bitIndex % 8));
}

void clearBits(std::array<uint8_t, READING_HISTORY_BYTES>& bits) { bits.fill(0); }

void shiftMinutesOlder(std::array<uint16_t, READING_MINUTE_HISTORY_DAYS>& minutes, const size_t shiftDays) {
  if (shiftDays == 0) return;
  if (shiftDays >= READING_MINUTE_HISTORY_DAYS) {
    minutes.fill(0);
    return;
  }
  std::array<uint16_t, READING_MINUTE_HISTORY_DAYS> shifted = {};
  for (size_t dayOffset = 0; dayOffset + shiftDays < READING_MINUTE_HISTORY_DAYS; ++dayOffset) {
    shifted[dayOffset + shiftDays] = minutes[dayOffset];
  }
  minutes = shifted;
}

void shiftHistoryOlder(std::array<uint8_t, READING_HISTORY_BYTES>& bits, const size_t shiftDays) {
  if (shiftDays == 0) {
    return;
  }
  if (shiftDays >= READING_HISTORY_DAYS) {
    clearBits(bits);
    return;
  }

  std::array<uint8_t, READING_HISTORY_BYTES> shifted = {};
  for (size_t bitIndex = 0; bitIndex + shiftDays < READING_HISTORY_DAYS; ++bitIndex) {
    if (isBitSet(bits, bitIndex)) {
      setBit(shifted, bitIndex + shiftDays);
    }
  }
  bits = shifted;
}

uint32_t secondsUntilNextBucketBoundary(const ReadingStatsDateTime& dt) {
  const uint32_t currentSecondOfDay =
      static_cast<uint32_t>(dt.hour) * 3600u + static_cast<uint32_t>(dt.minute) * 60u + dt.second;
  uint32_t nextBoundarySecondOfDay = 24u * 3600u;
  if (dt.hour < 5) {
    nextBoundarySecondOfDay = 5u * 3600u;
  } else if (dt.hour < 12) {
    nextBoundarySecondOfDay = 12u * 3600u;
  } else if (dt.hour < 17) {
    nextBoundarySecondOfDay = 17u * 3600u;
  } else if (dt.hour < 21) {
    nextBoundarySecondOfDay = 21u * 3600u;
  }

  if (nextBoundarySecondOfDay <= currentSecondOfDay) {
    return 1;
  }
  return nextBoundarySecondOfDay - currentSecondOfDay;
}
}  // namespace

bool ReadingStatsDate::isValid() const { return isValidReadingStatsDate(*this); }

void ReadingStatsDate::clear() {
  year = 0;
  month = 0;
  day = 0;
}

bool ReadingStatsDateTime::isValid() const { return date.isValid(); }

bool isLeapYear(const uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

uint8_t daysInMonth(const uint16_t year, const uint8_t month) {
  static constexpr uint8_t DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 0;
  }
  if (month == 2 && isLeapYear(year)) {
    return 29;
  }
  return DAYS[month - 1];
}

bool isValidReadingStatsDate(const ReadingStatsDate& date) {
  if (date.year < 2000 || date.year > 2099) {
    return false;
  }
  const uint8_t monthDays = daysInMonth(date.year, date.month);
  return monthDays > 0 && date.day >= 1 && date.day <= monthDays;
}

int compareReadingStatsDate(const ReadingStatsDate& lhs, const ReadingStatsDate& rhs) {
  if (lhs.year != rhs.year) {
    return lhs.year < rhs.year ? -1 : 1;
  }
  if (lhs.month != rhs.month) {
    return lhs.month < rhs.month ? -1 : 1;
  }
  if (lhs.day != rhs.day) {
    return lhs.day < rhs.day ? -1 : 1;
  }
  return 0;
}

void addDaysToReadingStatsDate(ReadingStatsDate& date, int delta) {
  if (!date.isValid() || delta == 0) {
    return;
  }

  while (delta > 0) {
    const uint8_t monthDays = daysInMonth(date.year, date.month);
    if (date.day < monthDays) {
      date.day++;
    } else {
      date.day = 1;
      if (date.month < 12) {
        date.month++;
      } else {
        date.month = 1;
        date.year++;
      }
    }
    delta--;
  }

  while (delta < 0) {
    if (date.day > 1) {
      date.day--;
    } else {
      if (date.month > 1) {
        date.month--;
      } else {
        date.month = 12;
        date.year--;
      }
      date.day = daysInMonth(date.year, date.month);
    }
    delta++;
  }
}

void addSecondsToReadingStatsDateTime(ReadingStatsDateTime& dt, uint32_t seconds) {
  if (!dt.isValid() || seconds == 0) {
    return;
  }

  uint32_t secondOfDay =
      static_cast<uint32_t>(dt.hour) * 3600u + static_cast<uint32_t>(dt.minute) * 60u + dt.second + seconds;
  const uint32_t daysToAdvance = secondOfDay / (24u * 3600u);
  secondOfDay %= (24u * 3600u);
  if (daysToAdvance > 0) {
    addDaysToReadingStatsDate(dt.date, static_cast<int>(daysToAdvance));
  }

  dt.hour = static_cast<uint8_t>(secondOfDay / 3600u);
  secondOfDay %= 3600u;
  dt.minute = static_cast<uint8_t>(secondOfDay / 60u);
  dt.second = static_cast<uint8_t>(secondOfDay % 60u);
}

uint32_t readingStatsDayIndex(const ReadingStatsDate& date) {
  uint32_t dayIndex = 0;
  for (uint16_t year = 2000; year < date.year; ++year) {
    dayIndex += isLeapYear(year) ? 366u : 365u;
  }
  for (uint8_t month = 1; month < date.month; ++month) {
    dayIndex += daysInMonth(date.year, month);
  }
  dayIndex += static_cast<uint32_t>(date.day - 1);
  return dayIndex;
}

bool readingStatsDateFromDayIndex(uint32_t dayIndex, ReadingStatsDate& outDate) {
  outDate = {};
  uint16_t year = 2000;
  while (year <= 2099) {
    const uint32_t yearDays = isLeapYear(year) ? 366u : 365u;
    if (dayIndex < yearDays) {
      break;
    }
    dayIndex -= yearDays;
    year++;
  }
  if (year > 2099) {
    return false;
  }

  uint8_t month = 1;
  while (month <= 12) {
    const uint8_t monthDays = daysInMonth(year, month);
    if (dayIndex < monthDays) {
      break;
    }
    dayIndex -= monthDays;
    month++;
  }
  if (month > 12) {
    return false;
  }

  outDate.year = year;
  outDate.month = month;
  outDate.day = static_cast<uint8_t>(dayIndex + 1u);
  return true;
}

uint8_t readingStatsDayOfWeekIndex(const ReadingStatsDate& date) {
  // 2000-01-01 was a Saturday. Convert so Monday = 0.
  const uint32_t dayIndex = readingStatsDayIndex(date);
  return static_cast<uint8_t>((5u + dayIndex) % 7u);
}

ReadingTimeBucket readingTimeBucketForHour(const uint8_t hour) {
  if (hour >= 5 && hour < 12) {
    return ReadingTimeBucket::Morning;
  }
  if (hour >= 12 && hour < 17) {
    return ReadingTimeBucket::Afternoon;
  }
  if (hour >= 17 && hour < 21) {
    return ReadingTimeBucket::Evening;
  }
  return ReadingTimeBucket::Night;
}

bool getCurrentLocalReadingStatsDateTime(ReadingStatsDateTime& outDateTime) {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  if (!halClock.getDateTime(year, month, day, hour, minute)) {
    outDateTime = {};
    return false;
  }

  outDateTime.date = {year, month, day};
  outDateTime.hour = hour;
  outDateTime.minute = minute;
  outDateTime.second = 0;
  if (!outDateTime.isValid()) {
    outDateTime = {};
    return false;
  }

  const int offsetMinutes = SETTINGS.clockEffectiveOffsetMin();
  int totalMinutes = static_cast<int>(outDateTime.hour) * 60 + static_cast<int>(outDateTime.minute) + offsetMinutes;

  while (totalMinutes < 0) {
    addDaysToReadingStatsDate(outDateTime.date, -1);
    totalMinutes += 24 * 60;
  }
  while (totalMinutes >= 24 * 60) {
    addDaysToReadingStatsDate(outDateTime.date, 1);
    totalMinutes -= 24 * 60;
  }

  outDateTime.hour = static_cast<uint8_t>(totalMinutes / 60);
  outDateTime.minute = static_cast<uint8_t>(totalMinutes % 60);
  return outDateTime.isValid();
}

bool readingStatsTrackingEnabled() { return SETTINGS.shouldTrackReadingStats(); }

uint16_t readingSpanDaysInclusive(const ReadingStatsDate& start, const ReadingStatsDate& end) {
  if (!start.isValid() || !end.isValid() || compareReadingStatsDate(end, start) < 0) {
    return 0;
  }
  const uint32_t startDay = readingStatsDayIndex(start);
  const uint32_t endDay = readingStatsDayIndex(end);
  return static_cast<uint16_t>(endDay - startDay + 1u);
}

uint16_t readingSpanDaysElapsed(const ReadingStatsDate& start, const ReadingStatsDate& end) {
  if (!start.isValid() || !end.isValid() || compareReadingStatsDate(end, start) < 0) {
    return 0;
  }
  const uint32_t startDay = readingStatsDayIndex(start);
  const uint32_t endDay = readingStatsDayIndex(end);
  return static_cast<uint16_t>(endDay - startDay);
}

void formatReadingStatsShortDate(const ReadingStatsDate& date, char* buf, const size_t len) {
  if (!buf || len == 0) {
    return;
  }
  if (!date.isValid()) {
    snprintf(buf, len, "-");
    return;
  }
  snprintf(buf, len, "%s %u", MONTH_NAMES[date.month - 1], static_cast<unsigned>(date.day));
}

void formatReadingStatsMonthToken(const ReadingStatsDate& date, char* buf, const size_t len) {
  if (!buf || len == 0) {
    return;
  }
  if (!date.isValid()) {
    snprintf(buf, len, "-");
    return;
  }
  snprintf(buf, len, "%s", MONTH_NAMES[date.month - 1]);
}

void formatCompactReadingDuration(const uint32_t seconds, char* buf, const size_t len) {
  if (!buf || len == 0) {
    return;
  }
  if (seconds < 60) {
    snprintf(buf, len, "<1m");
    return;
  }

  const uint32_t minutes = (seconds >= UINT32_MAX - 30U) ? (UINT32_MAX / 60U) : ((seconds + 30U) / 60U);
  if (minutes < 60) {
    snprintf(buf, len, "%lum", static_cast<unsigned long>(minutes));
    return;
  }

  const uint32_t hours = minutes / 60U;
  const uint32_t remainingMinutes = minutes % 60U;
  if (remainingMinutes == 0) {
    snprintf(buf, len, "%luh", static_cast<unsigned long>(hours));
  } else {
    snprintf(buf, len, "%luh %lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(remainingMinutes));
  }
}

void WpmWindow::record(const uint32_t seconds, const uint16_t wordsOnPage) {
  if (seconds == 0 || wordsOnPage == 0) {
    return;
  }
  const uint32_t wpm = (static_cast<uint32_t>(wordsOnPage) * 60U) / seconds;
  if (wpm > WPM_HARD_CAP) {
    return;
  }
  samples[pos] = static_cast<uint16_t>(wpm < WPM_FLOOR ? WPM_FLOOR : wpm);
  pos = static_cast<uint8_t>((pos + 1) % WPM_WINDOW_SIZE);
  if (count < WPM_WINDOW_SIZE) {
    count++;
  }
  avg = trimmedMean();
}

uint16_t WpmWindow::trimmedMean() const {
  if (count == 0) {
    return 0;
  }
  uint32_t sum = 0;
  // Not enough samples to trim both ends: plain mean.
  if (count <= WPM_TRIM_COUNT * 2) {
    sum = std::accumulate(samples.begin(), samples.begin() + count, 0u);
    return static_cast<uint16_t>(sum / count);
  }
  uint16_t sorted[WPM_WINDOW_SIZE];
  for (uint8_t i = 0; i < count; i++) {
    sorted[i] = samples[i];
  }
  for (uint8_t i = 1; i < count; i++) {
    const uint16_t key = sorted[i];
    uint8_t j = i;
    while (j > 0 && sorted[j - 1] > key) {
      sorted[j] = sorted[j - 1];
      j--;
    }
    sorted[j] = key;
  }
  const uint8_t last = count - WPM_TRIM_COUNT;
  for (uint8_t i = WPM_TRIM_COUNT; i < last; i++) {
    sum += sorted[i];
  }
  return static_cast<uint16_t>(sum / (last - WPM_TRIM_COUNT));
}

void WpmWindow::normalize() {
  if (count > WPM_WINDOW_SIZE) {
    count = WPM_WINDOW_SIZE;
  }
  if (count < WPM_WINDOW_SIZE) {
    // A persisted partial window must have pos == count, so the next
    // record() writes into the next empty slot. An in-range pos that
    // differs from count would leave a newly-counted slot unwritten and
    // stale samples in the trimmed mean.
    pos = count;
  } else if (pos >= WPM_WINDOW_SIZE) {
    pos = 0;
  }
  avg = trimmedMean();
}

void SessionWindow::record(const uint32_t seconds) {
  if (seconds < SESSION_MIN_SECONDS) {
    return;
  }
  // Sample unit is uint16_t seconds; anything above ~18.2 h clamps. The
  // trimmed mean is the outlier defense, so there is no other cap.
  samples[pos] = static_cast<uint16_t>(seconds > UINT16_MAX ? UINT16_MAX : seconds);
  pos = static_cast<uint8_t>((pos + 1) % SESSION_WINDOW_SIZE);
  if (count < SESSION_WINDOW_SIZE) {
    count++;
  }
  avg = trimmedMean();
}

uint16_t SessionWindow::trimmedMean() const {
  if (count == 0) {
    return 0;
  }
  uint32_t sum = 0;
  // Not enough samples to trim both ends: plain mean.
  if (count <= SESSION_TRIM_COUNT * 2) {
    sum = std::accumulate(samples.begin(), samples.begin() + count, 0u);
    return static_cast<uint16_t>(sum / count);
  }
  uint16_t sorted[SESSION_WINDOW_SIZE];
  for (uint8_t i = 0; i < count; i++) {
    sorted[i] = samples[i];
  }
  for (uint8_t i = 1; i < count; i++) {
    const uint16_t key = sorted[i];
    uint8_t j = i;
    while (j > 0 && sorted[j - 1] > key) {
      sorted[j] = sorted[j - 1];
      j--;
    }
    sorted[j] = key;
  }
  const uint8_t last = count - SESSION_TRIM_COUNT;
  for (uint8_t i = SESSION_TRIM_COUNT; i < last; i++) {
    sum += sorted[i];
  }
  return static_cast<uint16_t>(sum / (last - SESSION_TRIM_COUNT));
}

void SessionWindow::normalize() {
  if (count > SESSION_WINDOW_SIZE) {
    count = SESSION_WINDOW_SIZE;
  }
  if (count < SESSION_WINDOW_SIZE) {
    // Same post-load invariant as WpmWindow: a persisted partial window must
    // have pos == count so the next record() writes into the next empty slot.
    pos = count;
  } else if (pos >= SESSION_WINDOW_SIZE) {
    pos = 0;
  }
  avg = trimmedMean();
}

std::optional<uint32_t> avgSessionSeconds(const uint16_t windowAvg, const uint8_t windowCount,
                                          const uint64_t totalReadingSeconds, const uint64_t sessionCount) {
  // Early display: from the 4th recorded session the window's running result
  // wins (plain mean up to 4 samples, trimmed beyond), replacing the all-time
  // arithmetic mean for good.
  if (windowCount >= SESSION_MIN_DISPLAY_SAMPLES && windowAvg > 0) {
    return windowAvg;
  }
  if (sessionCount > 0) {
    // 64-bit intermediate: the global record's totals must not overflow.
    return static_cast<uint32_t>(totalReadingSeconds / sessionCount);
  }
  return std::nullopt;
}

std::optional<uint32_t> resolveReadingPaceSecondsPerPage(const BookReadingStats& bookStats,
                                                         const GlobalReadingStats& globalStats,
                                                         const uint16_t wordsOnPage) {
  // Words-per-page for the WPM→seconds conversion: the live page's engine
  // word count when known (same chapter/font as the pages being estimated),
  // else the calibrated average. The 220 constant ignores font size and page
  // density — with engine wordCount the live value is per-book exact.
  const uint32_t wordsPerPage = wordsOnPage > 0 ? wordsOnPage : CALIBRATED_WORDS_PER_PAGE;
  // Prefer the book's own WPM estimate once its window is full: 15 trimmed
  // samples converted through the words-per-page figure. Until the window
  // fills there is no book-level reading-speed estimate.
  if (bookStats.wpm.count >= WPM_WINDOW_SIZE && bookStats.wpm.avg > 0) {
    const uint32_t pace = (wordsPerPage * 60U) / bookStats.wpm.avg;
    if (pace > 0) {
      return pace;
    }
  }
  if (globalStats.wpm.count >= WPM_WINDOW_SIZE && globalStats.wpm.avg > 0) {
    const uint32_t pace = (wordsPerPage * 60U) / globalStats.wpm.avg;
    if (pace > 0) {
      return pace;
    }
  }
  if (globalStats.totalPagesTurned >= MIN_GLOBAL_PACE_PAGE_TURNS) {
    const uint32_t pace = globalStats.totalReadingSeconds / globalStats.totalPagesTurned;
    if (pace > 0) {
      return pace;
    }
  }
  return std::nullopt;
}

std::optional<uint32_t> estimateChapterTimeLeftSeconds(const BookReadingStats& bookStats,
                                                       const GlobalReadingStats& globalStats,
                                                       const uint16_t pagesRemaining, const uint16_t wordsOnPage) {
  const auto pace = resolveReadingPaceSecondsPerPage(bookStats, globalStats, wordsOnPage);
  if (!pace) {
    return std::nullopt;
  }
  if (pagesRemaining == 0) {
    return 0u;
  }
  const uint64_t total = static_cast<uint64_t>(*pace) * static_cast<uint64_t>(pagesRemaining);
  return static_cast<uint32_t>(total > UINT32_MAX ? UINT32_MAX : total);
}

std::optional<uint32_t> estimateBookTimeLeftSeconds(const BookReadingStats& bookStats,
                                                    const GlobalReadingStats& globalStats,
                                                    const uint32_t estimatedRemainingPages,
                                                    const uint16_t wordsOnPage) {
  const auto pace = resolveReadingPaceSecondsPerPage(bookStats, globalStats, wordsOnPage);
  if (!pace) {
    return std::nullopt;
  }
  if (estimatedRemainingPages == 0) {
    return 0u;
  }
  const uint64_t total = static_cast<uint64_t>(*pace) * static_cast<uint64_t>(estimatedRemainingPages);
  return static_cast<uint32_t>(total > UINT32_MAX ? UINT32_MAX : total);
}

void formatChapterTimeLeft(const uint32_t seconds, char* buf, const size_t len) {
  if (!buf || len == 0) {
    return;
  }
  if (seconds < 60) {
    snprintf(buf, len, "%s", tr(STR_TIME_LEFT_LESS_THAN_MIN));
    return;
  }
  const uint32_t minutes = (seconds >= UINT32_MAX - 30U) ? (UINT32_MAX / 60U) : ((seconds + 30U) / 60U);
  snprintf(buf, len, tr(STR_TIME_LEFT_MIN), static_cast<unsigned long>(minutes));
}

void formatHomeProgressLine(const BookReadingStats& stats, char* buf, const size_t len) {
  if (!buf || len == 0) {
    return;
  }
  buf[0] = '\0';
  if (stats.lastBookProgressPercent == UNKNOWN_BOOK_PROGRESS_PERCENT) {
    snprintf(buf, len, "-");
    return;
  }

  char duration[16];
  if (stats.estimatedTimeLeftSeconds == 0) {
    snprintf(duration, sizeof(duration), "%s", tr(STR_TIME_LEFT_UNAVAILABLE));
  } else {
    formatCompactReadingDuration(stats.estimatedTimeLeftSeconds, duration, sizeof(duration));
  }
  snprintf(buf, len, tr(STR_PROGRESS_LINE_FMT), static_cast<unsigned>(stats.lastBookProgressPercent), duration);
}

void formatSleepProgressLine(const int percent, const uint32_t timeLeftSeconds, const int pagesLeft, char* buf,
                             const size_t len) {
  if (!buf || len == 0) {
    return;
  }
  buf[0] = '\0';
  if (percent < 0) {
    return;
  }

  char line[48];
  size_t pos = 0;
  line[0] = '\0';

  char segment[32];
  const auto append = [&](const char* text) {
    if (pos > 0) {
      const int written = snprintf(line + pos, sizeof(line) - pos, " • %s", text);
      if (written < 0 || static_cast<size_t>(written) >= sizeof(line) - pos) {
        return;
      }
      pos += static_cast<size_t>(written);
    } else {
      const int written = snprintf(line + pos, sizeof(line) - pos, "%s", text);
      if (written < 0 || static_cast<size_t>(written) >= sizeof(line) - pos) {
        return;
      }
      pos += static_cast<size_t>(written);
    }
  };

  // percent >= 0 is guaranteed by the early return above.
  snprintf(segment, sizeof(segment), "%d%%", percent);
  append(segment);

  if (timeLeftSeconds > 0) {
    char duration[16];
    formatCompactReadingDuration(timeLeftSeconds, duration, sizeof(duration));
    snprintf(segment, sizeof(segment), "~%s", duration);
    append(segment);
  }

  if (pagesLeft >= 0) {
    snprintf(segment, sizeof(segment), tr(STR_SLEEP_PAGES_LEFT_FMT), static_cast<unsigned long>(pagesLeft));
    append(segment);
  }

  snprintf(buf, len, "%s", line);
}

std::string formatStatCell(const uint32_t value, const bool hasValue) {
  if (!hasValue) {
    return tr(STR_STATS_VALUE_UNAVAILABLE);
  }
  return std::to_string(value);
}

void recordReadingSpanIntoBuckets(std::array<uint32_t, READING_TIME_BUCKET_COUNT>& timeOfDaySeconds,
                                  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT>& dayOfWeekSeconds,
                                  const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  if (!localStart.isValid() || seconds == 0) {
    return;
  }

  ReadingStatsDateTime cursor = localStart;
  uint32_t remaining = seconds;
  while (remaining > 0) {
    const uint8_t bucketIndex = static_cast<uint8_t>(readingTimeBucketForHour(cursor.hour));
    const uint8_t dayIndex = readingStatsDayOfWeekIndex(cursor.date);
    const uint32_t segment =
        remaining < secondsUntilNextBucketBoundary(cursor) ? remaining : secondsUntilNextBucketBoundary(cursor);
    timeOfDaySeconds[bucketIndex] += segment;
    dayOfWeekSeconds[dayIndex] += segment;
    remaining -= segment;
    addSecondsToReadingStatsDateTime(cursor, segment);
  }
}

void markReadingHistoryDay(uint32_t& anchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& bits,
                           const uint32_t dayIndex) {
  if (anchorDay == 0 && !isBitSet(bits, 0)) {
    anchorDay = dayIndex;
    clearBits(bits);
    setBit(bits, 0);
    return;
  }

  if (dayIndex > anchorDay) {
    shiftHistoryOlder(bits, dayIndex - anchorDay);
    anchorDay = dayIndex;
  }

  const uint32_t delta = anchorDay - dayIndex;
  if (delta >= READING_HISTORY_DAYS) {
    return;
  }
  setBit(bits, static_cast<size_t>(delta));
}

void recordReadingSpanIntoHistory(uint32_t& anchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& bits,
                                  std::array<uint16_t, READING_MINUTE_HISTORY_DAYS>& dailyMinutes,
                                  const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  if (!localStart.isValid() || seconds == 0) {
    return;
  }

  ReadingStatsDateTime cursor = localStart;
  uint32_t remaining = seconds;
  while (remaining > 0) {
    const uint32_t dayIndex = readingStatsDayIndex(cursor.date);
    const uint32_t previousAnchorDay = anchorDay;
    markReadingHistoryDay(anchorDay, bits, dayIndex);
    const uint32_t secondsUntilMidnight =
        (24u * 3600u) - (static_cast<uint32_t>(cursor.hour) * 3600u + static_cast<uint32_t>(cursor.minute) * 60u +
                         static_cast<uint32_t>(cursor.second));
    const uint32_t segment = remaining < secondsUntilMidnight ? remaining : secondsUntilMidnight;
    remaining -= segment;

    // Round each calendar-day segment to minutes and cap one call's
    // contribution at one day's worth of minutes. The array is uint16 and
    // per-day totals also stay <= 1440.
    const uint32_t minutesToAdd = std::min<uint32_t>(READING_MINUTES_PER_DAY, (segment + 30u) / 60u);
    if (previousAnchorDay == 0 && dailyMinutes[0] == 0) {
      dailyMinutes.fill(0);
    } else if (dayIndex > previousAnchorDay) {
      shiftMinutesOlder(dailyMinutes, dayIndex - previousAnchorDay);
    }
    const uint32_t dayOffset = anchorDay - dayIndex;
    if (dayOffset < READING_MINUTE_HISTORY_DAYS) {
      const uint32_t updated = std::min<uint32_t>(READING_MINUTES_PER_DAY, dailyMinutes[dayOffset] + minutesToAdd);
      dailyMinutes[dayOffset] = static_cast<uint16_t>(updated);
    }

    addSecondsToReadingStatsDateTime(cursor, segment);
  }
}

uint16_t readingMinutesForDay(const uint32_t anchorDay,
                              const std::array<uint16_t, READING_MINUTE_HISTORY_DAYS>& dailyMinutes,
                              const uint32_t dayIndex) {
  if (dayIndex > anchorDay) {
    return 0;
  }
  const uint32_t dayOffset = anchorDay - dayIndex;
  return dayOffset < READING_MINUTE_HISTORY_DAYS ? dailyMinutes[dayOffset] : 0;
}

void mergeReadingHistory(uint32_t& targetAnchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& targetBits,
                         const uint32_t sourceAnchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& sourceBits) {
  if (sourceAnchorDay == 0 && !isBitSet(sourceBits, 0)) {
    return;
  }

  if (targetAnchorDay == 0 && !isBitSet(targetBits, 0)) {
    targetAnchorDay = sourceAnchorDay;
    targetBits = sourceBits;
    return;
  }

  if (sourceAnchorDay > targetAnchorDay) {
    shiftHistoryOlder(targetBits, sourceAnchorDay - targetAnchorDay);
    targetAnchorDay = sourceAnchorDay;
  }

  for (size_t bitIndex = 0; bitIndex < READING_HISTORY_DAYS; ++bitIndex) {
    if (!isBitSet(sourceBits, bitIndex)) {
      continue;
    }
    const uint32_t dayIndex = sourceAnchorDay - static_cast<uint32_t>(bitIndex);
    if (dayIndex > targetAnchorDay) {
      continue;
    }
    const uint32_t delta = targetAnchorDay - dayIndex;
    if (delta < READING_HISTORY_DAYS) {
      setBit(targetBits, static_cast<size_t>(delta));
    }
  }
}

uint16_t computeReadingHistoryLongestStreak(const uint32_t anchorDay,
                                            const std::array<uint8_t, READING_HISTORY_BYTES>& bits) {
  if (anchorDay == 0 && !isBitSet(bits, 0)) {
    return 0;
  }

  uint16_t best = 0;
  uint16_t current = 0;
  for (int bitIndex = static_cast<int>(READING_HISTORY_DAYS) - 1; bitIndex >= 0; --bitIndex) {
    if (isBitSet(bits, static_cast<size_t>(bitIndex))) {
      current++;
      if (current > best) {
        best = current;
      }
    } else {
      current = 0;
    }
  }
  return best;
}

std::string bookStatsDbKey(const std::string& cachePath) {
  // FNV-1a 64-bit over the cache-path string. Cheap, no allocation beyond the
  // result, no file I/O. Produces a fixed 16-char hex key for the DB PRIMARY KEY.
  constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
  constexpr uint64_t kPrime = 1099511628211ULL;
  uint64_t hash = kOffsetBasis;
  for (const unsigned char c : cachePath) {
    hash ^= static_cast<uint64_t>(c);
    hash *= kPrime;
  }
  char buf[17];
  snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
  return std::string(buf);
}

uint16_t computeReadingHistoryCurrentStreak(uint32_t anchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& bits,
                                            const ReadingStatsDate* today) {
  if (anchorDay == 0 && !isBitSet(bits, 0)) {
    return 0;
  }
  if (!isBitSet(bits, 0)) {
    return 0;
  }

  if (today && today->isValid()) {
    const uint32_t todayDay = readingStatsDayIndex(*today);
    if (anchorDay + 1u < todayDay) {
      return 0;
    }
  }

  uint16_t streak = 0;
  for (size_t bitIndex = 0; bitIndex < READING_HISTORY_DAYS; ++bitIndex) {
    if (!isBitSet(bits, bitIndex)) {
      break;
    }
    streak++;
  }
  return streak;
}
