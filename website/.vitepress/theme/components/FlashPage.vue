<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { CROSSPOINT, GITHUB, RELEASES } from '../links'
import { loadEspWebTools } from '../lib/espWebTools.js'

const props = defineProps<{
  version: string
  binUrl: string
  binName: string
  sizeMB: number
  releaseUrl: string
  /** Per-board SD images of the newest stable release (board key -> {url, name, sizeMB}). */
  boardBins?: Record<string, { url: string; name: string; sizeMB: number }>
}>()

/**
 * The six boards the web installer can flash, in grid order. Resolutions are
 * verified against freeink-sdk BoardConfig.h (BoardProfile displayWidth/
 * displayHeight, portrait housing order); every board here is ESP32-S3, so
 * one manifest shape fits all. X4 Pro ships the stable channel; the other
 * five are supported best-effort (beta).
 */
const boards = [
  {
    key: 'x4pro',
    name: 'X4 Pro',
    res: '480 × 800',
    beta: false,
    manifest: '/xerif-manifest.json',
  },
  {
    key: 'x4c',
    name: 'X4 Classic',
    res: '480 × 800',
    beta: true,
    manifest: '/xerif-manifest-x4c.json',
  },
  {
    key: 'metalio_eink4',
    name: 'Metalio E-Ink 4',
    res: '480 × 800',
    beta: true,
    manifest: '/xerif-manifest-metalio_eink4.json',
  },
  {
    key: 'papermono',
    name: 'Paper Mono',
    res: '480 × 800',
    beta: true,
    manifest: '/xerif-manifest-papermono.json',
  },
  {
    key: 'eego_a4',
    name: 'EEGO A4',
    res: '552 × 768',
    beta: true,
    manifest: '/xerif-manifest-eego_a4.json',
  },
  {
    key: 'picco',
    name: 'Picco',
    res: '480 × 800',
    beta: true,
    manifest: '/xerif-manifest-picco.json',
  },
]

// X4 Pro is the default and only stable-channel board.
const selectedKey = ref('x4pro')
const selected = computed(
  () => boards.find((b) => b.key === selectedKey.value) || boards[0],
)

function select(board) {
  selectedKey.value = board.key
}

// The SD section downloads the selected board's own image; X4 Pro rides the
// existing binUrl/binName props. A board whose release carries no image falls
// back to the GitHub releases link so we never point at the wrong board's
// firmware.
const sdBin = computed(() => {
  if (selected.value.key === 'x4pro') {
    return { url: props.binUrl, name: props.binName, sizeMB: props.sizeMB }
  }
  return props.boardBins?.[selected.value.key] || null
})

// "Beta builds" in step 2 scrolls back to the grid and briefly marks the
// beta boards so they are easy to spot.
const betaHinted = ref(false)
let betaHintTimer = null
function hintBeta() {
  betaHinted.value = true
  clearTimeout(betaHintTimer)
  betaHintTimer = setTimeout(() => (betaHinted.value = false), 2000)
  document
    .querySelector('.xp-devices')
    ?.scrollIntoView({ behavior: 'smooth', block: 'start' })
}

// WebSerial support can only be checked in the browser, so SSR assumes it
// works and shows the installer; a non-Chromium browser swaps to the notice
// once the page hydrates.
const hasSerial = ref(true)

const connectSteps = [
  'Connect your e-reader to this computer with a standard USB data cable.',
  "Click 'Install Xerif via USB' below and choose the serial device from the browser prompt.",
  'Keep the cable connected until your display restarts — about two minutes.',
]

onMounted(() => {
  if (typeof navigator === 'undefined' || !('serial' in navigator)) {
    hasSerial.value = false
  }
  // Register <esp-web-install-button> from the self-hosted bundle; custom
  // elements upgrade in place, so the buttons above become live.
  loadEspWebTools().catch((err) => console.error(err))
})
</script>

