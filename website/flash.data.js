// Build-time data loader for the /flash page: pins the newest stable release
// carrying Xerif images so the page never needs a manual version bump.
import { getReleases, latestStableRelease } from './.vitepress/lib/releases.mjs'

export default {
  async load() {
    const releases = await getReleases()
    const latest = latestStableRelease(releases)
    const asset = latest?.assets.find((a) => /-x4pro\.bin$/.test(a.name)) || null

    if (!latest || !asset) {
      return { version: '', binUrl: '', binName: '', sizeMB: 0, date: '', releaseUrl: '' }
    }

    return {
      version: latest.version,
      binUrl: asset.url,
      binName: asset.name,
      sizeMB: Math.round((asset.size / (1024 * 1024)) * 10) / 10,
      date: latest.publishedAt,
      releaseUrl: latest.url,
    }
  },
}
