#pragma once

#include <cstdint>

// Heap pre-flight for the CrossPointWebServer activity's web stack (http
// server + ~30 route lambdas + WebDAV + WebSocket client array). The X3 aborts
// on a throwing `new` inside CrossPointWebServer::begin() when the
// reading-session heap is too fragmented to host these allocations; the verdict
// here lets the activity reboot once into a pristine heap, or degrade to Home
// instead of aborting (see docs/design/2026-10-26-x3-wifi-connect-abort.md
// Addendum 3).
//
// Pure: no Arduino/ESP includes, so it is host-unit-testable, mirroring
// src/util/TlsHeapGuard.h (the TLS floor this gate extends for the web-server
// footprint).
namespace webgate {

enum class Verdict : uint8_t {
  Proceed,   // heap can host the web stack at this floor
  Defrag,    // below this floor but a heap-defrag reboot may rescue it
  Fallback,  // structurally short, or the one-shot reboot retry is exhausted
};

// Cumulative web-stack footprint, measured on the X3 abort log (issue #258:
// 928 B left after the ~44 KB of http server + routes + WebDAV had landed, and
// the WebSocket-server allocation still aborted). Sized inside the design
// doc's 32-48 KB range.
constexpr uint32_t kWebStackFloor = 44000;
// The WiFi + lwIP + mDNS resident set that lands *after* mode selection (the
// CrossPointWebServerActivity::onEnter comment: "WiFi (~45KB) plus the web
// server"). The mode-selection gate must require room for BOTH sets in free
// heap, so it uses this plus kWebStackFloor.
constexpr uint32_t kWifiWorkingSetReserve = 45000;
constexpr uint32_t kModeSelectionFreeFloor = kWebStackFloor + kWifiWorkingSetReserve;
// Contiguous-block criterion, mirroring tlsheap::kMinLargestBlock (the TLS
// record-buffer precedent): the largest single begin()-time allocations
// (per-client WebSocket buffers, upload staging) are below it, but the
// criterion leaves fragmentation headroom for them. Deliberately NOT the full
// free-floor — the web stack is many small allocations, not one big block.
constexpr uint32_t kMaxBlockFloor = 20000;

// freeHeap / maxAllocHeap are ESP.getFreeHeap() / ESP.getMaxAllocHeap().
// freeFloor is the site's free-heap threshold (kModeSelectionFreeFloor at
// mode-selection, kWebStackFloor at the begin() tail). retryExhausted is true
// once a heap-defrag reboot has been spent for this activity instance, so a
// structurally-underprovisioned board degrades to Home instead of looping.
inline Verdict verdict(uint32_t freeHeap, uint32_t maxAllocHeap, uint32_t freeFloor, bool retryExhausted) {
  if (freeHeap >= freeFloor && maxAllocHeap >= kMaxBlockFloor) return Verdict::Proceed;
  if (retryExhausted) return Verdict::Fallback;
  return Verdict::Defrag;
}

}  // namespace webgate