<template>
  <div class="xp-flash xp-shell">
    <header class="xp-flash__head">
      <p class="xp-eyebrow">Installer</p>
      <div class="xp-flash__head-row">
        <h1 class="xp-flash__title">Install Xerif</h1>
        <p class="xp-flash__help">
          New to custom firmware?
          <a href="/docs/installation">Read the beginner's guide.</a>
        </p>
      </div>
      <p class="xp-flash__sub">
        A few steps to a sharper reader. Pick your device and install Xerif
        over USB.
      </p>
      <p class="xp-flash__hint">
        No serial port offered? Some third-party Xteink units ship with USB
        flashing locked — see
        <a href="/docs/locked-devices">USB-locked devices</a>.
      </p>
    </header>

    <div class="xp-wiz">
      <!-- Step 1: device selector -->
      <section class="xp-card xp-wiz__panel" aria-label="Choose your device">
        <h2 class="xp-wiz__step-label xp-mono">[1] Choose your device</h2>
        <div class="xp-devices" role="radiogroup" aria-label="Your device">
          <button
            v-for="b in boards"
            :key="b.key"
            type="button"
            role="radio"
            :aria-checked="b.key === selectedKey"
            class="xp-device"
            :class="{
              'is-selected': b.key === selectedKey,
              'is-hinted': b.beta && betaHinted,
            }"
            @click="select(b)"
          >
            <span class="xp-device__top">
              <span class="xp-device__name">{{ b.name }}</span>
              <svg
                v-if="b.key === selectedKey"
                class="xp-device__check"
                viewBox="0 0 24 24"
                width="20"
                height="20"
                aria-hidden="true"
              >
                <circle
                  cx="12"
                  cy="12"
                  r="10"
                  fill="none"
                  stroke="currentColor"
                  stroke-width="2"
                />
                <path
                  d="M7 12.5l3.2 3.2L17 9"
                  fill="none"
                  stroke="currentColor"
                  stroke-width="2"
                />
              </svg>
            </span>
            <span class="xp-device__foot">
              <span
                class="xp-chip"
                :class="b.beta ? 'xp-chip--beta' : 'xp-chip--stable'"
                >{{ b.beta ? 'Beta' : 'Stable' }}</span
              >
              <span class="xp-mono xp-device__res">{{ b.res }}</span>
            </span>
          </button>
        </div>
        <hr class="xp-rule xp-devices__rule" />
        <p class="xp-devices__footnote">
          Picco: web installer only &mdash; no SD-card update.
        </p>
        <p class="xp-devices__links">
          <template v-if="selected.key !== 'picco'">
            <a href="#sd-card">{{
              selected.beta
                ? 'SD-card update path (for a reader already running Xerif)'
                : 'SD-card manual install instead'
            }}</a>
            <span aria-hidden="true">&middot;</span>
          </template>
          <a href="#device-note">My device isn't listed</a>
        </p>
        <p id="device-note" class="xp-devices__note">
          Unsupported here: the Xteink X3 &mdash;
          <a :href="CROSSPOINT" target="_blank" rel="noopener">CrossPoint</a>
          covers the X3. Also see
          <a href="/docs/supported-devices">supported devices</a>.
        </p>
      </section>

      <!-- Step 2: release chooser -->
      <section class="xp-card xp-wiz__panel" aria-label="Your Xerif release">
        <h2 class="xp-wiz__step-label xp-mono">[2] Your Xerif release</h2>
        <div class="xp-release">
          <div class="xp-release__row">
            <span class="xp-release__name">Xerif {{ version || '—' }}</span>
            <span class="xp-chip xp-chip--active">
              {{ selected.beta ? 'Beta' : 'Stable · Recommended' }}
            </span>
          </div>
          <p class="xp-release__desc" v-if="!selected.beta">
            Latest stable release. Ready for everyday reading.
          </p>
          <p class="xp-release__desc" v-else>
            Best-effort testing —
            <a
              :href="`${GITHUB}/issues`"
              target="_blank"
              rel="noopener"
              >report issues on GitHub</a
            >.
          </p>
        </div>
        <button
          type="button"
          class="xp-beta"
          @click="hintBeta"
        >
          <span class="xp-beta__text">
            <span class="xp-beta__title">Beta builds</span>
            <span class="xp-beta__note"
              >Try upcoming changes &mdash; best-effort testing.</span
            >
          </span>
          <span class="xp-chip xp-chip--beta">Beta</span>
        </button>
        <details class="xp-adv">
          <summary class="xp-mono xp-adv__summary">
            Advanced options<span class="xp-adv__mark" aria-hidden="true">+</span>
          </summary>
          <div class="xp-adv__body">
            <p class="xp-adv__warn">
              If the installer offers to erase the device, leave
              <strong>Erase device</strong> unchecked: the web installer writes
              only the firmware and cannot restore what an erase wipes. Your
              books, fonts, and settings on the SD card are never touched
              either way.
            </p>
            <p class="xp-adv__note">
              Desktop Chrome, Edge, or Opera only. Other browsers: use the
              SD-card install below. The installer logs every step; a failed
              flash can simply be retried.
            </p>
          </div>
        </details>
      </section>
    </div>

    <!-- Step 3: connect and install -->
    <section class="xp-card xp-connect" aria-label="Connect and install">
      <h2 class="xp-wiz__step-label xp-mono">[3] Connect and install</h2>
      <ol class="xp-connect__steps">
        <li v-for="(text, i) in connectSteps" :key="i" class="xp-connect__step">
          <span class="xp-mono xp-connect__n">0{{ i + 1 }}</span>
          <p class="xp-connect__text">{{ text }}</p>
        </li>
      </ol>
      <p class="xp-connect__erase">
        Leave <strong>Erase device</strong> unchecked — the installer writes
        only the firmware, and an erase cannot be undone.
      </p>
      <div class="xp-inkpanel">
        <template v-if="hasSerial">
          <div class="xp-inkpanel__head">
            <span class="xp-inkpanel__title">Xerif Firmware</span>
            <span class="xp-mono xp-inkpanel__ver">{{ version || '—' }}</span>
          </div>
          <div class="xp-inkpanel__row">
            <esp-web-install-button :manifest="selected.manifest">
              <button
                type="button"
                slot="activate"
                class="xp-btn xp-btn--teal xp-inkpanel__btn"
              >
                <svg
                  viewBox="0 0 24 24"
                  width="14"
                  height="14"
                  aria-hidden="true"
                >
                  <path
                    d="M13 2L4.5 13.5H11L9.5 22 19 10h-6.5L13 2z"
                    fill="currentColor"
                  />
                </svg>
                Install Xerif via USB
              </button>
            </esp-web-install-button>
            <span class="xp-inkpanel__links">
              <a :href="releaseUrl || RELEASES" target="_blank" rel="noopener"
                >Release notes</a
              >
              <span aria-hidden="true">&middot;</span>
              <span>About two minutes</span>
            </span>
          </div>
        </template>
        <template v-else>
          <div class="xp-inkpanel__unsupported">
            <p>
              <strong>This browser can't flash over USB.</strong> Web-based
              flashing needs a desktop Chromium browser (Chrome, Edge, or
              Opera).
            </p>
            <p>
              Use the
              <a href="#sd-card">SD-card install</a> instead &mdash; it works
              from any browser on any device.
            </p>
          </div>
        </template>
      </div>
    </section>

    <section id="sd-card" class="xp-sd" aria-label="SD-card install">
      <h2 class="xp-sd__title">Install with an SD card</h2>
      <p class="xp-sd__sub">
        No desktop Chromium needed — any browser, any computer. The SD card
        also doubles as a recovery path (see below).
      </p>
      <ol class="xp-sd__steps">
        <li>
          <a v-if="sdBin?.url" :href="sdBin.url" target="_blank" rel="noopener"
            >Download <code>{{ sdBin.name }}</code></a
          >
          <span v-else>Download your board's image from
            <a :href="`${GITHUB}/releases`" target="_blank" rel="noopener">GitHub releases</a></span
          >
          ({{ sdBin?.sizeMB || '?' }} MB).
        </li>
        <li>Copy it to the top folder of the reader's SD card, keeping the
          file name as-is.</li>
        <li>
          On the reader open <strong>Settings &rsaquo; System &rsaquo; SD Card
          Firmware Update</strong>, select the <code>.bin</code> file, and
          confirm. The reader installs Xerif and restarts.
        </li>
      </ol>
      <p class="xp-sd__hint">
        These steps use the reader's own Settings menu — Xerif's
        <strong>Settings &rsaquo; System &rsaquo; SD Card Firmware
        Update</strong>. That makes this an update path: on the X4 Pro, stock
        Xteink firmware has the same menu, so it also works as a first
        install there; on the other boards it updates a reader that already
        runs Xerif. The Picco has no SD-card update menu — web installer
        only. It is not the tool for a reader that won't start — see the
        recovery notes below.
      </p>
      <p class="xp-sd__hint">
        Latest stable version: <span class="xp-mono">v{{ version || '—' }}</span
        >&nbsp;&middot; <a :href="releaseUrl" target="_blank" rel="noopener">release notes</a>
      </p>
    </section>

    <section class="xp-restore" aria-label="If the reader misbehaves">
      <h2 class="xp-sd__title">If the reader misbehaves after a flash</h2>
      <p class="xp-sd__sub">
        As long as the reader still starts, the firmware ships a built-in
        recovery path:
      </p>
      <ol class="xp-sd__steps">
        <li>
          Hold <strong>Next</strong> and press/hold <strong>Power</strong> as
          the reader starts. It boots straight into a "Recovery Mode" file
          picker for SD-card firmware updates.
        </li>
        <li>
          Pick a Xerif <code>.bin</code> from the SD card (the file name stays
          as downloaded) and confirm. The picker repoints the boot selector at
          the freshly written slot first.
        </li>
      </ol>
      <p class="xp-sd__hint">
        This covers an update gone wrong or a reader stuck booting the old
        firmware (for example after the web installer wrote the app while the
        boot selector still pointed at the previous slot). It does
        <strong>not</strong> work if the reader cannot start at all — if the
        bootloader itself was erased, only USB flashing (for example the
        maker's flash tool below) can bring it back.
      </p>
    </section>

    <section
      id="restore-stock-xteink-firmware"
      class="xp-restore"
      aria-label="Restore stock firmware"
    >
      <h2 class="xp-sd__title">Restore stock Xteink firmware</h2>
      <p class="xp-sd__sub">
        Xerif is not a one-way door. If you want the original Xteink system
        back, restore it with the maker's own flash tool.
      </p>
      <ol class="xp-sd__steps">
        <li>
          Connect the X4 Pro over USB and power it on, exactly like above.
        </li>
        <li>
          Open the official
          <a href="https://www.xteink.cn/app/firmware-flash" target="_blank" rel="noopener"
            >XTEINK firmware flash page</a
          >
          and pick the official firmware update entry.
        </li>
        <li>Confirm the prompts, select the reader's serial port, and wait
          until the flash finishes.</li>
      </ol>
      <p class="xp-sd__hint">
        Note: the maker's page states that flashing third-party firmware
        affects warranty eligibility — check their terms before flashing back
        or forth.
      </p>
    </section>
  </div>
</template>

<style scoped>
.xp-flash {
  padding-top: var(--xp-margin);
}

/* Header */
.xp-flash__head-row {
  display: flex;
  align-items: flex-end;
  justify-content: space-between;
  gap: var(--xp-gutter);
  flex-wrap: wrap;
}

.xp-flash__title {
  font-family: var(--xp-serif);
  font-weight: 400;
  font-size: 2.75rem;
  line-height: 1.2;
  letter-spacing: -0.015em;
  color: var(--xp-ink);
  margin: 0.5rem 0 0;
}

.xp-flash__help {
  font-family: var(--xp-sans);
  font-size: 0.85rem;
  color: var(--xp-ink-subtle);
  margin: 0 0 0.4rem;
}

.xp-flash__help a {
  color: var(--xp-teal);
}

.xp-flash__sub {
  font-family: var(--xp-sans);
  font-size: 1.05rem;
  line-height: 1.6;
  color: var(--xp-ink-muted);
  max-width: 44rem;
  margin: 0.75rem 0 0;
}

.xp-flash__hint {
  font-family: var(--xp-sans);
  font-size: 0.9rem;
  line-height: 1.5;
  color: var(--xp-ink-muted);
  margin: 0.75rem 0 0;
}

.xp-flash__hint a {
  color: var(--xp-teal);
  text-underline-offset: 3px;
}

/* Two-column band */
.xp-wiz {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 24px;
  align-items: start;
  margin-top: var(--xp-gutter);
}

.xp-wiz__panel {
  padding: 1.25rem 1.5rem 1.5rem;
}

.xp-wiz__step-label {
  font-size: 0.72rem;
  font-weight: 600;
  letter-spacing: 0.1em;
  text-transform: uppercase;
  color: var(--xp-ink-subtle);
  margin: 0 0 1rem;
  padding-bottom: 0.75rem;
  border-bottom: 1px solid var(--xp-rule);
}

/* Step 1: device grid */
.xp-devices {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 0.75rem;
}

.xp-device {
  display: flex;
  flex-direction: column;
  gap: 0.9rem;
  padding: 0.85rem 1rem;
  background: var(--xp-surface);
  border: 1px solid var(--xp-rule);
  border-radius: var(--xp-radius);
  font: inherit;
  text-align: left;
  cursor: pointer;
}

.xp-device:hover {
  border-color: var(--xp-ink);
}

.xp-device.is-selected {
  border-color: var(--xp-teal-selected);
  background: var(--xp-teal-soft);
  box-shadow: 2px 2px 0 var(--xp-teal-selected);
}

.xp-device.is-hinted {
  border-color: var(--xp-teal-selected);
}

.xp-device:focus-visible {
  outline: 2px solid var(--xp-teal-selected);
  outline-offset: 1px;
}

.xp-device__top {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 0.5rem;
}

.xp-device__name {
  font-family: var(--xp-sans);
  font-weight: 600;
  font-size: 0.95rem;
  color: var(--xp-ink);
}

.xp-device__check {
  color: var(--xp-teal-selected);
  flex: none;
}

.xp-device__foot {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 0.5rem;
}

.xp-chip--stable {
  border-color: var(--xp-teal);
  background: var(--xp-teal-soft);
  color: var(--xp-teal);
}

.xp-chip--beta {
  border-color: var(--xp-rule);
  background: var(--xp-surface-strong);
  color: var(--xp-ink-muted);
}

.xp-device__res {
  font-size: 0.7rem;
  color: var(--xp-ink-muted);
  white-space: nowrap;
}

.xp-devices__rule {
  margin: 1.25rem 0 0;
}

.xp-devices__footnote {
  font-family: var(--xp-sans);
  font-size: 0.85rem;
  color: var(--xp-ink-subtle);
  margin: 0.85rem 0 0;
}

.xp-devices__links {
  display: flex;
  flex-wrap: wrap;
  gap: 0.5rem;
  font-family: var(--xp-sans);
  font-size: 0.85rem;
  margin: 0.5rem 0 0;
}

.xp-devices__links a {
  color: var(--xp-teal);
}

.xp-devices__note {
  font-family: var(--xp-sans);
  font-size: 0.85rem;
  line-height: 1.6;
  color: var(--xp-ink-muted);
  margin: 0.85rem 0 0;
  scroll-margin-top: calc(var(--vp-nav-height) + 1rem);
}

.xp-devices__note a {
  color: var(--xp-teal);
}

/* Step 2: release card */
.xp-release {
  background: var(--xp-paper);
  border: 1px solid var(--xp-teal);
  box-shadow: var(--xp-relief-teal);
  padding: 1.1rem 1.25rem 1.25rem;
}

.xp-release__row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 0.75rem;
  flex-wrap: wrap;
}

