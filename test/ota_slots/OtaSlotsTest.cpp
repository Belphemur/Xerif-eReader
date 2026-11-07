#include <gtest/gtest.h>

#include <cstring>

#include "network/OtaSlots.h"
#include "stubs/esp_ota_ops.h"
#include "stubs/esp_partition.h"

namespace {
// Build an esp_app_desc_t image prefix at `address`: image header (24 B) +
// segment header (8 B) + app descriptor (magic, secure_version, reserv1[2],
// version[32]).
void installAppImage(const uint32_t address, const char* version, const bool withMagic = true) {
  uint8_t buf[0x20 + 48];
  memset(buf, 0xFF, sizeof(buf));
  buf[0] = 0xE9;  // ESP_IMAGE_MAGIC (not checked by OtaSlots, kept realistic)
  if (withMagic) {
    constexpr uint32_t kMagic = 0xABCD5432;
    memcpy(buf + 0x20, &kMagic, sizeof(kMagic));
  }
  if (version) {
    // Exact 32-byte copy: the test intentionally exercises a full-width,
    // unpadded version field, so no NUL terminator is written.
    const size_t len = strlen(version) < 32 ? strlen(version) : 32;
    memcpy(buf + 0x20 + 16, version, len);
  }
  testOtaSlotsFlashWrite(address, buf, sizeof(buf));
}

void setupStandardTable() {
  testOtaSlotsReset();
  testOtaSlotsAddPartition("app0", 0x10000, 0x640000, ESP_PARTITION_SUBTYPE_APP_OTA_0);
  testOtaSlotsAddPartition("app1", 0x650000, 0x640000, ESP_PARTITION_SUBTYPE_APP_OTA_1);
}
}  // namespace

TEST(OtaSlotsTest, RolesAreFixedByPosition) {
  setupStandardTable();
  const esp_partition_t* stable = ota_slots::stableSlot();
  const esp_partition_t* dev = ota_slots::devSlot();
  ASSERT_NE(stable, nullptr);
  ASSERT_NE(dev, nullptr);
  EXPECT_STREQ(stable->label, "app0");
  EXPECT_STREQ(dev->label, "app1");
  EXPECT_FALSE(ota_slots::sameSlot(stable, dev));
  EXPECT_TRUE(ota_slots::sameSlot(stable, stable));
}

TEST(OtaSlotsTest, ReadSlotVersion) {
  setupStandardTable();
  installAppImage(0x10000, "2.11.0");
  ota_slots::SlotInfo info;
  EXPECT_TRUE(ota_slots::readSlotInfo(ota_slots::stableSlot(), info));
  EXPECT_TRUE(info.present);
  EXPECT_STREQ(info.version, "2.11.0");
}

TEST(OtaSlotsTest, EmptySlotReportsNotPresent) {
  setupStandardTable();
  ota_slots::SlotInfo info;
  // app1 has no image (erased flash reads 0xFF, no magic).
  EXPECT_FALSE(ota_slots::readSlotInfo(ota_slots::devSlot(), info));
  EXPECT_FALSE(info.present);
  EXPECT_EQ(info.version[0], '\0');
}

TEST(OtaSlotsTest, MagicMismatchIsNotPresent) {
  setupStandardTable();
  installAppImage(0x10000, "1.2.3", /*withMagic=*/false);
  ota_slots::SlotInfo info;
  EXPECT_FALSE(ota_slots::readSlotInfo(ota_slots::stableSlot(), info));
}

TEST(OtaSlotsTest, NullPartitionIsNotPresent) {
  ota_slots::SlotInfo info;
  EXPECT_FALSE(ota_slots::readSlotInfo(nullptr, info));
  EXPECT_FALSE(info.present);
}

TEST(OtaSlotsTest, RunningSlotByAddress) {
  setupStandardTable();
  installAppImage(0x650000, "2.11.0-dev-abc1234");
  testOtaSlotsSetRunning(0x650000);
  const esp_partition_t* running = ota_slots::runningSlot();
  ASSERT_NE(running, nullptr);
  EXPECT_TRUE(ota_slots::sameSlot(running, ota_slots::devSlot()));

  // Not an OTA slot (e.g. factory app): nullptr.
  testOtaSlotsSetRunning(0x999999);
  EXPECT_EQ(ota_slots::runningSlot(), nullptr);
}

TEST(OtaSlotsTest, MissingDevSlot) {
  testOtaSlotsReset();
  testOtaSlotsAddPartition("app0", 0x10000, 0x640000, ESP_PARTITION_SUBTYPE_APP_OTA_0);
  EXPECT_NE(ota_slots::stableSlot(), nullptr);
  EXPECT_EQ(ota_slots::devSlot(), nullptr);
}

TEST(OtaSlotsTest, VersionTruncatesAt32Bytes) {
  setupStandardTable();
  // A full-width 32-byte version field (no NUL padding) must still read as a
  // terminated C string.
  installAppImage(0x10000, "0123456789abcdef0123456789abcdef0123456789");
  ota_slots::SlotInfo info;
  EXPECT_TRUE(ota_slots::readSlotInfo(ota_slots::stableSlot(), info));
  EXPECT_EQ(strlen(info.version), 32u);
}
