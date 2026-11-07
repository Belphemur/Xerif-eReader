#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

// Single-writer gate for .pxc cache production between the background image
// predecode worker (core 0) and the foreground render path (loop task, core
// 1). Protocol: design §5.7 (carried over from the #292 predecode design
// §3.4):
//
//   rule 1 — one writer per cache file: a path claim must be held to decode
//            into a .pxc. The registry is keyed by the path's FNV-1a hash, so
//            entries stay tiny and no path length can split a claim.
//   rule 2 — foreground wins: acquireForeground() marks any worker claim for
//            the path abort and waits a bounded time for the release.
//   rule 3 — decoder access races: the converters' decode state is call-local
//            (local JPEGDEC/PNG/AnimatedGIF per call), so concurrent decodes
//            are safe by construction; the one real race is the factory's
//            lazy singleton init, guarded inside ImageDecoderFactory.
//   rule 4 — decode bus: a decode reaches SHARED book surfaces beyond its
//            own cache path (the book source's seek+read window, and for
//            GIF/BMP the Epub ZIP extraction cache), so per-path claims
//            alone cannot serialize two decodes. The bus is one mutex held
//            for the whole decode: the worker takes it in a cancellable
//            poll; the foreground requests it (foregroundBusPending makes
//            the worker yield at its next band boundary) and takes it with
//            a bounded wait.
//
// The registry mutex is never held across a wait: the foreground's bounded
// wait polls with sleeps outside the lock.
class ImageDecodeGate {
 public:
  static constexpr uint32_t kForegroundWaitMs = 200;
  static constexpr uint32_t kForegroundPollMs = 5;
  // At most two claims can ever be live (one foreground + one worker decode);
  // four entries leave headroom for a wedged release.
  static constexpr size_t kMaxEntries = 4;

  // FNV-1a over the path — shared claim key for both sides, so they always
  // agree on the identity of a cache file.
  static uint64_t pathHash(const std::string& path);

  // Foreground render path. Returns true with the claim held (release via
  // releaseForeground); false when a worker claim did not release within
  // kForegroundWaitMs. On false the caller draws the reserved space empty —
  // a later render pass retries (and by then the worker's cache is typically
  // warm).
  static bool acquireForeground(uint64_t hash);
  static void releaseForeground(uint64_t hash);
  // Worker dequeue path. False when the path is already claimed (foreground
  // mid-decode, or another worker job on the same image).
  static bool tryAcquireWorker(uint64_t hash);
  static void releaseWorker(uint64_t hash);
  // Band-boundary abort poll (converter callbacks; worker decodes only).
  static bool workerAbortRequested(uint64_t hash);

  // Decode bus (rule 4). Worker side: poll tryAcquireDecodeBus() with
  // cancellation checks between attempts; releaseDecodeBus() when the
  // decode returns. Foreground side: acquireForegroundBus() marks the yield
  // flag, then takes the mutex with a bounded wait (false = keep the space
  // blank this pass, a later pass retries); releaseForegroundBus() drops
  // both the mutex and the flag.
  static bool tryAcquireDecodeBus();
  static void releaseDecodeBus();
  static bool foregroundBusPending();
  static bool acquireForegroundBus(uint32_t maxWaitMs);
  static void releaseForegroundBus();
  // Reader teardown: flag every worker-held claim so a decode still in
  // flight stops at its next band boundary instead of outliving the bounded
  // cancel() join. Foreground claims are never touched.
  static void abortAllWorkerClaims();

  // Test seam: drop every entry (host tests run many scenarios in one process).
  static void resetForTest();
};
