// The band-buffered pixel-cache writer the framebuffer converters stream
// through — the shared identity-headered core (lib/ImageBake/ImageBakeCache.h,
// design Amendment B.1), formerly the duplicated PixelCache.h.
#include <ImageBakeCache.h>
#include <gtest/gtest.h>

#include <filesystem>

#include "test/ScopedTempDir.h"

namespace {
class BandCacheWriterTest : public ::testing::Test {
 protected:
  ScopedTempDir tempDir;
  const std::string path = (tempDir.path() / "pixels.bin").string();
  void SetUp() override {
    HalFileTest::failNextWrite = false;
    HalFileTest::failNextClose = false;
  }
  void TearDown() override {
    HalFileTest::failNextWrite = false;
    HalFileTest::failNextClose = false;
  }
};
TEST_F(BandCacheWriterTest, PublishesCompleteCache) {
  imagebake::BandCacheWriter cache;
  ASSERT_TRUE(
      cache.begin(path.c_str(), 4, 1, 0, 0, 1, imagebake::CacheIdentity{imagebake::kFramebufferCacheLetter, 0}));
  EXPECT_TRUE(cache.finalize());
  EXPECT_TRUE(Storage.exists(path.c_str()));
  EXPECT_EQ(std::filesystem::file_size(path), imagebake::kCacheHeaderBytes + 1u);
}
TEST_F(BandCacheWriterTest, RejectsFinalWriteFailure) {
  imagebake::BandCacheWriter cache;
  ASSERT_TRUE(
      cache.begin(path.c_str(), 4, 1, 0, 0, 1, imagebake::CacheIdentity{imagebake::kFramebufferCacheLetter, 0}));
  HalFileTest::failNextWrite = true;
  EXPECT_FALSE(cache.finalize());
  EXPECT_FALSE(Storage.exists(path.c_str()));
}
TEST_F(BandCacheWriterTest, RejectsCloseFailure) {
  imagebake::BandCacheWriter cache;
  ASSERT_TRUE(
      cache.begin(path.c_str(), 4, 1, 0, 0, 1, imagebake::CacheIdentity{imagebake::kFramebufferCacheLetter, 0}));
  HalFileTest::failNextClose = true;
  EXPECT_FALSE(cache.finalize());
  EXPECT_FALSE(Storage.exists(path.c_str()));
}
}  // namespace
