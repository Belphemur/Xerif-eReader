#pragma once

#include <Arduino.h>
#include <InputManager.h>

// Display SPI pins (custom pins for XteinkX4, not hardware SPI defaults)
#define EPD_SCLK 8   // SPI Clock
#define EPD_MOSI 10  // SPI MOSI (Master Out Slave In)
#define EPD_CS 21    // Chip Select
#define EPD_DC 4     // Data/Command
#define EPD_RST 5    // Reset
#define EPD_BUSY 6   // Busy

#define SPI_MISO 7  // SPI MISO, shared between SD card and display (Master In Slave Out)

#define BAT_GPIO0 0  // Battery voltage

#define UART0_RXD 20  // Used for USB connection detection

// Xteink X3 Hardware
#define X3_I2C_SDA 20
#define X3_I2C_SCL 0
#define X3_I2C_FREQ 400000

// TI BQ27220 Fuel gauge I2C
#define I2C_ADDR_BQ27220 0x55  // Fuel gauge I2C address
#define BQ27220_SOC_REG 0x2C   // StateOfCharge() command code (%)
#define BQ27220_CUR_REG 0x0C   // Current() command code (signed mA)
#define BQ27220_VOLT_REG 0x08  // Voltage() command code (mV)

// Analog DS3231 RTC I2C
#define I2C_ADDR_DS3231 0x68  // RTC I2C address
#define DS3231_SEC_REG 0x00   // Seconds command code (BCD)

// QST QMI8658 IMU I2C
#define I2C_ADDR_QMI8658 0x6B        // IMU I2C address
#define I2C_ADDR_QMI8658_ALT 0x6A    // IMU I2C fallback address
#define QMI8658_WHO_AM_I_REG 0x00    // WHO_AM_I command code
#define QMI8658_WHO_AM_I_VALUE 0x05  // WHO_AM_I expected value

class HalGPIO {
#if CROSSPOINT_EMULATED == 0
  InputManager inputMgr;
#endif

  bool lastUsbConnected = false;
  bool usbStateChanged = false;
  uint8_t previousCapacitivePageButtons = 0;
  bool capacitivePagePressed = false;

 public:
  enum class DeviceType : uint8_t { X4, X3 };

 private:
  DeviceType _deviceType = DeviceType::X4;

 public:
  HalGPIO() = default;

  // Inline device type helpers for cleaner downstream checks
  inline bool deviceIsX3() const { return _deviceType == DeviceType::X3; }
  inline bool deviceIsX4() const { return _deviceType == DeviceType::X4; }
  bool isXteinkDevice() const;

  // True when the board's page buttons sit on the left/right screen edges
  // (X3, X4 Pro) rather than an off-screen vertical rocker. Drives side-hint
  // placement and the flipped large-step direction in selection activities.
  // Keyed off the active BoardConfig profile, not the X3/X4 runtime detection.
  bool hasEdgeSideButtons() const;

  // Start button GPIO and setup SPI for screen and SD card
  void begin();
  void prepareForDeepSleep();

