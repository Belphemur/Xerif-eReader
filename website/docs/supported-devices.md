# Supported devices

Xerif targets e-readers with RAM to spare for its rendering engine
(PSRAM-equipped boards).

## Supported

| Device | Status |
| --- | --- |
| **Xteink X4 Pro** | Fully supported — the stable installer on this site's [flasher](/flash) is built for it |
| **X4 Classic** | Supported (beta) — beta installer on the [flasher](/flash) |
| **Metalio E-Ink 4** | Supported (beta) — beta installer on the [flasher](/flash) |
| **Paper Mono** | Supported (beta) — beta installer on the [flasher](/flash) |
| **EEGO A4** | Supported (beta) — beta installer on the [flasher](/flash) |
| **Picco** | Supported (beta) — beta installer on the [flasher](/flash) |

Beta means best-effort: the installers work, but these boards get less
testing than the X4 Pro — report problems on
[GitHub](https://github.com/Belphemur/Xerif-eReader/issues).

## Also in releases

GitHub releases also include an image for the Sticky (deliberately built
without PSRAM). It is not served by this site's flasher; install it by
copying the `.bin` to the SD card or flashing with `esptool`.

## Not supported

| Device | Why | Instead |
| --- | --- | --- |
| **Xteink X3** | No PSRAM — Xerif's engine needs more memory than the X3 has | Use [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader), which supports the X3 |

## Check before buying

The X4 Pro ships with 16 MB flash / 8 MB PSRAM, a 4.3" 400×300 e-ink
panel with adjustable front light, touch, and buttons. Units bought
directly from the maker's own store are not locked against third-party
firmware; bought elsewhere, check the seller's listing first.
