import { defineConfig } from 'vitepress'
import { getReleases, latestStableRelease } from './lib/releases.mjs'
import {
  PSRAM_BOARDS,
  boardAsset,
  buildBoardManifest,
  buildFlashManifest,
  buildVersionManifest,
  firmwarePath,
} from './lib/manifest.mjs'
import { vendorEspWebTools } from './lib/vendor-esp-web-tools.mjs'

// Self-host the WebSerial installer component (no third-party CDN at runtime).
vendorEspWebTools()

const GITHUB = 'https://github.com/Belphemur/Xerif-eReader'
const CROSSPOINT = 'https://github.com/crosspoint-reader/crosspoint-reader'
const SUPPORT_CROSSPOINT = 'https://app.royalty.dev/crosspoint-reader/crosspoint-reader'

/**
 * Serves the esp-web-tools manifest for the newest stable release.
 * In `build` it is emitted as a static asset; in `dev` it is served by a
 * middleware so /flash works without a prior build.
 */
function flashManifestPlugin() {
  let source = ''
  let isBuild = false
  return {
    name: 'xerif-flash-manifest',
    configResolved(config) {
      isBuild = config.command === 'build'
    },
    async buildStart() {
      const releases = await getReleases()
      const flash = buildFlashManifest(releases)
      // A build must never ship a flasher with nothing to install: an empty
      // release list means the API failed and no cache exists. Dev stays
      // tolerant so /flash can be worked on offline.
      if (isBuild && (releases.length === 0 || flash.builds.length === 0)) {
        this.error(
          '[xerif] no stable releases available (API failure without cache) — refusing to emit an empty flash manifest',
        )
      }
      source = JSON.stringify(flash, null, 2) + '\n'
      // emitFile is not available while serving (dev/preview).
      if (isBuild) {
        this.emitFile({ type: 'asset', fileName: 'xerif-manifest.json', source })
        // One manifest per PSRAM board for the latest release: esp-web-tools
        // refuses a manifest with several builds for the same chip family, and
        // all six boards are ESP32-S3.
        for (const board of PSRAM_BOARDS) {
          this.emitFile({
            type: 'asset',
            fileName: `xerif-manifest-${board}.json`,
            source: JSON.stringify(buildBoardManifest(releases, board), null, 2) + '\n',
          })
        }
        // Per-release manifests for the changelog's "Flash this version", plus
        // the same-origin firmware binaries they point at (GitHub download
        // URLs send no CORS headers, so the browser must fetch from this site).
        // The release the board manifests resolve through latestStableRelease()
        // also emits the five beta-board images the /flash beta section
        // installs; other releases keep X4 Pro only — the beta section always
        // flashes that release, and six images for every release would push
        // ~1.2 GB into the site output.
        const stable = latestStableRelease(releases)
        for (const release of releases) {
          const boards = release === stable ? PSRAM_BOARDS : ['x4pro']
          const manifest = buildVersionManifest(release, boards)
          if (manifest.builds.length === 0) continue
          for (const board of boards) {
            const asset = boardAsset(release, board)
            if (!asset) continue
            const res = await fetch(asset.url, {
              headers: { 'User-Agent': 'xerif-website-build' },
            })
            if (!res.ok) {
              this.error(
                `[xerif] downloading ${asset.name} failed: ${res.status} ${res.statusText}`,
              )
            }
            this.emitFile({
              type: 'asset',
              fileName: firmwarePath(release, board),
              source: Buffer.from(await res.arrayBuffer()),
            })
          }
          this.emitFile({
            type: 'asset',
            fileName: `xerif-manifest-${release.version}.json`,
            source: JSON.stringify(
              buildVersionManifest(release, ['x4pro']),
              null,
              2,
            ) + '\n',
          })
        }
      }
    },
    configureServer(server) {
      let versionManifests = {}
      let boardManifests = {}
      server.middlewares.use((req, res, next) => {
        const path = (req.url || '').split('?')[0]
        if (path === '/xerif-manifest.json') {
          res.setHeader('Content-Type', 'application/json; charset=utf-8')
          res.setHeader('Cache-Control', 'no-cache')
          res.end(source || JSON.stringify({ name: 'Xerif', builds: [] }))
          return
        }
        const boardMatch = path.match(/^\/xerif-manifest-([a-z0-9_]+)\.json$/)
        if (boardMatch && boardManifests[boardMatch[1]]) {
          res.setHeader('Content-Type', 'application/json; charset=utf-8')
          res.setHeader('Cache-Control', 'no-cache')
          res.end(boardManifests[boardMatch[1]])
          return
        }
        const match = path.match(/^\/xerif-manifest-(\d+\.\d+\.\d+)\.json$/)
        if (match) {
          res.setHeader('Content-Type', 'application/json; charset=utf-8')
          res.setHeader('Cache-Control', 'no-cache')
          res.end(versionManifests[match[1]] || '{}')
          return
        }
        next()
      })
      getReleases({ force: true }).then((releases) => {
        versionManifests = Object.fromEntries(
          releases
            .map((r) => [r.version, buildVersionManifest(r)])
            .filter(([, m]) => m.builds.length > 0)
            .map(([v, m]) => [v, JSON.stringify(m)]),
        )
        boardManifests = Object.fromEntries(
          PSRAM_BOARDS.map((board) => [
            board,
            JSON.stringify(buildBoardManifest(releases, board)),
          ]),
        )
      })
    },
  }
}


