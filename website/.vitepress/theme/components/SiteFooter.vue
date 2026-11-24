<script setup lang="ts">
import { useSidebar } from 'vitepress/theme'
import Logo from './Logo.vue'
import { CROSSPOINT, GITHUB, LICENSE, SUPPORT_CROSSPOINT } from '../links'

// VitePress's docs sidebar is position:fixed and spans the viewport's left
// edge at every width; the layout-bottom footer is its sibling with no
// offset, so on docs pages it slides under the sidebar. Mirror the left
// offsets VPContent.has-sidebar applies to the page content.
const { hasSidebar } = useSidebar()
</script>

<template>
  <footer class="xp-footer" :class="{ 'xp-footer--offset': hasSidebar }">
    <div class="xp-footer__inner">
      <div class="xp-footer__brand">
        <RouterLink to="/" class="xp-footer__mark">
          <Logo :size="34" />
          <span class="xp-footer__word">Xerif</span>
        </RouterLink>
        <p class="xp-footer__tagline">
          Free, open-source software that makes your e-reader faster and sharper.
        </p>
      </div>

      <nav class="xp-footer__nav" aria-label="Footer">
        <div class="xp-footer__col">
          <span class="xp-footer__label">Product</span>
          <RouterLink to="/flash">Flash</RouterLink>
          <RouterLink to="/docs/">Docs</RouterLink>
          <RouterLink to="/changelog">Changelog</RouterLink>
        </div>
        <div class="xp-footer__col">
          <span class="xp-footer__label">Project</span>
          <a :href="GITHUB" target="_blank" rel="noopener">GitHub</a>
          <a :href="CROSSPOINT" target="_blank" rel="noopener">CrossPoint</a>
          <a :href="LICENSE" target="_blank" rel="noopener">MIT license</a>
        </div>
      </nav>

      <div class="xp-footer__credit">
        <p>
          Xerif is a community fork of CrossPoint — all credit for the core
          firmware belongs to the CrossPoint team and community.
        </p>
        <a
          :href="SUPPORT_CROSSPOINT"
          target="_blank"
          rel="noopener"
          class="xp-footer__support"
          >Please support CrossPoint <span aria-hidden="true">→</span></a
        >
      </div>
    </div>

    <div class="xp-footer__baseline">
      <span class="xp-mono">© 2026 Xerif contributors · MIT</span>
      <span class="xp-mono">xerif.aaflalo.me</span>
    </div>
  </footer>
</template>

<style scoped>
.xp-footer {
  border-top: 1px solid var(--xp-rule);
  background: var(--xp-paper-alt);
  color: var(--xp-ink-muted);
}

.xp-footer__inner {
  max-width: var(--xp-content-width);
  margin: 0 auto;
  padding: 3rem var(--xp-margin) 2rem;
  display: grid;
  grid-template-columns: 1.2fr 1fr 1.4fr;
  gap: 2.5rem;
}

.xp-footer__mark {
  display: inline-flex;
  align-items: center;
  gap: 0.6rem;
  color: var(--xp-ink);
  text-decoration: none;
}

.xp-footer__word {
  font-family: var(--xp-serif);
  font-size: 1.6rem;
  letter-spacing: -0.01em;
}

.xp-footer__tagline {
  margin: 0.9rem 0 0;
  max-width: 22rem;
  font-size: 0.9rem;
  line-height: 1.6;
}

.xp-footer__nav {
  display: flex;
  gap: 2.5rem;
}

.xp-footer__col {
  display: flex;
  flex-direction: column;
  gap: 0.55rem;
}

.xp-footer__label {
  font-family: var(--xp-mono);
  font-size: 0.66rem;
  font-weight: 600;
  letter-spacing: 0.12em;
  text-transform: uppercase;
  color: var(--xp-ink-subtle);
  margin-bottom: 0.2rem;
}

/* Links must read as links at rest: subtle underline, teal on hover. */
.xp-footer__col a {
  color: var(--xp-ink);
  text-decoration: underline;
  text-decoration-color: var(--xp-rule-strong);
  text-decoration-thickness: 1px;
  text-underline-offset: 3px;
  font-size: 0.9rem;
}

.xp-footer__col a:hover {
  color: var(--xp-teal);
  text-decoration-color: var(--xp-teal);
}

.xp-footer__credit p {
  margin: 0 0 0.85rem;
  font-size: 0.9rem;
  line-height: 1.6;
}

.xp-footer__support {
  display: inline-block;
  font-family: var(--xp-mono);
  font-size: 0.74rem;
  font-weight: 600;
  letter-spacing: 0.06em;
  text-transform: uppercase;
  color: var(--xp-teal);
  text-decoration: none;
  border-bottom: 1px solid var(--xp-teal);
  padding-bottom: 0.15rem;
}

.xp-footer__support:hover {
  color: var(--xp-teal-hover);
  border-color: var(--xp-teal-hover);
}

.xp-footer__baseline {
  max-width: var(--xp-content-width);
  margin: 0 auto;
  padding: 1.1rem var(--xp-margin) 1.6rem;
  border-top: 1px solid var(--xp-rule);
  display: flex;
  justify-content: space-between;
  gap: 1rem;
  font-size: 0.68rem;
  letter-spacing: 0.06em;
  text-transform: uppercase;
  color: var(--xp-ink-subtle);
}

@media (max-width: 900px) {
  .xp-footer__inner {
    grid-template-columns: 1fr 1fr;
  }
  .xp-footer__credit {
    grid-column: 1 / -1;
  }
}

@media (max-width: 640px) {
  .xp-footer__inner {
    grid-template-columns: 1fr;
    padding-top: 2.2rem;
    gap: 2rem;
  }
  .xp-footer__nav {
    flex-wrap: wrap;
    gap: 1.75rem;
  }
  .xp-footer__baseline {
    flex-direction: column;
    gap: 0.35rem;
  }
}

/* Docs pages: clear the fixed sidebar exactly as VPContent.has-sidebar does
   so the footer's columns align with the content column instead of sliding
   behind the sidebar. Below 960px the sidebar is a closed drawer. */
@media (min-width: 960px) {
  .xp-footer--offset {
    padding-left: var(--vp-sidebar-width);
  }
}

@media (min-width: 1440px) {
  .xp-footer--offset {
    padding-left: calc((100vw - var(--vp-layout-max-width)) / 2 + var(--vp-sidebar-width));
    padding-right: calc((100vw - var(--vp-layout-max-width)) / 2);
  }
}
</style>
