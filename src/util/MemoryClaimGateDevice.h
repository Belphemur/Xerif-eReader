#pragma once

// Device adapter for the memory-claim gate (pure core: util/MemoryClaimGate.h
// — the part the host unit tests exercise). It owns the two real levers (§4 of
// docs/design/2026-11-03-memory-claims.md):
//   - `releaseFontCaches()`, the shared helper replacing the in-scope ad-hoc
//     releaseSdFontCaches() calls (the font screens under the claim; the sites
//     that release for their own reasons — web server, CatalogActivity,
//     reader, sleep, dictionary — keep their direct calls);
//   - `radioSuspend()` via one non-blocking stop pass per pump pass, over the
//     screen-change hook's primitive bleturner::beforeScreenChange(0, false)
//     == suspend(0) (Runtime.cpp): it refuses while a start is in flight and
//     drives ONE cancel/disconnect pass, returning false while teardown work
//     remains. Restoring the radio NEVER runs from the claim: claim end runs
//     under the RenderLock and a synchronous switchOn() would deadlock there
//     (beginOwned() spins on a render-lock try it cannot take from the lock
//     holder) — the restore is realized exclusively by the runtime tick's
//     scene-conditioned re-arm (Runtime.cpp), which a plain suspend(0) stop
//     leaves armed (it never sets radioIdleStopped).
//
// Every step is at most one non-blocking pass (§5): no settleStart(),
// no stopNow(), no switchOn()-family call runs inside a frame.

#include <Arduino.h>
#include <BlePageTurner.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <cstdint>

#include "util/MemoryClaimGate.h"
#include "util/TlsHeapGuard.h"

namespace memclaim {

// The release lever's one shared helper (§4). Synchronous and immediate; the
// caches rebuild lazily on the next render, so a release that freed nothing
// (already empty) is still a valid, pullable lever.
inline void releaseFontCaches(GfxRenderer& renderer) {
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseSdFontCaches();
  }
}

// Default floors for the cheap claims (§4: plugin catalog / OTA reuse the
// plain tlsheap floor). The font screens add their manifest buffer's DRAM
// budget on top — see the per-screen kManifestDramBudgetBytes constants.
constexpr uint32_t kDefaultFreeFloor = tlsheap::kMinFreeHeap;
constexpr uint32_t kDefaultMaxBlockFloor = tlsheap::kMinLargestBlock;

class DeviceClaim {
 public:
  DeviceClaim(GfxRenderer& renderer, const char* tag = "MEM") : renderer_(renderer), tag_(tag) {}

  ~DeviceClaim() { end(); }

  DeviceClaim(const DeviceClaim&) = delete;
  DeviceClaim& operator=(const DeviceClaim&) = delete;

  // Begins (or restarts) the claim: records the radio's state BEFORE anything
  // is suspended and the pre-claim heap measurement. NO blocking call — safe
  // from the activity's per-pass update (§3.2 claim.begin). Idempotent by
  // restart: the .cpfont screen's manifest-level RETRY re-enters through here.
  void begin(const uint32_t freeFloor, const uint32_t maxBlockFloor) {
    freeFloor_ = freeFloor;
    maxBlockFloor_ = maxBlockFloor;
    radioWasUp_ = bleturner::status().running;
    stepsTried_ = 0;
    radioStopStarted_ = false;
    radioFirstStopMs_ = 0;
    measure(freeBefore_, maxBlockBefore_);
    LOG_INF(tag_, "[MEM] claim begin: free=%u maxBlock=%u radioWasUp=%d floors=%u/%u", freeBefore_, maxBlockBefore_,
            radioWasUp_ ? 1 : 0, freeFloor_, maxBlockFloor_);
  }

  // Records the FontCaches lever as pulled for a release made OUTSIDE the pump
  // (§3's unconditional pre-allocation releases) so the planner never
  // re-suggests it and the fallback log names the levers truthfully.
  void noteFontCachesPulled() { stepsTried_ = memclaim::triedAfter(stepsTried_, Step::FontCaches); }

  uint8_t stepsTried() const { return stepsTried_; }

  // ONE plan-act pass (§3.2): plan with the current stepsTried; pull
  // FontCaches synchronously within the pass (a no-op release still counts as
  // pulled); then — if the re-plan hands over to the radio — issue ONE
  // non-blocking stop pass and return. Waiting spreads across activity passes;
  // Back stays live the whole time. Returns the pass's final Phase: Defrag
  // means "re-issue next pass", Fallback means the existing STR_MEMORY_ERROR
  // branch (the pump has logged the `[MEM] claim` evidence line, §5).
  Phase pumpOnePass() {
    Phase p = planNow();
    if (p.verdict == Verdict::Defrag && p.next == Step::FontCaches) {
      pullFontCaches();
      p = planNow();
    }
    if (p.verdict == Verdict::Defrag && p.next == Step::Radio && pullRadioOnePass()) {
      stepsTried_ = memclaim::triedAfter(stepsTried_, Step::Radio);
      p = planNow();
    }
    if (p.verdict == Verdict::Fallback) logFallback();
    return p;
  }

