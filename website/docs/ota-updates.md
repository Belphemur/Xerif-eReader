# OTA updates

Xerif can update itself over Wi-Fi — no cable needed.

## How to update

1. Open **Settings › System** on the reader.
2. Choose **Check for updates** (or **Install update** when one is offered).
3. The reader downloads the latest stable release, verifies it, and
   restarts into the new version.

Books, reading positions, and settings survive updates.

## How updates are protected

Every release ships with a signed manifest. The reader checks the
Ed25519 signature before applying an update, so a corrupted or tampered
download is refused rather than installed. Updates live in two firmware
slots — the reader falls back to the previous version if an install
doesn't complete.

## No Wi-Fi?

Use the SD-card method from [Installation](/docs/installation#alternative-install-from-the-sd-card)
or flash the latest image with the [web installer](/flash).
