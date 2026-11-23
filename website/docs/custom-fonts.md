# Custom fonts

Xerif reads any `.ttf`, `.otf`, or `.ttc` file you place on the SD card —
no conversion needed.

## Install a font

Three ways:

1. **From the device:** open **Settings › Reader › Manage Fonts** (the
   built-in Font Browser) and download a font directly to the SD card (the
   firmware fetches it for you).
2. **Web browser:** while the reader is in File Transfer mode, open the web
   interface and upload the font on the fonts page.
3. **Manual copy:** put the file in the `fonts/` folder on the SD card.

Then pick the family in the reader's **Text Settings** screen: open a book,
open the reader menu, choose **Text Settings**, and use the **Font** tab.

## What you get

- Real kerning, ligatures, and smooth anti-aliased rendering with any
  TrueType family.
- Fractional point sizes instead of fixed bitmap sizes.
- The layout is cached per section, so the first page of a book takes a
  moment and the rest is fast.

## Non-Latin scripts

Fallbacks cover the user interface for scripts without a full font on the
card (including CJK and Indic ranges). Drop a font covering your script on
the SD card to get proper book rendering.
