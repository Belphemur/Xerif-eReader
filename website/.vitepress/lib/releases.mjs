// Fetches Xerif release metadata from the GitHub releases API at build time.
// The result is cached on disk so a build does not hammer the API (the flash
// manifest plugin and the changelog data loader both read through here).
import { mkdirSync, readFileSync, writeFileSync, existsSync } from 'node:fs'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = dirname(fileURLToPath(import.meta.url))
const CACHE_DIR = resolve(HERE, '..', 'cache')
const CACHE_FILE = resolve(CACHE_DIR, 'releases.json')
const TTL_MS = 60 * 60 * 1000

export const GITHUB_REPO = 'Belphemur/Xerif-eReader'

/** @typedef {{ name: string, size: number, url: string }} ReleaseAsset */
/** @typedef {{ tag: string, version: string, name: string, url: string, publishedAt: string, body: string, assets: ReleaseAsset[] }} Release */

/** Keep only published stable releases and normalise GitHub's shape. */
export function normalizeReleases(raw) {
  return (Array.isArray(raw) ? raw : [])
    .filter((r) => r && !r.draft && !r.prerelease)
    .map((r) => ({
      tag: String(r.tag_name || ''),
      version: String(r.tag_name || '').replace(/^v/, ''),
      name: r.name || r.tag_name || '',
      url: r.html_url || `https://github.com/${GITHUB_REPO}/releases/tag/${r.tag_name}`,
      publishedAt: r.published_at || '',
      body: r.body || '',
      assets: (Array.isArray(r.assets) ? r.assets : []).map((a) => ({
        name: a.name || '',
        size: a.size || 0,
        url: a.browser_download_url || '',
      })),
    }))
}

const PER_PAGE = 100
const MAX_PAGES = 10

async function fetchFromGitHub() {
  const headers = {
    Accept: 'application/vnd.github+json',
    'User-Agent': 'xerif-website-build',
  }
  if (process.env.GITHUB_TOKEN) {
    headers.Authorization = `Bearer ${process.env.GITHUB_TOKEN}`
  }
  // Follow pagination: with per_page=40 the changelog silently dropped every
  // release past the first page. Stop when a page comes back short (last
  // page) or at a sane cap; the combined list is cached as a whole.
  const releases = []
  for (let page = 1; page <= MAX_PAGES; page++) {
    const res = await fetch(
      `https://api.github.com/repos/${GITHUB_REPO}/releases?per_page=${PER_PAGE}&page=${page}`,
      { headers },
    )
    if (!res.ok) {
      throw new Error(`GitHub releases API responded ${res.status} ${res.statusText}`)
    }
    const raw = await res.json()
    releases.push(...normalizeReleases(raw))
    if (!Array.isArray(raw) || raw.length < PER_PAGE) break
  }
  return releases
}

function readCache() {
  try {
    if (!existsSync(CACHE_FILE)) return null
    const parsed = JSON.parse(readFileSync(CACHE_FILE, 'utf8'))
    if (!parsed || !Array.isArray(parsed.releases)) return null
    return parsed
  } catch {
    return null
  }
}

function writeCache(releases) {
  try {
    mkdirSync(CACHE_DIR, { recursive: true })
    writeFileSync(
      CACHE_FILE,
      JSON.stringify({ savedAt: new Date().toISOString(), releases }, null, 2) + '\n',
    )
  } catch (err) {
    console.warn(`[xerif] could not write release cache: ${err.message}`)
  }
}

let memory = null

/**
 * Returns stable releases, newest first.
 * Order: in-process cache -> fresh disk cache -> GitHub API -> stale disk cache.
 * Never throws: an offline build degrades to the stale cache or an empty list.
 *
 * @param {{ force?: boolean }} [opts]
 * @returns {Promise<Release[]>}
 */
export async function getReleases({ force = false } = {}) {
  if (memory && !force) return memory

  const cached = readCache()
  if (!force && cached) {
    const age = Date.now() - Date.parse(cached.savedAt)
    if (Number.isFinite(age) && age >= 0 && age < TTL_MS) {
      memory = cached.releases
      return memory
    }
  }

  try {
    memory = await fetchFromGitHub()
    writeCache(memory)
    return memory
  } catch (err) {
    if (cached) {
      console.warn(`[xerif] releases API unavailable (${err.message}); using cached data`)
      memory = cached.releases
      return memory
    }
    console.warn(`[xerif] releases API unavailable (${err.message}); no cache present`)
    memory = []
    return memory
  }
}

/**
 * Latest stable release that carries a PSRAM-board image. The X4 Pro asset
 * stays the marker for "this release is installable": CI publishes the six
 * PSRAM-board images together, so picking the newest x4pro carrier keeps the
 * default manifest and the beta manifests pointing at the same release.
 */
export function latestStableRelease(releases) {
  return releases.find((r) => r.assets.some((a) => /-x4pro\.bin$/.test(a.name))) || null
}
