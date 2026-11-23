# Plugins

Plugins extend the firmware without rebuilding it. They live on the SD
card and are written in JavaScript.

## How it works

- Put a plugin's files in the `plugins/` folder on the SD card (each
  plugin in its own subfolder with a small `plugin.js` plus a manifest).
- Enable the plugin system and install plugins through **Plugin Hub** in
  the web interface or on-device screens.
- Plugins can add web pages served by the reader, hook into the plugin
  job queue (for automation, e.g. from scripts), and register on-device
  catalog screens.

## Examples of what plugins do

Small utilities like dictionaries or extra file-management helpers run as
plugins; community plugins are distributed as SD-card content.

## For developers

The plugin event API, job-queue protocol, and manifest format are
documented in
[docs/sd-plugins.md](https://github.com/Belphemur/Xerif-eReader/blob/develop/docs/sd-plugins.md)
and
[docs/plugin-events.md](https://github.com/Belphemur/Xerif-eReader/blob/develop/docs/plugin-events.md)
in the repository.
