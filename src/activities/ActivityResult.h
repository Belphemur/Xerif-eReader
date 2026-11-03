#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

struct WifiResult {
  bool connected = false;
  std::string ssid;
  std::string ip;
};

struct KeyboardResult {
  std::string text;
};

struct DictionarySearchResult {
  std::string text;
};

struct MenuResult {
  int action = -1;
  uint8_t orientation = 0;
  uint8_t pageTurnOption = 0;
};

struct ChapterResult {
  int spineIndex = 0;
  std::string anchor;
};

struct PercentResult {
  int percent = 0;
};

struct IntervalResult {
  uint32_t value = 0;
};

struct PageResult {
  uint32_t page = 0;
};

struct ClippingResult {
  enum class Action : uint8_t { Clip, Lookup, Bookmark };
  Action action = Action::Clip;
  std::string text;
  uint16_t startPageOffset = 0;
  uint16_t endPageOffset = 0;
  uint16_t startWordIndex = 0;
  uint16_t endWordIndex = 0;
  uint16_t wordCount = 0;
  uint32_t startOffset = UINT32_MAX;
  uint32_t endOffset = UINT32_MAX;
  // True when the cursor (not the anchor) was on the first word, i.e. the selection was extended leftwards.
  bool cursorAtStart = false;
};

struct ProgressChangeResult {
  int spineIndex = 0;
  int page = 0;
  int totalPages = 0;
  std::string xpath;
  float percentage = 0.0f;
  bool hasSavedProgress = false;
  // Exact visible-codepoint offset within spineIndex, when the source (a bookmark) has one.
  // Preferred over xpath/percentage on resolution: it is immune to re-pagination.
  bool hasVisibleTextOffset = false;
  uint32_t visibleTextOffset = 0;
};

enum class NetworkMode;

struct NetworkModeResult {
  NetworkMode mode;
};

struct FootnoteResult {
  std::string href;
};

struct FilePathResult {
  std::string path;
};

// Sent by BookStatsActivity when the user activates "Clear reading speed":
// the reader (owner of the authoritative per-book record) applies the clear.
struct ClearPaceResult {};

// Sent by FontPreviewActivity when the user closes it after changing a
// rendering setting: the reader (owner of the caches and the reading
// position) runs its settings-driven clean reindex. A close with no changes
// pops as cancelled (silent return, zero reflow, zero SD writes).
//
// The preview's relayout is page-LOCAL (ADR 2026-10-14): the window pass
// cannot know the rebuilt layout's absolute page numbering, so it reports no
// page index — the reader restores the position through the chapter char
// offset it already carries (ttfCurrentCharStart), which the rebuild covers
// incrementally (pageForChar maps over the built prefix).
struct QuickFontPreviewResult {
  bool changed = false;
};

using ResultVariant =
    std::variant<std::monostate, WifiResult, KeyboardResult, DictionarySearchResult, MenuResult, ChapterResult,
                 PercentResult, IntervalResult, PageResult, ClippingResult, ProgressChangeResult, NetworkModeResult,
                 FootnoteResult, FilePathResult, ClearPaceResult, QuickFontPreviewResult>;

struct ActivityResult {
  bool isCancelled = false;
  // True once setResult() has been called. Distinguishes an explicit empty
  // (monostate) result — e.g. ConfirmationActivity's Confirm — from a
  // default-constructed result left by an activity that popped without
  // calling setResult().
  bool hasResult = false;
  ResultVariant data;

  explicit ActivityResult() = default;

  template <typename ResultType>
    requires std::is_constructible_v<ResultVariant, ResultType&&>
  // cppcheck-suppress noExplicitConstructor
  ActivityResult(ResultType&& result) : hasResult{true}, data{std::forward<ResultType>(result)} {}
};

using ActivityResultHandler = std::function<void(const ActivityResult&)>;

// The production delivery stamp used by Activity::setResult(): marks a result
// as explicitly set so normalizeActivityResult passes it through untouched.
inline void markActivityResultDelivered(ActivityResult& result) { result.hasResult = true; }

// Pop-result policy: an activity that popped without ever calling setResult()
// leaves a default-constructed result (hasResult=false, isCancelled=false,
// data=monostate); a parent handler doing std::get<T>(data) on it would abort
// with std::bad_variant_access. normalizeActivityResult rewrites that case to
// cancelled so the handler takes its safe branch, and reports whether it did.
// An explicit setResult() call (hasResult=true) always passes through — even
// an empty-data Confirm from ConfirmationActivity.
inline bool normalizeActivityResult(ActivityResult& result) {
  if (!result.hasResult && !result.isCancelled) {
    result.isCancelled = true;
    return true;
  }
  return false;
}
