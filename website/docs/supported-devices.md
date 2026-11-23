# Supported devices

Xerif targets e-readers with RAM to spare for its rendering engine
(PSRAM-equipped boards).

## Supported

| Device | Status |
| --- | --- |
| **Xteink X4 Pro** | Fully supported — this site's [flasher](/flash) serves the X4 Pro image |

## Also in releases

GitHub releases include images for other FreeInk-supported boards:
X4 Classic, Paper Mono, Picco, Sticky, EEGO A4, and Metalio Eink4. The
website flasher currently serves the X4 Pro image only; other boards are
installed by copying the matching `.bin` to the SD card or flashing with
`esptool`.

## Not supported

| Device | Why | Instead |
| --- | --- | --- |
| **Xteink X3** | No PSRAM — Xerif's engine needs more memory than the X3 has | Use [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader), which supports the X3 |

## Check before buying

The X4 Pro ships with 16 MB flash / 8 MB PSRAM, a 4.3" 400×300 e-ink
panel with adjustable front light, touch, and buttons. Units bought
directly from the maker's own store are not locked against third-party
firmware; bought elsewhere, check the seller's listing first.
