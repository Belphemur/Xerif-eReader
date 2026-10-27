#include <BatteryMonitor.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <PowerManager.h>
#include <Preferences.h>
#include <SPI.h>
#include <XteinkDetect.h>
#include <esp_sleep.h>

#if FREEINK_DEVICE_LILYGO
#include <BoardT5S3.h>
#endif

// Global HalGPIO instance
HalGPIO gpio;

namespace {
constexpr char HW_NAMESPACE[] = "cphw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";  // 0=auto, 1=x4, 2=x3
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det";    // 0=unknown, 1=x4, 2=x3

enum class NvsDeviceValue : uint8_t { Unknown = 0, X4 = 1, X3 = 2 };

NvsDeviceValue readNvsDeviceValue(const char* key, NvsDeviceValue defaultValue) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, true)) {
    return defaultValue;
  }
  const uint8_t raw = prefs.getUChar(key, static_cast<uint8_t>(defaultValue));
  prefs.end();
  if (raw > static_cast<uint8_t>(NvsDeviceValue::X3)) {
    return defaultValue;
  }
  return static_cast<NvsDeviceValue>(raw);
}

void writeNvsDeviceValue(const char* key, NvsDeviceValue value) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, false)) {
    return;
  }
  prefs.putUChar(key, static_cast<uint8_t>(value));
  prefs.end();
}

HalGPIO::DeviceType nvsToDeviceType(NvsDeviceValue value) {
  return value == NvsDeviceValue::X3 ? HalGPIO::DeviceType::X3 : HalGPIO::DeviceType::X4;
}

HalGPIO::DeviceType detectDeviceTypeWithFingerprint() {
  // Explicit override for recovery/support:
  // 0 = auto, 1 = force X4, 2 = force X3
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_DEV_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue == NvsDeviceValue::X3 || overrideValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Device override active: %s", overrideValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(overrideValue);
  }

  const NvsDeviceValue cachedValue = readNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::Unknown);
  if (cachedValue == NvsDeviceValue::X3 || cachedValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Using cached device type: %s", cachedValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(cachedValue);
  }

  // No cache yet: use FreeInk's canonical two-pass X3 fingerprint and persist
  // only confirmed results. Inconclusive probes deliberately remain uncached.
  uint8_t score1 = 0;
  uint8_t score2 = 0;
  const freeink::XteinkVerdict verdict = freeink::detectXteinkVerdict(&score1, &score2);
  LOG_INF("HW", "Xteink probe scores: pass1=%u pass2=%u verdict=%u", score1, score2, static_cast<unsigned>(verdict));

  if (verdict == freeink::XteinkVerdict::X3Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X3);
    return HalGPIO::DeviceType::X3;
  }

  if (verdict == freeink::XteinkVerdict::X4Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X4);
    return HalGPIO::DeviceType::X4;
  }

  // Conservative fallback for first boot with inconclusive probes.
  return HalGPIO::DeviceType::X4;
}

}  // namespace

void HalGPIO::begin() {
#if FREEINK_MCU_C3
  _deviceType = detectDeviceTypeWithFingerprint();
  BoardConfig::selectDevice(deviceIsX3() ? BoardConfig::Board::XteinkX3 : BoardConfig::Board::XteinkX4);

  // Resolve the per-batch controller before SPI owns the display pins. FreeInk
  // checks the OEM hw_calib/screenType value first, then falls back to its
  // two-pass display-bus probe. X3's facade keys panel selection off the sibling
  // board profile, so preserve a detected UC8279 through setDisplayX3().
  freeink::applyXteinkDisplayController();
  if (deviceIsX3() && BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8279) {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX3Uc8279);
  }

  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);

  if (deviceIsX4()) {
    pinMode(BAT_GPIO0, INPUT);
    pinMode(UART0_RXD, INPUT);
  }
#else
  _deviceType = DeviceType::X4;
#endif
  inputMgr.begin();
