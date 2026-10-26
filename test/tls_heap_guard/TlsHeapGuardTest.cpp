#include <util/TlsHeapGuard.h>

#include <gtest/gtest.h>

// The pure TLS heap pre-flight decision shared by HttpDownloader and the
// clock-sync tz-detect guard. The floor must stay identical to
// HttpDownloader::MIN_TLS_FREE_HEAP / MIN_TLS_MAX_ALLOC (aliased there), and
// the two checks are independent: free heap covers the session, max-alloc
// covers the largest contiguous block for the record buffer.
static_assert(tlsheap::verdict(tlsheap::kMinFreeHeap, tlsheap::kMinLargestBlock) == tlsheap::Verdict::Proceed);

namespace {

TEST(TlsHeapGuardTest, ProceedAtAndAboveFloor) {
  EXPECT_EQ(tlsheap::verdict(tlsheap::kMinFreeHeap, tlsheap::kMinLargestBlock), tlsheap::Verdict::Proceed);
  EXPECT_EQ(tlsheap::verdict(tlsheap::kMinFreeHeap + 1, tlsheap::kMinLargestBlock + 1), tlsheap::Verdict::Proceed);
  EXPECT_EQ(tlsheap::verdict(300000, 200000), tlsheap::Verdict::Proceed);
}

TEST(TlsHeapGuardTest, SkipOnLowFreeHeap) {
  EXPECT_EQ(tlsheap::verdict(tlsheap::kMinFreeHeap - 1, tlsheap::kMinLargestBlock), tlsheap::Verdict::Skip);
  EXPECT_EQ(tlsheap::verdict(0, tlsheap::kMinLargestBlock), tlsheap::Verdict::Skip);
}

// The X3 failure mode is fragmentation: plenty of free heap in total, but no
// contiguous block large enough for the TLS record buffer.
TEST(TlsHeapGuardTest, SkipOnFragmentedHeapEvenWithFreeTotal) {
  EXPECT_EQ(tlsheap::verdict(tlsheap::kMinFreeHeap, tlsheap::kMinLargestBlock - 1), tlsheap::Verdict::Skip);
  EXPECT_EQ(tlsheap::verdict(300000, tlsheap::kMinLargestBlock - 1), tlsheap::Verdict::Skip);
  EXPECT_EQ(tlsheap::verdict(tlsheap::kMinFreeHeap, 0), tlsheap::Verdict::Skip);
}

TEST(TlsHeapGuardTest, SkipWhenBothBelowFloor) {
  EXPECT_EQ(tlsheap::verdict(tlsheap::kMinFreeHeap - 1, tlsheap::kMinLargestBlock - 1), tlsheap::Verdict::Skip);
  EXPECT_EQ(tlsheap::verdict(0, 0), tlsheap::Verdict::Skip);
}

TEST(TlsHeapGuardTest, FloorMatchesHttpDownloaderConstants) {
  EXPECT_EQ(tlsheap::kMinFreeHeap, 40000u);
  EXPECT_EQ(tlsheap::kMinLargestBlock, 20000u);
}

}  // namespace
