#include <gtest/gtest.h>
#include <mbedtls/sha256.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>

#include "FirmwareFlasher.h"
#include "HalStorage.h"
#include "esp_ota_ops.h"

namespace {

// Chip id compiled into the synthetic image and the stub's reset default.
constexpr uint16_t kSyntheticChipId = 0x1234;

// A minimal ESP app image with one segment and no SHA trailer. Its total size
// is the 16-byte aligned body plus the checksum byte, and its embedded board
// tag matches the x4pro board compiled into this host target.
std::string buildValidImage() {
  constexpr uint32_t kSegmentDataLen = 65500;
  constexpr size_t kTotalSize = 65536;  // 24-byte header + 8-byte seg header + data + 4-byte pad
  constexpr char kBoardTag[] = "CROSSPOINT-BOARD-V1:x4pro;";

  std::string image(kTotalSize, '\0');
  image[0] = '\xE9';  // ESP_IMAGE_MAGIC
  image[1] = 1;       // one segment
  std::memcpy(image.data() + 12, &kSyntheticChipId, sizeof(kSyntheticChipId));
  image[23] = 0;  // no appended SHA-256 trailer

  std::memcpy(image.data() + 24 + 4, &kSegmentDataLen, sizeof(kSegmentDataLen));
  std::memcpy(image.data() + 32, kBoardTag, sizeof(kBoardTag) - 1);
  for (size_t i = sizeof(kBoardTag) - 1; i < kSegmentDataLen; ++i) image[32 + i] = '\xA5';

  uint8_t checksum = 0xEF;
  for (uint32_t i = 0; i < kSegmentDataLen; ++i) checksum ^= static_cast<uint8_t>(image[32 + i]);
  image[kTotalSize - 1] = static_cast<char>(checksum);

  return image;
}

}  // namespace

// testSetRunningChipId() writes a process-global that validateImageFile reads
// per call under HOST_TEST. Reset it around every test so --gtest_shuffle can
// never leak chip state (e.g. the fixture's 0x0009) into another test.
struct FirmwareFlasherValidateImageFile : public ::testing::Test {
  void SetUp() override { testSetRunningChipId(kSyntheticChipId); }
  void TearDown() override { testSetRunningChipId(kSyntheticChipId); }
};

// The SD picker filters by .bin only; validateImageFile must therefore judge
// firmware by its bytes, not by whether its filename follows a release-asset
// convention.
TEST_F(FirmwareFlasherValidateImageFile, AcceptsAnyBinFilenameForIdenticalImageBytes) {
  const std::string image = buildValidImage();
  constexpr size_t kPartitionSize = 2 * 1024 * 1024;
  auto& files = Storage.files;
  files["/xerif-1.16.0-x4pro.bin"] = image;
  files["/xpoint-1.16.0-x4pro.bin"] = image;
  files["/locally-built-firmware.bin"] = image;

  EXPECT_EQ(firmware_flash::validateImageFile("/xerif-1.16.0-x4pro.bin", kPartitionSize), firmware_flash::Result::OK);
  EXPECT_EQ(firmware_flash::validateImageFile("/xpoint-1.16.0-x4pro.bin", kPartitionSize), firmware_flash::Result::OK);
  EXPECT_EQ(firmware_flash::validateImageFile("/locally-built-firmware.bin", kPartitionSize),
            firmware_flash::Result::OK);
}

// §16 dance identity: the outSha256 digest must be the SHA-256 of the WHOLE
// validated file — the same bytes the header/segment/checksum verdicts were
// computed from — so a consent record bound to it can never describe a file
// swapped in before or after validation.
namespace {
void expectDigestMatchesFile(const std::string& image, const char* path, size_t partitionSize) {
  auto& files = Storage.files;
  files[path] = image;
  uint8_t digest[32];
  ASSERT_EQ(firmware_flash::validateImageFile(path, partitionSize, digest), firmware_flash::Result::OK);
  // Reference: the host stub implements FIPS 180-4; hash the same bytes with
  // a fresh context.
  mbedtls_sha256_context ref;
  mbedtls_sha256_init(&ref);
  ASSERT_EQ(mbedtls_sha256_starts(&ref, 0), 0);
  ASSERT_EQ(mbedtls_sha256_update(&ref, reinterpret_cast<const uint8_t*>(image.data()), image.size()), 0);
  uint8_t expected[32];
  ASSERT_EQ(mbedtls_sha256_finish(&ref, expected), 0);
  mbedtls_sha256_free(&ref);
  EXPECT_EQ(0, std::memcmp(digest, expected, 32));
  // Deterministic across calls: the digest comes from the validation read,
  // not from per-call state.
  uint8_t digest2[32];
  ASSERT_EQ(firmware_flash::validateImageFile(path, partitionSize, digest2), firmware_flash::Result::OK);
  EXPECT_EQ(0, std::memcmp(digest2, expected, 32));
  files.erase(path);
}
}  // namespace

TEST_F(FirmwareFlasherValidateImageFile, OutSha256MatchesWholeValidatedFile) {
  constexpr size_t kPartitionSize = 2 * 1024 * 1024;
  expectDigestMatchesFile(buildValidImage(), "/whole-file-digest.bin", kPartitionSize);
}

