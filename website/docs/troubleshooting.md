# Troubleshooting

## The reader won't turn on or is stuck

Press and release **Reset**, then quickly press and hold **Power** for a
few seconds to reboot. If the reader still starts but misbehaves after an
update, hold **Next** and press **Power** as it starts: the firmware's
recovery hatch boots straight into an SD-card firmware picker and repoints
the boot selector at the freshly written slot. If the device does not start
at all, the SD-card method cannot reach it either (its picker lives in the
reader's own menus) — redo the install over USB with the
[web flasher](/flash), or use the
[maker's flash tool](/flash#restore-stock-xteink-firmware) to restore stock
firmware.

## Can't see the device in the browser

- Use a **USB data cable** (charge-only cables carry no data — the most
  common cause).
- Use desktop Chrome, Edge, or Opera; the web flasher needs WebSerial.
- Power the reader on before clicking **Flash it now**.
- Still no device after all of this? Some third-party Xteink units ship
  with USB flashing locked from the factory — see
  [USB-locked devices](/docs/locked-devices) for how to check and unlock.

## Web interface not reachable

- Check the address shown on the reader's File Transfer screen.
- Both devices must be on the same network; try the numeric IP rather
  than `crosspoint.local`.
- Corporate/guest Wi-Fi often isolates clients — use the hotspot mode.

## Upload fails or times out

- Large files over slow Wi-Fi take a while; try the USB drive mode.
- Check free space on the SD card.
- If one particular EPUB misbehaves, run the built-in **EPUB Optimizer**
  on it.

## Wi-Fi password "not working"

Saved passwords are never shown back in the web UI. Edit the network,
leave the password field blank, and save — that keeps the stored
password. To replace it, type a new one and save.

## Frontlight didn't come back after sleep

Check **Restore Light on Wake** in Display Settings (on by default).

## Still stuck?

Search or open an issue on
[GitHub](https://github.com/Belphemur/Xerif-eReader/issues) — include your
reader model and the version shown in **Settings › System**.
