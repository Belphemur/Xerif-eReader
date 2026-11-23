// Builds the esp-web-tools manifest from the newest stable release.
// The released xerif-<version>-x4pro.bin is the application image, flashed at
// the app0 offset used by partitions.csv and the documented esptool command.
import { latestX4ProRelease } from './releases.mjs'

export const APP_OFFSET = 0x10000
export const CHIP_FAMILY = 'ESP32-S3'

/**
 * @param {import('./releases.mjs').Release[]} releases
 */
export function buildFlashManifest(releases) {
  const release = latestX4ProRelease(releases)
  const asset = release?.assets.find((a) => /-x4pro\.bin$/.test(a.name)) || null

  return {
    name: 'Xerif',
    version: release?.version || '',
    new_install_prompt_erase: true,
    builds: asset
      ? [
          {
            chipFamily: CHIP_FAMILY,
            parts: [{ path: asset.url, offset: APP_OFFSET }],
          },
        ]
      : [],
  }
}
