#pragma once

#include <cstddef>
#include <cstdint>

using esp_err_t = int;

#define ESP_OK 0

// Partition subtypes OtaSlots cares about (mirrors esp_partition.h on device).
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 0x10
#define ESP_PARTITION_SUBTYPE_APP_OTA_1 0x11

struct esp_partition_t {
  const char* label;
  uint32_t address;
  size_t size;
  uint8_t subtype;
};

// Backed by a test-owned flash image (see Stubs.cpp).
const esp_partition_t* esp_partition_find_first(uint8_t type, uint8_t subtype, const char* label);
esp_err_t esp_partition_read(const esp_partition_t* partition, size_t src_offset, void* dst, size_t size);

// Test hooks: install a simulated partition table + flash image.
void testOtaSlotsReset();
void testOtaSlotsAddPartition(const char* label, uint32_t address, size_t size, uint8_t subtype);
// Writes raw bytes into the simulated flash at `address`.
void testOtaSlotsFlashWrite(uint32_t address, const void* data, size_t len);
void testOtaSlotsSetRunning(uint32_t address);
