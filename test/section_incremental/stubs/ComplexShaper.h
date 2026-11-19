// Host stub for section_incremental: mirrors the ComplexShaper surface the
// compiled production sources use (upstream #3787) without pulling lib/OtShaper.
#pragma once

#include <cstddef>

class ComplexShaper {
 public:
  static bool startsComplexScript(const unsigned char*) { return false; }
  static bool containsComplexScript(const char*) { return false; }
  static void setDocumentLanguage(const char*) {}
  static size_t releaseAll() { return 0; }
  static void beginMemo() {}
};
