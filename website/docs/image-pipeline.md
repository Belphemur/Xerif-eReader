# How images are prepared

E-ink screens are great for text and slow for surprises. Showing a photo or a
book cover on the X4 Pro's panel is genuinely hard — the screen can only show
**four shades** (white, light gray, dark gray, black), and decoding a picture
takes seconds on a small device. Xerif solves both problems with a pipeline
that does all the heavy work *before* you turn the page. This page explains it
in plain words; the full engineering record is the
[image pipeline ADR](https://github.com/Belphemur/Xerif-eReader/blob/develop/docs/design/2026-11-25-image-pipeline-adr.md)
(architecture decision record) in the repository.

## The four-shade problem

Your X4 Pro's e-ink panel cannot show a photo. It can show only white, light
gray, dark gray, and black. What it *can* show is a clever pattern of dots —
like a newspaper photo printed in four inks — that your eye blends into a
smooth picture from reading distance.

Deciding which of the four shades each dot gets is called **dithering**. Pick
the pattern badly and you get muddy midtones, checkered flat areas, or thin
lines in drawings that vanish entirely.

## The dot pattern: blue noise

Xerif decides each dot with a fixed 64×64 **blue-noise** table — a pattern
tuned so the dots look like fine film grain instead of regular checkerboards.
Two things make it a good fit for an e-reader:

- **It is the same every time.** The pattern carries no memory of previous
  pixels, so the background processor and the emergency "no cache" path
  produce *bit-for-bit identical* results, and rotating the device never
  shifts the texture.
- **It won a blind taste test.** The dot patterns were compared side by side
  in a blind vote against every alternative (including the previous
  algorithms); the blue-noise bake won every matchup.

The math detail that makes the shades land right: brightness is measured in
*linear light* (how much light a pixel actually represents) before comparing
to the panel's real four shades, so an image's average brightness comes out
matching the screen's average brightness. An earlier approach that adjusted
brightness in the eye-friendlier gamma domain collapsed every midtone into
the two lightest shades — the washed-out look — so it was dropped.

## Keeping thin lines alive: the resize

Images are usually bigger than the screen, so they must be shrunk. The
obvious way — averaging each little square of the original — has a hidden
cost: a thin dark line gets averaged with its white surroundings and fades
into a pale gray that dithering then rounds back to white. The line
disappears.

Xerif instead uses a **detail-preserving resizer** (DPID, a published
algorithm by Nehab and colleagues) that gives more influence to pixels that
differ from their neighborhood — exactly the thin dark line — so strokes
inside illustrations survive the shrink. Shrink first, then dither: doing any
of this *after* the dot pattern is painted would destroy it.

When memory is tight, the resize steps down a ladder — full detail → less
detail → plain average — but a degraded result is never saved to the cache;
the next attempt re-decodes the image and hopes for a better moment.

## The waiting game: prepare ahead, blit later

Decoding a photo on an e-reader's processor takes seconds — the original
firmware froze the page turn for 3.5–4.5 seconds the first time you opened an
image page. Xerif moves that work into the background:

1. **While you read**, a background worker notices which images are coming up
   (including on the next chapter), decodes them — JPEG, PNG, GIF (first
   frame), and BMP are all handled — resizes them, and applies the dot
   pattern.
2. **The result is saved** on the SD card as a small cache file (`.pxc`,
   about 2 bits per pixel), stamped with a fingerprint of exactly what
   produced it: which book page, which image, which placement, which bake.
3. **When you turn to that page**, the device simply copies the saved dots to
   the screen — no decoding, no waiting. If the fingerprint doesn't match
   (you changed layout, or the cache is stale), the file is thrown out and
   rebuilt; you pay the cost once, invisibly, while the worker re-prepares.
4. **The screen shows the four shades through a special gray waveform** so
   they actually appear as four distinct shades instead of collapsing to
   black and white. Pages with images ride this route; text-only pages keep
   the standard fast path.

The same preparation and fingerprint system also serves the sleep-screen
cover, the home screen's Continue-Reading thumbnail, and library covers — one
pipeline, one quality bar, and a file with the wrong fingerprint is never
trusted.

## One decoder for JPEG

Earlier firmware carried two half-maintained JPEG decoders. Today every JPEG
(baseline, progressive, even damaged files) goes through a single decoder
(JPEGDEC) vendored into the shared FreeInk SDK, so a decoder bug is fixed
once, where automated tests can actually run.

## If you remember one thing

**Prepare in the background → one high-quality dot pattern → instant copy at
page turn.** And if an image ever looks washed out on your device, the first
suspect is the display route (which waveform the page rode), not the dot
pattern — that's how the developers triage it too.