  // Button input methods
  void update();
  // Consume the poll task's touch/home-key one-shots into the per-tick
  // snapshot (soak-fix7 pop pattern): the touch getters read the snapshot
  // until the next consume. Button edges are consumed per-read via
  // wasPressed/wasReleased. No-op on sync builds.
  void consumeTouchFrame();
  bool isPressed(uint8_t buttonIndex) const;
  // True while the SDK input poll task owns edge sampling (async mode): an
  // edge probe consumes one QUEUED edge per call, so the app layer drains a
  // button's queue in a loop to recover how many presses arrived (queued-input
  // aggregation, docs/design/2026-10-27-queued-input-aggregation.md §3.1).
  // False on sync builds, where an edge probe is idempotent within a frame
  // (the event bit lives until update()) and must be called exactly once.
  bool asyncInputActive() const;
  bool wasPressed(uint8_t buttonIndex) const;
  bool wasAnyPressed() const;
  bool wasReleased(uint8_t buttonIndex) const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  unsigned long getPowerButtonHeldTime() const;
  // True when any button contact is closed right now, read straight from the
  // hardware (ADC ladder off its idle rail, the power GPIO asserted, or the T5S3
  // expander key, one I2C read), without going through the debounced state.
  // Cheap enough to call every few ms.
  // On DigitalButtons boards (X4 Pro and friends) readButtonAdc() reports raw = -1,
  // so the nav keys are invisible to the ADC half; the T5S3 expander key and the
  // power pin are still covered. Use anyPhysicalButtonHeld() for "is some key down
  // at all".
  bool rawInputActive();
  // rawInputActive() OR the latched state of every physical button index, so a
  // held nav key counts on DigitalButtons boards too (design
  // 2026-09-24-gt911-idle-sleep.md §3 Tier B relies on side buttons staying
  // live while the GT911 is parked). Non-const because the rawInputActive() half
  // reads the ADC.
  bool anyPhysicalButtonHeld();
  // Drain and DISCARD the async poll task's queued button edges (press AND
  // release) without delivering them — the silent-boot boundary uses this in
  // place of the sync absorb pair, which is a no-op off the poll task. Level
  // state (isPressed) is untouched: a button still held at the boundary keeps
  // its held state on the poll task and its later release edge is queued
  // normally. No-op on sync builds (no queue exists).
  void discardQueuedButtonEdges();
  // GT911 Sleep-mode control (docs/design/2026-09-24-gt911-idle-sleep.md §3
  // Tier B). setTouchSleep(true) parks the controller (no scanning, ~70–120 µA
  // vs 8 mA active); wakeTouch() wakes it and returns only once the controller
  // ACKs again. While parked the capacitive home key is unreachable and touch
  // polling no-ops; physical side buttons are unaffected. No-op on boards
  // without a GT911 (gt911Addr == 0). Call sites gate on
  // SETTINGS.touchIdleSleep. Both may block up to ~250 ms — call only from
  // contexts that may stall (idle window, activity entry).
  bool setTouchSleep(bool asleep);
  bool wakeTouch();
  // True while the GT911 is parked in Sleep mode.
  bool isTouchAsleep() const;
  // True while a finger is in contact with the touch panel right now.
  bool isTouchContactActive() const;
  bool hasTouch() const;
  // Capacitive Home key reported by the touch controller (X4 Pro). The tap
  // event fires on release and excludes a long hold.
  bool hasHomeKey() const;
  // Two-position slide switch (Picco). Debounced position and its change edge.
  bool hasToggleSwitch() const;
  bool isToggleSwitchOn() const;
  bool wasHomeKeyPressed() const;
  bool wasHomeKeyTapped() const;
  bool wasHomeKeyLongPressed() const;
  bool wasTouchTap(float& nx, float& ny) const;
  bool wasTouchDown(float& nx, float& ny) const;
  bool wasCapacitivePagePressed() const { return capacitivePagePressed; }
  bool isCapacitivePagePressed(uint8_t buttonIndex) const {
    return buttonIndex < 8 && (previousCapacitivePageButtons & (1u << buttonIndex)) != 0;
  }
  // Raw release edge, reported even when the contact was not a tap (swipe end,
  // drag-off). Snapshot builders forward it so interaction routing can clear
  // pressed state.
  bool wasTouchReleased() const;
  bool isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const;
  bool isTouchHeldAt(float& nx, float& ny) const;
  // One-shot long-press, fired by the SDK classifier while the finger is still
  // down (stationary contact held past its threshold). Position = touch-down
  // point. Callers that act on it should suppressTouchContact() so the lift
  // cannot also tap.
  bool wasTouchLongPress(float& nx, float& ny) const;
  // Ignore the remainder of the current contact (its continued hold and its
  // release edge). Self-clears once the contact ends.
  void suppressTouchContact();
  unsigned long lastTouchHeldMs() const;
  bool wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const;
  bool wasTouchActivity() const;
  void setSharedConfirmPowerShortPressEmitsPower(bool enabled);

  // Verify that the physical power button remains held through input debounce.
  // Returns true if verification succeeded, false if device should return to sleep.
  // Should only be called when wakeup reason is PowerButton.
  bool verifyPowerButtonWakeup();

  // True when the last reset was a deep-sleep EXT1/GPIO wake (the hardware wake
  // source armed at sleep entry). Used to distinguish a genuine deep-sleep wake
  // from a cold boot that also reports WakeupReason::PowerButton.
  bool wokeFromDeepSleep() const;

  // Check if USB is connected
  bool isUsbConnected() const;

  // Whether a cold boot with no USB detected can be trusted to mean a held
  // power button (Xteink-style button-energized rail with reliable USB
  // detection). When false, cold boots always proceed to a normal boot.
  bool coldBootImpliesPowerButton() const;

  // Returns true once per edge (plug or unplug) since the last update()
  bool wasUsbStateChanged() const;

  enum class WakeupReason { PowerButton, AfterFlash, AfterUSBPower, Timer, Other };

  WakeupReason getWakeupReason() const;

  // Button indices
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
};

extern HalGPIO gpio;
