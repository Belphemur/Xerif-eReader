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
3. Check **Send Metadata**: it is **on by default** and sends the book's
   file name, title, and authors with each progress sync. On servers that
   support extended metadata it also sends ISBN, ASIN, and series data
   where the book's metadata provides them. Turn it off if you'd rather
   share progress only.

   Note: a saved sync configuration that predates this option (or a config
   file without the key) loads with Send Metadata off, so an upgrade can
   silently change the behavior until you re-check it.

## Syncing while reading

Progress syncs when you close a book or leave the reader, and can sync from
the reader menu. If you use the same account on another device, both stay
on the same page.
