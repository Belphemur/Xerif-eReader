#include <HalDisplay.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <esp_system.h>  // esp_reset_reason: distinguishes a deep-sleep wake reset from a cold boot

// Global HalDisplay instance
HalDisplay display;

#define SD_SPI_MISO 7

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}

HalDisplay::~HalDisplay() {}

HalDisplay::Controller HalDisplay::getController() const { return BoardConfig::ACTIVE.displayController; }

void HalDisplay::begin(bool seamless) {
  // ESP_RST_DEEPSLEEP means the chip just left deep sleep, so the panel spent
  // the sleep window in DSLP; einkDisplay.begin()'s controller reset is what
  // releases it (DSLP ignores bus commands).
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
    LOG_INF("EPD", "wake from deep sleep (DSLP released by controller reset)");
  }

  // Set X3-specific panel mode before initializing.
  if (gpio.deviceIsX3()) {
    einkDisplay.setDisplayX3();
  }

  einkDisplay.begin();

  if (seamless) {
    // Defuse the SDK's X3 _x3InitialFullSyncsRemaining counter (no-op on X4)
    // so the first paint isn't promoted to FULL (~770ms). Skips the wakeup-
    // gated requestResync() below for the same reason.
    einkDisplay.skipInitialResync();
    return;
  }
  // Request resync after specific wakeup events to ensure clean display state.
  const auto wakeupReason = gpio.getWakeupReason();
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton || wakeupReason == HalGPIO::WakeupReason::AfterFlash ||
      wakeupReason == HalGPIO::WakeupReason::Other) {
    einkDisplay.requestResync();
  }
}

void HalDisplay::clearScreen(uint8_t color) const { einkDisplay.clearScreen(color); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  einkDisplay.drawImageTransparent(imageData, x, y, w, h, fromProgmem);
}

EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
  // Per-refresh booster cycle: PON before paint, POF (zero-power image hold)
  // after when turnOff — DEBUG so page-turn cadence does not drown the INFO lines.
  LOG_DBG("EPD", "refresh mode=%d turnOff=%d (booster PON for paint; POF idle-hold after when turnOff=1)",
          static_cast<int>(mode), turnOffScreen);
}

void HalDisplay::displayBufferAsync(HalDisplay::RefreshMode mode) {
  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.displayBufferAsyncNoShadow(convertRefreshMode(mode));
  // Async refreshes never power off the panel (no turnOff parameter), so only
  // the PON half of the booster cycle happens here.
  LOG_DBG("EPD", "async refresh mode=%d (booster PON for paint)", static_cast<int>(mode));
}

void HalDisplay::waitRefreshComplete() { einkDisplay.waitRefreshComplete(); }

bool HalDisplay::refreshBusy() { return einkDisplay.refreshBusy(); }

void HalDisplay::requestResync() { einkDisplay.requestResync(); }

bool HalDisplay::supportsAsyncRefresh() const { return einkDisplay.supportsAsyncRefresh(); }

HalDisplay::GrayscaleCapabilities HalDisplay::grayscaleCapabilities(GrayscaleMode mode) const {
  return einkDisplay.grayscaleCapabilities(mode);
}

bool HalDisplay::supportsAsyncGrayscaleBase() const { return grayscaleCapabilities().asyncBase; }

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.refreshDisplay(convertRefreshMode(mode), turnOffScreen);
  LOG_DBG("EPD", "refresh mode=%d turnOff=%d (booster PON for paint; POF idle-hold after when turnOff=1)",
          static_cast<int>(mode), turnOffScreen);
}

void HalDisplay::setInverted(bool inverted) { einkDisplay.setInverted(inverted); }

bool HalDisplay::toggleInverted() { return einkDisplay.toggleInverted(); }

bool HalDisplay::isInverted() const { return einkDisplay.isInverted(); }

void HalDisplay::deepSleep() {
  // UC8279 DSLP (cmd 0x07 + 0xA5 check code): the panel keeps the last frame
  // at zero power until the next controller reset. Covers deep sleep AND the
  // power-off teardown (both paths funnel here).
  LOG_INF("EPD", "deep sleep entered (DSLP, image retained)");
  einkDisplay.deepSleep();
}

bool HalDisplay::powerOffScreen() {
  // UC8279 POF (cmd 0x02): the panel keeps the last frame at zero power; the
  // next refresh re-powers via powerOnIfNeeded's PON (~40 ms), which precedes
  // every paint anyway — no wake latency is added. The facade drains any
  // in-flight async refresh first, so this can never POF mid-refresh.
  if (!einkDisplay.powerOffScreen()) return false;
  LOG_INF("EPD", "power off (POF, idle hold)");
  return true;
}

void HalDisplay::controllerIdle() { einkDisplay.controllerIdle(); }

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

uint8_t* HalDisplay::lendFrameBufferStorage(uint32_t* sizeOut) { return einkDisplay.lendBuildStorage(sizeOut); }

void HalDisplay::returnFrameBufferStorage() { einkDisplay.returnBuildStorage(); }

bool HalDisplay::displayGrayscaleBase(GrayscaleMode mode, RefreshMode fallback, bool turnOffScreen) {
  if (gpio.deviceIsX3() && fallback == HALF_REFRESH) einkDisplay.requestResync();
  const bool ok =
      einkDisplay.displayGrayscaleBase(mode, static_cast<EInkDisplay::RefreshMode>(fallback), turnOffScreen);
  LOG_DBG("EPD", "gray refresh mode=%d turnOff=%d (booster PON for paint; POF idle-hold after when turnOff=1)",
          static_cast<int>(fallback), turnOffScreen);
  return ok;
}

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
}

void HalDisplay::displayGrayscaleBase(RefreshMode fallback, bool turnOffScreen) {
  // X3: a HALF fallback means the caller wants a clean base (e.g. the sleep
  // cover, a full-screen swap from arbitrary prior content). Without this, the
  // X3 grayscale base takes its gentle differential happy path and the prior
  // home/reader frame ghosts through the soft aa_pre_bw_mid waveform. Forcing a
  // resync makes displayGrayscaleBase clear first, matching displayBuffer(HALF).
  // The reader's FAST path is deliberately left on the differential path so
  // per-page grayscale stays cheap.
  if (gpio.deviceIsX3() && fallback == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.displayGrayscaleBase(convertRefreshMode(fallback), turnOffScreen);
  LOG_DBG("EPD", "gray refresh mode=%d turnOff=%d (booster PON for paint; POF idle-hold after when turnOff=1)",
          static_cast<int>(fallback), turnOffScreen);
}

void HalDisplay::preconditionGrayscale() { einkDisplay.preconditionGrayscale(); }

void HalDisplay::preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  einkDisplay.preconditionGrayscale(x, y, w, h);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen) {
  einkDisplay.displayGrayBuffer(turnOffScreen);
  LOG_DBG("EPD", "gray refresh turnOff=%d (booster PON for paint; POF idle-hold after when turnOff=1)", turnOffScreen);
}

void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows) {
  einkDisplay.writeGrayscalePlaneStrip(lsbPlane ? EInkDisplay::GRAY_PLANE_LSB : EInkDisplay::GRAY_PLANE_MSB, rows,
                                       yStart, numRows);
}

bool HalDisplay::supportsStripGrayscale() const { return grayscaleCapabilities().stripUploads; }

bool HalDisplay::combinesGrayscaleBase() const { return grayscaleCapabilities().base == GrayscaleBase::Combined; }

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }

uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }
