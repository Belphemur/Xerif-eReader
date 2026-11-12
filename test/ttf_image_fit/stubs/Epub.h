// Host-test stub of Epub.h: TtfImageEngine.cpp needs only the PSRAM
// extraction member for the GIF/BMP branch. The real header drags the whole
// parser stack, so this stub carries an injectable href→bytes map (the test
// seam) and hands out malloc'd copies that PoolBytes releases correctly
// (poolMalloc/poolFree are plain malloc/free on host).
#pragma once

#include <Memory.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

class Epub {
 public:
  // The buffer MUST come from the pool allocator: the caller wraps it in
  // PoolBytes, which releases it through poolFree (new[]/free would be UB).
  uint8_t* extractItemToPsram(const char* href, size_t* outSize) {
    if (outSize != nullptr) *outSize = 0;
    const auto it = items.find(href != nullptr ? href : "");
    if (it == items.end() || it->second.empty()) return nullptr;
    uint8_t* buf = static_cast<uint8_t*>(poolMalloc(it->second.size()));
    if (buf == nullptr) return nullptr;
    memcpy(buf, it->second.data(), it->second.size());
    if (outSize != nullptr) *outSize = it->second.size();
    return buf;
  }

  // Test seam: href → raw item bytes.
  std::map<std::string, std::vector<uint8_t>> items;
};