.xp-release__name {
  font-family: var(--xp-serif);
  font-size: 1.6rem;
  line-height: 1.25;
  color: var(--xp-ink);
}

.xp-release__desc {
  font-family: var(--xp-sans);
  font-size: 0.9rem;
  line-height: 1.6;
  color: var(--xp-ink-muted);
  margin: 0.5rem 0 0;
}

.xp-release__desc a {
  color: var(--xp-teal);
}

/* Step 2: beta builds card */
.xp-beta {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 0.75rem;
  width: 100%;
  margin-top: 0.75rem;
  padding: 0.85rem 1rem;
  background: var(--xp-paper);
  border: 1px solid var(--xp-rule);
  border-radius: var(--xp-radius);
  font: inherit;
  text-align: left;
  cursor: pointer;
}

.xp-beta:hover {
  border-color: var(--xp-ink);
}

.xp-beta:focus-visible {
  outline: 2px solid var(--xp-teal-selected);
  outline-offset: 1px;
}

.xp-beta__title {
  display: block;
  font-family: var(--xp-sans);
  font-weight: 600;
  font-size: 0.9rem;
  color: var(--xp-ink);
}

.xp-beta__note {
  display: block;
  font-family: var(--xp-sans);
  font-size: 0.8rem;
  color: var(--xp-ink-muted);
  margin-top: 0.15rem;
}

