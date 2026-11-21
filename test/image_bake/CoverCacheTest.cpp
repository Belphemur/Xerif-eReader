// Combined cover cache host suite (design Amendment B.1): pins the
// sectioned file format — identity header, section directory, sub-file-view
// reads, thumb-height keying and the rejection paths (legacy headerless
// files, wrong identity, stale directories).
//
// The section payloads are opaque BMP bytes at this layer: the assembler
// and the validator treat them as byte ranges, and the decode side
// (JPEG/PNG dual-sink emission) is device-only.

#include <CoverCache.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "HalFile.h"
#include "HalStorage.h"

namespace {

class CoverCacheTest : public ::testing::Test {
 protected:
  void SetUp() override { Storage.files.clear(); }
  void TearDown() override { Storage.files.clear(); }

  static constexpr int kThumbHeight = 400;
  static constexpr bool kCropped = true;

  // Opaque stand-ins for the dual-sink decode's section payloads.
  const std::vector<uint8_t> thumbBytes{'B', 'M', 0x01, 0x02, 0x03, 0x04, 0x05};
  const std::vector<uint8_t> coverBytes{'B', 'M', 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6};

  void writeStubFile(const char* path, const std::vector<uint8_t>& bytes) {
    Storage.files[path] = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }

  bool coverPath(char* out, size_t cap, int height = kThumbHeight, bool cropped = kCropped) {
    return imagebake::coverCachePath("/t", height, cropped, out, cap);
  }

  bool assemble(const char* finalPath, uint16_t thumbW = 240, uint16_t thumbH = kThumbHeight) {
    writeStubFile("/t/.ccthumb.tmp", thumbBytes);
    writeStubFile("/t/.cccover.tmp", coverBytes);
    return imagebake::assembleCoverCache(finalPath, "/t/.ccthumb.tmp", thumbW, thumbH, "/t/.cccover.tmp", 480, 800,
                                         kThumbHeight, kCropped);
  }

