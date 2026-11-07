// Host tests for Epub::ZipFileCache — the PSRAM-resident ZIP entry cache the
// render-path heap gate drops as its one-shot stall relief (render-heap-gate
// recovery, 2026-11). The cache class and its pool-allocated map only exist
// under BOARD_HAS_PSRAM, so this target compiles the Epub/ZipFile stack with
// that define set (poolMalloc collapses to plain malloc on the host — same
// container semantics, no PSRAM to observe). On firmware the same map lands
// in PSRAM via heap_caps_malloc(MALLOC_CAP_SPIRAM).
//
// Coverage: load hit/miss lookups, single-entry invalidate, unload (the
// gate's relief valve: isLoaded() false + empty map afterwards), reload after
// unload, a zero-entry archive, and a failed load leaving the cache unloaded.

#include <Epub.h>
#include <ZipFile.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Minimal 22-byte end-of-central-directory record for an empty archive:
// signature, disk numbers, 0 entries, 0 central-dir size/offset, no comment.
std::vector<uint8_t> emptyZipBytes() {
  std::vector<uint8_t> out;
  const auto putLe16 = [&out](uint16_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
  };
  const auto putLe32 = [&out, &putLe16](uint32_t v) {
    putLe16(static_cast<uint16_t>(v));
    putLe16(static_cast<uint16_t>(v >> 16));
  };
  putLe32(0x06054b50);
  putLe16(0);
  putLe16(0);
  putLe16(0);
  putLe16(0);
  putLe32(0);
  putLe32(0);
  putLe16(0);
  return out;
}

}  // namespace

class ZipFileCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    tmpDir_ =
        fs::temp_directory_path() / ("zip_file_cache_" + std::string(info->name()) + "_" + std::to_string(::getpid()));
    fs::remove_all(tmpDir_);
    fs::create_directories(tmpDir_);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmpDir_, ec);
  }

  // Writes bytes to a file under the per-test temp dir; returns the path.
  std::string writeFile(const std::string& name, const std::vector<uint8_t>& bytes) {
    const fs::path p = tmpDir_ / name;
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return p.string();
  }

  std::string copyRealEpub() {
    const fs::path p = tmpDir_ / "book.epub";
    fs::copy_file(TEST_EPUB_SOURCE, p, fs::copy_options::overwrite_existing);
    return p.string();
  }

  // First path in the loaded cache (any entry) as an owned string.
  static std::string firstCachePath(const Epub::ZipFileCache& cache) {
    for (const auto& entry : cache.cacheRef()) {
      return std::string{entry.first.data(), entry.first.size()};
    }
    return {};
  }

  fs::path tmpDir_;
};

TEST_F(ZipFileCacheTest, LoadPopulatesAndServesHitsAndMisses) {
  const std::string epubPath = copyRealEpub();
  Epub::ZipFileCache cache;
  cache.load(epubPath);
  ASSERT_TRUE(cache.isLoaded());
  EXPECT_FALSE(cache.cacheRef().empty());

  const std::string knownPath = firstCachePath(cache);
  ASSERT_FALSE(knownPath.empty());
  const ZipFile::FileStatSlim* hit = cache.get(knownPath);
  ASSERT_NE(hit, nullptr);
  // No size assertion here: firstCachePath() returns an arbitrary entry
  // (unordered_map order) and real EPUBs legitimately contain zero-byte
  // entries. The contract under test is the hit lookup itself.

  EXPECT_EQ(cache.get("no/such/entry.bin"), nullptr);
}

TEST_F(ZipFileCacheTest, InvalidateDropsOneEntryOnly) {
  const std::string epubPath = copyRealEpub();
  Epub::ZipFileCache cache;
  cache.load(epubPath);
  ASSERT_TRUE(cache.isLoaded());
  const size_t before = cache.cacheRef().size();

  const std::string victim = firstCachePath(cache);
  ASSERT_FALSE(victim.empty());
  cache.invalidate(victim);
  EXPECT_EQ(cache.get(victim), nullptr);
  EXPECT_EQ(cache.cacheRef().size(), before - 1);

  // Invalidation of an unknown entry is a no-op, and the cache stays loaded.
  cache.invalidate("no/such/entry.bin");
  EXPECT_TRUE(cache.isLoaded());
  const std::string survivor = firstCachePath(cache);
  if (!survivor.empty()) {
    EXPECT_NE(cache.get(survivor), nullptr);
  }
}

TEST_F(ZipFileCacheTest, UnloadClearsMapAndMarksUnloaded) {
  const std::string epubPath = copyRealEpub();
  Epub::ZipFileCache cache;
  cache.load(epubPath);
  ASSERT_TRUE(cache.isLoaded());
  const std::string knownPath = firstCachePath(cache);
  ASSERT_FALSE(knownPath.empty());

  cache.unload();
  EXPECT_FALSE(cache.isLoaded());
  EXPECT_TRUE(cache.cacheRef().empty());
  // Post-unload lookups miss even for a previously cached path — callers fall
  // back to per-call central-directory scans (the gate's relief contract).
  EXPECT_EQ(cache.get(knownPath), nullptr);
}

TEST_F(ZipFileCacheTest, LoadRebuildsAfterUnload) {
  const std::string epubPath = copyRealEpub();
  Epub::ZipFileCache cache;
  cache.load(epubPath);
  ASSERT_TRUE(cache.isLoaded());
  cache.unload();
  ASSERT_FALSE(cache.isLoaded());

  cache.load(epubPath);
  EXPECT_TRUE(cache.isLoaded());
  const std::string knownPath = firstCachePath(cache);
  ASSERT_FALSE(knownPath.empty());
  EXPECT_NE(cache.get(knownPath), nullptr);
}

TEST_F(ZipFileCacheTest, LoadAcceptsZeroEntryArchive) {
  const std::string emptyZip = writeFile("empty.zip", emptyZipBytes());
  Epub::ZipFileCache cache;
  cache.load(emptyZip);
  EXPECT_TRUE(cache.isLoaded());
  EXPECT_TRUE(cache.cacheRef().empty());
  EXPECT_EQ(cache.get("anything"), nullptr);
}

TEST_F(ZipFileCacheTest, FailedLoadLeavesCacheUnloaded) {
  Epub::ZipFileCache cache;
  cache.load((tmpDir_ / "missing.epub").string());
  EXPECT_FALSE(cache.isLoaded());
  EXPECT_TRUE(cache.cacheRef().empty());
}
