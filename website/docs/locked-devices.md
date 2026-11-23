# USB-locked devices (Xteink Unlocker)

Some Xteink units purchased from third-party stores (e.g. AliExpress) ship
with USB flashing **locked from the factory**. A locked device refuses the
web flasher until it has been unlocked once with the **Xteink Unlocker**
tool at [crosspointreader.com/#unlock-tool](https://crosspointreader.com/#unlock-tool).

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

After unlocking, come back to the [web flasher](/flash) and install Xerif.
