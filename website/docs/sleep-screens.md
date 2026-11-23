# Sleep screens

The sleep screen is what the panel shows while the device rests. E-ink
holds an image without power, so this costs no battery.

## Modes

Under **Settings › Display › Sleep Screen**:

- **Dark / Light** — the Xerif mark on a dark or white background.
- **Custom** — your own image from the SD card.
- **Cover** — the cover of the book you were reading.
- **Cover + Custom** — your image, or the cover while actively reading.
- **Transparent** — a PNG/BMP overlay drawn over the current screen, so
  the page stays visible under a decorative frame.
- **Cover + Transparent** — the overlay over the last book cover.
- **None** — blank.

## Custom images

- **A folder of them (recommended):** create a `.sleep` folder at the root
  of the SD card and drop in any number of `.bmp` images — one is picked at
  random each time the device sleeps.
- **A single image:** a file named `sleep.bmp` at the root wins over the
  folder.

Covers and custom images can be fitted or cropped, with optional
greyscale, contrast, or inversion filters. PNG sleep screens (with alpha
transparency) are supported. The first sleep with a new image takes longer
while it is converted.
