#pragma once
#include <ArduinoJson.h>
#include <BlePageTurner.h>
#include <Epub/ReaderRenderSpec.h>
#include <Logging.h>
#include <PersistableStore.h>

#include <cstdint>
#include <ctime>

#include "BoardFeatures.h"
#include "util/HomeButtonInput.h"

// Forward declaration: the live offset resolver lives in lib/hal/HalTimeZone.cpp
// (which pulls in the AceTime database). Declaring it here keeps CrossPointSettings.h
// free of that heavy dependency for the many TUs that include it. The production
// link provides the real symbol; unit-test shims provide a stub.
namespace freeink {
bool resolveUtcOffsetMinutes(const char* ianaId, int64_t utcEpochSeconds, int& offsetMin);
}

class CrossPointSettings : public PersistableStore<CrossPointSettings> {
 private:
  // Private constructor for singleton
  CrossPointSettings() = default;

  friend class PersistableStore<CrossPointSettings>;

 public:
  enum SLEEP_SCREEN_MODE {
    DARK = 0,
    LIGHT = 1,
    CUSTOM = 2,
    COVER = 3,
    COVER_CUSTOM = 4,
    BLANK = 5,
    QUICK_RESUME = 6,
    TRANSPARENT_CUSTOM = 7,
    COVER_TRANSPARENT = 8,
    SLEEP_SCREEN_MODE_COUNT
  };
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1, SLEEP_SCREEN_COVER_MODE_COUNT };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
    SLEEP_SCREEN_COVER_FILTER_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR {
    BOOK_PROGRESS = 0,
    CHAPTER_PROGRESS = 1,
    HIDE_PROGRESS = 2,
    STATUS_BAR_PROGRESS_BAR_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR_THICKNESS {
    PROGRESS_BAR_THIN = 0,
    PROGRESS_BAR_NORMAL = 1,
    PROGRESS_BAR_THICK = 2,
    STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT
  };
  enum STATUS_BAR_TITLE { BOOK_TITLE = 0, CHAPTER_TITLE = 1, HIDE_TITLE = 2, STATUS_BAR_TITLE_COUNT };
  enum XTC_STATUS_BAR_MODE {
    XTC_STATUS_BAR_HIDE = 0,
    XTC_STATUS_BAR_BOTTOM = 1,
    XTC_STATUS_BAR_TOP = 2,
    XTC_STATUS_BAR_MODE_COUNT
  };

  enum STATUS_BAR_CLOCK_MODE {
    STATUS_BAR_CLOCK_HIDE = 0,
    STATUS_BAR_CLOCK_RIGHT = 1,
    STATUS_BAR_CLOCK_LEFT = 2,
    STATUS_BAR_CLOCK_MODE_COUNT
  };

  // Chapter-time-left status-bar placement, mirroring STATUS_BAR_CLOCK_MODE.
  // HIDE keeps it off entirely; RIGHT/LEFT dock it into the matching cluster.
  enum STATUS_BAR_CHAPTER_TIME_LEFT_MODE {
    STATUS_BAR_CHAPTER_TIME_LEFT_HIDE = 0,
    STATUS_BAR_CHAPTER_TIME_LEFT_RIGHT = 1,
    STATUS_BAR_CHAPTER_TIME_LEFT_LEFT = 2,
    STATUS_BAR_CHAPTER_TIME_LEFT_MODE_COUNT
  };

  enum ORIENTATION {
    PORTRAIT = 0,       // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,   // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,       // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3,  // 800x480 logical coordinates, native panel orientation
    ORIENTATION_COUNT
  };

  // Front button layout options (legacy)
  // Default: Back, Confirm, Left, Right
  // Swapped: Left, Right, Back, Confirm
  enum FRONT_BUTTON_LAYOUT {
    BACK_CONFIRM_LEFT_RIGHT = 0,
    LEFT_RIGHT_BACK_CONFIRM = 1,
    LEFT_BACK_CONFIRM_RIGHT = 2,
    BACK_CONFIRM_RIGHT_LEFT = 3,
    FRONT_BUTTON_LAYOUT_COUNT
  };

  // Front button hardware identifiers (for remapping)
  enum FRONT_BUTTON_HARDWARE {
    FRONT_HW_BACK = 0,
    FRONT_HW_CONFIRM = 1,
    FRONT_HW_LEFT = 2,
    FRONT_HW_RIGHT = 3,
    FRONT_BUTTON_HARDWARE_COUNT
  };

  // Side button layout options
  // Default: Up = Previous, Down = Next. NEXT_NEXT / PREV_PREV assign both
  // buttons to the same direction for one-handed reading.
  enum SIDE_BUTTON_LAYOUT {
    PREV_NEXT = 0,
    NEXT_PREV = 1,
    SIDE_BUTTONS_DISABLED = 2,
    NEXT_NEXT = 3,
    PREV_PREV = 4,
    SIDE_BUTTON_LAYOUT_COUNT
  };

  // Font family options (built-in fonts only; SD card fonts use sdFontFamilyName).
  // Upstream #4022 moved NotoSerif out of the firmware: it is now an optional SD
  // download (lib/EpdFont/scripts/sd-fonts.yaml) and the only built-in family is
  // Atkinson Hyperlegible Next, registered under the NOTOSANS_* font IDs.
  // Persisted values keep their historical meaning — never reorder them:
  //   0 = NotoSerif (legacy default; migrated to sdFontFamilyName="NotoSerif" on load)
  //   1 = NotoSans (legacy alias for the same Atkinson data)
  //   2 = ATKINSON_HN (fork's explicit alias, same Atkinson data)
  //   3 = legacy OpenDyslexic (migrated to sdFontFamilyName="OpenDyslexic" on load)
  enum FONT_FAMILY { NOTOSANS = 0, FONT_FAMILY_COUNT = 1 };
  static constexpr uint8_t LEGACY_NOTOSERIF = 0;
  static constexpr uint8_t LEGACY_ATKINSON_HN = 2;
  static constexpr uint8_t LEGACY_OPENDYSLEXIC = 3;
  // Persisted IDs stay stable even though the built-in menu now starts at zero.
  static constexpr uint8_t STORED_NOTOSANS = 1;
  static constexpr uint8_t BUILTIN_FONT_COUNT = FONT_FAMILY_COUNT;
  // Reader font size is a point size, not an enum slot — see fontPointSize.
  // Legacy 1.4-and-earlier files stored a 0..3 SMALL/MEDIUM/LARGE/EXTRA_LARGE
  // slot; fromJson() folds that range up (see LEGACY_FONT_SIZE_MAX).
  static constexpr uint8_t LEGACY_FONT_SIZE_MAX = 3;
  static constexpr uint8_t DEFAULT_FONT_POINT_SIZE = 14;
  // Line height as a percent of font size.
  enum LINE_COMPRESSION {
    LINE_SPACING_100 = 0,
    LINE_SPACING_110 = 1,
    LINE_SPACING_120 = 2,
    LINE_SPACING_130 = 3,
    LINE_SPACING_145 = 4,
    LINE_SPACING_160 = 5,
    LINE_SPACING_175 = 6,
    LINE_COMPRESSION_COUNT
  };
  enum PARAGRAPH_ALIGNMENT {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    BOOK_STYLE = 4,
    PARAGRAPH_ALIGNMENT_COUNT
  };

  // Auto-sleep timeout options (in minutes)
  enum SLEEP_TIMEOUT {
    SLEEP_1_MIN = 0,
    SLEEP_5_MIN = 1,
    SLEEP_10_MIN = 2,
    SLEEP_15_MIN = 3,
    SLEEP_30_MIN = 4,
    SLEEP_TIMEOUT_COUNT
  };

  // E-ink refresh frequency (pages between full refreshes).
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
    REFRESH_NEVER = 5,
    REFRESH_FREQUENCY_COUNT
  };

  // Short power button press actions. Touch boards get PWR_CONFIRM in place of
  // CREATE_CLIPPING; touch boards without a home key also get the reader
  // shortcuts (see SettingsList.h). Persisted by index: append only.
  enum SHORT_PWRBTN {
    IGNORE = 0,
    SLEEP = 1,
    PAGE_TURN = 2,
    FORCE_REFRESH = 3,
    FOOTNOTES = 4,
    PWR_CONFIRM = 5,
    CREATE_CLIPPING = 6,
    PWR_BOOKMARK = 7,
    PWR_SYNC = 8,
    PWR_READER_MENU = 9,
    SHORT_PWRBTN_COUNT
  };

  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };

  // Page turn button long press behavior
  enum LONG_PRESS_BUTTON_BEHAVIOR {
    OFF = 0,
    CHAPTER_SKIP = 1,
    ORIENTATION_CHANGE = 2,
    LONG_PRESS_BUTTON_BEHAVIOR_COUNT
  };

  // UI Theme
  enum UI_THEME { CLASSIC = 0, LYRA = 1, LYRA_3_COVERS = 2, ROUNDEDRAFF = 3, COVER_GRID = 4 };

  // Image rendering in EPUB reader. IMAGES_DISPLAY_BW lays images out like
  // IMAGES_DISPLAY but draws them as a B/W dither, so image pages skip the
  // extra grayscale refresh.
  enum IMAGE_RENDERING {
    IMAGES_DISPLAY = 0,
    IMAGES_PLACEHOLDER = 1,
    IMAGES_SUPPRESS = 2,
    IMAGES_DISPLAY_BW = 3,
    IMAGE_RENDERING_COUNT
  };

  // How Select opens the reader menu: the classic full-screen list, or a toolbar
  // overlay (top/bottom bars with Contents / Text / More bottom-sheet panels)
  // painted over the page.
  enum READER_MENU_STYLE { READER_MENU_LIST = 0, READER_MENU_TOOLBAR = 1, READER_MENU_STYLE_COUNT };

  enum TILT_PAGE_TURN { TILT_OFF = 0, TILT_NORMAL = 1, TILT_NVERTED = 2, TILT_PAGE_TURN_COUNT };
  enum VIBRATION { VIBRATION_TOUCH = 0, VIBRATION_TOUCH_PAGE = 1, VIBRATION_OFF = 2 };
  uint8_t vibration = VIBRATION_OFF;
  enum HAPTIC_INTENSITY { HAPTIC_LOW = 0, HAPTIC_MEDIUM = 1, HAPTIC_HIGH = 2 };
  uint8_t hapticIntensity = HAPTIC_HIGH;

  enum TOUCH_READER_CONTROLS { TOUCH_READER_OFF = 0, TOUCH_READER_ON = 1, TOUCH_READER_CONTROLS_COUNT };

  // Per-direction reader page-turn gestures. INVERTED_TAP is tap-only; either
  // direction set to it swaps both directions' shared tap zones (see
  // ReaderUtils::detectTouchPageTurn).
  enum PAGE_TURN_GESTURE {
    TAP_AND_SWIPE = 0,
    TAP_ONLY = 1,
    SWIPE_ONLY = 2,
    INVERTED_TAP = 3,
    PAGE_TURN_GESTURE_DISABLED = 4,
    PAGE_TURN_GESTURE_COUNT
  };

  // Action for a long press on the reading surface (touch boards only).
  // Persisted uint8_t BY MENU-POSITION INDEX — APPEND ONLY (see appendEnumEmplace).
  enum TOUCH_LONG_PRESS_ACTION {
    TOUCH_LP_DICTIONARY = 0,
    TOUCH_LP_IGNORE = 1,
    TOUCH_LP_FOOTNOTE = 2,
    TOUCH_LONG_PRESS_ACTION_COUNT
  };

  // How the reader menu opens on touch boards. Persisted under the legacy
  // "tapForReaderMenu" key: 0/1 keep their old Off/Tap meaning.
  enum SHOW_READER_MENU { READER_MENU_OFF = 0, READER_MENU_TAP = 1, READER_MENU_SWIPE_UP = 2, SHOW_READER_MENU_COUNT };

  enum QUICK_RESUME_SLEEP_SCREEN {
    QUICK_RESUME_NEVER = 0,
    QUICK_RESUME_AFTER_TIMEOUT = 1,
    QUICK_RESUME_SLEEP_SCREEN_COUNT
  };

  // Sleep screen settings
  uint8_t sleepScreen = DARK;
  // Night mode: inverted output polarity, applied to every activity per
  // render by ActivityManager. The sleep screen opts out itself.
  uint8_t screenInverted = 0;
  // Sleep screen cover mode settings
  uint8_t sleepScreenCoverMode = FIT;
  // Sleep screen cover filter
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  // Show the current book's reading progress % on cover sleep/shutdown screens
  uint8_t sleepScreenCoverShowProgress = 1;
  // Status bar settings
  uint8_t statusBarChapterPageCount = 1;
  // Chapter-time-left status-bar placement. Supersedes the old unreleased
  // boolean key; stale values are ignored and fall back to the default.
  uint8_t statusBarChapterTimeLeftMode = STATUS_BAR_CHAPTER_TIME_LEFT_RIGHT;
  uint8_t statusBarBookProgressPercentage = 1;
  uint8_t statusBarProgressBar = HIDE_PROGRESS;
  uint8_t statusBarProgressBarThickness = PROGRESS_BAR_NORMAL;
  uint8_t statusBarTitle = CHAPTER_TITLE;
  uint8_t statusBarBattery = 1;
  uint8_t xtcStatusBarMode = XTC_STATUS_BAR_HIDE;
  // Clock display in status bar (X3 only, requires DS3231 RTC)
  uint8_t statusBarClock = STATUS_BAR_CLOCK_HIDE;
  // Auto-detected IANA time zone id, e.g. "America/Toronto". Empty = not detected (show UTC).
  char clockTimeZoneId[40] = "";
  // Detected current UTC offset in MINUTES (signed), e.g. -240 = UTC-4.
  // 0 is a valid UTC offset (zones like Europe/London sit at 0 part of the
  // year), so detection is tracked by clockTimeZoneId being non-empty, NOT by
  // this being non-zero. Clamped to [-720, +840] on load and detection.
  int16_t clockTzOffsetMin = 0;
  // True when the detected zone is currently in DST (informational; the offset already folds DST).
  uint8_t clockTzIsDst = 0;
  // Clock display format: 0 = 24-hour, 1 = 12-hour
  uint8_t clockFormat = 0;
  // Show the current date + time in the top-left of the screen header (every
  // GUI.drawHeader caller: Home, File Browser, Settings, network screens).
  // Defaults to on; the reader has its own status-bar clock instead.
  uint8_t headerClock = 1;
  // Set once an NTP sync succeeds. Used to skip re-syncing on every WiFi connect.
  // Resetting to 0 (e.g. via the web UI) forces a re-sync on next WiFi connect.
  uint8_t clockHasBeenSynced = 0;
  // A time-zone detect was skipped by the TLS heap guard (never attempted).
  // The next WiFi connect retries it once the heap passes the floor; cleared
  // when an attempt is actually made, valid or not, so a reachable-but-failing
  // geolocation service cannot turn every connect into an extra TLS session.
  uint8_t clockTzPending = 0;
  // Text rendering settings
  uint8_t extraParagraphSpacing = 1;
  // Text render mode (same settings row as the former textAntiAliasing bool,
  // fully replaced per owner directive — no compat key, no JSON migration):
  // Smooth = 8-bit AA coverage + dual-plane gray paint (hinted, Light);
  // Crisp = hinted 1-bit mono glyphs straight from FT — no coverage, no
  // tone quantization, no plane walk. DEFAULT CRISP (mono hinted glyphs is
  // the fast, hard-edged baseline; Smooth is the opt-in AA mode).
  enum TEXT_RENDER_MODE { TEXT_RENDER_SMOOTH = 0, TEXT_RENDER_CRISP = 1 };
  uint8_t textRenderMode = TEXT_RENDER_CRISP;
  // Paragraph first-line indentation for EPUB: Off (0) or 1–5 scaled space
  // advances (upstream #3727). Overrides missing/zero/positive CSS text-indent;
  // negative hanging indents are preserved.
  uint8_t paragraphIndentSpaces = 2;
  // Word spacing: percent of the font's space advance. Character spacing is a
  // signed pixel offset stored 0..4 (= -2..+2 px).
  static constexpr uint8_t WORD_SPACING_MIN = 50;
  static constexpr uint8_t WORD_SPACING_MAX = 200;
  static constexpr uint8_t WORD_SPACING_STEP = 25;
  uint8_t wordSpacing = 100;
  static constexpr uint8_t CHARACTER_SPACING_OFFSET = 2;  // stored 0..4 maps to -2..+2 px
  uint8_t characterSpacing = CHARACTER_SPACING_OFFSET;
  int8_t getCharacterSpacing() const { return static_cast<int8_t>(characterSpacing - CHARACTER_SPACING_OFFSET); }
  // Short power button click behaviour. The default Sleep binding makes the
  // short click sleep; a long press (see POWER_BUTTON_HOLD_MS) always powers
  // the device off, regardless of this setting.
  uint8_t shortPwrBtn = SLEEP;
  // EPUB reading orientation settings
  // 0 = portrait (default), 1 = landscape clockwise, 2 = inverted, 3 = landscape counter-clockwise
  uint8_t orientation = PORTRAIT;
  // Button layouts (front layout retained for migration only)
  uint8_t frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  uint8_t sideButtonLayout = PREV_NEXT;
  uint8_t frontButtonFollowOrientation = 0;
  // Front button remap (logical -> hardware)
  // Used by MappedInputManager to translate logical buttons into physical front buttons.
  uint8_t frontButtonBack = FRONT_HW_BACK;
  uint8_t frontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeft = FRONT_HW_LEFT;
  uint8_t frontButtonRight = FRONT_HW_RIGHT;
  // Reader font settings
  uint8_t fontFamily = NOTOSANS;
  // Point size of the reader font. Only sizes the active family actually ships
  // are selectable; SdCardFontSystem::ensureLoaded() snaps this to the nearest
  // available size (and persists the snap) whenever the family changes.
  uint8_t fontPointSize = DEFAULT_FONT_POINT_SIZE;
  uint8_t lineSpacing = LINE_SPACING_120;
  uint8_t paragraphAlignment = JUSTIFIED;
  // Auto-sleep timeout setting (default 10 minutes). Legacy sleepTimeout enum values are migration-only.
  uint8_t sleepTimeoutMinutes = 10;
  // Auto power off: hours the device may dwell in deep sleep before shutting
  // down. Runs as an RTC timer armed on each deep sleep entry; the max value
  // means "off" (no shutdown timer).
  static constexpr uint8_t AUTO_POWER_OFF_MIN_HOURS = 2;
  static constexpr uint8_t AUTO_POWER_OFF_MAX_HOURS = 12;
  static constexpr uint8_t AUTO_POWER_OFF_STEP_HOURS = 2;
  static constexpr uint8_t AUTO_POWER_OFF_DEFAULT_HOURS = 4;
  uint8_t autoPowerOffHours = AUTO_POWER_OFF_DEFAULT_HOURS;
  // GT911 touch Sleep mode at sustained idle (design
  // 2026-09-24-gt911-idle-sleep.md §3 Tier B): park the touch controller after
  // GT911_IDLE_SLEEP_MS of no input. 0 = disabled, for owners whose module
  // misbehaves on the best-effort sleep opcode — the poll throttle keeps
  // running either way. No settings-menu entry yet; JSON-only (owner adds UI).
  uint8_t touchIdleSleep = 1;
  // E-ink refresh frequency (default 15 pages)
  uint8_t refreshFrequency = REFRESH_15;
  uint8_t hyphenationEnabled = 0;

  // Reader screen margin settings
  static constexpr uint8_t SCREEN_MARGIN_MIN = 5;
  static constexpr uint8_t SCREEN_MARGIN_MAX = 40;
  static constexpr uint8_t SCREEN_MARGIN_STEP = 5;
  uint8_t screenMargin = SCREEN_MARGIN_MIN;
  // OPDS download destination folder ("" = SD root). Global; edited from the
  // OPDS server list. Persisted via a category-less SettingInfo::String in
  // SettingsList.h, so it stays out of the on-device Settings screen.
  char opdsDownloadFolder[64] = "";
  // On-disk filename format for OPDS downloads (0=Author-Title default, 1=Title-Author,
  // 2=Title). See OpdsFilenameFormat. Persisted via a category-less SettingInfo::Enum,
  // edited from the OPDS server list; hidden from the on-device Settings screen.
  uint8_t opdsFilenameFormat = 0;
  // Hide battery percentage
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  // Long-press page turn button behavior
  uint8_t longPressButtonBehavior = OFF;
  // UI Theme
  uint8_t uiTheme = LYRA;
  // Sunlight fading compensation
  uint8_t fadingFix = 0;
  // Power button return from footnotes (1 = enabled, 0 = disabled)
  uint8_t pwrBtnFootnoteBack = 1;
  // Use book's embedded CSS styles for EPUB rendering (1 = enabled, 0 = disabled)
  uint8_t embeddedStyle = 1;
  // Focus Reading - emphasizes the first part of words with bold
  uint8_t focusReadingEnabled = 0;
  uint8_t readerMenuStyle = READER_MENU_LIST;
  // SD card font family name (empty = use built-in fontFamily)
  char sdFontFamilyName[32] = "";
  // Reader font engine (design §3.6/§14): BITMAP = legacy EpdFont reader
  // path (rollback), TTF = the FreeInkBook native chain. The default follows
  // the device class (§14.1): TTF on CROSSPOINT_TTF_READER builds, bitmap
  // elsewhere. Only the TTF runtime consults this key.
  enum READER_FONT_ENGINE { READER_ENGINE_BITMAP = 0, READER_ENGINE_TTF = 1 };
