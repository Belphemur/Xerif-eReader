// Host stubs for OtaSlots: a simulated partition table backed by a small
// in-memory flash image. Only what OtaSlots.cpp touches is implemented.
#include <cstring>
#include <vector>

#include "esp_ota_ops.h"
#include "esp_partition.h"

namespace {
constexpr size_t kFlashSize = 16 * 1024 * 1024;
std::vector<uint8_t> g_flash;
std::vector<esp_partition_t> g_partitions;
const esp_partition_t* g_running = nullptr;
}  // namespace

void testOtaSlotsReset() {
  g_flash.assign(kFlashSize, 0xFF);
  g_partitions.clear();
  g_running = nullptr;
}

void testOtaSlotsAddPartition(const char* label, const uint32_t address, const size_t size, const uint8_t subtype) {
  g_partitions.push_back(esp_partition_t{label, address, size, subtype});
}

void testOtaSlotsFlashWrite(const uint32_t address, const void* data, const size_t len) {
  if (g_flash.empty()) g_flash.assign(kFlashSize, 0xFF);
  memcpy(g_flash.data() + address, data, len);
}

void testOtaSlotsSetRunning(const uint32_t address) {
  g_running = nullptr;
  for (size_t i = 0; i < g_partitions.size(); ++i) {
    if (g_partitions[i].address == address) {
      g_running = &g_partitions[i];
      return;
    }
  }
}

const esp_partition_t* esp_partition_find_first(const int type, const int subtype, const char* label) {
  if (type != ESP_PARTITION_TYPE_APP) return nullptr;
  for (const auto& p : g_partitions) {
    if (p.subtype != subtype) continue;
    if (label && strcmp(p.label, label) != 0) continue;
    return &p;
  }
  return nullptr;
}

esp_err_t esp_partition_read(const esp_partition_t* partition, const size_t src_offset, void* dst, const size_t size) {
  if (g_flash.empty()) g_flash.assign(kFlashSize, 0xFF);
  if (!partition || src_offset + size > partition->size) return -1;
  memcpy(dst, g_flash.data() + partition->address + src_offset, size);
  return ESP_OK;
}

const esp_partition_t* esp_ota_get_running_partition() { return g_running; }