/* Step 2: advanced options */
.xp-adv {
  display: block;
  margin-top: 0.75rem;
  border: 1px solid var(--xp-rule);
  background: var(--xp-paper);
}

.xp-adv__summary {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 0.5rem;
  padding: 0.7rem 1rem;
  font-size: 0.75rem;
  color: var(--xp-ink);
  cursor: pointer;
  list-style: none;
}

.xp-adv__summary::-webkit-details-marker {
  display: none;
}

.xp-adv__mark {
  color: var(--xp-ink-subtle);
}

.xp-adv[open] .xp-adv__mark {
  transform: rotate(45deg);
}

.xp-adv__body {
  border-top: 1px solid var(--xp-rule);
  padding: 0.85rem 1rem 1rem;
}

.xp-adv__warn,
.xp-adv__note {
  font-family: var(--xp-sans);
  font-size: 0.85rem;
  line-height: 1.6;
  color: var(--xp-ink-muted);
  margin: 0 0 0.75rem;
}

.xp-adv__note {
  margin-bottom: 0;
}

/* Step 3: connect instructions */
.xp-connect {
  margin-top: 24px;
  padding: 1.25rem 1.5rem 1.5rem;
}

.xp-connect__steps {
  list-style: none;
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 0.75rem;
  padding: 0;
  margin: 0;
}

