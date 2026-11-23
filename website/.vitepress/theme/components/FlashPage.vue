<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { CROSSPOINT, GITHUB, SUPPORT_CROSSPOINT } from '../links'
import { loadEspWebTools } from '../lib/espWebTools.js'

const props = defineProps<{
  version: string
  binUrl: string
  binName: string
  sizeMb: number
  releaseUrl: string
}>()

// WebSerial support can only be checked in the browser, so SSR assumes it
// works and shows the installer; a non-Chromium browser swaps to the notice
// once the page hydrates.
const hasSerial = ref(true)
const steps = [
  { n: '1', title: 'Plug in', text: 'USB cable, power the reader on' },
  { n: '2', title: 'Click Flash', text: 'Allow the serial port when asked' },
  { n: '3', title: 'Read', text: 'The reader restarts into Xerif' },
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
  <div class="xp-flash">
    <header class="xp-flash__head">
      <p class="xp-eyebrow">Install &middot; stable channel</p>
      <h1 class="xp-flash__title">Flash Xerif to your reader</h1>
      <p class="xp-flash__sub">
        Runs in your browser: plug the reader in over USB and click one button.
        Your books, fonts, and settings on the SD card are kept.
      </p>
      <ol class="xp-flash__steps" aria-label="Install steps">
        <li v-for="step in steps" :key="step.n" class="xp-flash__step">
          <span class="xp-mono xp-flash__step-n">{{ step.n }}</span>
          <span class="xp-flash__step-t">{{ step.title }}</span>
          <span class="xp-flash__step-x">{{ step.text }}</span>
        </li>
      </ol>
    </header>

    <section class="xp-banner" aria-label="Device support">
      <div class="xp-banner__cell xp-banner__cell--ok">
        <span class="xp-banner__mark" aria-hidden="true">&#10003;</span>
        <div>
          <span class="xp-banner__name">Xteink X4 Pro</span>
          <span class="xp-banner__note">Supported &mdash; this installer is built for it</span>
        </div>
      </div>
      <div class="xp-banner__cell xp-banner__cell--no">
        <span class="xp-banner__mark" aria-hidden="true">&#10007;</span>
        <div>
          <span class="xp-banner__name">Xteink X3</span>
          <span class="xp-banner__note">
            Not supported &mdash;
            <a :href="CROSSPOINT" target="_blank" rel="noopener">CrossPoint</a> covers the X3
          </span>
        </div>
      </div>
    </section>

    <section class="xp-flash__panel xp-card xp-relief" aria-label="Web installer">
      <div class="xp-flash__panel-head">
        <span class="xp-chip xp-chip--active">v{{ version || '—' }}</span>
        <span class="xp-chip">stable</span>
        <span class="xp-chip">X4 Pro &middot; ESP32-S3</span>
      </div>

      <template v-if="hasSerial">
        <p class="xp-flash__warn">
          If the installer offers to erase the device, leave
          <strong>Erase device</strong> unchecked: the web installer writes
          only the firmware and cannot restore what an erase wipes. Your
          books, fonts, and settings on the SD card are never touched either
          way. The whole flash takes about two minutes; keep the reader
          plugged in.
        </p>
        <div class="xp-flash__install">
          <esp-web-install-button manifest="/xerif-manifest.json">
            <button
              type="button"
              slot="activate"
              class="xp-btn xp-btn--teal xp-flash__install-btn"
            >
              Flash it now
            </button>
          </esp-web-install-button>
        </div>
        <p class="xp-flash__hint xp-mono">
          Desktop Chrome, Edge, or Opera only. Other browsers: use the
          SD-card install below. The installer logs every step; a failed
          flash can simply be retried.
        </p>
      </template>
      <template v-else>
        <div class="xp-flash__unsupported">
          <p>
            <strong>This browser can't flash over USB.</strong> Web-based
            flashing needs a desktop Chromium browser (Chrome, Edge, or Opera).
          </p>
          <p>
            Use the SD-card install below — it works from any browser on any
            device.
          </p>
        </div>
      </template>
    </section>

    <section id="sd-card" class="xp-sd" aria-label="SD-card install">
      <h2 class="xp-sd__title">Install with an SD card</h2>
      <p class="xp-sd__sub">
        No desktop Chromium needed — any browser, any computer. The SD card
        also doubles as your recovery path.
      </p>
      <ol class="xp-sd__steps">
        <li>
          <a v-if="binUrl" :href="binUrl" target="_blank" rel="noopener"
            >Download <code>{{ binName }}</code></a
          >
          <span v-else>Download the latest X4 Pro image from
            <a :href="`${GITHUB}/releases`" target="_blank" rel="noopener">GitHub releases</a></span
          >
          ({{ sizeMB || '?' }} MB).
        </li>
        <li>Copy it to the top folder of the reader's SD card and rename it
          <code>XERIF.ATM</code>.</li>
        <li>
          On the reader open <strong>Settings &rsaquo; System &rsaquo; Install
          from SD</strong> and confirm. The reader installs Xerif and restarts.
        </li>
      </ol>
      <p class="xp-sd__hint">
        Latest stable version: <span class="xp-mono">v{{ version || '—' }}</span
        >&nbsp;&middot; <a :href="releaseUrl" target="_blank" rel="noopener">release notes</a>
      </p>
    </section>

    <section class="xp-restore" aria-label="Restore stock firmware">
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

.xp-flash__title {
  font-family: var(--xp-serif);
  font-weight: 400;
  font-size: 2.75rem;
  line-height: 1.2;
  letter-spacing: -0.015em;
  color: var(--xp-ink);
  margin: 0.5rem 0 0;
}

.xp-flash__sub {
  font-family: var(--xp-sans);
  font-size: 1.05rem;
  line-height: 1.6;
  color: var(--xp-ink-muted);
  max-width: 44rem;
  margin: 0.75rem 0 0;
}

.xp-flash__steps {
  list-style: none;
  display: flex;
  gap: var(--xp-gutter);
  padding: 0;
  margin: 1.75rem 0 0;
}

.xp-flash__step {
  display: flex;
  align-items: baseline;
  gap: 0.5rem;
}

.xp-flash__step-n {
  color: var(--xp-teal);
  font-size: 0.85rem;
  font-weight: 600;
}

.xp-flash__step-t {
  font-family: var(--xp-serif);
  font-size: 1.05rem;
  color: var(--xp-ink);
}

.xp-flash__step-x {
  font-family: var(--xp-sans);
  font-size: 0.85rem;
  color: var(--xp-ink-subtle);
}

/* Device support banner */
.xp-banner {
  display: grid;
  grid-template-columns: 1fr 1fr;
  border: 1px solid var(--xp-rule);
  background: var(--xp-surface);
  margin: 1.75rem 0;
}

.xp-banner__cell {
  display: flex;
  gap: 0.75rem;
  align-items: center;
  padding: 0.9rem 1.25rem;
}

.xp-banner__cell + .xp-banner__cell {
  border-left: 1px solid var(--xp-rule);
}

.xp-banner__mark {
  font-family: var(--xp-mono);
  font-weight: 700;
}

.xp-banner__cell--ok .xp-banner__mark {
  color: var(--xp-teal);
}

.xp-banner__cell--no .xp-banner__mark {
  color: var(--xp-error);
}

.xp-banner__name {
  display: block;
  font-family: var(--xp-serif);
  font-size: 1.05rem;
  color: var(--xp-ink);
}

.xp-banner__note,
.xp-banner__note a {
  font-family: var(--xp-sans);
  font-size: 0.85rem;
  color: var(--xp-ink-muted);
}

/* Installer panel */
.xp-flash__panel {
  padding: 1.5rem 1.75rem 1.75rem;
}

.xp-flash__panel-head {
  display: flex;
  flex-wrap: wrap;
  gap: 0.5rem;
  margin-bottom: 1rem;
}

.xp-flash__warn {
  font-family: var(--xp-sans);
  font-size: 0.9rem;
  line-height: 1.6;
  color: var(--xp-ink-muted);
  margin: 0 0 1rem;
}

.xp-flash__install-btn {
  padding: 1rem 2.25rem;
  font-size: 0.9rem;
}

.xp-flash__noserial {
  display: block;
  font-family: var(--xp-sans);
  font-size: 0.9rem;
  color: var(--xp-error);
  margin-top: 0.75rem;
}

.xp-flash__hint {
  font-size: 0.72rem;
  color: var(--xp-ink-subtle);
  margin-top: 1rem;
}

.xp-flash__unsupported p {
  font-family: var(--xp-sans);
  font-size: 0.95rem;
  line-height: 1.6;
  color: var(--xp-ink-muted);
  margin: 0.25rem 0;
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

  .xp-flash__steps {
    flex-direction: column;
    gap: 0.5rem;
  }

  .xp-banner {
    grid-template-columns: 1fr;
  }

  .xp-banner__cell + .xp-banner__cell {
    border-left: 0;
    border-top: 1px solid var(--xp-rule);
  }

  .xp-flash__panel {
    padding: 1.25rem 1.25rem 1.5rem;
  }

  .xp-flash__install-btn {
    width: 100%;
  }
}
</style>
