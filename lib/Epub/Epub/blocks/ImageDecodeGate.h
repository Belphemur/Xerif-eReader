#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#ifdef IMAGE_DECODE_GATE_HOST
// Host test build: the test provides the clock/wait (firmware uses Arduino's
// millis()/delay()). Declared here so the gate's .cpp sees them without an
// Arduino include on host.
namespace ImageDecodeGateHost {
uint32_t nowMsFn();
void sleepMs(uint32_t ms);
}  // namespace ImageDecodeGateHost
#define IMAGE_GATE_NOW() ::ImageDecodeGateHost::nowMsFn()
#define IMAGE_GATE_SLEEP(ms) ::ImageDecodeGateHost::sleepMs(ms)
#else
#include <Arduino.h>
#define IMAGE_GATE_NOW() millis()
#define IMAGE_GATE_SLEEP(ms) delay(ms)
#endif

// Single-writer gate for .pxc cache production between the background image
// predecode worker (core 0) and the foreground render path (loop task, core
// 1). Protocol: docs/design/2026-11-06-image-predecode.md §3.4.
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

  // FNV-1a over the path — shared claim key and ImageBlock failure-bookkeeping
  // hash, so both sides always agree on the identity of a cache file.
  static uint64_t pathHash(const std::string& path);

  // Foreground render path. Returns true with the claim held (release via
  // releaseForeground); false when a worker claim did not release within
  // kForegroundWaitMs. On false the caller leaves the cache absent and draws
  // the placeholder — a later render pass retries (and by then the worker's
  // cache is typically warm).
  static bool acquireForeground(uint64_t hash);
  static void releaseForeground(uint64_t hash);
  // Worker dequeue path. False when the path is already claimed (foreground
  // mid-decode, or another worker job on the same image).
  static bool tryAcquireWorker(uint64_t hash);
  static void releaseWorker(uint64_t hash);
  // Band-boundary abort poll (converter callbacks; worker decodes only).
  static bool workerAbortRequested(uint64_t hash);
  // Reader teardown: flag every worker-held claim so a decode still in
  // flight stops at its next band boundary instead of outliving the bounded
  // cancel() join. Foreground claims are never touched.
  static void abortAllWorkerClaims();

  // Test seam: drop every entry (host tests run many scenarios in one process).
  static void resetForTest();
};