#if defined(XERIF_ASYNC_INPUT)
  // Async input sampling (docs/design/2026-10-26-x3-x4-async-input.md,
  // extending the 2026-09-20 design): a 10 ms poll task samples buttons
  // through e-ink flush windows and multi-second build/render windows so
  // presses are no longer dropped by a main-loop stall (issue #259).
  // XERIF_ASYNC_INPUT is defined for every env in [base] (platformio.ini);
  // a board opts out only on later evidence via -UXERIF_ASYNC_INPUT in its
  // env. Runtime cost is ~6.5 KB DRAM (4 KB task stack + queues + TCB);
  // safe degradation: if xTaskCreate() fails _asyncTask stays null,
  // asyncActive() reads false, and every consumer's !asyncActive() branch
  // keeps the historical polled behavior (SDK InputManager.cpp:324).
  inputMgr.beginAsync(/*taskPriority=*/2, /*pollMs=*/10, /*queueLen=*/16);
#endif
#if FREEINK_DEVICE_LILYGO
  // T5S3 IO48 key sits behind the PCA9535 expander. Mirrors BoardT5S3::begin(), which also
  // re-inits I2C/SPI and is unsafe to call here. inputMgr.begin() has started Wire.
  if (BoardConfig::ACTIVE.board == BoardConfig::Board::LilyGoT5S3) {
    BoardT5S3::setPca9535PinMode(PCA9535_IO12_BUTTON, INPUT);
    InputManager::setButtonHook(
        []() -> uint8_t { return BoardT5S3::readButton() ? static_cast<uint8_t>(1U << InputManager::BTN_DOWN) : 0; });
  }
#endif
}

void HalGPIO::update() {
  inputMgr.update();
  const uint8_t pageButtons = inputMgr.capacitivePageButtonMask();
  capacitivePagePressed = (pageButtons & ~previousCapacitivePageButtons) != 0;
  previousCapacitivePageButtons = pageButtons;
  const bool connected = isUsbConnected();
  usbStateChanged = (connected != lastUsbConnected);
  lastUsbConnected = connected;
}

// Consume the poll task's touch/home-key one-shots into the per-tick
// snapshot (soak-fix7 pop pattern). No-op on sync builds.
void HalGPIO::consumeTouchFrame() {
#if CROSSPOINT_EMULATED == 0
  inputMgr.consumeTouchFrame();
#else
  (void)0;
#endif
}

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

void HalGPIO::prepareForDeepSleep() {
  if (!inputMgr.prepareForDeepSleep()) LOG_ERR("GPIO", "Failed to put touch controller to sleep");
}

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return inputMgr.wasPressed(buttonIndex); }

bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed(); }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return inputMgr.wasReleased(buttonIndex); }

bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased(); }

bool HalGPIO::rawInputActive() {
  if (inputMgr.isPowerButtonPhysicallyPressed()) return true;
#if FREEINK_DEVICE_LILYGO
  if (BoardConfig::ACTIVE.board == BoardConfig::Board::LilyGoT5S3 && BoardT5S3::readButton()) return true;
#endif
  InputManager::ButtonAdcSample g1{}, g2{};
  inputMgr.readButtonAdc(g1, g2);
  // The Xteink ladder idles at the ADC full-scale rail (~4095); every button band sits below 3900.
  constexpr int kIdleRailMin = 4000;
  if ((g1.raw >= 0 && g1.raw < kIdleRailMin) || (g2.raw >= 0 && g2.raw < kIdleRailMin)) return true;
  // OnePageAdcLadder: the 4 front keys ride the GPIO4 ladder, but readButtonAdc()
  // is Xteink-only (InputManager.cpp early-return), so sample it here or front-key
  // taps stay invisible to the idle slice (kody PR #171). getState() bands top out
  // at 2800 mV; the unpressed rail sits at full scale.
  if (BoardConfig::ACTIVE.inputStyle == BoardConfig::InputStyle::OnePageAdcLadder &&
      BoardConfig::ACTIVE.input.adcLadderPin >= 0) {
    const int mv = analogReadMilliVolts(BoardConfig::ACTIVE.input.adcLadderPin);
    constexpr int kOnePageIdleMinMv = 2900;
    if (mv >= 0 && mv < kOnePageIdleMinMv) return true;
  }
  return false;
}