// Same binding for a hash_appended image: the trailer participates in the
// whole-file digest even though the image's own SHA-256 verdict excludes it.
TEST_F(FirmwareFlasherValidateImageFile, OutSha256MatchesWholeValidatedFileWithTrailer) {
  constexpr size_t kPartitionSize = 2 * 1024 * 1024;
  std::string image = buildValidImage();
  image[23] = 1;  // hash_appended: the trailer is the SHA-256 of [0, padEnd)
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  ASSERT_EQ(mbedtls_sha256_starts(&ctx, 0), 0);
  ASSERT_EQ(mbedtls_sha256_update(&ctx, reinterpret_cast<const uint8_t*>(image.data()), image.size()), 0);
  uint8_t trailer[32];
  ASSERT_EQ(mbedtls_sha256_finish(&ctx, trailer), 0);
  mbedtls_sha256_free(&ctx);
  image.append(reinterpret_cast<const char*>(trailer), sizeof(trailer));
  expectDigestMatchesFile(image, "/whole-file-digest-trailed.bin", kPartitionSize);
}

namespace {

// The real upstream CrossPoint 1.6.0 release image for the x4pro — the exact
// file a locked X4 Pro owner would copy to SD to downgrade. CMake fetches it
// at configure time and verifies the pinned SHA-256.
constexpr uint16_t kX4ProChipId = 0x0009;  // ESP32-S3 (esp_image_header_t offset 12)
constexpr size_t kExpectedFixtureSize = 5341040;
constexpr size_t kX4ProOtaPartitionSize = 6 * 1024 * 1024;

std::optional<std::string> readFixtureImage() {
  std::ifstream in(X4PRO_FIXTURE_PATH, std::ios::binary);
  if (!in.good()) return std::nullopt;
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

uint32_t firstSegmentDataLen(const std::string& image) {
  uint32_t len = 0;
  std::memcpy(&len, image.data() + 24 + 4, sizeof(len));  // seg header: load addr(4) + len(4)
  return len;
}

}  // namespace

// The locked-user escape hatch: an untagged upstream image must pass every
// validation gate (magic, chip id, segment walk, XOR checksum, SHA-256
// trailer, board-tag scan) on an x4pro-class device.
TEST_F(FirmwareFlasherValidateImageFile, AcceptsRealUpstreamX4ProReleaseImage) {
  const std::optional<std::string> fixture = readFixtureImage();
  if (!fixture) GTEST_SKIP() << "fixture not downloaded at configure time";
  const std::string& image = *fixture;
  ASSERT_EQ(image.size(), kExpectedFixtureSize);
  ASSERT_EQ(static_cast<uint8_t>(image[0]), 0xE9) << "fixture is not an ESP app image";
  uint16_t chip = 0;
  std::memcpy(&chip, image.data() + 12, sizeof(chip));
  ASSERT_EQ(chip, kX4ProChipId) << "fixture chip_id mismatch";

  testSetRunningChipId(kX4ProChipId);
  Storage.files["/upstream-crosspoint-1.6.0-x4pro.bin"] = image;
  EXPECT_EQ(firmware_flash::validateImageFile("/upstream-crosspoint-1.6.0-x4pro.bin", kX4ProOtaPartitionSize),
            firmware_flash::Result::OK);
}

// A single flipped data byte must be caught by the XOR checksum — guards
// against a validation test that would trivially pass on any input.
TEST_F(FirmwareFlasherValidateImageFile, RejectsRealImageWithFlippedByteAsBadChecksum) {
  const std::optional<std::string> fixture = readFixtureImage();
  if (!fixture) GTEST_SKIP() << "fixture not downloaded at configure time";
  const std::string& image = *fixture;
  std::string corrupted = image;
  // Flip a byte in the middle of segment 0's data so only the checksum (not
  // the segment table) is affected.
  const size_t flipOffset = 24 + 8 + firstSegmentDataLen(image) / 2;
  corrupted[flipOffset] = static_cast<char>(corrupted[flipOffset] ^ 0xFF);

  testSetRunningChipId(kX4ProChipId);
  Storage.files["/corrupted.bin"] = corrupted;
  EXPECT_EQ(firmware_flash::validateImageFile("/corrupted.bin", kX4ProOtaPartitionSize),
            firmware_flash::Result::BAD_CHECKSUM);
}

// A tag naming another board (sticky) must fail the board scan on the x4pro
// class. The tag is planted straddling a 4096-byte feed-chunk boundary to also
// prove the streaming scanner reassembles needles split across chunks.
TEST_F(FirmwareFlasherValidateImageFile, RejectsForeignBoardTagOnX4Pro) {
  const std::optional<std::string> fixture = readFixtureImage();
  if (!fixture) GTEST_SKIP() << "fixture not downloaded at configure time";
  const std::string& image = *fixture;
  std::string tagged = image;
  constexpr char kStickyTag[] = "CROSSPOINT-BOARD-V1:sticky;";
  // Segment 0's data starts at file offset 32; the scanner's first feed chunk
  // covers [32, 32+4096), so a tag starting at 4118 spans the chunk boundary.
  constexpr size_t kTagOffset = 4118;
  static_assert(kTagOffset + sizeof(kStickyTag) - 1 > 32 + 4096, "tag must straddle the feed boundary");
  std::memcpy(tagged.data() + kTagOffset, kStickyTag, sizeof(kStickyTag) - 1);

  testSetRunningChipId(kX4ProChipId);
  Storage.files["/sticky-tagged.bin"] = tagged;
  EXPECT_EQ(firmware_flash::validateImageFile("/sticky-tagged.bin", kX4ProOtaPartitionSize),
            firmware_flash::Result::WRONG_BOARD);
}
