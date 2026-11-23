# Installation

Two ways to install Xerif on your Xteink X4 Pro. Both keep the books,
fonts, and settings already on your SD card.

## Recommended: flash from the browser

1. Open the [Flash page](/flash) in a desktop Chrome, Edge, or Opera window.
2. Connect the reader with a USB data cable and power it on.
3. Click **Flash it now** and pick the reader's serial port when the
   browser asks.

The whole flash takes about two minutes. The installer always offers the
latest stable version; there is no manual download.

## Alternative: install from the SD card

Works from any browser on any device:

1. Download `xerif-<version>-x4pro.bin` from the
   [latest release](https://github.com/Belphemur/Xerif-eReader/releases/latest).
2. Copy it to the top folder of the reader's SD card, keeping the file name
   as-is (the device's picker only shows `.bin` files).
3. On the reader open **Settings › System › SD Card Firmware Update**, select
   the file, and confirm.

This path needs the reader to boot far enough to open its Settings — it
updates a working reader (Xerif or stock Xteink firmware). If the reader
still starts but is stuck, hold **Next** and press **Power** as it starts to
land directly in the recovery picker; see the
[Flash page](/flash#sd-card) for details.

## After installing

The reader restarts into Xerif. Continue with
[First-time setup](/docs/first-time-setup).