.xp-connect__step {
  background: var(--xp-paper);
  border: 1px solid var(--xp-rule);
  padding: 0.85rem 1rem;
}

.xp-connect__erase {
  margin: 0 0 0.75rem;
  font-size: 0.9rem;
}

.xp-connect__n {
  font-size: 0.75rem;
  font-weight: 600;
  color: var(--xp-teal);
}

.xp-connect__text {
  font-family: var(--xp-sans);
  font-size: 0.9rem;
  line-height: 1.6;
  color: var(--xp-ink-muted);
  margin: 0.35rem 0 0;
}

/* Step 3: dark install panel */
.xp-inkpanel {
  margin-top: 0.75rem;
  background: var(--xp-ink);
  border: 1px solid var(--xp-ink);
  padding: 1.25rem 1.5rem 1.5rem;
}

.xp-inkpanel__head {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: 0.75rem;
  flex-wrap: wrap;
  padding-bottom: 1rem;
  border-bottom: 1px solid color-mix(in srgb, var(--xp-paper) 20%, transparent);
}

.xp-inkpanel__title {
  font-family: var(--xp-serif);
  font-size: 1.4rem;
  color: var(--xp-paper);
}

.xp-inkpanel__ver {
  font-size: 0.8rem;
  color: color-mix(in srgb, var(--xp-paper) 60%, transparent);
}

