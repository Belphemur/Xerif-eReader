# Xerif Website — Feature Spec

Source of truth for what the site must do. Companion to `DESIGN.md` (tokens)
and the UX design doc in the eink workspace. Stack: VitePress + esp-web-tools,
deployed on Cloudflare at **xerif.aaflalo.me**.

## 1. Pages

| Page | Kind | Must-have |
|---|---|---|
| `/` (Home) | custom hero layout | Monogram logo + "Give your e-reader superpowers." hero, Flash-it-now / SD-card CTAs, 3-step strip, 3 plain feature cards, CrossPoint support line |
| `/flash` | custom layout | esp-web-tools installer (stable channel only), device support banner (X4 Pro ✓, X3 ✗ → CrossPoint), SD-card instructions (XERIF.ATM), **stock firmware restore** section, browser-support note |
| `/docs/*` | VitePress docs | sidebar groups: Getting started / Features / Reference (pages below) |
| `/changelog` | generated page | releases from GitHub API at build time, version chips + "Latest" badge, "Flash this version" deep link, upstream CrossPoint credit where changes came from upstream, excluded from search index |

## 2. Docs pages (rebuilt from repo docs — short, user-facing)

Getting started: Installation · First-time setup · Loading books · OTA updates.
Features: Reading experience · Image & cover rendering · KOReader sync ·
Custom fonts · Sleep screens · Web interface · Plugins.
Reference: Supported devices · FAQ · Troubleshooting · Controls.

Mappings live in the UX design doc's "Docs content plan" table. Developer docs
(contributing, design, i18n workflows) stay repo-only and link to GitHub.

## 3. Site-wide features

- **Identity:** Xerif monogram (open-book serif x, teal on paper) as navbar
  logo, favicon, and OG image. Hero visual SVG from Stitch (`59e55b5c…`).
- **Attribution:** "Please support CrossPoint" (hero line, footer on every
  page, docs Acknowledgements, changelog upstream notes). Links point to
  CrossPoint's sponsorship page. Never any donation wording of our own.
- **Stable channel only** for the web flasher. PSRAM devices only: X4 Pro
  supported, X3 explicitly unsupported with a CrossPoint pointer.
- **Release-driven data:** latest version + binaries fetched from GitHub
  releases API at build time (manifest for esp-web-tools, version chips,
  changelog). No manual version bumping in site content.
- **Search:** VitePress local search, changelog excluded.
- **Dark mode:** default light (paper); dark variant using ink/paper inversion.
- **Fonts:** self-hosted Newsreader, Inter, JetBrains Mono (no third-party CDN).
- **Mobile:** all pages reflow to single column ≤ 768px; flash page usable on
  desktop-Chromium only (show WebSerial-unsupported notice elsewhere).
- **CI:** build gate on PRs touching `website/**`; deploy develop/main via
  Cloudflare to xerif.aaflalo.me.

## 4. Out of scope (v1)

Beta/Dev channels in the flasher · X3 support · custom-font firmware builder ·
blog · i18n content (scaffold only) · donations of any kind.
