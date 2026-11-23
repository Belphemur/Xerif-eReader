<script setup lang="ts">
import HeroDevice from './HeroDevice.vue'
import { SUPPORT_CROSSPOINT } from '../links'

const steps = [
  { n: '01', title: 'Plug in', text: 'Connect your reader to a computer with its USB cable.' },
  { n: '02', title: 'Click Flash', text: 'One button in your browser. No accounts, no tools.' },
  { n: '03', title: 'Read', text: 'Your books and settings are right where you left them.' },
]

const cards = [
  { title: 'Fast page turns', text: 'Pages arrive in a blink.' },
  { title: 'Sharper images', text: 'Crisper text, clearer covers.' },
  { title: 'Saves your battery', text: 'Weeks of reading per charge.' },
]
</script>

<template>
  <section class="xp-hero">
    <div class="xp-hero__copy">
      <p class="xp-eyebrow">Xerif &middot; open-source reader firmware</p>
      <h1 class="xp-hero__title">Give your e-reader superpowers.</h1>
      <p class="xp-hero__sub">
        Free, open-source software that makes your e-reader faster and sharper.
      </p>

      <div class="xp-hero__actions">
        <a href="/flash" class="xp-btn xp-btn--teal xp-hero__cta">Flash it now</a>
        <a href="/flash#sd-card" class="xp-btn xp-btn--secondary xp-hero__cta">Use an SD card</a>
      </div>

      <p class="xp-hero__trust">Free forever. 2-minute setup. Keeps your books and settings.</p>
      <p class="xp-hero__attribution">
        Made possible by the CrossPoint community.
        <a :href="SUPPORT_CROSSPOINT" target="_blank" rel="noopener">Please support CrossPoint</a>.
      </p>
    </div>

    <div class="xp-hero__visual">
      <HeroDevice />
    </div>
  </section>

  <section class="xp-steps" aria-label="Three steps to install">
    <ol class="xp-steps__list">
      <li v-for="step in steps" :key="step.n" class="xp-steps__item xp-card xp-relief">
        <span class="xp-steps__number xp-mono">{{ step.n }}</span>
        <span class="xp-steps__title">{{ step.title }}</span>
        <p class="xp-steps__text">{{ step.text }}</p>
      </li>
    </ol>
  </section>

  <section class="xp-features" aria-label="What you get">
    <div class="xp-features__grid">
      <article v-for="card in cards" :key="card.title" class="xp-features__card xp-card xp-relief">
        <h3 class="xp-features__title">{{ card.title }}</h3>
        <p class="xp-features__text">{{ card.text }}</p>
      </article>
    </div>
  </section>

  <hr class="xp-rule xp-home-rule" />
  <p class="xp-home-more">
    Curious what else changed? <a href="/docs/">Read the docs</a> or browse the
    <a href="/changelog">changelog</a>.
  </p>
</template>

<style scoped>
.xp-hero {
  display: grid;
  grid-template-columns: minmax(0, 1.15fr) minmax(0, 0.85fr);
  gap: var(--xp-margin);
  align-items: center;
  padding-block: var(--xp-margin);
}

.xp-hero__title {
  font-family: var(--xp-serif);
  font-weight: 400;
  font-size: 3.5rem;
  line-height: 1.15;
  letter-spacing: -0.02em;
  color: var(--xp-ink);
  margin: 0.75rem 0 0;
}

.xp-hero__sub {
  font-family: var(--xp-sans);
  font-size: 1.125rem;
  line-height: 1.6;
  color: var(--xp-ink-muted);
  margin: 1rem 0 0;
  max-width: 34rem;
}

.xp-hero__actions {
  display: flex;
  flex-wrap: wrap;
  gap: 0.75rem;
  margin-top: 1.75rem;
}

.xp-hero__cta {
  min-width: 11rem;
}

.xp-hero__trust {
  font-family: var(--xp-mono);
  font-size: 0.78rem;
  letter-spacing: 0.04em;
  color: var(--xp-ink-muted);
  margin: 1.5rem 0 0;
}

.xp-hero__attribution {
  font-family: var(--xp-sans);
  font-size: 0.9rem;
  color: var(--xp-ink-subtle);
  margin: 0.4rem 0 0;
}

.xp-hero__attribution a {
  color: var(--xp-teal);
  text-decoration-thickness: 1px;
  text-underline-offset: 2px;
  font-weight: 500;
}

.xp-hero__visual {
  align-self: center;
}

/* Steps strip */
.xp-steps__list {
  list-style: none;
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: var(--xp-gutter);
  margin: 0 0 var(--xp-margin);
  padding: 0;
  counter-reset: none;
}

.xp-steps__item {
  padding: 1.25rem 1.5rem 1.35rem;
}

.xp-steps__number {
  font-size: 0.72rem;
  font-weight: 600;
  letter-spacing: 0.12em;
  color: var(--xp-teal);
}

.xp-steps__title {
  display: block;
  font-family: var(--xp-serif);
  font-size: 1.375rem;
  font-weight: 500;
  color: var(--xp-ink);
  margin-top: 0.35rem;
}

.xp-steps__text {
  font-family: var(--xp-sans);
  font-size: 0.9rem;
  line-height: 1.55;
  color: var(--xp-ink-muted);
  margin: 0.5rem 0 0;
}

/* Feature cards */
.xp-features {
  margin-bottom: var(--xp-margin);
}

.xp-features__grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: var(--xp-gutter);
}

.xp-features__card {
  padding: 1.25rem 1.5rem 1.35rem;
}

.xp-features__title {
  font-family: var(--xp-serif);
  font-size: 1.25rem;
  font-weight: 500;
  color: var(--xp-ink);
  margin: 0;
}

.xp-features__text {
  font-family: var(--xp-sans);
  font-size: 0.9rem;
  color: var(--xp-ink-muted);
  margin: 0.45rem 0 0;
}

.xp-home-rule {
  margin-bottom: 1rem;
}

.xp-home-more {
  font-family: var(--xp-sans);
  font-size: 0.95rem;
  color: var(--xp-ink-muted);
  margin-bottom: var(--xp-margin);
}

.xp-home-more a {
  color: var(--xp-teal);
}

/* Mobile reflow: single column at <=768px (FEATURES.md section 3) */
@media (max-width: 768px) {
  .xp-hero {
    grid-template-columns: 1fr;
    padding-block: 1.25rem;
  }

  .xp-hero__title {
    font-size: 2.375rem;
    line-height: 1.2;
    letter-spacing: -0.015em;
  }

  .xp-hero__sub {
    font-size: 1rem;
  }

  .xp-hero__cta {
    flex: 1 1 100%;
  }

  .xp-hero__visual {
    order: -1;
  }

  .xp-steps__list,
  .xp-features__grid {
    grid-template-columns: 1fr;
  }
}
</style>