bool HalGPIO::anyPhysicalButtonHeld() {
  // Fresh hardware read FIRST: on sync builds (beginAsync compiled out, no poll
  // task) nothing commits currentState while the loop sits inside the 50 ms
  // idle slice, so the latch alone is stale for the whole slice and a short tap
  // would be dropped. The ADC ladder and the power pin are readable live; the
  // latch is what adds the digital-only nav-key banks the ADC path cannot see.
  if (rawInputActive()) return true;
  for (uint8_t i = 0; i <= InputManager::BTN_POWER; ++i) {
    if (inputMgr.isPressed(i)) return true;
  }
  // Sticky-style boards route the nav keys to plain GPIOs (INPUT_PULLUP) that
  // rawInputActive()'s ADC read cannot see. Sample them live for every input
  // style EXCEPT the pure ADC-ladder profiles: XteinkAdcLadder (X4/X3) wires
  // input.up/down to display DC/RST output pins, so reading them spuriously
  // breaks the idle slice (Kody PR #171 K2 on 90a84566). OnePageAdcLadder is
  // NOT excluded — despite the name it still has GPIO side nav keys per its
  // BoardConfig doc (InputStyle::OnePageAdcLadder, line 425), so it must be
  // sampled too; the exclusion is precise to XteinkAdcLadder only.
  if (BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::XteinkAdcLadder) {
    const BoardConfig::InputPins& pins = BoardConfig::ACTIVE.input;
    if (pins.up >= 0 && digitalRead(pins.up) == LOW) return true;
    if (pins.down >= 0 && digitalRead(pins.down) == LOW) return true;
  }
  return false;
}

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

bool HalGPIO::setTouchSleep(const bool asleep) { return inputMgr.setTouchSleep(asleep); }

bool HalGPIO::wakeTouch() { return inputMgr.wakeTouch(); }

bool HalGPIO::isTouchAsleep() const { return inputMgr.isTouchAsleep(); }

unsigned long HalGPIO::getPowerButtonHeldTime() const { return inputMgr.getPowerButtonHeldTime(); }

bool HalGPIO::hasTouch() const { return inputMgr.hasTouch(); }

bool HalGPIO::hasHomeKey() const { return BoardConfig::hasHomeKey(); }

bool HalGPIO::wasHomeKeyPressed() const { return inputMgr.wasHomeKeyPressed(); }

bool HalGPIO::wasHomeKeyTapped() const { return inputMgr.wasHomeKeyTapped(); }

bool HalGPIO::hasToggleSwitch() const { return inputMgr.hasToggleSwitch(); }

bool HalGPIO::isToggleSwitchOn() const { return inputMgr.isToggleSwitchOn(); }

bool HalGPIO::wasHomeKeyLongPressed() const { return inputMgr.wasHomeKeyLongPressed(); }

bool HalGPIO::wasTouchTap(float& nx, float& ny) const { return inputMgr.wasTouchTap(nx, ny); }

bool HalGPIO::wasTouchDown(float& nx, float& ny) const { return inputMgr.wasTouchPressedAt(nx, ny); }

bool HalGPIO::wasTouchReleased() const { return inputMgr.wasTouchReleased(); }

bool HalGPIO::isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const {
  return inputMgr.isTouchTapCandidate(nx, ny, heldMs);
}

bool HalGPIO::isTouchHeldAt(float& nx, float& ny) const { return inputMgr.isTouchHeldAt(nx, ny); }

bool HalGPIO::isTouchContactActive() const { return inputMgr.isTouchPressed(); }

bool HalGPIO::wasTouchLongPress(float& nx, float& ny) const { return inputMgr.wasTouchLongPress(nx, ny); }

void HalGPIO::suppressTouchContact() { inputMgr.suppressTouchContact(); }

unsigned long HalGPIO::lastTouchHeldMs() const { return inputMgr.lastTouchHeldMs(); }

bool HalGPIO::wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const {
  return inputMgr.wasSwipe(nxStart, nyStart, nxEnd, nyEnd);
}

bool HalGPIO::wasTouchActivity() const { return inputMgr.wasTouchActivity(); }

void HalGPIO::setSharedConfirmPowerShortPressEmitsPower(const bool enabled) {
  InputManager::setSharedConfirmPowerShortPressEmitsPower(enabled);
}

bool HalGPIO::hasEdgeSideButtons() const {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4Pro ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4Classic;
}

