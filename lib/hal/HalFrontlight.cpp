#include "HalFrontlight.h"

#include <Logging.h>

HalFrontlight HalFrontlight::instance;

void HalFrontlight::begin(const uint8_t brightness, const uint8_t warmth, const bool on) {
#ifdef FREEINK_FRONTLIGHT_LS
  // Drop any deep-sleep pad hold left by FrontlightManager::park() on the
  // previous cycle so the LEDC channels re-attach cleanly (a held pad makes the
  // driver's drive a no-op). No-op when the device wasn't parked.
  manager.releaseOnWake();
#endif
  // begin() runs the hardware probe (EEGO A4: an I2C ACK from the LM3630A at
  // 0x36) that decides present(), so it must come FIRST. Guarding on present()
  // before begin() would skip the probe forever and hide the light on units
  // that actually have one. begin() is inert on boards without a frontlight.
  manager.begin();
  if (!manager.present()) {
    LOG_INF("LIGHT", "begin: frontlight not present, skipping");
    return;
  }
  lastBrightness = brightness > 100 ? 100 : brightness;
  lit = on;
  // Brightness first: warmth applied at brightness 0 is an off write, which
  // puts the Picco LED driver in standby right before the on write.
  manager.setBrightness(lit ? lastBrightness : 0);
  manager.setColorTemperature(warmth > 100 ? 100 : warmth);
  LOG_INF("LIGHT", "Frontlight up: %u%% warm=%u%% %s", lastBrightness, manager.colorTemperature(), lit ? "on" : "off");
}

void HalFrontlight::setBrightness(const uint8_t percent) {
  lastBrightness = percent > 100 ? 100 : percent;
  if (lit) manager.setBrightness(lastBrightness);
}

void HalFrontlight::setWarmth(const uint8_t warmPercent) {
  manager.setColorTemperature(warmPercent > 100 ? 100 : warmPercent);
}

void HalFrontlight::setOn(const bool on) {
  if (on == lit) return;
  lit = on;
  manager.setBrightness(lit ? lastBrightness : 0);
}
