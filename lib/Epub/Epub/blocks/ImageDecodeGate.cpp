#include "ImageDecodeGate.h"

#include <Arduino.h>
#include <Logging.h>

namespace {

enum class Owner : uint8_t { Free = 0, Worker, Foreground };

struct ClaimEntry {
  uint64_t hash = 0;
  Owner owner = Owner::Free;
  bool abort = false;  // foreground asked the worker decode to stop
};

ClaimEntry entries[ImageDecodeGate::kMaxEntries];
std::mutex registryMux;

// rule 4 decode bus. The mutex is try-lock only on the caller side: both
// entrants poll, so no holder can be blocked indefinitely behind another
// task's priority. The pending flag is the worker's band-boundary yield
// signal (checked inside its abort hook, outside this mutex).
std::mutex decodeBusMux;
std::atomic<bool> foregroundBusFlag{false};

ClaimEntry* findEntry(uint64_t hash) {
  for (auto& entry : entries) {
    if (entry.owner != Owner::Free && entry.hash == hash) return &entry;
  }
  return nullptr;
}

ClaimEntry* allocEntry(uint64_t hash) {
  for (auto& entry : entries) {
    if (entry.owner == Owner::Free) {
      entry.hash = hash;
      entry.abort = false;
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace

uint64_t ImageDecodeGate::pathHash(const std::string& path) {
  uint64_t hash = 14695981039346656037ull;
  for (const char c : path) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool ImageDecodeGate::acquireForeground(const uint64_t hash) {
  const uint32_t deadline = millis() + kForegroundWaitMs;
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(registryMux);
      if (ClaimEntry* entry = findEntry(hash)) {
        if (entry->owner == Owner::Foreground) {
          // Same task re-entering (should not happen — one decode per image —
          // but foreground wins regardless).
          return true;
        }
        entry->abort = true;  // worker sees this at its next band boundary
      } else if (ClaimEntry* fresh = allocEntry(hash)) {
        fresh->owner = Owner::Foreground;
        return true;
      } else {
        // Table full: with one decode at a time this is unreachable; the
        // foreground proceeds unclaimed (the worker's dequeue re-check of the
        // cache file still skips it once the cache exists).
        LOG_ERR("IMG", "Decode gate table full — foreground proceeds unclaimed");
        return true;
      }
    }
    if (static_cast<int32_t>(millis() - deadline) >= 0) return false;
    // Worker tasks sit below the loop task's priority, so a short sleep here
    // costs nothing but the wait itself.
    delay(kForegroundPollMs);
  }
}

void ImageDecodeGate::releaseForeground(const uint64_t hash) {
  std::lock_guard<std::mutex> lock(registryMux);
  if (ClaimEntry* entry = findEntry(hash)) {
    if (entry->owner == Owner::Foreground) {
      entry->owner = Owner::Free;
      entry->hash = 0;
      entry->abort = false;
    }
  }
}

bool ImageDecodeGate::tryAcquireWorker(const uint64_t hash) {
  std::lock_guard<std::mutex> lock(registryMux);
  if (findEntry(hash) != nullptr) return false;
  ClaimEntry* entry = allocEntry(hash);
  if (entry == nullptr) return false;
  entry->owner = Owner::Worker;
  return true;
}

void ImageDecodeGate::releaseWorker(const uint64_t hash) {
  std::lock_guard<std::mutex> lock(registryMux);
  if (ClaimEntry* entry = findEntry(hash)) {
    if (entry->owner == Owner::Worker) {
      entry->owner = Owner::Free;
      entry->hash = 0;
      entry->abort = false;
    }
  }
}

bool ImageDecodeGate::workerAbortRequested(const uint64_t hash) {
  std::lock_guard<std::mutex> lock(registryMux);
  const ClaimEntry* entry = findEntry(hash);
  return entry != nullptr && entry->owner == Owner::Worker && entry->abort;
}

void ImageDecodeGate::abortAllWorkerClaims() {
  std::lock_guard<std::mutex> lock(registryMux);
  for (auto& entry : entries) {
    if (entry.owner == Owner::Worker) entry.abort = true;
  }
}

bool ImageDecodeGate::tryAcquireDecodeBus() { return decodeBusMux.try_lock(); }

void ImageDecodeGate::releaseDecodeBus() { decodeBusMux.unlock(); }

bool ImageDecodeGate::foregroundBusPending() { return foregroundBusFlag.load(std::memory_order_acquire); }

bool ImageDecodeGate::acquireForegroundBus(const uint32_t maxWaitMs) {
  foregroundBusFlag.store(true, std::memory_order_release);
  const uint32_t deadline = millis() + maxWaitMs;
  for (;;) {
    if (decodeBusMux.try_lock()) return true;
    if (static_cast<int32_t>(millis() - deadline) >= 0) {
      foregroundBusFlag.store(false, std::memory_order_release);
      return false;
    }
    delay(kForegroundPollMs);
  }
}

void ImageDecodeGate::releaseForegroundBus() {
  decodeBusMux.unlock();
  foregroundBusFlag.store(false, std::memory_order_release);
}

void ImageDecodeGate::resetForTest() {
  std::lock_guard<std::mutex> lock(registryMux);
  for (auto& entry : entries) {
    entry.hash = 0;
    entry.owner = Owner::Free;
    entry.abort = false;
  }
  foregroundBusFlag.store(false, std::memory_order_release);
}
