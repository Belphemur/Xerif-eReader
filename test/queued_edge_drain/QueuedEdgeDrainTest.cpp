#include <gtest/gtest.h>

#include "util/QueuedEdgeDrain.h"

#include <deque>

namespace {

// A fake of the SDK consume-on-check edge probe (InputManager::wasPressed):
// each call consumes one queued edge and reports whether one remained; a call
// on an empty queue consumes nothing and returns false.
struct FakeEdgeProbe {
  std::deque<bool> queue;

  bool operator()() {
    if (queue.empty()) return false;
    const bool edge = queue.front();
    queue.pop_front();
    return edge;
  }
};

// Three rapid presses queued behind one dispatch must drain as a count of 3,
// not three one-edge frames (issue #259 replay complaint).
TEST(QueuedEdgeDrainTest, ThreeQueuedPressesDrainAsCountThree) {
  FakeEdgeProbe probe;
  for (int i = 0; i < 3; ++i) probe.queue.push_back(true);
  EXPECT_EQ(edgecount::drainCount(probe), 3);
  EXPECT_TRUE(probe.queue.empty());
}

// A held button queues exactly ONE press edge (§3.3): the count must stay 1.
TEST(QueuedEdgeDrainTest, HeldButtonQueuesOneEdgeSoCountIsOne) {
  FakeEdgeProbe probe;
  probe.queue.push_back(true);
  EXPECT_EQ(edgecount::drainCount(probe), 1);
  EXPECT_TRUE(probe.queue.empty());
}

// No queued edges: the drain consumes nothing and reports zero.
TEST(QueuedEdgeDrainTest, EmptyQueueDrainsZero) {
  FakeEdgeProbe probe;
  EXPECT_EQ(edgecount::drainCount(probe), 0);
}

// The cap is the queue capacity: draining stops there and the leftover edges
// stay queued for the next frame (the count is a lower bound only on overflow).
TEST(QueuedEdgeDrainTest, CapStopsAtMaxQueuedEdgesAndPreservesTheLeftover) {
  FakeEdgeProbe probe;
  for (int i = 0; i < edgecount::kMaxQueuedEdges + 4; ++i) probe.queue.push_back(true);
  EXPECT_EQ(edgecount::drainCount(probe), edgecount::kMaxQueuedEdges);
  EXPECT_EQ(probe.queue.size(), 4u);
  // The edge at the cap boundary must NOT have been consumed and dropped:
  // the next frame's drain recovers the full remainder.
  EXPECT_EQ(edgecount::drainCount(probe), 4);
}

// The drain stops at the first false probe (the queue empties mid-drain).
TEST(QueuedEdgeDrainTest, DrainStopsOnFirstFalse) {
  FakeEdgeProbe probe;
  probe.queue.push_back(true);
  EXPECT_EQ(edgecount::drainCount(probe), 1);
  EXPECT_TRUE(probe.queue.empty());
  // Further probes (and further drains) report zero.
  EXPECT_EQ(edgecount::drainCount(probe), 0);
}

}  // namespace
