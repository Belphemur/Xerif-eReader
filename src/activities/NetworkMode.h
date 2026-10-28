#pragma once

// The network-entry mode the web-server activity resumes after a heap-defrag
// reboot. MAX is a sentinel meaning "no resume target — show the mode picker"
// (constructed only by the normal user-entry path; the boot resolver passes a
// real mode to resume directly into STA or AP). Separated from
// NetworkModeSelectionActivity.h so ActivityManager.h can thread the resume mode
// through goToFileTransfer() without pulling the full menu activity (which would
// include Activity.h → ActivityManager.h, a cycle).
enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT, USB_DRIVE, MAX };