bool HalGPIO::isXteinkDevice() const {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4;
}

bool HalGPIO::verifyPowerButtonWakeup() {
  // M5Paper v1.1: the classic ESP32's reset-to-setup() latency exceeds a normal
  // wheel click, so a click wake is always released before this samples and
  // verification would re-sleep on every wake. Its wheel has hard external
  // pull-ups, so the ghost-wake debounce this implements is not needed.
  if (BoardConfig::isPaperMono() || BoardConfig::isM5PaperV11() || BoardConfig::ACTIVE.input.power < 0) {
    return true;
  }

  constexpr unsigned long POWER_WAKE_STABILITY_MS = 10;
  const bool heldAtFirstSample = inputMgr.isPowerButtonPhysicallyPressed();
  const unsigned long sampleStart = millis();
  // Async mode: the poll task keeps the edge state alive at pollMs cadence
  // (and update() is a no-op off the poll task), so this loop needs no
  // input work at all — popping here would consume edges the real
  // consumer has not read.
  const bool needsOwnSampling = !inputMgr.asyncActive();
  if (needsOwnSampling) inputMgr.update();
  while (millis() - sampleStart < POWER_WAKE_STABILITY_MS || inputMgr.isDebouncePending()) {
    delay(1);
    if (needsOwnSampling) inputMgr.update();
  }
  return heldAtFirstSample && inputMgr.isPowerButtonPhysicallyPressed();
}

bool HalGPIO::wokeFromDeepSleep() const {
  const auto resetReason = esp_reset_reason();
  return resetReason == ESP_RST_DEEPSLEEP;
}

bool HalGPIO::isUsbConnected() const {
  if (!deviceIsX3() && BoardConfig::ACTIVE.usbDetect >= 0) {
    return digitalRead(BoardConfig::ACTIVE.usbDetect) == HIGH;
  }
  // X3 uses GPIO20 for I2C, not USB detection. Boards without a digital
  // USB-detect line (e.g. Sticky, whose PWR_IN_VOLT is an analog
  // divider): infer external power from charging state instead. BatteryMonitor
  // picks the board's best source — charger IC status, gauge Current() sign, or
  // a /STAT pin — and reports false on boards with no battery telemetry at all.
  // A charger IC that reports its input rail (BQ25896, SGM41562, M5 PMIC)
  // answers directly; otherwise infer from charging state, where charge
  // termination at 100% reads as "not connected".
  static const BatteryMonitor battery;
  bool known = false;
  const bool present = battery.isExternalPowerPresent(&known);
  if (known) return present;
  return battery.isCharging();
}

bool HalGPIO::coldBootImpliesPowerButton() const {
  // Xteink-style power topology: the power button energizes the rail until
  // firmware latches it, so a no-USB POWERON can only be a still-held button
  // boot, and plugging USB into an off device should charge-sleep, not boot.
  // Everything else boots on any cold boot: boards with no USB detection at
  // all (M5Paper v1.1, PaperColor, Murphy, de-link) would misread USB and
  // post-flash boots as battery button boots, and STAT-only boards like the
  // EEGO A4 misread them the same way once the charger terminates at 100%
  // (STAT inactive reads as "no USB").
  // The Metalio sleeps powered off and only its button turns it back on.
  return isXteinkDevice() || BoardConfig::isPaperMono() || BoardConfig::isSticky() || BoardConfig::isMetalioEInk4();
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  const bool usbConnected = isUsbConnected();

  if (resetReason == ESP_RST_DEEPSLEEP &&
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO || wakeupCause == ESP_SLEEP_WAKEUP_EXT1)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected &&
      coldBootImpliesPowerButton()) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnected) {
    return WakeupReason::AfterFlash;
  }
  // Auto power off: the dwell RTC timer elapsed while asleep (before Other, so
  // the main.cpp intercept can branch on the HAL API instead of raw IDF calls).
  if (resetReason == ESP_RST_DEEPSLEEP && wakeupCause == ESP_SLEEP_WAKEUP_TIMER) {
    return WakeupReason::Timer;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected) {
    return WakeupReason::AfterUSBPower;
  }
  return WakeupReason::Other;
}
