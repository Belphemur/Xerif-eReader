#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <InputManager.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include <cassert>

#include "HalFrontlight.h"
#include "HalGPIO.h"

#if FREEINK_DEVICE_EEGO_A4 || FREEINK_DEVICE_PICCO
#include <HalFrontlight.h>
#endif

#if FREEINK_DEVICE_PICCO
#include <driver/gpio.h>
#endif

#if FREEINK_DEVICE_PAPERMONO
#include <M5Pm1.h>
#endif

HalPowerManager powerManager;  // Singleton instance

// RTC slow memory survives deep sleep and firmware updates. Bump this whenever
// the layout/meaning of any RTC_DATA field below changes, so a stale record
// from an older build is discarded on boot (see begin()).
static constexpr uint8_t RTC_DATA_VERSION = 1;
static RTC_DATA_ATTR uint8_t _rtcDataVersion = 0;

// Battery-gauge health tracking, persisted in RTC slow memory: the last
// known-good percentage and read state survive deep sleep, so a gauge that
// starts failing across a sleep cycle reports the pre-sleep good value instead
// of a frozen or 0% cache.
static RTC_DATA_ATTR uint16_t _batteryHealthLastKnownPct = 0;
static RTC_DATA_ATTR uint32_t _batteryHealthLastValidMs = 0;  // millis() of last successful gauge read
static RTC_DATA_ATTR uint8_t _batteryHealthFails = 0;         // consecutive failed gauge reads
static RTC_DATA_ATTR bool _batteryHealthKnownGood = false;    // a real gauge sample has ever succeeded
static RTC_DATA_ATTR uint8_t _batteryHealthState = 0;         // HalPowerManager::BatteryHealthState
// Gauge counts as STALE after this many consecutive failed reads...
static constexpr uint8_t BATTERY_HEALTH_MAX_FAILS = 3;
// ...or when no successful read happened for this long (ms).
static constexpr unsigned long BATTERY_HEALTH_STALE_MS = 60000;

// Stock-parity shutdown marker code ((reason<<8)|1, 0 = none) read at boot from
// the freeink::PowerManager RTC marker. Persisted so takeLastShutdownKind()
// can report it after the source cells are consumed.
static RTC_DATA_ATTR uint16_t _lastShutdownReasonCode = 0;

// GPIO13 controls the X4 battery latch and the X3 SD power rail on the C3
// Xteink boards. Other boards use it for unrelated signals, including the
// X4 Pro display chip select.
static constexpr gpio_num_t XTEINK_C3_GPIO13 = GPIO_NUM_13;

