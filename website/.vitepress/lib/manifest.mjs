// Builds the esp-web-tools manifests from the newest stable release.
// The released xerif-<version>-<board>.bin is the application image, flashed
// at the app0 offset (ota_0 in partitions.csv) — the same single-part write
// the documented esptool command performs.
//
// Parts point at same-origin copies: GitHub release download URLs send no
// CORS headers, so a browser-side installer cannot fetch them directly. The
// flash-manifest plugin downloads each image at build time and emits it into
// the site output under firmware/.
//
// One build per chip family: esp-web-tools refuses a manifest that lists more
// than one build for the same chipFamily, and every PSRAM-class board is an
// ESP32-S3. The six boards therefore get six per-board manifests
// (/xerif-manifest-<board>.json) instead of one six-build manifest. The bare
// /xerif-manifest.json stays the X4 Pro image: the stable installer, the
// changelog's "Flash this version", and older bookmarks all point at it.
//
// Erase safety: the release ships only the application image. In esp-web-tools
// 10.5.2, new_install_prompt_erase=false makes the installer erase the WHOLE
// chip before writing (src/install-dialog.ts: "Default is to erase a device
// that does not support Improv Serial" -> _startInstall(true)), which leaves
// the reader without a bootloader. With the flag on, the erase screen appears
// and erasing only happens when the user explicitly checks "Erase device" —
// the site copy tells owners to leave it unchecked, because no release asset
// can restore the erased bootloader/partition table.
import { latestStableRelease } from './releases.mjs'

export const APP_OFFSET = 0x10000
export const CHIP_FAMILY = 'ESP32-S3'
export const FIRMWARE_DIR = 'firmware'

/** Board id -> human name (order matches PSRAM_BOARDS). */
export const BOARD_NAMES = {
  x4pro: 'Xteink X4 Pro',
  x4c: 'X4 Classic',
  metalio_eink4: 'Metalio E-Ink 4',
  papermono: 'Paper Mono',
  eego_a4: 'EEGO A4',
  picco: 'Picco',
}

/**
 * The PSRAM-class boards Xerif ships release images for — the S3 envs in
 * platformio.ini that define BOARD_HAS_PSRAM. Sticky is deliberately
 * PSRAM-less and the X3 (ESP32-C3) is CrossPoint's territory.
 */
export const PSRAM_BOARDS = [
  'x4pro',
  'x4c',
  'metalio_eink4',
  'papermono',
  'eego_a4',
  'picco',
]

/** The boards the /flash page lists as beta (everything but the stable X4 Pro). */
export const BETA_BOARDS = PSRAM_BOARDS.filter((board) => board !== 'x4pro')

/** The asset of a release for one board, if any. */
export function boardAsset(release, board) {
  return (
    release?.assets.find((a) => a.name.endsWith(`-${board}.bin`)) || null
  )
}

/** The x4pro asset of a release, if any. */
export function x4ProAsset(release) {
  return boardAsset(release, 'x4pro')
}

/**
 * Same-origin path under which the build emits a release's firmware.
 * Namespaced per version: older releases shipped assets with identical
 * names (e.g. firmware-x4pro.bin), so the bare asset name would collide.
 */
export function firmwarePath(release, board = 'x4pro') {
  const asset = boardAsset(release, board)
  if (!asset || !release?.version) return null
  return `${FIRMWARE_DIR}/${release.version}/${asset.name}`
}

/**
 * Builds a manifest pinned to one specific release, so the changelog can
 * offer a "Flash this version" button for older builds.
 * @param {import('./releases.mjs').Release} release
 * @param {string[]} [boards] board ids to include (default: X4 Pro only)
 */
export function buildVersionManifest(release, boards = ['x4pro']) {
  return {
    name: `Xerif ${release?.version || ''}`,
    version: release?.version || '',
    new_install_prompt_erase: true,
    builds: boards.flatMap((board) => {
      const path = firmwarePath(release, board)
      return path
        ? [
            {
              chipFamily: CHIP_FAMILY,
              parts: [{ path: `/${path}`, offset: APP_OFFSET }],
            },
          ]
        : []
    }),
  }
}

/**
 * Manifest for the newest stable release carrying images, for one board.
 * One build per manifest — see the chip-family note at the top.
 * @param {import('./releases.mjs').Release[]} releases
 * @param {string} board
 */
export function buildBoardManifest(releases, board) {
  const release = latestStableRelease(releases)
  const path = firmwarePath(release, board)
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
 * Default flash manifest: the newest stable release's X4 Pro image.
 * @param {import('./releases.mjs').Release[]} releases
 */
export function buildFlashManifest(releases) {
  return buildBoardManifest(releases, 'x4pro')
}