.xp-inkpanel__row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 0.75rem 1.5rem;
  flex-wrap: wrap;
  padding-top: 1.25rem;
}

.xp-inkpanel__btn {
  padding: 0.9rem 1.75rem;
  font-family: var(--xp-sans);
  font-size: 0.9rem;
  letter-spacing: 0;
  text-transform: none;
  border-color: var(--xp-teal-selected);
}

.xp-inkpanel__links {
  display: inline-flex;
  align-items: center;
  gap: 0.5rem;
  font-family: var(--xp-sans);
  font-size: 0.85rem;
  color: color-mix(in srgb, var(--xp-paper) 70%, transparent);
}

.xp-inkpanel__links a {
  color: var(--xp-paper);
}

.xp-inkpanel__unsupported p {
  font-family: var(--xp-sans);
  font-size: 0.95rem;
  line-height: 1.6;
  color: color-mix(in srgb, var(--xp-paper) 85%, transparent);
  margin: 0.25rem 0;
}

.xp-inkpanel__unsupported strong {
  color: var(--xp-paper);
}

.xp-inkpanel__unsupported a {
  color: var(--xp-paper);
}

/* SD + restore sections */
.xp-sd,
.xp-restore {
  border-top: 1px solid var(--xp-rule);
  padding-top: 1.75rem;
  margin-top: 2.25rem;
  scroll-margin-top: calc(var(--vp-nav-height) + 1rem);
}