void HalPowerManager::begin() {
  if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
  // Board charger setup (Picco SGM41562: stock charge voltage and limits); a
  // no-op on boards without a configurable charger.
  if (BatteryMonitor::configureCharger()) LOG_INF("PWR", "Charger configured");
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);

  // RTC slow memory survives firmware updates, so a record written by an older
  // build (different layout/meaning) must be discarded before we trust it.
  if (_rtcDataVersion != RTC_DATA_VERSION) {
    _batteryHealthLastKnownPct = 0;
    _batteryHealthLastValidMs = 0;
    _batteryHealthFails = 0;
    _batteryHealthKnownGood = false;
    _batteryHealthState = 0;
    _lastShutdownReasonCode = 0;
    _rtcDataVersion = RTC_DATA_VERSION;
  }
  // Re-base the health timestamp onto this boot's millis() so the stale
  // timeout math (now - _batteryHealthLastValidMs) can't wrap after a deep-sleep
  // reboot. The cached known-good percentage is preserved across the rebase.
  if (_batteryHealthKnownGood) {
    _batteryHealthLastValidMs = millis();
  }
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // Held across the check and the switch: a Lock taken in between would find full speed, do
  // nothing, and then run at LOW_POWER_FREQ until the next key press.
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  const LockMode mode = currentLockMode;

  // Any request for normal speed (render lock, user input, active build)
  // refreshes the dwell window. Without this, the input-idle low-power entry
  // fights every render: each render's Lock restores full speed and the next
  // idle tick drops back down — measured several enter/restore pairs per
  // second while a background build polled with renders.
  if (!enabled || mode != None) {
    lastNormalMs = millis();
  }

  if (mode == None && enabled && !isLowPower) {
    if (millis() - lastNormalMs < NORMAL_POWER_DWELL_MS) {
      xSemaphoreGive(modeMutex);
      return;  // recent full-speed work: stay at normal frequency
    }
    const int prev = getCpuFrequencyMhz();
    LOG_INF("PWR", "CPU freq %d->%d MHz [idle-enter]", prev, LOW_POWER_FREQ);
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_ERR("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      xSemaphoreGive(modeMutex);
      return;
    }
    InputManager::setLowPowerPolling(true);
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    const int prev = getCpuFrequencyMhz();
    LOG_INF("PWR", "CPU freq %d->%d MHz [normal-restore]", prev, normalFreq);
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_ERR("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      xSemaphoreGive(modeMutex);
      return;
    }
    InputManager::setLowPowerPolling(false);
    isLowPower = false;
  }

  // Otherwise, no change needed
  xSemaphoreGive(modeMutex);
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio, const uint64_t autoPowerOffTimerUs,
                                     const bool wakeOnHeldButton) const {
#if FREEINK_DEVICE_EEGO_A4 || FREEINK_DEVICE_PICCO
  // The frontlight driver and touch share I2C; turn the light off before touch
  // releases the bus. On Picco this also puts the TMA525C to sleep.
  Frontlight.setOn(false);
  gpio.prepareForDeepSleep();
#else
  // GT911 gesture-park deep-sleep boundary (design
  // 2026-11-16-gt911-gesture-touch-wake.md §2.3): drop the parked-touch INT
  // edge capture before the rail cut below — the INT pad floats while its
  // rail drops, and an ISR armed on it is a spurious wake source across deep
  // sleep. No-op without a GT911; the rail-down itself depowers the
  // controller.
  if (BoardConfig::ACTIVE.touch.controller == BoardConfig::TouchController::Gt911) {
    gpio.prepareForDeepSleep();
  }
#endif
#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

#if !SOC_PM_SUPPORT_EXT1_WAKEUP
  if (gpio.isXteinkDevice()) {
    // GPIO13 gates the battery MOSFET on both Xteink C3 boards; driving it low
    // is the battery power-off (the SDK wake source still handles USB power).
    // Release any surviving pad hold first: hold_en survives deep sleep via
    // the SDK's deepSleep() (esp_sleep_config_gpio_isolate +
    // gpio_deep_sleep_hold_en), and a held pad silently ignores the drive.
    gpio_hold_dis(XTEINK_C3_GPIO13);
    gpio_set_direction(XTEINK_C3_GPIO13, GPIO_MODE_OUTPUT);
    gpio_set_level(XTEINK_C3_GPIO13, 0);
    gpio_hold_en(XTEINK_C3_GPIO13);
  }
#endif

  // Hold every configured power-latch pin HIGH through deep sleep. These are
  // keep-alive enables (the X4 Pro's master peripheral rail on GPIO1, the
  // Sticky's PWR_HOLD/PWR_LOCK): deepSleep() isolates all pads
  // (esp_sleep_config_gpio_isolate), so a latch without an armed hold loses its
  // output driver and floats — on the X4 Pro the latch drops as soon as
  // external power leaves (serial/pogo adapter unplugged), and the next power-
  // button press cold-boots instead of fast-wakes. holdPowerRails() asserted
  // the latches at boot but arms no sleep hold; arm it here instead. Skips
  // XTEINK_C3_GPIO13: it IS power.latch0 on the C3 Xteink boards, where the
  // block above drives it LOW on purpose (battery power-off).
  for (const int8_t pin : {BoardConfig::ACTIVE.power.latch0, BoardConfig::ACTIVE.power.latch1}) {
    if (pin < 0 || static_cast<gpio_num_t>(pin) == XTEINK_C3_GPIO13) continue;
    const auto g = static_cast<gpio_num_t>(pin);
    // Release any surviving pad hold first: a held pad silently ignores the drive below.
    gpio_hold_dis(g);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
    gpio_hold_en(g);
  }

  // Cut the gated peripheral rails (touch/SD/EPD on boards like the Sticky) and
  // hold the enables off through deep sleep — otherwise the GT911 and SD card
  // stay powered all through "off" and drain the battery. No-op on boards with
  // no switched rails (X4/X3). Trade-off: no touch-to-wake; wake is the power
  // button. Must run after display.deepSleep() so the panel controller gets its
  // deep-sleep command while its rail is still up (enterDeepSleep() in main.cpp
  // guarantees that ordering).
  freeink::PowerManager::powerDownRailsForSleep();

  // Park the frontlight pads so they don't leak current through deep sleep.
  // On the X4 Pro the master rail is held up (PR #3215 keeps power.latch0 / GPIO1
  // HIGH for fast-wake) and the frontlight LEDs use LEDC_SLEEP_MODE_KEEP_ALIVE,
  // so without this the frontlight driver keeps drawing quiescent current. park()
  // drives GPIO8/9 LOW and holds them, and releases the LEDC KEEP_ALIVE clock;
  // releaseOnWake() (called at boot in HalFrontlight::begin) undoes the hold so
  // the LEDC channels re-attach cleanly. Guarded by FREEINK_FRONTLIGHT_LS so it is
  // a no-op on boards without a frontlight (e.g. papermono).
#if FREEINK_FRONTLIGHT_LS
  Frontlight.park();
#endif

#if FREEINK_DEVICE_PAPERMONO
  // Its power button is behind the M5PM1 PMIC rather than an ESP GPIO, so
  // normal GPIO deep sleep would have no wake source. Ask the PMIC to shut the
  // device down; a button click then restarts it through a cold boot.
  if (freeink::m5pm1::requestShutdown()) {
    delay(1000);  // allow the PMIC firmware time to drop power
  }
#endif
#if FREEINK_DEVICE_METALIO_EINK4
  // Deep sleep leaves the board, 4G modem and audio module powered, so cut
  // power instead; the button cold-boots it. Returns only on USB power.
  freeink::metalio::powerOff();
#endif

  if (wakeOnHeldButton) {
    freeink::PowerManager::armPowerButtonWakeup();
    freeink::PowerManager::deepSleep();
  }
  // Waits for the power button to be physically released (so holding it doesn't
  // immediately wake the device again), then arms the wake source and sleeps.
  LOG_INF("PWR", "CPU freq %d->0 MHz [deep-sleep]", getCpuFrequencyMhz());
  LOG_DBG("PWR", "Entering deep sleep");

  // Auto power off: arm the dwell timer so the device wakes (and shuts down)
  // if it is left in deep sleep this long.
  if (autoPowerOffTimerUs > 0) {
    esp_sleep_enable_timer_wakeup(autoPowerOffTimerUs);
  }
  freeink::PowerManager::deepSleepUntilPowerButton();
}

