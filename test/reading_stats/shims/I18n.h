#pragma once

// Host test shim for I18n.h — supplies just enough for BookReadingStats.cpp
// (which uses tr(STR_STATS_LESS_THAN_MIN) in formatDuration).

enum class StrId {
  STR_STATS_LESS_THAN_MIN = 0,
  STR_STATS_DURATION_MIN,
  STR_STATS_DURATION_HM,
  STR_STATS_COMPLETED,
  STR_STATS_STARTED,
  STR_STATS_FINISHED,
  STR_STATS_AVG_PAGE_PACE,
  STR_STATS_EST_TIME_LEFT,
  STR_TIME_LEFT_MIN,
  STR_TIME_LEFT_LESS_THAN_MIN,
  STR_TIME_LEFT_UNAVAILABLE,
  STR_PROGRESS_LINE_FMT,
  STR_SLEEP_PAGES_LEFT_FMT,
  STR_STATS_VALUE_UNAVAILABLE,
  STR_STATS_PAGES_PER_MIN,
  STR_STATS_NO_RTC_BANNER,
  STR_YES,
  STR_NO,
};

class I18n {
 public:
  static I18n& getInstance() {
    static I18n instance;
    return instance;
  }
  const char* get(StrId id) const {
    switch (id) {
      case StrId::STR_STATS_LESS_THAN_MIN:
        return "< 1 min";
      case StrId::STR_STATS_DURATION_MIN:
        return "%lu min";
      case StrId::STR_STATS_DURATION_HM:
        return "%luh %lu min";
      case StrId::STR_STATS_COMPLETED:
        return "Completed";
      case StrId::STR_STATS_STARTED:
        return "Started";
      case StrId::STR_STATS_FINISHED:
        return "Finished";
      case StrId::STR_STATS_AVG_PAGE_PACE:
        return "Avg Page Pace";
      case StrId::STR_STATS_EST_TIME_LEFT:
        return "Est Time Left";
      case StrId::STR_TIME_LEFT_MIN:
        return "~%lu min left";
      case StrId::STR_TIME_LEFT_LESS_THAN_MIN:
        return "< 1 min left";
      case StrId::STR_TIME_LEFT_UNAVAILABLE:
        return "--";
      case StrId::STR_PROGRESS_LINE_FMT:
        return "%u%% \xE2\x80\xA2 %s";
      case StrId::STR_SLEEP_PAGES_LEFT_FMT:
        return "%lu pages left";
      case StrId::STR_STATS_VALUE_UNAVAILABLE:
        return "--";
      case StrId::STR_STATS_PAGES_PER_MIN:
        return "Pages/Min";
      case StrId::STR_STATS_NO_RTC_BANNER:
        return "RTC clock not available";
      case StrId::STR_YES:
        return "Yes";
      case StrId::STR_NO:
        return "No";
    }
    return "";
  }
};

#define tr(id) I18n::getInstance().get(StrId::id)
