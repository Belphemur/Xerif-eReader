#pragma once

// Stand-in for the Arduino String class, narrowed for this suite: explicit
// constructors so FsHelpers.h's std::string_view overloads win overload
// resolution against the const String& ones (the shared fs_helpers stub keeps
// implicit ctors, which makes a std::string argument ambiguous between the
// two).
#include <string>

class String {
 public:
  String() = default;
  explicit String(const char* s) : value_(s ? s : "") {}
  explicit String(const std::string& s) : value_(s) {}

  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.length(); }

 private:
  std::string value_;
};
