# USB-locked devices (Xteink Unlocker)

Some Xteink units purchased from third-party stores (e.g. AliExpress) ship
with USB flashing **locked from the factory**. A locked device refuses the
web flasher, and the **Xteink Unlocker** tool at
[crosspointreader.com/#unlock-tool](https://crosspointreader.com/#unlock-tool)
does not change that: it installs its own officially supported firmware
(CrossPoint or CrossInk) over the air instead of unlocking the USB port for
other firmware.

**You do not need this tool if you bought your device directly from
xteink.com** — those units are not locked.

## Check whether your device is locked

Power the device on, connect the USB-C cable, and try the
[web flasher](/flash) first. If the browser's serial device picker does not
show your device, try a different USB port or a different browser before
assuming the device is locked — only reach for the unlocker if the device
still doesn't appear.

## Before you unlock — read this

::: warning ⚠️ READ THIS BEFORE USING THE UNLOCKER

**The only officially supported firmwares in the unlock tool are CrossPoint
and CrossInk.**

Flashing any other firmware on a USB-locked device may **permanently brick
the device** or leave it **permanently stuck on that firmware with no
recovery path**. Once USB flashing is re-locked, your only way back is via
OTA — and if the firmware you flashed doesn't support OTA, **there is no
way out**.

:::

## Locked devices and Xerif

There is **no verified way to install Xerif on a USB-locked device**. The
unlocker is not a step toward Xerif: using it to flash anything other than
CrossPoint or CrossInk is exactly the unsupported-firmware case the warning
above describes. If your device is locked, either leave it on the maker's
firmware or unlock it only if you intend to run
[CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader), which
also covers the X3 (see [Supported devices](/docs/supported-devices)).

Content in this page derives from the
[CrossPoint USB-locked devices guide](https://github.com/crosspoint-reader/crosspoint-reader#usb-locked-devices-xteink-unlocker).
