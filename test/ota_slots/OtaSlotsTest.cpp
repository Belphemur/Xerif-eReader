#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <vector>

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

namespace {
// Build a realistic segmented app image at `address`: 24-byte image header
// (magic + segment count) followed by `segmentCount` segments of 8-byte
// headers (load address + length) and zero-filled data. `markerOffset` is an
// absolute image offset for the resume-hook marker; kNoMarker omits it.
constexpr uint32_t kNoMarker = 0xFFFFFFFF;
void installSegmentedImage(const uint32_t address, const size_t segmentCount, const uint32_t markerOffset,
                           const uint32_t badMagicByte = 0) {
  // Two segments of 4 KiB each keep the image larger than the scanner's read
  // chunk so the overlap logic is exercised.
  const uint32_t dataLen = 4096;
  const uint32_t imageLen = 24 + segmentCount * 8 + segmentCount * dataLen;
  std::vector<uint8_t> image(imageLen, 0xFF);
  image[0] = 0xE9;  // ESP_IMAGE_MAGIC
  image[1] = static_cast<uint8_t>(segmentCount);
  uint32_t pos = 24;
  for (size_t i = 0; i < segmentCount; ++i) {
    const uint32_t load = 0x3C000000 + static_cast<uint32_t>(i) * dataLen;
    memcpy(image.data() + pos, &load, sizeof(load));
    memcpy(image.data() + pos + 4, &dataLen, sizeof(dataLen));
    pos += 8 + dataLen;
  }
  if (badMagicByte != 0) image[0] = badMagicByte;
  if (markerOffset != kNoMarker) {
    memcpy(image.data() + markerOffset, ota_slots::kResumeHookMarker, sizeof(ota_slots::kResumeHookMarker) - 1);
  }
  testOtaSlotsFlashWrite(address, image.data(), image.size());
}
}  // namespace

TEST(OtaSlotsTest, ImageWithMarkerIsResumeCapable) {
  setupStandardTable();
  // Marker inside the second segment's data, past the first read chunk.
  installSegmentedImage(0x10000, 2, 24 + 8 + 4096 + 8 + 256);
  EXPECT_TRUE(ota_slots::imageHasResumeMarker(ota_slots::stableSlot()));
}

TEST(OtaSlotsTest, MarkerStraddlingReadChunkBoundaryIsFound) {
  setupStandardTable();
  // Place the marker so it crosses a 2048-byte boundary inside the first
  // segment's data — the scan's overlapping reads must reassemble it.
  const uint32_t boundary = 2048;
  installSegmentedImage(0x10000, 2, boundary - 13);
  EXPECT_TRUE(ota_slots::imageHasResumeMarker(ota_slots::stableSlot()));
}

TEST(OtaSlotsTest, MarkerAtImageEndIsFound) {
  setupStandardTable();
  // Marker ending exactly at the image extent (24 + 2*8 + 2*4096).
  constexpr uint32_t kExtent = 24 + 2 * 8 + 2 * 4096;
  installSegmentedImage(0x10000, 2, kExtent - (sizeof(ota_slots::kResumeHookMarker) - 1));
  EXPECT_TRUE(ota_slots::imageHasResumeMarker(ota_slots::stableSlot()));
}

TEST(OtaSlotsTest, ImageWithoutMarkerRefuses) {
  setupStandardTable();
  installSegmentedImage(0x10000, 2, kNoMarker);
  EXPECT_FALSE(ota_slots::imageHasResumeMarker(ota_slots::stableSlot()));
  // A parseable esp_app_desc_t.version inside the image does not substitute
  // for the marker: the check exists precisely because the version field is
  // unreliable provenance (release images built with the prebuilt Arduino
  // core carry opaque constants there, e.g. "6671d0b").
  constexpr uint32_t kAppMagic = 0xABCD5432;
  const char* version = "2.15.0";
  testOtaSlotsFlashWrite(0x10000 + 0x20, &kAppMagic, sizeof(kAppMagic));
  testOtaSlotsFlashWrite(0x10000 + 0x20 + 16, version, strlen(version));
  EXPECT_FALSE(ota_slots::imageHasResumeMarker(ota_slots::stableSlot()));
}

TEST(OtaSlotsTest, MalformedImageRefuses) {
  setupStandardTable();
  // Wrong image magic.
  installSegmentedImage(0x10000, 2, kNoMarker, 0xE8);
  EXPECT_FALSE(ota_slots::imageHasResumeMarker(ota_slots::stableSlot()));
  // Zero segments.
  installSegmentedImage(0x10000, 0, kNoMarker);
  EXPECT_FALSE(ota_slots::imageHasResumeMarker(ota_slots::stableSlot()));
  // Empty slot (all 0xFF).
  testOtaSlotsReset();
  setupStandardTable();
  EXPECT_FALSE(ota_slots::imageHasResumeMarker(ota_slots::stableSlot()));
  EXPECT_FALSE(ota_slots::imageHasResumeMarker(nullptr));
}

TEST(OtaSlotsTest, SegmentLengthOverflowRefuses) {
  setupStandardTable();
  // A segment whose declared data length runs past the partition must be
  // rejected, not scanned partially.
  const uint32_t dataLen = 0x700000;  // > app0's 0x640000 slot size
  uint8_t image[24 + 8];
  memset(image, 0xFF, sizeof(image));
  image[0] = 0xE9;
  image[1] = 1;
  memcpy(image + 24 + 4, &dataLen, sizeof(dataLen));
  testOtaSlotsFlashWrite(0x10000, image, sizeof(image));
  EXPECT_FALSE(ota_slots::imageHasResumeMarker(ota_slots::stableSlot()));
}
