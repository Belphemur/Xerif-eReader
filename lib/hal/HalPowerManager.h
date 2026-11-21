#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;

  mutable int _batteryCachedPercent = 0;         // Last read battery percentage (0-100)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect access to currentLockMode

  // Hysteresis for the low-power entry: millis() of the last request for
  // normal speed (render lock, user input). Low power re-engages only after
  // this dwell expires, so a burst of renders or a background build polling
  // with renders cannot flip-flop the CPU frequency between loop ticks
  // (measured: enter/restore pairs several times per second). Atomic:
  // pokeNormalSpeed() runs on the worker task, the governor loop reads/writes
  // it on the main task (review r5) — a torn 32-bit read is not possible on
  // this ABI, but atomicity also gives the load/store the right memory
  // ordering against the dwell decision.
  std::atomic<unsigned long> lastNormalMs{0};

 public:
#if BOARD_HAS_PSRAM
  // 80 MHz idle floor on PSRAM boards (ESP32-S3). The SDMMC host's register
  // interface and its ISR run off APB, which follows the CPU clock, so a floor
  // below 80 costs SD transfers: at 40 MHz the driver loses the completion
  // event for a multi-block transfer and cannot even recover with a status
  // query (sdmmc_read_sectors_dma ... returned 0x107).
  static constexpr int LOW_POWER_FREQ = 80;  // MHz
#else
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
#endif
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
  // Idle threshold at which the EPD booster is powered off (POF, image
  // retained — e-ink is bistable at zero power). 30 s: the wake is the PON
  // (~40 ms) that precedes every paint anyway, so there is no user-visible
  // latency to trade off, but the window stays below the 60 s Home/reader
  // header-clock repaint cadence so the panel spends most of each idle
  // minute powered off. Sits between IDLE_POWER_SAVING_MS (CPU clock) and
  // GT911_IDLE_SLEEP_MS (touch park) on the same idle trunk.
  static constexpr unsigned long EPD_IDLE_POF_MS = 30UL * 1000UL;  // 30 s
  // Sustained-idle threshold at which the GT911 touch controller is parked in
  // its datasheet Sleep mode (design 2026-09-24-gt911-idle-sleep.md §3 Tier
  // B). Rides the same lastActivityTime idle trunk as IDLE_POWER_SAVING_MS —
  // no separate timer. 0 touch input is accepted while parked (design
  // trade-off §3); any button press or preventAutoSleep() wakes the chip.
  static constexpr unsigned long GT911_IDLE_SLEEP_MS = 5UL * 60UL * 1000UL;  // 5 min
  // Minimum spacing between wake attempts when a wake failed (dead bus): each
  // call blocks on the poll task servicing the command, so an unresponsive
  // controller must not be retried every loop iteration (kody PR #171).
  static constexpr unsigned long GT911_WAKE_RETRY_MS = 1000UL;  // 1 s
  // Backoff between park attempts after a failed entry (uncertainty path): a
  // persistent failure costs one ~40 ms blocking attempt per window instead of
  // every idle loop iteration.
  static constexpr unsigned long GT911_PARK_RETRY_MS = 30UL * 1000UL;  // 30 s
  // Minimum dwell at full speed after the last normal-speed request before
  // low power may re-engage (see lastNormalMs).
  static constexpr unsigned long NORMAL_POWER_DWELL_MS = 2000;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 1500;        // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Refresh the full-speed dwell without holding the (single) NormalSpeed
  // lock: background build ticks call this per page so the governor cannot
  // throttle an active build, and re-engage low power the moment the ticks
  // stop. Any-task safe — one 32-bit store, same raciness as the rest of
  // the dwell bookkeeping.
  void pokeNormalSpeed() { lastNormalMs.store(millis(), std::memory_order_relaxed); }

  // Setup wake up GPIO and enter deep sleep. When autoPowerOffTimerUs is
  // non-zero an RTC timer is armed so the device wakes after that many
  // microseconds of dwell (auto power off).
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO& gpio, uint64_t autoPowerOffTimerUs = 0, bool wakeOnHeldButton = false) const;

  // wakeOnHeldButton: arm the wake without waiting for the power button's
  // release, so a press that is still down wakes the device straight back up
  // through the normal power-button wake path (upstream Picco USB-charge hold).

  // Final software power-off for auto power off: drives the master rail
  // latches LOW (held through sleep), disarms any RTC timer and re-enters
  // deep sleep waking only on the power button. Never returns.
  [[noreturn]] void enterPowerOffSleep(HalGPIO& gpio);

  // True when the battery is on external power / charging: the reader's
  // progress saver must not treat <5% as low battery in that state (design
  // decision log, 2026-09-10).
  bool isBatteryCharging() const;

#if FREEINK_DEVICE_PICCO
  // While USB powers the device, light-sleep instead of deep sleep (deep sleep
  // cannot observe the charger), calling onStatus whenever the charging state
  // or percentage changes. Returns true when the power button ended the hold,
  // false when USB went away or was never present.
  bool holdWhileUsbPowered(HalGPIO& gpio, void (*onStatus)(bool charging, uint16_t percent)) const;
#endif

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // Gauge-read health: a transiently failing Coulomb gauge must never surface
  // a frozen or 0% cache as a real low-battery signal (auto-sleep, OTA gate).
  // HEALTHY = last poll succeeded; STALE = repeated/long-failed reads, the
  // reported percentage is the last known-good value; UNSUPPORTED is reserved for
  // a board with no gauge but is currently unused — ADC boards report HEALTHY
  // (their reads cannot fail), so every supported board lands in HEALTHY or STALE.
  enum class BatteryHealthState : uint8_t { HEALTHY = 0, STALE = 1, UNSUPPORTED = 2 };
  BatteryHealthState getBatteryHealthState() const;
  bool isBatteryHealthStale() const;  // true only when state == STALE

  // One fresh reading. False when the battery cannot be read, which
  // getBatteryPercentage() reports as its last value or 0.
  bool readBatteryPercentage(uint16_t& percent) const;

  // HAL-routed stock-parity shutdown marker (the freeink::PowerManager calls
  // stay inside the HAL; main.cpp must not touch SDK classes directly). See
  // docs/design/shutdown-reason-marker.md.
  //
  //   ShutdownKind             which clean power off happened last time;
  //                            None = no marker (normal boot, crash, brown-out)
  //   takeLastShutdownKind()   read + clear the RTC marker once per boot
  //   stageAutoPowerOff()      record that THIS sleep ends in an automatic
  //                            power off: writes the RTC marker for the next
  //                            boot
  //   stageUserPowerOff()      same, for the manual power-button power off
  enum class ShutdownKind : uint8_t { None = 0, User = 1, AutoOff = 2 };
  static ShutdownKind takeLastShutdownKind();
  static void stageAutoPowerOff();
  static void stageUserPowerOff();
  // Clear any staged shutdown marker. Used to suppress a marker staged at
  // sleep entry when the wake was NOT the expected timer wake (e.g. user
  // interrupted auto-power-off dwell with a button press).
  static void clearShutdownMarker();

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
