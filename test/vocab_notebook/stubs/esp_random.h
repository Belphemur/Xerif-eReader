#pragma once
#include <cstddef>
#include <cstdint>
namespace fake_esp {
inline uint8_t next = 0;
}  // namespace fake_esp
inline void esp_fill_random(void* buffer, size_t size) {
  auto* bytes = static_cast<uint8_t*>(buffer);
  for (size_t i = 0; i < size; ++i) bytes[i] = ++fake_esp::next;
}
inline uint32_t esp_random() {
  esp_fill_random(&fake_esp::next, 1);
  return fake_esp::next;
}
