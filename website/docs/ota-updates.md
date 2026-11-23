# OTA updates

Xerif can update itself over Wi-Fi — no cable needed.

## How to update

1. Open **Settings › System** on the reader.
2. Choose **Check for updates** (or **Install update** when one is offered).
3. The reader downloads the latest stable release, verifies it, and
   restarts into the new version.

Books, reading positions, and settings survive updates.

## How updates are protected

Stable-channel releases ship with a signed manifest. When the reader runs
the stable slot, it refuses to install a release whose Ed25519-signed
manifest is missing or fails verification, and it checks the downloaded
image against the digest in that manifest — a corrupted or tampered
download is refused rather than installed. Updates live in two firmware
slots, so the reader falls back to the previous version if an install
doesn't complete.

Two exceptions, for completeness:

- A release published without a manifest cannot be installed from the
  stable slot at all.
- A reader running the development channel can still write an update
  directly (that path skips manifest verification and relies on the
  device's own image guards). If you only use stable releases, this never
  applies to you.

## No Wi-Fi?

Use the SD-card method from [Installation](/docs/installation#alternative-install-from-the-sd-card)
or flash the latest image with the [web installer](/flash).
