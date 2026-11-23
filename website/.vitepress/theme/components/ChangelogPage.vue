<script setup lang="ts">
import { CROSSPOINT, SUPPORT_CROSSPOINT } from '../links'

defineProps<{
  releases: Array<{
    tag: string
    version: string
    name: string
    url: string
    date: string
    highlights: string[]
    upstream: boolean
    flashable: boolean
  }>
}>()
</script>

<template>
  <div class="xp-changelog xp-shell">
    <header class="xp-changelog__head">
      <p class="xp-eyebrow">Stable releases</p>
      <h1 class="xp-changelog__title">Changelog</h1>
      <p class="xp-changelog__sub">
        Every stable release of Xerif. Newer entries are written summaries;
        older ones are the generated change lists from the repository.
      </p>
    </header>

    <ol class="xp-changelog__list">
      <li
        v-for="(release, i) in releases"
        :key="release.tag"
        class="xp-release xp-card"
      >
        <div class="xp-release__head">
          <span class="xp-chip xp-chip--active xp-mono">{{ release.tag }}</span>
          <span v-if="i === 0" class="xp-chip xp-chip--latest xp-mono">Latest</span>
          <span class="xp-release__date xp-mono">{{ release.date }}</span>
        </div>

        <ul v-if="release.highlights.length" class="xp-release__notes">
          <li v-for="(line, j) in release.highlights" :key="j">{{ line }}</li>
        </ul>
        <p v-else class="xp-release__notes-empty">
          No notes were attached to this release —
          <a :href="release.url" target="_blank" rel="noopener">view it on GitHub</a>.
        </p>

        <div class="xp-release__actions">
          <a
            class="xp-btn xp-btn--secondary"
            :href="release.url"
            target="_blank"
            rel="noopener"
            >Release notes</a
          >
          <esp-web-install-button
            v-if="release.flashable"
            :manifest="`/xerif-manifest-${release.version}.json`"
          >
            <button type="button" slot="activate" class="xp-btn xp-btn--teal">
              Flash this version
            </button>
          </esp-web-install-button>
        </div>

        <p v-if="release.upstream" class="xp-release__credit">
          Includes changes merged from
          <a :href="CROSSPOINT" target="_blank" rel="noopener">upstream CrossPoint</a>.
        </p>
      </li>
    </ol>

    <footer class="xp-changelog__foot">
      <p>
        Xerif is a community fork of
        <a :href="CROSSPOINT" target="_blank" rel="noopener">CrossPoint</a>; each
        release's full notes show what came from upstream. If this firmware is
        useful to you,
        <a :href="SUPPORT_CROSSPOINT" target="_blank" rel="noopener"
          >please support CrossPoint</a
        >.
      </p>
    </footer>
  </div>
</template>

<style scoped>
.xp-changelog {
  padding-top: var(--xp-margin);
  max-width: 50rem;
  margin-inline: auto;
}

.xp-changelog__title {
  font-family: var(--xp-serif);
  font-weight: 400;
  font-size: 2.5rem;
  line-height: 1.2;
  letter-spacing: -0.015em;
  color: var(--xp-ink);
  margin: 0.5rem 0 0;
}

.xp-changelog__sub {
  font-family: var(--xp-sans);
  font-size: 1rem;
  line-height: 1.6;
  color: var(--xp-ink-muted);
  margin: 0.6rem 0 0;
}

.xp-changelog__list {
  list-style: none;
  padding: 0;
  margin: 2rem 0 0;
  display: flex;
  flex-direction: column;
  gap: var(--xp-gutter);
}

.xp-release {
  padding: 1.25rem 1.5rem 1.35rem;
}

.xp-release__head {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  flex-wrap: wrap;
}

.xp-chip--latest {
  border-color: var(--xp-ink);
  background: var(--xp-ink);
  color: var(--xp-paper);
}

.xp-release__date {
  margin-left: auto;
  font-size: 0.72rem;
  color: var(--xp-ink-subtle);
}

.xp-release__notes {
  margin: 0.9rem 0 0;
  padding-left: 1.25rem;
}

.xp-release__notes li {
  font-family: var(--xp-sans);
  font-size: 0.9rem;
  line-height: 1.6;
  color: var(--xp-ink-muted);
  margin: 0.3rem 0;
}

.xp-release__notes-empty {
  font-family: var(--xp-sans);
  font-size: 0.9rem;
  color: var(--xp-ink-muted);
  margin: 0.9rem 0 0;
}

.xp-release__actions {
  display: flex;
  gap: 0.75rem;
  flex-wrap: wrap;
  margin-top: 1rem;
}

.xp-release__actions a {
  padding: 0.55rem 1.1rem;
  font-size: 0.72rem;
}

.xp-release__credit {
  font-family: var(--xp-sans);
  font-size: 0.85rem;
  color: var(--xp-ink-subtle);
  margin: 0.9rem 0 0;
}

.xp-release__credit a {
  color: var(--xp-teal);
}

.xp-changelog__foot {
  border-top: 1px solid var(--xp-rule);
  margin-top: 2.25rem;
  padding-top: 1.25rem;
}

.xp-changelog__foot p {
  font-family: var(--xp-sans);
  font-size: 0.9rem;
  line-height: 1.6;
  color: var(--xp-ink-muted);
  margin: 0;
}

.xp-changelog__foot a {
  color: var(--xp-teal);
}

@media (max-width: 768px) {
  .xp-changelog__title {
    font-size: 2rem;
  }

  .xp-release__date {
    margin-left: 0;
    width: 100%;
  }
}
</style>