// Final software power-off for auto power off: drop the master rail latches
// LOW so everything but the wake logic loses power, then deep sleep until the
// power button is pressed (next press = normal cold boot). Assumes the
// shutdown screen has already been rendered.
[[noreturn]] void HalPowerManager::enterPowerOffSleep(HalGPIO& gpio) {
#ifdef ENABLE_SERIAL_LOG
  logSerial.end();
#endif

#if !SOC_PM_SUPPORT_EXT1_WAKEUP
  if (gpio.isXteinkDevice()) {
    // C3 Xteink boards: GPIO13 IS power.latch0 and gates the battery MOSFET —
    // driving it LOW is the battery power-off. Without this, a manual power-off
    // on the C3 X4/X3 would leave the battery MOSFET enabled (CodeRabbit finding).
    // Mirrors the block in startDeepSleep().
    gpio_hold_dis(XTEINK_C3_GPIO13);
    gpio_set_direction(XTEINK_C3_GPIO13, GPIO_MODE_OUTPUT);
    gpio_set_level(XTEINK_C3_GPIO13, 0);
    gpio_hold_en(XTEINK_C3_GPIO13);
  }
#endif

  // Cut the gated peripheral rails and park the frontlight pads while the
  // master rail is still up (same ordering as startDeepSleep()).
  // GT911 gesture-park boundary: disarm the parked-touch INT edge capture
  // first (design 2026-11-16-gt911-gesture-touch-wake.md §2.3) — same rule as
  // startDeepSleep(), the pad must not carry an armed ISR across the cut.
  if (BoardConfig::ACTIVE.touch.controller == BoardConfig::TouchController::Gt911) {
    gpio.prepareForDeepSleep();
  }
  freeink::PowerManager::powerDownRailsForSleep();
#if FREEINK_FRONTLIGHT_LS
  Frontlight.park();
#endif

  // Drive the keep-alive latches LOW and hold them through sleep. This is the
  // power-off itself, not a keep-alive: deepSleep() runs
  // esp_sleep_config_gpio_isolate(), which strips every pad WITHOUT an armed
  // hold — a merely-driven (unheld) latch loses its output driver and FLOATS,
  // and a floating rail enable on X4 Pro does not reliably stay LOW (it drops
  // only when external power leaves; on USB/pogo it can drift back up).
  // hold_en pins the OFF level through the isolation, so the master rail is
  // deterministically dead for the whole sleep.
  // Stock-parity note (ghidra_poweroff_report.md, FINAL CONCLUSION): stock's
  // power-off is a deep-sleep transaction — wake-config arm (16-byte 0x101
  // CRC32'd record, mask 0x0101010101010101), "SRCX" marker + reason byte to
  // RTC slow RAM (0x50000004/0x50000000), ownership quiesce poll, then commit
  // into the IDF sleep core. Stock holds NO rail (it lets the master rail
  // collapse), our LOW+hold is the deterministic variant of the same end
  // state. The portable stock delta for this sink: write the same RTC RAM
  // marker (magic 0x58435253 + reason byte) before sleeping, and read/clear
  // it at boot for shutdown-reason reporting. Skips XTEINK_C3_GPIO13 —
  // it IS power.latch0 on the C3 Xteink boards, where driving it low is the
  // battery power-off and must not be clobbered here.
  for (const int8_t pin : {BoardConfig::ACTIVE.power.latch0, BoardConfig::ACTIVE.power.latch1}) {
    if (pin < 0 || static_cast<gpio_num_t>(pin) == XTEINK_C3_GPIO13) continue;
    const auto g = static_cast<gpio_num_t>(pin);
    // Release any surviving pad hold first: a held pad silently ignores the drive.
    gpio_hold_dis(g);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    gpio_hold_en(g);
  }

  // The RTC timer that woke us has served its purpose; make sure it cannot
  // wake this power-off sleep.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

  freeink::PowerManager::deepSleepUntilPowerButton();
}

