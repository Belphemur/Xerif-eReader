# KOReader sync

Xerif syncs reading progress with KOReader-compatible servers — useful if
you read the same book on another KOReader device or app, or simply want
your position backed up.

## Quick setup

Under **Settings › Web Settings › KOReader Sync**:

1. Pick a **Server Type**:
   - **CrossPoint** — the default service (`sync.crosspointreader.com`) or a
     self-hosted CrossPoint-compatible server.
   - **KoSync** — standard KOReader Sync servers such as
     `sync.koreader.rocks`.
   - **Other** — custom servers that explicitly support CrossPoint's richer
     progress payload.
2. Enter the server address and your credentials.
3. Enable **Send Metadata** only if you're comfortable sharing book title,
   author, and (where recognized) ISBN/ASIN and series information. It is
   off by default for privacy.

## Syncing while reading

Progress syncs when you close a book or leave the reader, and can sync from
the reader menu. If you use the same account on another device, both stay
on the same page.
