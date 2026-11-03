# SD Card Fonts

CrossPoint supports loading additional fonts from the SD card, including fonts
with extended Unicode coverage (CJK, Cyrillic, Greek, etc.).

The font releases include a `LICENSE` file with the original font notices.
If you distribute font copies, keep the applicable notices with those copies.
The source notices are in [licenses/fonts.txt](../licenses/fonts.txt).

## Installing Fonts

There are three ways to install fonts:

### Option 1: Download from device (recommended)

1. Connect your CrossPoint reader to Wi-Fi
2. Go to **Settings > Reader > Manage Fonts**
3. Browse available font families and tap to download
4. Downloaded fonts appear immediately in **Settings > Reader > Font Family**

### Option 2: Upload via web browser

1. Start **File Transfer** and connect through **Join Network** or **Create Hotspot**
2. Open the web interface URL shown on the reader
3. Navigate to the **Fonts** tab
4. Upload `.cpfont` files using the upload form

### Option 3: Manual SD card copy

1. Download font files from the
   [crosspoint-fonts repository](https://github.com/crosspoint-reader/crosspoint-fonts)
2. Copy font family folders to one of two locations on your SD card:

   - `/.fonts/` — hidden directory (preferred; keeps the SD root tidy
     when mounted on a desktop)
   - `/fonts/` — visible directory (use this if your OS hides dot-files
     and you'd rather see the folder in your file manager)

   Both roots are always scanned at boot. Native TTF/OTF families are
   merged across roots: a family installed in `/fonts/` shows up even when
   `/.fonts/` also exists, and vice versa. If the same family name appears
   in both, the family is merged: `/.fonts/` wins for any style it
   provides, and styles available only in `/fonts/` are added to the same
   family. `.cpfont` bundles are different: the bundled-font registry
   de-duplicates by family name and keeps the `/.fonts/` copy when the
   same name exists in both roots (files from `/fonts/` are not merged
   into that family).

       SD Card Root/
       ├── .fonts/                     ← Hidden root (preferred)
       │   └── Literata/
       │       ├── Literata_12.cpfont
       │       ├── Literata_14.cpfont
       │       ├── Literata_16.cpfont
       │       └── Literata_18.cpfont
       └── fonts/                      ← Visible root (equally valid)
           └── Merriweather/
               ├── Merriweather_12.cpfont
               └── ...

3. Insert the SD card and power on your CrossPoint reader

## Non-Latin Scripts Fallback in the User Interface

The built-in UI fonts are Latin-only, so by default the interface (book titles
in the library, file names in the browser, list rows, headers) shows
replacement boxes for non-Latin text — CJK, Greek, Cyrillic, Armenian, and any
other script the SD-font converter ships an interval for — even when book
*content* renders correctly with a selected SD-card font.

To avoid shipping every script's glyph set in flash, CrossPoint instead reuses
the SD-card font you already selected: when a UI string contains a character
the built-in font cannot draw, that whole string is rendered with your selected
SD-card font instead.

The fallback is **size-matched**. The built-in UI fonts render at 8 pt
(small/author lines), 10 pt (list rows) and 12 pt (book-cover titles, headers),
so CrossPoint loads your SD family at those sizes too and maps each UI font to
its same-size SD font. Non-Latin book names therefore appear at the same size
as the Latin text around them. A `.cpfont` family must contain files at sizes
**8, 10 and 12** for this UI fallback (in addition to the reader sizes 12–18) —
**missing these is the most common reason non-Latin UI text still shows
replacement boxes even though the book itself renders fine.** Any missing UI
size keeps showing boxes for that script at that size.

These UI sizes only take effect if the reader-size font also matches one of
CrossPoint's built-in fallback-detection codepoints — see `kFallbackProbes` in
`src/SdCardFontSystem.cpp` (and the same list in the native-TTF reader's
`src/TtfUiFallback.cpp`) for the current codepoints. A script outside that list
gets no UI fallback even with the correct `.cpfont` sizes present.

Note that **Settings > Reader > Font Size** lists every size the family ships,
so a family built at 8,10,12,14,16,18 offers all six as reading sizes — the UI
sizes are not hidden from the list. Reading at 8 pt is your call; if you would
rather not see the small sizes there, convert two families (one with the UI
sizes for fallback, one with only the reading sizes you want).

When converting your own font, include the UI sizes:

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyCJKFont-Regular.otf \
      --intervals cjk \
      --sizes 8,10,12,14,16,18 \
      --style regular \
      --name MyCJKFont \
      --output-dir ./MyCJKFont/

What this means in practice:

- Select a non-Latin-capable SD font under **Settings > Reader > Font Family**
  (see [Installing Fonts](#installing-fonts) and the interval presets for
  `.cpfont` under [Converting Custom Fonts](#converting-custom-fonts)). That
  single selection drives both book content *and* size-matched fallback in
  the UI.
- Pure-Latin UI strings keep the crisp built-in font; only strings that
  actually contain a character the built-in font lacks are routed to the SD
  font.
- The fallback is per *string*, not per glyph: a mixed title such as
  `三体 Vol.1` renders entirely in the SD font (including the Latin part). If
  that SD font is a `Mono` family, the Latin portion will appear half/full
  width.
- If no SD font is selected (a built-in reading font is active), there is no
  fallback and the UI again shows boxes for non-Latin text — pick an SD font
  covering that script to restore it.

## Available Pre-Built Fonts

The current list of pre-built fonts is maintained in the
[crosspoint-fonts repository](https://github.com/crosspoint-reader/crosspoint-fonts).

## Converting Custom Fonts

To convert your own TrueType/OpenType fonts:

### Prerequisites

    pip install freetype-py fonttools

### Single font (one style)

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyFont-Regular.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --style regular \
      --name MyFont \
      --output-dir ./MyFont/

### Multi-style font

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      --regular MyFont-Regular.ttf \
      --bold MyFont-Bold.ttf \
      --italic MyFont-Italic.ttf \
      --bolditalic MyFont-BoldItalic.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --name MyFont \
      --output-dir ./MyFont/

> **Using this font for non-Latin UI fallback too?** The examples above only
> include the reader sizes (12–18). To also get fallback for this font's
> script in the library, headers, and file browser (see
> [Non-Latin Scripts Fallback in the User Interface](#non-latin-scripts-fallback-in-the-user-interface)),
> add the UI sizes: `--sizes 8,10,12,14,16,18`.

### Available Unicode interval presets

| Preset | Coverage |
|--------|----------|
| `ascii` | U+0020–U+007E (Basic Latin) |
| `latin1` | U+0080–U+00FF (Latin-1 Supplement) |
| `latin-ext` | European languages (Latin + Extended-A/B + punctuation + currency + letterlike symbols such as ™ and № + minus sign + ligatures) |
| `greek` | Greek + Extended Greek |
| `cyrillic` | Cyrillic + Supplement |
| `hebrew` | Hebrew + Alphabetic Presentation Forms |
| `arabic` | Arabic + Supplement + Extended-A + Presentation Forms A/B (RTL, contextual shaping) |
| `georgian` | Georgian + Georgian Supplement |
| `armenian` | Armenian |
| `ethiopic` | Ethiopic + Extended |
| `vietnamese` | Vietnamese subset (ơ/ư and combining marks) |
| `ipa-chars` | IPA Extensions + Spacing Modifier Letters (phonetic transcription) |
| `punctuation` | General punctuation (U+2000–U+206F) |
| `cjk` | CJK Unified Ideographs + Hiragana + Katakana + Fullwidth |
| `hangul` | Korean Hangul syllables + Jamo + Compatibility Jamo |
| `cherokee` | Cherokee (historic + supplement block) |
| `tifinagh` | Tifinagh |
| `symbols` | Math, currency, arrows, box-drawing, misc symbols, dingbats |
| `reading` | Literary fiction coverage: Latin, Greek, Cyrillic, math/symbol blocks, supplemental punctuation, and CJK quote marks |
| `builtin` | Matches the firmware's built-in font conversion intervals |

Combine presets with commas: `--intervals latin-ext,greek,cyrillic`

You can also specify arbitrary Unicode ranges directly:
`--intervals 'latin-ext,(0x2190-0x21FF)'`

To list all presets with codepoint counts:

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py --list-presets

### Additional options

`--force-autohint` — force FreeType's auto-hinter instead of the font's native hinting (useful when a font's built-in hints produce poor results at small sizes).

Install custom fonts via the web interface or manual SD card copy.
