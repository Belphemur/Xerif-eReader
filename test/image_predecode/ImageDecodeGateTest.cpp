#include <gtest/gtest.h>

#include <cstdint>

// Single-TU white-box harness: ImageDecodeGate.cpp is included (not linked)
// so the host clock seam below feeds the firmware-path timing code, and the
// entry table can be inspected directly after resetForTest().
#ifdef IMAGE_DECODE_GATE_HOST
namespace ImageDecodeGateHost {
uint32_t nowMs = 0;
uint32_t nowMsFn() { return nowMs; }
void sleepMs(uint32_t ms) { nowMs += ms; }
}  // namespace
#endif

#include "ImageDecodeGate.cpp"

namespace {

constexpr uint64_t hashOf(const char* path) { return ImageDecodeGate::pathHash(path); }

class ImageDecodeGateTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ImageDecodeGate::resetForTest();
#ifdef IMAGE_DECODE_GATE_HOST
    ImageDecodeGateHost::nowMs = 1000;
#endif
  }
};

TEST_F(ImageDecodeGateTest, ForegroundAcquiresAndReleasesWhenFree) {
  const uint64_t hash = hashOf("/cache/img_1_0.jpg");
  EXPECT_TRUE(ImageDecodeGate::acquireForeground(hash));
  ImageDecodeGate::releaseForeground(hash);
  // The entry is reusable after release.
  EXPECT_TRUE(ImageDecodeGate::acquireForeground(hash));
  ImageDecodeGate::releaseForeground(hash);
}

TEST_F(ImageDecodeGateTest, WorkerClaimBlocksForegroundAndFlagsAbort) {
  const uint64_t hash = hashOf("/cache/img_1_1.jpg");
  ASSERT_TRUE(ImageDecodeGate::tryAcquireWorker(hash));
  EXPECT_FALSE(ImageDecodeGate::workerAbortRequested(hash));

  // Foreground takeover request: flags the abort, waits bounded, gives up.
  EXPECT_FALSE(ImageDecodeGate::acquireForeground(hash));
  EXPECT_TRUE(ImageDecodeGate::workerAbortRequested(hash));  // flag stays set for the worker

  // Worker sees the flag at its band boundary and releases: the foreground
  // then acquires (priority takeover, §3.4 rule 2).
  ImageDecodeGate::releaseWorker(hash);
  EXPECT_FALSE(ImageDecodeGate::workerAbortRequested(hash));
  EXPECT_TRUE(ImageDecodeGate::acquireForeground(hash));
  ImageDecodeGate::releaseForeground(hash);
}

TEST_F(ImageDecodeGateTest, ForegroundWaitIsBoundedByTheDesignConstant) {
  const uint64_t hash = hashOf("/cache/img_1_2.jpg");
  ASSERT_TRUE(ImageDecodeGate::tryAcquireWorker(hash));
  const uint32_t before = ImageDecodeGateHost::nowMs;
  EXPECT_FALSE(ImageDecodeGate::acquireForeground(hash));
  // The wait consumed at least the design's bounded window and no more than
  // one poll interval past it.
  EXPECT_GE(ImageDecodeGateHost::nowMs - before, ImageDecodeGate::kForegroundWaitMs);
  EXPECT_LT(ImageDecodeGateHost::nowMs - before,
            ImageDecodeGate::kForegroundWaitMs + 2 * ImageDecodeGate::kForegroundPollMs);
}

TEST_F(ImageDecodeGateTest, WorkerClaimSucceedsOnlyWhenFree) {
  const uint64_t hash = hashOf("/cache/img_1_3.jpg");
  ASSERT_TRUE(ImageDecodeGate::acquireForeground(hash));
  EXPECT_FALSE(ImageDecodeGate::tryAcquireWorker(hash));
  ImageDecodeGate::releaseForeground(hash);
  EXPECT_TRUE(ImageDecodeGate::tryAcquireWorker(hash));
  EXPECT_FALSE(ImageDecodeGate::tryAcquireWorker(hash));  // no double claim
  ImageDecodeGate::releaseWorker(hash);
}

TEST_F(ImageDecodeGateTest, ReleaseOfAnUnheldPathIsInert) {
  const uint64_t held = hashOf("/cache/img_1_4.jpg");
  const uint64_t unheld = hashOf("/cache/img_1_5.jpg");
  ASSERT_TRUE(ImageDecodeGate::tryAcquireWorker(held));
  // Wrong-side release must not clear the other side's claim...
  ImageDecodeGate::releaseForeground(unheld);
  EXPECT_TRUE(ImageDecodeGate::tryAcquireWorker(unheld));
  // ...nor clear the held entry.
  ImageDecodeGate::releaseForeground(held);
  EXPECT_FALSE(ImageDecodeGate::tryAcquireWorker(held));
  EXPECT_TRUE(ImageDecodeGate::workerAbortRequested(held) == false);
  ImageDecodeGate::releaseWorker(held);
  ImageDecodeGate::releaseWorker(unheld);
}

TEST_F(ImageDecodeGateTest, TableExhaustionRefusesWorkerClaims) {
  uint64_t hashes[ImageDecodeGate::kMaxEntries + 1] = {};
  char path[32];
  for (size_t i = 0; i <= ImageDecodeGate::kMaxEntries; ++i) {
    snprintf(path, sizeof(path), "/cache/img_2_%zu.jpg", i);
    hashes[i] = hashOf(path);
  }
  for (size_t i = 0; i < ImageDecodeGate::kMaxEntries; ++i) {
    ASSERT_TRUE(ImageDecodeGate::tryAcquireWorker(hashes[i]));
  }
  // With one decode at a time this is unreachable in production; the worker
  // simply skips rather than evicting a live claim.
  EXPECT_FALSE(ImageDecodeGate::tryAcquireWorker(hashes[ImageDecodeGate::kMaxEntries]));
}

TEST_F(ImageDecodeGateTest, ResetForTestClearsEveryEntry) {
  const uint64_t hash = hashOf("/cache/img_1_6.jpg");
  ASSERT_TRUE(ImageDecodeGate::tryAcquireWorker(hash));
  ImageDecodeGate::resetForTest();
  EXPECT_TRUE(ImageDecodeGate::tryAcquireWorker(hash));
}

}  // namespace