export default defineConfig({
  lang: 'en-US',
  title: 'Xerif',
  titleTemplate: ':title — Xerif',
  description:
    'Free, open-source software that makes your e-reader faster and sharper.',
  cleanUrls: true,
  appearance: true,
  lastUpdated: false,
  srcExclude: ['**/node_modules/**', '**/README.md', 'DESIGN.md', 'FEATURES.md'],
  sitemap: { hostname: 'https://xerif.aaflalo.me' },
  head: [
    ['link', { rel: 'icon', type: 'image/svg+xml', href: '/favicon.svg' }],
    ['link', { rel: 'apple-touch-icon', href: '/logo.svg' }],
    ['meta', { name: 'theme-color', content: '#fbf9f5' }],
    ['meta', { property: 'og:type', content: 'website' }],
    ['meta', { property: 'og:site_name', content: 'Xerif' }],
    [
      'meta',
      {
        property: 'og:title',
        content: 'Xerif — Give your e-reader superpowers.',
      },
    ],
    [
      'meta',
      {
        property: 'og:description',
        content:
          'Free, open-source software that makes your e-reader faster and sharper.',
      },
    ],
    ['meta', { property: 'og:image', content: 'https://xerif.aaflalo.me/og-image.png' }],
    ['meta', { name: 'twitter:card', content: 'summary_large_image' }],
  ],
  themeConfig: {
    logo: {
      light: '/logo.svg',
      dark: '/logo-dark.svg',
      alt: 'Xerif logo',
    },
    siteTitle: 'Xerif',
    nav: [
      { text: 'Flash', link: '/flash' },
      { text: 'Docs', link: '/docs/' },
      { text: 'Changelog', link: '/changelog' },
      { text: 'GitHub', link: GITHUB },
    ],
    sidebar: {
      '/docs/': [
        {
          text: 'Getting started',
          items: [
            { text: 'Installation', link: '/docs/installation' },
            { text: 'First-time setup', link: '/docs/first-time-setup' },
            { text: 'Loading books', link: '/docs/loading-books' },
            { text: 'OTA updates', link: '/docs/ota-updates' },
          ],
        },
        {
          text: 'Features',
          items: [
            { text: 'Reading experience', link: '/docs/reading-experience' },
            { text: 'Reading statistics', link: '/docs/reading-statistics' },
            { text: 'Dictionary lookup', link: '/docs/dictionary' },
            { text: 'Focus Reading', link: '/docs/focus-reading' },
            { text: 'Image & cover rendering', link: '/docs/image-cover-rendering' },
            { text: 'How images are prepared', link: '/docs/image-pipeline' },
            { text: 'KOReader sync', link: '/docs/koreader-sync' },
            { text: 'Custom fonts', link: '/docs/custom-fonts' },
            { text: 'Sleep screens', link: '/docs/sleep-screens' },
            { text: 'Web interface', link: '/docs/web-interface' },
            { text: 'Plugins', link: '/docs/plugins' },
          ],
        },
        {
          text: 'Reference',
          items: [
            { text: 'Supported devices', link: '/docs/supported-devices' },
            { text: 'FAQ', link: '/docs/faq' },
            { text: 'Troubleshooting', link: '/docs/troubleshooting' },
            { text: 'Controls', link: '/docs/controls' },
            { text: 'USB-locked devices', link: '/docs/locked-devices' },
          ],
        },
      ],
    },
    search: {
      provider: 'local',
      options: {
        detailedView: 'auto',
      },
    },
    outline: { level: [2, 3], label: 'On this page' },
    docFooter: { prev: 'Previous', next: 'Next' },
    darkModeSwitchLabel: 'Appearance',
    sidebarMenuLabel: 'Menu',
    returnToTopLabel: 'Back to top',
    langMenuLabel: 'Language',
    externalLinkIcon: true,
  },
  vite: {
    plugins: [flashManifestPlugin()],
  },
  vue: {
    template: {
      compilerOptions: {
        isCustomElement: (tag) => tag.startsWith('esp-web-'),
      },
    },
  },
})

// Referenced by components through Vite's define if ever needed.
export const links = { GITHUB, CROSSPOINT, SUPPORT_CROSSPOINT }