  // §4's cheap claim for the synchronous sites (plugin catalog installs, OTA):
  // pump every remaining lever in bounded, non-blocking calls — at most one
  // per statement — then return the final plan. The site's own guard
  // (HttpDownloader::heapAvailableForTransfer / the catalog's floor pre-check)
  // stays the FINAL arbiter: a Defrag (stop still in flight) or a Fallback
  // only logs the evidence; this helper never adds a failure path.
  Phase runCheap() {
    begin(freeFloor_, maxBlockFloor_);
    Phase p = pumpOnePass();
    // The radio lever is multi-pass by design; a cheap site has no per-pass
    // pump of its own, so it gets exactly one further settle attempt. A stop
    // still in flight finishes harmlessly in the background through the
    // runtime tick's 250 ms stop-finisher (Runtime.cpp), and the deferred
    // restore rule (§3.3) still applies to a stop that completes later.
    if (p.verdict == Verdict::Defrag && p.next == Step::Radio) p = pumpOnePass();
    return p;
  }

  // Ends (or is destroyed at) the claim: records state ONLY — never a blocking
  // call, never a restore (§3.3). Idempotent.
  void end() {
    if (!begun()) return;
    uint32_t free = 0;
    uint32_t maxBlock = 0;
    measure(free, maxBlock);
    LOG_DBG(tag_, "[MEM] claim end: free=%u maxBlock=%u levers=0x%02x", free, maxBlock, stepsTried_);
    freeBefore_ = 0;
    maxBlockBefore_ = 0;
  }

 private:
  bool begun() const { return freeBefore_ != 0 || maxBlockBefore_ != 0; }

  void measure(uint32_t& free, uint32_t& maxBlock) {
    free = ESP.getFreeHeap();
    maxBlock = ESP.getMaxAllocHeap();
  }

  Phase planNow() {
    uint32_t free = 0;
    uint32_t maxBlock = 0;
    measure(free, maxBlock);
    return memclaim::plan(free, maxBlock, freeFloor_, maxBlockFloor_, stepsTried_);
  }

  // Synchronous, within the pass; always counts as pulled even when the
  // caches were already empty (§3.2), because re-suggesting a no-op lever
  // would spin the pass and lock Back out.
  void pullFontCaches() {
    uint32_t freeBefore = 0;
    uint32_t maxBlockBefore = 0;
    measure(freeBefore, maxBlockBefore);
    releaseFontCaches(renderer_);
    stepsTried_ = memclaim::triedAfter(stepsTried_, Step::FontCaches);
    uint32_t free = 0;
    uint32_t maxBlock = 0;
    measure(free, maxBlock);
    LOG_DBG(tag_, "[MEM] claim lever=FontCaches free=%u->%u maxBlock=%u->%u", freeBefore, free, maxBlockBefore,
            maxBlock);
  }

  // ONE non-blocking stop pass. Returns true when the stop is settled NOW —
  // either by this pass or after the release deadline expired without
  // settling (the latched recorded state, §3.2 Boundedness: plan() then reads
  // the lever as pulled and can only answer Fallback; the re-issued passes
  // consumed the budget, so a teardown that would have settled on a later
  // pass is never misreported before those passes ran).
  bool pullRadioOnePass() {
    const bleturner::Status st = bleturner::status();
    if (!st.running && !st.stopping && !st.starting) {
      return true;  // radio already down (the common case at these screens)
    }
    const bool settled = bleturner::beforeScreenChange(0, false) == true;
    if (!radioStopStarted_) {
      radioStopStarted_ = true;
      radioFirstStopMs_ = millis();
    }
    if (settled) LOG_INF(tag_, "[MEM] claim lever=Radio stopped radio=%d", bleturner::status().running ? 1 : 0);
    return settled ||
           memclaim::radioStopDeadlineExpired(millis(), radioStopStarted_, radioFirstStopMs_, kBleStopDeadlineMs);
  }

  void logFallback() {
    uint32_t free = 0;
    uint32_t maxBlock = 0;
    measure(free, maxBlock);
    // F3's evidence format (design §5): heap before and after the levers,
    // which levers were pulled, and the radio state observed in the same log.
    LOG_ERR(tag_,
            "[MEM] claim fallback: free=%u maxBlock=%u (before %u/%u) levers=0x%02x "
            "fontCaches=%d radio=%d radioRunning=%d radioWasUp=%d floors=%u/%u",
            free, maxBlock, freeBefore_, maxBlockBefore_, stepsTried_,
            memclaim::stepTried(stepsTried_, Step::FontCaches) ? 1 : 0,
            memclaim::stepTried(stepsTried_, Step::Radio) ? 1 : 0, bleturner::status().running ? 1 : 0,
            radioWasUp_ ? 1 : 0, freeFloor_, maxBlockFloor_);
  }

  GfxRenderer& renderer_;
  const char* tag_;
  uint32_t freeFloor_ = tlsheap::kMinFreeHeap;
  uint32_t maxBlockFloor_ = tlsheap::kMinLargestBlock;
  // The pre-claim measurement doubles as the begun flag (§3.2: begin records
  // before anything is suspended).
  uint32_t freeBefore_ = 0;
  uint32_t maxBlockBefore_ = 0;
  uint8_t stepsTried_ = 0;
  bool radioWasUp_ = false;
  bool radioStopStarted_ = false;
  uint32_t radioFirstStopMs_ = 0;

  // §3.2 Boundedness: the same release budget the runtime gives a
  // build-starved radio release (kBuildReleaseTimeoutMs, Runtime.cpp).
  static constexpr uint32_t kBleStopDeadlineMs = 3000;
};

}  // namespace memclaim