HalPowerManager::ShutdownKind HalPowerManager::takeLastShutdownKind() {
  // Always overwrite the staged code, including with 0: the marker cells are
  // consume-once, so a boot without a marker must not leak the previous
  // session's code into its own sleep-trace row.
  const uint16_t code = freeink::PowerManager::takeShutdownReason();
  _lastShutdownReasonCode = code;
  if (code == 0) {
    return ShutdownKind::None;
  }
  const auto reason = static_cast<uint8_t>(code >> 8);
  if (reason == static_cast<uint8_t>(freeink::PowerManager::ShutdownReason::ShutdownAutoOff)) {
    return ShutdownKind::AutoOff;
  }
  if (reason == static_cast<uint8_t>(freeink::PowerManager::ShutdownReason::ShutdownUser)) {
    return ShutdownKind::User;
  }
  return ShutdownKind::None;
}

static void stageShutdownMarkerImpl(freeink::PowerManager::ShutdownReason reason) {
  // Write the RTC marker for the NEXT boot (magic last = torn-write safe).
  freeink::PowerManager::setShutdownReason(reason);
  _lastShutdownReasonCode = static_cast<uint16_t>(static_cast<uint16_t>(reason) << 8) | 1;
}

void HalPowerManager::stageAutoPowerOff() {
  stageShutdownMarkerImpl(freeink::PowerManager::ShutdownReason::ShutdownAutoOff);
}

void HalPowerManager::stageUserPowerOff() {
  stageShutdownMarkerImpl(freeink::PowerManager::ShutdownReason::ShutdownUser);
}

void HalPowerManager::clearShutdownMarker() {
  // Consume + drop any marker staged for the current boot. The freeink SDK
  // cells are consume-once, so takeShutdownReason() is the read-clear path.
  // We discard the result: the caller already knows what wake it got and is
  // suppressing the auto-off marker staged at sleep entry.
  (void)freeink::PowerManager::takeShutdownReason();
  _lastShutdownReasonCode = 0;
}

bool HalPowerManager::isBatteryCharging() const {
  static const BatteryMonitor battery;
  return battery.isCharging();
}

#if FREEINK_DEVICE_PICCO
// ponytail: 30 s poll; the SGM41562 status is I2C-only, so USB unplug and
// charge progress are only seen when the timer fires.
static constexpr uint64_t USB_HOLD_POLL_US = 30ULL * 1000 * 1000;

