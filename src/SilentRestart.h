#pragma once

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session. The live frontlight
// state rides along in the same flag so the reboot is invisible: the light
// comes back exactly as it was, regardless of the Restore Light on Wake
// preference.

void silentRestart();            // home screen
void silentRestartToReader();    // currently-open EPUB (APP_STATE.openEpubPath)
void silentRestartToSettings();  // settings screen
// Reboot into the File Transfer > Join Network flow on a pristine heap, so the
// WiFi + TLS working set has the contiguous RAM it needs on tight boards. A
// no-op on touch boards (a soft reset would cycle their externally-powered
// rails); the caller then proceeds without a reboot.
void silentRestartToJoinNetwork();
// Reboot into the File Transfer > Create Hotspot flow on a pristine heap
// (AP/QR file-sharing path), by analogy to Join Network. No-op on touch
// boards; the caller then proceeds.
void silentRestartToHotspot();
// Reboot into the Bluetooth Page Turner settings screen on a pristine heap,
// after a radio start refused for memory there. The saved opt-in retries the
// start on re-entry. No-op on touch boards (a soft reset would cycle their
// externally-powered rails); the caller then proceeds. False when the host
// declined the reboot (touch boards, sleep or power-off in progress): the
// pairing restart must not spend its RTC memo on a declined ask.
bool silentRestartToBleSettings();

// Reboots immediately after an activity releases exclusive raw storage. The
// RTC target ensures setup() lands on Home instead of resuming a reader.
void restartToHomeAfterStorageHandoff();
