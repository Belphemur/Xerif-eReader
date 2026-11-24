// Build-time data loader for the /flash page: pins the newest stable release
// carrying Xerif images so the page never needs a manual version bump.
import { getReleases, latestStableRelease } from './.vitepress/lib/releases.mjs'
import { PSRAM_BOARDS } from './.vitepress/lib/manifest.mjs'

// Per-board SD images of the newest stable release, so the /flash SD section
// can download the selected board's own image instead of always the X4 Pro.
function boardBin(latest, board) {
  const asset = latest?.assets.find((a) => a.name.endsWith(`-${board}.bin`)) || null
  if (!asset) return null
  return {
    url: asset.url,
    name: asset.name,
    sizeMB: Math.round((asset.size / (1024 * 1024)) * 10) / 10,
  }
}

export default {
  async load() {
    const releases = await getReleases()
    const latest = latestStableRelease(releases)
    const asset = latest?.assets.find((a) => /-x4pro\.bin$/.test(a.name)) || null

    if (!latest || !asset) {
      return {
        version: '',
        binUrl: '',
        binName: '',
        sizeMB: 0,
        date: '',
        releaseUrl: '',
        boardBins: {},
      }
    }

    return {
      version: latest.version,
      binUrl: asset.url,
      binName: asset.name,
      sizeMB: Math.round((asset.size / (1024 * 1024)) * 10) / 10,
      date: latest.publishedAt,
      releaseUrl: latest.url,
      boardBins: Object.fromEntries(
        PSRAM_BOARDS.map((board) => [board, boardBin(latest, board)]),
      ),
    }
  },
}
