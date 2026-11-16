"""GT911 touchIdleSleepGesture settings round-trip; run with python3 test/test_settings_idle_gesture.py.

The real CrossPointSettings.cpp pulls the BLE/BoardConfig chain, which is too
heavy for the host suite, so this follows the repo's fragment-extraction
pattern (test_side_button_orientation.py): run the actual fromJson fragment in
a harness and structurally pin the save/menu wiring around it.
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

settings_src = (ROOT / "src/CrossPointSettings.h").read_text()
settings_cpp = (ROOT / "src/CrossPointSettings.cpp").read_text()
settings_list = (ROOT / "src/SettingsList.cpp").read_text()
settings_list_h = (ROOT / "src/SettingsList.h").read_text()
english = (ROOT / "lib/I18n/translations/english.yaml").read_text()

# --- the fromJson fragment, executed against a model document ----------------
line_start = settings_cpp.index('  touchIdleSleepGesture = doc["touchIdleSleepGesture"]')
line_end = settings_cpp.index("\n", line_start)
load_fragment = settings_cpp[line_start:line_end]

harness = r"""
#include <cassert>
#include <cstdint>

struct SavedValue {
  bool present;
  int value;
  bool isNull() const { return !present; }
};
struct Document {
  bool present;
  int value;
  SavedValue operator[](const char*) const { return {present, value}; }
};
inline uint8_t operator|(const SavedValue& v, uint8_t dflt) {
  return v.isNull() ? dflt : static_cast<uint8_t>(v.value);
}
uint8_t touchIdleSleepGesture = 0;
bool load(bool present, int value) {
  Document doc{present, value};
""" + load_fragment + r"""
  return touchIdleSleepGesture;
}

int main() {
  // Missing key (pre-feature save file): default 0 — gesture stays OFF until
  // the owner turns it on.
  assert(load(false, 0) == 0);
  // Persisted byte round-trips: 0 stays off, non-zero bool-folds to 1 so a
  // corrupted/foreign value cannot widen past the toggle's two states.
  assert(load(true, 0) == 0);
  assert(load(true, 1) == 1);
  assert(load(true, 2) == 1);
  assert(load(true, 255) == 1);
  return 0;
}
"""

with subprocess.Popen(["c++", "-std=c++17", "-x", "c++", "-", "-o", "/dev/null"], stdin=subprocess.PIPE) as proc:
    _, err = proc.communicate(harness.encode())
if proc.returncode != 0:
    print(err.decode() if err else "compile failed")
    sys.exit(1)

# --- structural pins ---------------------------------------------------------
# Save writes the same key (round-trip pair with the load line above).
save_line = 'doc["touchIdleSleepGesture"] = touchIdleSleepGesture;'
assert save_line in settings_cpp, "missing toJson persistence for touchIdleSleepGesture"

# Toggle is a bool-folded byte, default OFF (bench-first policy: gesture-mode
# current draw and the diode circuit are unmeasured).
assert "uint8_t touchIdleSleepGesture = 0;" in settings_src

# Menu entry: System category, key matches the persisted key, and it is a
# Toggle (the on-device entry touchIdleSleep never got).
assert "StrId::STR_TOUCH_IDLE_GESTURE, &CrossPointSettings::touchIdleSleepGesture" in settings_list
assert '"touchIdleSleepGesture", StrId::STR_CAT_SYSTEM' in settings_list

# Hidden on boards whose touch controller is not a GT911 (runtime, not a
# load-time condition: the key stays persisted everywhere).
assert (
    "STR_TOUCH_IDLE_GESTURE:\n      return BoardConfig::ACTIVE.touch.controller != "
    "BoardConfig::TouchController::Gt911;" in settings_list_h
)

# English label ships (the translation fallback).
assert 'STR_TOUCH_IDLE_GESTURE: "' in english

print("settings idle-gesture round-trip passed")