#if defined(CROSSPOINT_TTF_READER)
  uint8_t readerFontEngine = READER_ENGINE_TTF;
#else
  uint8_t readerFontEngine = READER_ENGINE_BITMAP;
#endif
  // Selected native TTF family under /fonts (empty = built-in fallback
  // chain). Keyed by the §14.4 scanner's display name.
  char ttfFontFamilyName[48] = "";
  // Continuous 8..72 pt body size for the TTF engine (§14.2). The bitmap
  // reader keeps the discrete fontPointSize list.
  static constexpr uint8_t TTF_FONT_POINT_SIZE_MIN = 8;
  static constexpr uint8_t TTF_FONT_POINT_SIZE_MAX = 72;
  static constexpr uint8_t DEFAULT_TTF_FONT_POINT_SIZE = 14;
  uint8_t ttfFontPointSize = DEFAULT_TTF_FONT_POINT_SIZE;
  // Dictionary folder name under /dictionaries (empty = no dictionary)
  char dictionaryName[32] = "";
  // Master switch for all SD-card plugin discovery/execution. New installs keep
  // this disabled; settings migration enables it when pre-existing plugin files
  // are found so a firmware update never silently disables an existing setup.
  uint8_t pluginsEnabled = 0;
  // User hid the Plugin Hub install row; cleared when plugins are turned back on.
  uint8_t pluginHubPromptHidden = 0;
  // Show hidden files/directories (starting with '.') in the file browser (0 = hidden, 1 = show)
  uint8_t showHiddenFiles = 0;
  // Show the title and author read from inside each book rather than its
  // filename. Users can disable this to make index rebuilds skip EPUB parsing.
  uint8_t libraryUseMetadata = 1;
  // Remove a book from the Recent Books list when its End-of-Book screen is reached (0 = off, 1 = on)
  uint8_t removeReadBooksFromRecents = 0;
  // Move epub to /Read/ folder on SD card when finished (0 = disabled, 1 = enabled)
  uint8_t moveFinishedToReadFolder = 0;
  // Short press Back goes to file browser instead of home (0 = disabled, 1 = enabled)
  uint8_t backShortToFileBrowser = 0;
  // Track reading statistics into the SQLite database (0 = disabled, 1 = enabled)
  uint8_t trackReadingStats = 1;
  // Proactive finish-reading prompt near the end of a book (10/5/1% remaining),
  // asking "Did you finish the book?" (0 = disabled, 1 = enabled)
  uint8_t finishReadingPrompt = 1;
  // Image rendering mode in EPUB reader
  uint8_t imageRendering = IMAGES_DISPLAY;
  // Tilt-based page turning (X3 only — requires QMI8658 IMU)
  uint8_t tiltPageTurn = TILT_OFF;
  // Master reader-touch toggle on boards with a touch controller.
  uint8_t touchReaderControls = TOUCH_READER_ON;
  // Which gestures turn the page in each direction (PAGE_TURN_GESTURE).
  uint8_t pageTurnGesture = SWIPE_ONLY;
  uint8_t previousPageGesture = SWIPE_ONLY;
  // Action for a long press on the reading surface (touch boards only).
  uint8_t touchLongPressAction = TOUCH_LP_DICTIONARY;
  // Reader menu open gesture (SHOW_READER_MENU: off / center tap / bottom-edge
  // up-swipe). Only surfaced on home-key boards, where Home is the capacitive
  // key and the bottom edge is free; elsewhere it stays at the Tap default.
  uint8_t showReaderMenu = READER_MENU_TAP;
  // X4 Pro: double-click power toggles the frontlight. Disabling frees the
  // power button for shortPwrBtn actions without the double-click wait.
  uint8_t doubleClickPwrLight = 1;
  // Capacitive Home-key actions. Persisted values are HomeButtonAction indices;
  // fields are ungated so every board compiles the same catalog. UI rows are
  // gated by board capability. Unknown or legacy fields fall back to defaults.
  uint8_t homeButtonTapAction = static_cast<uint8_t>(HomeButtonAction::GoBack);
  uint8_t homeButtonDoubleTapAction = static_cast<uint8_t>(HomeButtonAction::ReaderMenu);
  uint8_t homeButtonLongPressAction = static_cast<uint8_t>(HomeButtonAction::ToggleFrontlight);
  // Frontlight quick-panel state. Category-less SettingsList entries persist
  // these without adding them to the regular Settings screen.
  uint8_t frontlightBrightness = 60;
  uint8_t frontlightWarmth = 50;  // 0 = cool .. 100 = warm
  uint8_t frontlightOn = 0;
  // Restore the saved on/off state after a normal boot or wake. Brightness and
  // warmth are always remembered even when this is disabled.
  uint8_t frontlightRestoreOnWake = 1;
  // Touch side-swipe gestures for frontlight control: left edge adjusts color
  // temperature, right edge adjusts brightness. Gated at compile time to
  // boards with FREEINK_CAP_FRONTLIGHT. Disableable per-device.
  uint8_t frontlightSideGestures = 1;
  // What the slide switch controls (boards with one): frontlight on/off or
  // night mode. The switch's position is applied at boot and on each change.
  enum SLIDE_SWITCH_ACTION : uint8_t {
    SLIDE_SWITCH_FRONTLIGHT = 0,
    SLIDE_SWITCH_NIGHT_MODE = 1,
    SLIDE_SWITCH_TOUCH_CONTROLS = 2,
  };
  uint8_t slideSwitchAction = SLIDE_SWITCH_FRONTLIGHT;
  // Language setting (Language enum index, default 0 = EN)
  uint8_t language = 0;
  // Keyboard layouts the user can reach, using keyboard_layouts::ALL table bits.
  // 0 means "not configured", resolved to the UI language's layout plus English.
  // Any other value is an explicit choice and is used as-is: the language of the
  // books someone reads is not necessarily the language of their UI.
  // See keyboard_layouts:: for the bit assignment and the defaulting rules.
  uint16_t keyboardLayouts = 0;
  // Bluetooth page turner, saved under the keys lib/BlePageTurner reads and writes (BlePageTurnerJson.h).
  bleturner::Config ble;
  // Quick Resume: keep current content visible with moon icon instead of showing a static sleep screen.
  uint8_t quickResumeSleepScreen = QUICK_RESUME_NEVER;

  static constexpr uint8_t MIN_SLEEP_TIMEOUT_MINUTES = 1;
  static constexpr uint8_t SLEEP_TIMEOUT_NEVER_MINUTES = 31;
  static constexpr uint8_t MAX_SLEEP_TIMEOUT_MINUTES = SLEEP_TIMEOUT_NEVER_MINUTES;

  // Callback to resolve SD card font IDs. Set by SdCardFontSystem::begin().
  // Returns font ID or 0 if not found.
  using SdFontIdResolver = int (*)(void* ctx, const char* familyName, uint8_t fontSize);
  SdFontIdResolver sdFontIdResolver = nullptr;
  void* sdFontResolverCtx = nullptr;

  // Hold time that turns a power-button press into a power off. 400ms matches
  // the SDK's CONFIRM_POWER_HOLD_MS (freeink-sdk InputManager.h:427), which
  // gates when the shared Confirm/Power pin emits BTN_POWER on X4 Pro with the
  // short-press-emits-power flag off (InputManager.cpp:380-383), so the hold
  // path in main.cpp can only fire once BTN_POWER actually exists. The old
  // 10ms threshold existed to make the hold path double as the short-click
  // sleep; sleep now has its own release path in main.cpp, so the hold path is
  // exclusively the power off gesture. Also bounds the PWR_CONFIRM click
  // detector (MappedInputManager.cpp:313), which always saw 400 before.
  static constexpr uint16_t POWER_BUTTON_HOLD_MS = 400;
  uint16_t getPowerButtonDuration() const { return POWER_BUTTON_HOLD_MS; }
  bool shouldTrackReadingStats() const { return trackReadingStats != 0; }
  int getReaderFontId() const;

  // Drop the SD font selection and fall back to the built-in family. The reader
  // point size comes back into BUILTIN_READER_POINT_SIZES with it, since that is
  // the only set a built-in family ships — otherwise the settings UI would keep
  // offering a size nothing renders at. Both fields are persisted in one write.
  void clearSdFontFamily();

  // Resolved status-bar composition. Consumers read the spec; only settings
  // editors read the raw fields.
  //
  // Deliberately NOT built under storeMutex: every field it reads is a single
  // byte, so a concurrent settings write can never produce a corrupt value —
  // only a snapshot mixing pre- and post-change fields. That costs at most one
  // e-ink frame drawn with a mixed status bar, which self-corrects on the next
  // refresh. Locking here would instead put a mutex on the render path and
  // stall it behind the SD write inside saveToFile(). Don't add one back.
  struct StatusBarSpec {
    bool showChapterPageCount = false;
    bool showChapterTimeLeft = true;  // matches chapterTimeLeftMode default (RIGHT)
    bool showBookProgressPercent = false;
    uint8_t titleMode = HIDE_TITLE;  // STATUS_BAR_TITLE
    bool showBattery = false;
    bool showBatteryPercent = false;
    uint8_t clockMode = STATUS_BAR_CLOCK_HIDE;  // STATUS_BAR_CLOCK_MODE
    bool clock12h = false;
    // Effective signed UTC offset in MINUTES for display (detected zone folds DST; 0 = UTC until detected).
    int clockUtcOffsetMin = 0;
    uint8_t chapterTimeLeftMode = STATUS_BAR_CHAPTER_TIME_LEFT_RIGHT;  // STATUS_BAR_CHAPTER_TIME_LEFT_MODE
    uint8_t progressBarMode = HIDE_PROGRESS;                           // STATUS_BAR_PROGRESS_BAR
    uint8_t progressBarHeightPx = 0;                                   // (thickness+1)*2; 0 when the bar is hidden
    uint8_t xtcMode = XTC_STATUS_BAR_HIDE;                             // XTC_STATUS_BAR_MODE

    bool showsProgressBar() const { return progressBarMode != HIDE_PROGRESS; }
    bool showsTitle() const { return titleMode != HIDE_TITLE; }
    bool showsClock() const { return clockMode != STATUS_BAR_CLOCK_HIDE; }
    bool showsChapterTimeLeft() const { return chapterTimeLeftMode != STATUS_BAR_CHAPTER_TIME_LEFT_HIDE; }
    // Visibility of the text lane. Clock hardware presence is the caller's
    // concern: pass halClock.isAvailable(), or true for layout reservation.
    bool textLaneVisible(bool clockAvailable) const {
      return showChapterPageCount || showChapterTimeLeft || showBookProgressPercent || showsTitle() || showBattery ||
             (showsClock() && clockAvailable);
    }
  };
  StatusBarSpec statusBarSpec() const;

  // Resolved text-rendering configuration for the Epub layout engine. The
  // viewport is renderer/orientation-derived, so the caller supplies it —
  // passing it in keeps a spec from ever existing in a half-filled state.
  // Unlocked for the same reason as statusBarSpec(); see the note above.
  ReaderRenderSpec readerRenderSpec(uint16_t viewportWidth, uint16_t viewportHeight) const;

  static const char* getFilePath() { return "/.crosspoint/settings.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Clamps a raw offset to the [-720, +840] window. Shared by the live and
  // cached paths so both clamp identically.
  static int clampUtcOffsetMinutes(int minutes) {
    if (minutes < -720) return -720;
    if (minutes > 840) return 840;
    return minutes;
  }

  // Effective signed UTC offset in MINUTES for display. UTC (0) until a zone has
  // been detected (clockTimeZoneId non-empty). Once detected, the offset is
  // resolved live from the IANA id (via the bundled AceTime database, folding DST
  // automatically at the current UTC time) so the clock stays correct across DST
  // transitions with no extra network calls. Falls back to the last-detected
  // cached offset when the id can't be resolved (e.g. offline). Clamped to the
  // supported [-720, +840] range so a corrupted settings file cannot push
  // ReadingStatsUtils' date-roll loops to an extreme iteration count.
  int clockEffectiveOffsetMin() const {
    const bool hasZone = clockTimeZoneId[0] != '\0';
    if (!hasZone) return 0;
    int live = 0;
    if (freeink::resolveUtcOffsetMinutes(clockTimeZoneId, static_cast<int64_t>(time(nullptr)), live)) {
      return clampUtcOffsetMinutes(live);
    }
    return clampUtcOffsetMinutes(clockTzOffsetMin);
  }

  static void validateFrontButtonMapping(CrossPointSettings& settings);
  static uint8_t sleepTimeoutEnumToMinutes(uint8_t legacyValue);

  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  uint32_t getAutoPowerOffMs() const;
  int getRefreshFrequency() const;
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