bool HalPowerManager::holdWhileUsbPowered(HalGPIO& gpio, void (*onStatus)(bool charging, uint16_t percent)) const {
  if (!gpio.isUsbConnected()) return false;
  LOG_DBG("PWR", "USB power present, holding in light sleep");
  const auto powerPin = static_cast<gpio_num_t>(BoardConfig::ACTIVE.input.power);
  // The press that requested sleep may still be down; it must not end the hold.
  while (digitalRead(powerPin) == LOW) delay(10);  // Picco power button is active-low
  const BatteryMonitor battery;
  int lastCharging = -1;
  int lastPercent = -1;
  while (gpio.isUsbConnected()) {
    const bool charging = battery.isCharging();
    const uint16_t percent = getBatteryPercentage();
    if (static_cast<int>(charging) != lastCharging || percent != lastPercent) {
      lastCharging = charging;
      lastPercent = percent;
      onStatus(charging, percent);
    }
    esp_sleep_enable_timer_wakeup(USB_HOLD_POLL_US);
    gpio_wakeup_enable(powerPin, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_light_sleep_start();
    gpio_wakeup_disable(powerPin);
    // gpio_wakeup_disable() keeps the LOW_LEVEL type, and a later pinMode() would
    // enable it as an interrupt that storms while the button is held.
    gpio_set_intr_type(powerPin, GPIO_INTR_DISABLE);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (digitalRead(powerPin) == LOW) return true;
  }
  return false;
}
#endif

uint16_t HalPowerManager::getBatteryPercentage() const {
  static const BatteryMonitor battery;
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    const unsigned long now = millis();
    // _batteryHealthLastValidMs is persisted across deep sleep but millis() restarts
    // at ~0 every boot, so a stale (large) value from before sleep would make
    // (now - _batteryHealthLastValidMs) wrap and wrongly trip STALE. Re-base it onto
    // this boot's clock the first time we see it ahead of now — the known-good
    // percentage is preserved.
    if (_batteryHealthKnownGood && _batteryHealthLastValidMs != 0 && now < _batteryHealthLastValidMs) {
      _batteryHealthLastValidMs = now;
    }
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      // Fresh cache: health was decided on the last real poll, leave it untouched.
      return _batteryCachedPercent;
    }

    _batteryLastPollMs = now;
    uint16_t percent = 0;
    if (!battery.readPercentageChecked(percent)) {
      // Failed gauge read. Until a valid sample has ever succeeded we have no
      // known-good value to fall back to, so report 0 only as an explicit "unknown"
      // (never treat it as a real low battery). Once we have a sample, fall back to
      // the last known-good value so a broken gauge never shows a frozen/0% level.
      if (!_batteryHealthKnownGood) {
        _batteryHealthFails++;
        return 0;
      }
      _batteryHealthFails++;
      if (_batteryHealthFails >= BATTERY_HEALTH_MAX_FAILS ||
          now - _batteryHealthLastValidMs > BATTERY_HEALTH_STALE_MS) {
        if (_batteryHealthState != static_cast<uint8_t>(BatteryHealthState::STALE)) {
          _batteryHealthState = static_cast<uint8_t>(BatteryHealthState::STALE);
          LOG_DBG("PWR", "battery health STALE (fails=%u)", _batteryHealthFails);
        }
        return _batteryHealthLastKnownPct;
      }
      return _batteryCachedPercent;
    }
    _batteryCachedPercent = percent;
    _batteryHealthLastKnownPct = percent;
    _batteryHealthLastValidMs = now;
    _batteryHealthFails = 0;
    _batteryHealthKnownGood = true;
    _batteryHealthState = static_cast<uint8_t>(BatteryHealthState::HEALTHY);
    return _batteryCachedPercent;
  }

  // ADC boards have no gauge to fail: every read succeeds.
  _batteryHealthState = static_cast<uint8_t>(BatteryHealthState::HEALTHY);

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

HalPowerManager::BatteryHealthState HalPowerManager::getBatteryHealthState() const {
  return static_cast<BatteryHealthState>(_batteryHealthState);
}

bool HalPowerManager::isBatteryHealthStale() const {
  return _batteryHealthState == static_cast<uint8_t>(BatteryHealthState::STALE);
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
