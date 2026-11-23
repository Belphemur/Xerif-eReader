# Image & cover rendering

Xerif treats the e-ink panel as a print medium: images are adapted to the
panel's greyscale at render time instead of being dumped on it raw.

## What you notice

- **Sharper images and covers.** Embedded JPG, PNG, and GIF images are
  processed with detail-preserving tone shaping and dithering sized for the
  panel, so artwork and book covers come out crisp instead of muddy.
- **Fast page turns stay fast.** Image processing happens at cache time;
  re-opening a book reuses the prepared result.
- Library and Home screens show rendered cover thumbnails, and reading
  progress (percent, bar, time left) appears under the current book.

## Adjusting

- **Settings › Reader › Images** controls how embedded images are shown.
- Cover display options (fit/crop, filters) live with the sleep-screen
  settings — see [Sleep screens](/docs/sleep-screens).

## Technical details

The tone-bake pipeline (blue-noise dithering, edge-aware diffusion,
per-image caches) is documented in the repository's design notes on
[GitHub](https://github.com/Belphemur/Xerif-eReader/tree/develop/docs/design).