.xp-sd__title {
  font-family: var(--xp-serif);
  font-size: 1.75rem;
  font-weight: 400;
  color: var(--xp-ink);
  margin: 0;
}

.xp-sd__sub {
  font-family: var(--xp-sans);
  font-size: 0.95rem;
  color: var(--xp-ink-muted);
  margin: 0.5rem 0 1rem;
  max-width: 44rem;
}

.xp-sd__steps {
  padding-left: 1.25rem;
}

.xp-sd__steps li {
  font-family: var(--xp-sans);
  font-size: 0.95rem;
  line-height: 1.65;
  color: var(--xp-ink-muted);
  margin: 0.4rem 0;
}

.xp-sd__steps code {
  font-family: var(--xp-mono);
  background: var(--xp-surface);
  border: 1px solid var(--xp-rule);
  padding: 0.05rem 0.35rem;
  font-size: 0.85em;
}

.xp-sd__steps a {
  color: var(--xp-teal);
}

.xp-sd__hint {
  font-family: var(--xp-sans);
  font-size: 0.85rem;
  color: var(--xp-ink-subtle);
  margin-top: 1rem;
}

.xp-sd__hint a {
  color: var(--xp-teal);
}

@media (max-width: 768px) {
  .xp-flash__title {
    font-size: 2rem;
  }

  .xp-wiz {
    grid-template-columns: 1fr;
  }

  .xp-connect__steps {
    grid-template-columns: 1fr;
  }

  .xp-inkpanel__btn {
    width: 100%;
  }
}

/* Very narrow screens: stack the chip over the resolution inside each
   device card so the 2x3 grid keeps fitting at 360px. */
@media (max-width: 480px) {
  .xp-wiz__panel {
    padding: 1rem;
  }

  .xp-device {
    padding: 0.75rem;
  }

  .xp-device__foot {
    flex-direction: column;
    align-items: flex-start;
    gap: 0.4rem;
  }
}
</style>
