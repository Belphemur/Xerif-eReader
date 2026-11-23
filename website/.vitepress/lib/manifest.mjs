// Builds the esp-web-tools manifest from the newest stable release.
// The released xerif-<version>-x4pro.bin is the application image, flashed at
// the app0 offset (ota_0 in partitions.csv) — the same single-part write the
// documented esptool command performs.
//
// Parts point at same-origin copies: GitHub release download URLs send no
// CORS headers, so a browser-side installer cannot fetch them directly. The
// flash-manifest plugin downloads each image at build time and emits it into
// the site output under firmware/.
//
// Erase safety: the release ships only the application image. In esp-web-tools
// 10.5.2, new_install_prompt_erase=false makes the installer erase the WHOLE
// chip before writing (src/install-dialog.ts: "Default is to erase a device
// that does not support Improv Serial" -> _startInstall(true)), which leaves
// the reader without a bootloader. With the flag on, the erase screen appears
// and erasing only happens when the user explicitly checks "Erase device" —
// the site copy tells owners to leave it unchecked, because no release asset
// can restore the erased bootloader/partition table.
import { latestX4ProRelease } from './releases.mjs'

export const APP_OFFSET = 0x10000
export const CHIP_FAMILY = 'ESP32-S3'
export const FIRMWARE_DIR = 'firmware'

/** The x4pro asset of a release, if any. */
export function x4ProAsset(release) {
  return release?.assets.find((a) => /-x4pro\.bin$/.test(a.name)) || null
}

/**
 * Same-origin path under which the build emits the release's firmware.
 * Namespaced per version: older releases shipped assets with identical
 * names (e.g. firmware-x4pro.bin), so the bare asset name would collide.
 */
export function firmwarePath(release) {
  const asset = x4ProAsset(release)
  if (!asset || !release?.version) return null
  return `${FIRMWARE_DIR}/${release.version}/${asset.name}`
}

/**
 * Builds a manifest pinned to one specific release, so the changelog can
 * offer a "Flash this version" button for older builds.
 * @param {import('./releases.mjs').Release} release
 */
export function buildVersionManifest(release) {
  const path = firmwarePath(release)
  return {
    name: `Xerif ${release?.version || ''}`,
    version: release?.version || '',
    new_install_prompt_erase: true,
    builds: path
      ? [
          {
            chipFamily: CHIP_FAMILY,
            parts: [{ path: `/${path}`, offset: APP_OFFSET }],
          },
        ]
      : [],
  }
}

/**
 * @param {import('./releases.mjs').Release[]} releases
 */
export function buildFlashManifest(releases) {
  const release = latestX4ProRelease(releases)
  const path = firmwarePath(release)
  return {
    name: 'Xerif',
    version: release?.version || '',
    new_install_prompt_erase: true,
    builds: path
      ? [
          {
            chipFamily: CHIP_FAMILY,
            parts: [{ path: `/${path}`, offset: APP_OFFSET }],
          },
        ]
      : [],
  }
}