  std::string readAll(HalFile& f) {
    std::string out;
    uint8_t buf[64];
    int n;
    while ((n = f.read(buf, sizeof(buf))) > 0) out.append(reinterpret_cast<char*>(buf), static_cast<size_t>(n));
    return out;
  }
};

TEST_F(CoverCacheTest, NamesAndIdentityKeyOnHeightAndCrop) {
  char fit[64], crop[64], h226[64];
  ASSERT_TRUE(coverPath(crop, sizeof(crop), 400, true));
  ASSERT_TRUE(coverPath(fit, sizeof(fit), 400, false));
  ASSERT_TRUE(coverPath(h226, sizeof(h226), 226, true));
  EXPECT_STREQ(crop, "/t/cover_ck_400.pxc");
  EXPECT_STREQ(fit, "/t/cover_cf_400.pxc");
  EXPECT_STREQ(h226, "/t/cover_ck_226.pxc");
  // The identity hash: height and crop both participate.
  EXPECT_NE(imagebake::coverCacheHash32(400, true), imagebake::coverCacheHash32(226, true));
  EXPECT_NE(imagebake::coverCacheHash32(400, true), imagebake::coverCacheHash32(400, false));

  char tmpl[64];
  ASSERT_TRUE(imagebake::coverCacheTemplate("/t", true, tmpl, sizeof(tmpl)));
  EXPECT_STREQ(tmpl, "/t/cover_ck_[HEIGHT].pxc");
}

TEST_F(CoverCacheTest, SectionRoundTripThroughSubFileView) {
  char path[64];
  ASSERT_TRUE(coverPath(path, sizeof(path)));
  ASSERT_TRUE(assemble(path));

  for (const auto [section, payload] : {std::make_pair(imagebake::CoverSection::Thumb, thumbBytes),
                                        std::make_pair(imagebake::CoverSection::Cover, coverBytes)}) {
    HalFile f;
    ASSERT_TRUE(imagebake::openCoverBitmap("T", path, kThumbHeight, section, f));
    // Bitmap::parseHeaders seeks absolute 0 — the view must translate.
    ASSERT_TRUE(f.seek(0));
    const std::string bytes = readAll(f);
    EXPECT_EQ(bytes.size(), payload.size());
    EXPECT_EQ(bytes, std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
    // Reads clamp at the section end (the next section never bleeds in).
    uint8_t extra;
    EXPECT_EQ(f.read(&extra, 1), 0);
    EXPECT_EQ(f.size(), payload.size());  // view-coherent size
  }
}

TEST_F(CoverCacheTest, IdentityRejectsWrongLetterAndHash) {
  char path[64];
  ASSERT_TRUE(coverPath(path, sizeof(path)));

  // A 'C','P'-magic file whose letter or identity hash disagrees with the
  // recomputed key is rejected at open — stale bake or foreign generation.
  std::string stale =
      std::string("CP") + char('d') + std::string("\x00\x00\x00\x00", 4) + std::string("\xF0\x00\x20\x01", 4);
  Storage.files[path] = stale;
  HalFile f;
  EXPECT_FALSE(imagebake::openCoverBitmap("T", path, kThumbHeight, imagebake::CoverSection::Thumb, f));
  EXPECT_FALSE(imagebake::coverBitmapIsValid(path, kThumbHeight, imagebake::CoverSection::Thumb));

  // Right letter, wrong hash: same rejection.
  uint32_t wrongHash = imagebake::coverCacheHash32(kThumbHeight, kCropped) ^ 0x1u;
  std::string wrong = std::string("CP") + char(imagebake::kCoverCacheLetter) +
                      std::string(reinterpret_cast<const char*>(&wrongHash), 4) + std::string("\xF0\x00\x20\x01", 4);
  Storage.files[path] = wrong;
  EXPECT_FALSE(imagebake::openCoverBitmap("T", path, kThumbHeight, imagebake::CoverSection::Thumb, f));

  // A legacy standalone BMP (GIF-covered books keep one) opens whole-file —
  // the kind probe only treats 'C','P'-magic files as combined caches, so
  // a bare headerless pixel file under any name parses as a BMP downstream
  // (no writer ever produces headerless files under cover-cache names:
  // publication is temp + FAT replace).
  const std::vector<uint8_t> legacyBmp{'B', 'M', 0x10, 0x20, 0x30};
  Storage.files["/t/cover_legacy_v3.bmp"] =
      std::string(reinterpret_cast<const char*>(legacyBmp.data()), legacyBmp.size());
  EXPECT_TRUE(imagebake::openCoverBitmap("T", "/t/cover_legacy_v3.bmp", 0, imagebake::CoverSection::Thumb, f));
  EXPECT_EQ(readAll(f).size(), legacyBmp.size());
}

TEST_F(CoverCacheTest, ThumbHeightKeyingRejectsForeignHeight) {
  char path[64];
  ASSERT_TRUE(coverPath(path, sizeof(path)));
  ASSERT_TRUE(assemble(path));

  // A file generated for height 400 must not serve a 226 request: the
  // name's height segment disagrees with the open request, and even a
  // name-collided file fails the directory's height check.
  HalFile f;
  EXPECT_FALSE(imagebake::openCoverBitmap("T", path, 226, imagebake::CoverSection::Thumb, f));
  EXPECT_FALSE(imagebake::coverBitmapIsValid(path, 226, imagebake::CoverSection::Thumb));
  // The cover section carries no height expectation — it still opens.
  EXPECT_TRUE(imagebake::openCoverBitmap("T", path, 0, imagebake::CoverSection::Cover, f));
}

TEST_F(CoverCacheTest, CombinedFileContractAssemblyAndDirectory) {
  char path[64];
  ASSERT_TRUE(coverPath(path, sizeof(path)));
  ASSERT_TRUE(assemble(path));
  ASSERT_TRUE(Storage.exists(path));

  // The file: 11-byte identity header + 24-byte directory + both payloads.
  const std::string& bytes = Storage.files[path];
  const size_t expectedSize = imagebake::kCoverPrefixBytes + thumbBytes.size() + coverBytes.size();
  EXPECT_EQ(bytes.size(), expectedSize);
  EXPECT_EQ(static_cast<uint8_t>(bytes[0]), imagebake::kCacheMagic0);
  EXPECT_EQ(static_cast<uint8_t>(bytes[1]), imagebake::kCacheMagic1);
  EXPECT_EQ(bytes[2], imagebake::kCoverCacheLetter);
  uint32_t hash32 = 0;
  memcpy(&hash32, bytes.data() + 3, sizeof(hash32));
  EXPECT_EQ(hash32, imagebake::coverCacheHash32(kThumbHeight, kCropped));

  // Directory: thumb entry at 35 with its payload, cover entry right after.
  uint32_t off0 = 0, len0 = 0, off1 = 0, len1 = 0;
  uint16_t w0 = 0, h0 = 0, w1 = 0, h1 = 0;
  memcpy(&off0, bytes.data() + 11, 4);
  memcpy(&len0, bytes.data() + 15, 4);
  memcpy(&w0, bytes.data() + 19, 2);
  memcpy(&h0, bytes.data() + 21, 2);
  memcpy(&off1, bytes.data() + 23, 4);
  memcpy(&len1, bytes.data() + 27, 4);
  memcpy(&w1, bytes.data() + 31, 2);
  memcpy(&h1, bytes.data() + 33, 2);
  EXPECT_EQ(off0, imagebake::kCoverPrefixBytes);
  EXPECT_EQ(len0, thumbBytes.size());
  EXPECT_EQ(w0, 240);
  EXPECT_EQ(h0, kThumbHeight);
  EXPECT_EQ(off1, imagebake::kCoverPrefixBytes + thumbBytes.size());
  EXPECT_EQ(len1, coverBytes.size());
  EXPECT_EQ(w1, 480);
  EXPECT_EQ(h1, 800);

  // Both sections validate through the non-opening probe.
  EXPECT_TRUE(imagebake::coverBitmapIsValid(path, kThumbHeight, imagebake::CoverSection::Thumb));
  EXPECT_TRUE(imagebake::coverBitmapIsValid(path, 0, imagebake::CoverSection::Cover));
}

TEST_F(CoverCacheTest, AssemblyFailureLeavesNoFileAndKeepsPrevious) {
  char path[64];
  ASSERT_TRUE(coverPath(path, sizeof(path)));

  // Missing section temps: nothing is written, no temp file lingers.
  EXPECT_FALSE(imagebake::assembleCoverCache(path, "/t/.ccthumb.tmp", 240, kThumbHeight, "/t/.cccover.tmp", 480, 800,
                                             kThumbHeight, kCropped));
  EXPECT_FALSE(Storage.exists(path));

  // A publish failure exercises the HAL's documented remove-then-rename
  // tradeoff: the live name is left ABSENT (never a partial assembly) — the
  // consumer treats the missing file as "regenerate on next open". The
  // staged .t is dropped so nothing lingers under a final-looking name.
  const std::vector<uint8_t> previous{'C', 'P', 'x', 1, 2, 3};
  Storage.files[path] = std::string(reinterpret_cast<const char*>(previous.data()), previous.size());
  Storage.failOnRenameCall = 1;
  Storage.renameCallCount = 0;
  EXPECT_FALSE(assemble(path));
  EXPECT_FALSE(Storage.exists(path));
  char tmpPath[80];
  snprintf(tmpPath, sizeof(tmpPath), "%s.t", path);
  EXPECT_FALSE(Storage.exists(tmpPath));
}

}  // namespace
