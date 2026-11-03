#pragma once

// Pure decision policy for the wake-release swallow (main.cpp's
// wakePowerReleasePending; docs/design/2026-11-03-wake-release-shutdown-swallow.md):
// a power press that started this boot — the shutdown-wake hold on rail-button
// boards like the X4 Pro whose cold boot cannot be classified as a
// power-button wake, or a classified deep-sleep wake — must never dispatch an
// in-app action on the boot it caused. Header-only, no dependencies: main.cpp
// arms and consumes with this, host tests drive the state machine
// (test/input_grammar/).
class WakeReleaseSwallow {
 public:
  // Arm when the wake classification says this boot was caused by the power
  // button (deep-sleep wake; its press often ends before setup can sample),
  // OR when the power button was physically held at the moment the input
  // subsystem came up — the raw sample every rail-button boot produces, taken
  // strictly before the poll task's first edge sample. Never arm on a silent
  // maintenance reboot: its boot boundary discards queued edges and
  // deliberately delivers a still-held button's release, so the swallow must
  // stay out of the way there.
  static bool shouldArm(const bool classifiedWake, const bool powerHeldAtInputStart, const bool silentReboot) {
    return classifiedWake || (powerHeldAtInputStart && !silentReboot);
  }

  // One-shot consume: fire exactly once, on the first frame where the button
  // level reads released. While the wake hold continues (level pressed) this
  // is false, so holding through boot costs nothing; the first released frame
  // — the wake press's own release, queued or live — ends the swallow.
  static bool shouldConsume(const bool pending, const bool powerPressed) { return pending && !powerPressed; }
};
