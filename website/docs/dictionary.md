# Dictionary lookup

Look up words and phrases while reading, using an offline
[StarDict](https://en.wikipedia.org/wiki/StarDict) dictionary stored on the
SD card — no internet needed on the device.

Content in this page derives from the
[CrossPoint dictionary documentation](https://github.com/crosspoint-reader/crosspoint-reader/blob/develop/docs/dictionary.md).

## Get a dictionary

Xerif uses **StarDict** dictionaries: look for "StarDict format" downloads —
files with `.dict`, `.idx`, and `.ifo` extensions. Good sources:

- [reader.dict](https://www.reader-dict.com) — daily Wiktionary dumps for
  many languages (monolingual and translation dictionaries).
- [WikDict](https://www.wikdict.com) — free bilingual dictionaries built
  from Wiktionary ([downloads](https://download.wikdict.com/dictionaries/stardict/)).
- [Wiktionary-Dictionaries](https://github.com/Vuizur/Wiktionary-Dictionaries) —
  many languages to English, including English–English.

A dictionary folder must contain an **uncompressed `.idx`** word index
(decompress a `.idx.gz` on your computer first). A `.dict.dz` works as-is —
but if the device reports "Not enough memory", unzip it. An optional `.syn`
file adds synonym support (e.g. `oxen` → `ox`).

## Set it up

1. Copy the dictionary folder to `/dictionaries/` on the SD card — one
   dictionary per folder, e.g. `/dictionaries/webster/webster.idx` +
   `webster.dict.dz`.
2. Open **Settings › Reader › Dictionary** on the device and pick the
   dictionary (or **None** to disable).

The Dictionary setting only appears when at least one usable dictionary
folder exists on the card.

## Looking up words

While reading, two ways to start:

- Open the reader menu (**Confirm**) and choose **Look Up**.
- Or assign Dictionary to a long press under **Settings › Controls**, then
  hold the button on a reading page.

**On touchscreen devices:** tap or drag the handles to highlight a word or
phrase, then tap **Look Up**.

**On button devices:** a cursor marks a word — navigate with **Left/Right**
and the side **Up/Down** buttons, press **Confirm** to anchor the start of a
selection, extend it across words, lines, or pages, then press **Confirm**
again to look up. **Back** cancels.

The first lookup with a dictionary builds small index sidecars next to it
("Indexing dictionary…" for a few seconds on large dictionaries). After
that, lookups are fast.

## How the match works

1. **Direct match** — case-insensitive search; leading/trailing punctuation
   is stripped automatically.
2. **Synonyms** — with a `.syn` file, alternate spellings and irregular
   forms resolve to their headword (`oxen` → `ox`, `colour` → `color`).
3. **Stemming** — common English forms are retried automatically
   (`stories` → `story`, `walked` → `walk`, `running` → `run`).
4. **Not found** — a short popup appears and you return to reading.

Multi-word phrases match dictionary entries containing spaces (idioms,
phrasal verbs). If a headword has several entries, they are all shown.

## The definition screen

Definitions render in your reading font, with formatting support for
headings, bold, italics, and lists where the dictionary provides it. Long
definitions are paginated.

- **Left/Right** (or side **Up/Down**) — previous / next page.
- **Confirm** — select a word inside the definition and look *it* up; the
  reader keeps up to 16 previous definitions, and **Back** steps back
  through them.
- **Long Back** (hold ~1 s) — close all definitions and return directly to
  reading.

IPA pronunciation symbols (ˈ ː ɹ…) are not covered by the built-in fonts —
install a [custom font](/docs/custom-fonts) that includes them to see
those glyphs.

## Vocabulary history

Successful lookups are appended to `/Vocabulary.txt` on the SD card — one
JSON record per line with the word, the matched headword, the definition,
and where in the book you looked it up. Failed lookups are not recorded,
and lookups never create highlights.
