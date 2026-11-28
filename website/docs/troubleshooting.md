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
  than the hostname (`xerif.local`, or `crosspoint.local` in Calibre
  Wireless mode).
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

## Providing logs for a bug report

A log makes a bug much easier to track down. Choose one of the two methods
below and attach the log to your issue. With Method 1 the capture itself
needs no computer; you still need a computer or a phone browser to copy the
file into the issue.

### Method 1 (recommended): save the log on the SD card

1. On the reader, open **Settings › System › SD Debug Log** and switch it
   **on**. If the reader asks "Clear the existing log?", choose **Clear**
   for a fresh capture so the file holds only the new reproduction.
   Choosing **Cancel** keeps appending to the old log.
2. Reproduce the problem as normal. The SD log receives the same messages
   as the serial monitor: debug level in debug builds, info level in
   release builds.
3. Switch **SD Debug Log** back **off**. While it is off the reader writes
   nothing to the card, and the file you captured stays there — turning it
   off stops new writes without deleting your evidence.
4. Get the file off the reader in either of these ways:
   - Over USB: open **File Transfer › USB Drive** on the reader and connect
     it to a computer. It can take up to 30 s to appear. Copy
     `.crosspoint/log.txt` from the SD card, and eject the drive on the
     computer before leaving that screen.
   - Over the web UI: open **File Transfer › Join a Network** (or **Create
     Hotspot**), then visit `http://xerif.local` or the reader's IP address
     in a browser on the same network. Open the **Logs** tab (`/logs`):
     **Refresh** shows the latest 64 KiB and the page refreshes every 2 s.
     **Download** saves the complete log as `log.txt`.

   If the Logs tab says "Log file not found — enable Settings → System → SD
   Debug Log on the device.", there is no log file to download yet.

The file is capped at 2 MiB: once it reaches that size, the reader starts a
new empty log at the next flush, so for a long reproduction the newest
messages are the useful ones.

Prefer this method: capturing the log takes only a few taps on the reader.

### Method 2 (fallback): capture the serial monitor

Use this when the reader cannot join your Wi-Fi or start its hotspot, or
when USB Drive mode is unavailable:

1. Clone the repository on a computer and install
   [uv](https://docs.astral.sh/uv/).
2. Connect the reader with a USB data cable.
3. From the repository root, run:

   ```sh
   uv run scripts/debugging_monitor.py
   ```

   `uv` resolves the script's dependencies (pyserial, matplotlib and
   colorama) automatically from its inline metadata.
4. Reproduce the problem and copy the relevant console output into the
   issue. Press Ctrl-C to stop the monitor.

The SD log from Method 1 contains the same messages as this serial stream,
so either capture works for a bug report.

**Sticky owners:** the Sticky build has no USB Drive mode; use Method 2.
The web and USB-Drive log paths need the SD-card and USB capabilities the
Sticky build leaves out, and the Logs tab needs Wi-Fi or a hotspot as well.

## Still stuck?

Search or open an issue on
[GitHub](https://github.com/Belphemur/Xerif-eReader/issues) — include your
reader model and the version shown in **Settings › System**.
