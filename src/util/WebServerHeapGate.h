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

// Web-stack core footprint: http server + routes + WebDAV + WebSocket client
// array. Sized to the X3 abort log (928 B left after the 30 route lambdas from
// a ~44 KB web stack) and the design doc's "32-48 KB" range; mirrors the
// HttpDownloader TLS floor (src/util/TlsHeapGuard.h) which this gate extends.
constexpr uint32_t kWebStackFloor = 40000;
// Extra headroom the mode-selection gate (before the WiFi working set lands)
// requires for the WiFi+lwIP+mDNS resident set (~45 KB combined per the
// CrossPointWebServerActivity::onEnter comment). The mode-selection floor is
// this plus kWebStackFloor.
constexpr uint32_t kWifiWorkingSetReserve = 5000;
constexpr uint32_t kModeSelectionFloor = kWebStackFloor + kWifiWorkingSetReserve;

// freeHeap / maxAllocHeap are ESP.getFreeHeap() / ESP.getMaxAllocHeap(). floor
// is the site's threshold (kModeSelectionFloor at mode-selection, kWebStackFloor
// at the begin() tail). retryExhausted is true once a heap-defrag reboot has
// been attempted for this activity instance, so a structurally-underprovisioned
// board degrades to Home instead of looping.
inline Verdict verdict(uint32_t freeHeap, uint32_t maxAllocHeap, uint32_t floor, bool retryExhausted) {
  if (freeHeap >= floor && maxAllocHeap >= floor) return Verdict::Proceed;
  if (retryExhausted) return Verdict::Fallback;
  return Verdict::Defrag;
}

}  // namespace webgate
