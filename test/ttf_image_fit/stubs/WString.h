// Host-test stand-in for Arduino WString.h: FsHelpers only passes the String
// type by const& — the std::string alias matches the HalStorage stub's.
#pragma once

#include <string>

using String = std::string;
