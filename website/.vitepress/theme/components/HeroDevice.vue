<template>
  <!-- Dedicated 512x512 hero visual: bezel-only e-reader with the open-book
       mark (repo logo, traced as SVG paths) as an e-ink boot splash and
       floating paper particles. -->
  <svg
    xmlns="http://www.w3.org/2000/svg"
    viewBox="0 0 512 512"
    class="xp-hero-device"
    role="img"
    aria-label="A slim e-reader showing the Xerif logo on its e-ink screen"
  >
    <!-- Floating paper particles -->
    <g class="xp-hero-device__motes">
      <rect x="88" y="112" width="14" height="14" transform="rotate(12 95 119)" />
      <rect x="424" y="84" width="10" height="10" transform="rotate(-18 429 89)" />
      <rect x="452" y="332" width="12" height="12" transform="rotate(24 458 338)" />
      <rect x="52" y="300" width="9" height="9" transform="rotate(-9 56 304)" />
      <rect x="404" y="228" width="7" height="7" transform="rotate(30 407 231)" />
      <rect x="120" y="396" width="11" height="11" transform="rotate(-15 125 401)" />
    </g>

    <!-- Bezel -->
    <rect
      x="118"
      y="34"
      width="276"
      height="444"
      rx="10"
      class="xp-hero-device__bezel"
    />
    <!-- Screen -->
    <rect x="140" y="72" width="232" height="308" class="xp-hero-device__screen" />

    <!-- Boot splash: open-book mark (repo logo, traced) over a rules line,
         X + arrow as an e-ink paper knockout -->
    <g class="xp-hero-device__mark" transform="translate(178.28 108) scale(0.67)">
      <path class="xp-hero-device__mark-cover" d="M14 21 H218 Q228 21 228 31 V139 Q228 147 220 151.5 C198 162.5 158 170.5 124 172 Q116 172.5 108 172 C74 170.5 34 162.5 12 151.5 Q4 147 4 139 V31 Q4 21 14 21 Z" />
      <path class="xp-hero-device__mark-halo" d="M36 4 H54 C80 5.5 98 14 112 28 C113.5 55 114 80 113 100 C112.3 115 111.2 128 110 136 C84 133.5 52 130 36 127 Q30 126 30 120 V10 Q30 4 36 4 Z" />
      <path class="xp-hero-device__mark-halo" d="M196 4 H178 C152 5.5 134 14 120 28 C118.5 55 118 80 119 100 C119.7 115 120.8 128 122 136 C148 133.5 180 130 196 127 Q202 126 202 120 V10 Q202 4 196 4 Z" />
      <path class="xp-hero-device__mark-page" d="M36 4 H54 C80 5.5 98 14 112 28 C113.5 55 114 80 113 100 C112.3 115 111.2 128 110 136 C84 133.5 52 130 36 127 Q30 126 30 120 V10 Q30 4 36 4 Z" />
      <path class="xp-hero-device__mark-page" d="M196 4 H178 C152 5.5 134 14 120 28 C118.5 55 118 80 119 100 C119.7 115 120.8 128 122 136 C148 133.5 180 130 196 127 Q202 126 202 120 V10 Q202 4 196 4 Z" />
      <g class="xp-hero-device__mark-x">
        <path d="M63.6 44 H88.5 L114.5 63.7 V82.4 Z" />
        <path d="M131.6 76.6 L168.4 104.3 L159.4 116.3 L122.6 88.6 Z" />
        <path d="M50 105.5 C72 109 94 104 112 93 C126 82 138 70 146 59 L154.6 68.5 C138 80 122 94.5 108 104 C92 113.5 70 116 50 105.5 Z" />
        <path d="M130 42 L171 37.5 L159 74.5 Z" />
      </g>
    </g>
    <text x="256" y="292" class="xp-hero-device__word" text-anchor="middle">Xerif</text>
    <rect x="196" y="310" width="120" height="2" class="xp-hero-device__rule" />
    <text x="256" y="344" class="xp-hero-device__boot" text-anchor="middle">Ready to read</text>

    <!-- Buttons -->
    <rect x="150" y="404" width="88" height="34" rx="4" class="xp-hero-device__btn" />
    <rect x="274" y="404" width="88" height="34" rx="4" class="xp-hero-device__btn" />
    <rect x="166" y="418" width="56" height="6" class="xp-hero-device__btn-ink" />
    <rect x="290" y="418" width="56" height="6" class="xp-hero-device__btn-ink" />
  </svg>
</template>

<style scoped>
.xp-hero-device {
  display: block;
  width: 100%;
  height: auto;
  max-width: 26rem;
  margin-inline: auto;
}

.xp-hero-device__motes rect {
  fill: var(--xp-rule-strong);
}

.xp-hero-device__bezel {
  fill: var(--xp-ink);
  filter: drop-shadow(4px 4px 0 var(--xp-rule-strong));
}

.xp-hero-device__screen {
  fill: var(--xp-paper-alt);
  stroke: var(--xp-rule);
  stroke-width: 1;
}

.xp-hero-device__mark-cover {
  fill: var(--xp-ink);
}

/* Paper margin carved around the pages where they overlap the cover; the
   stroke widens the page silhouette so the screen tone shows through. */
.xp-hero-device__mark-halo {
  fill: var(--xp-paper-alt);
  stroke: var(--xp-paper-alt);
  stroke-width: 14;
  stroke-linejoin: round;
}

.xp-hero-device__mark-page {
  fill: var(--xp-teal);
}

.xp-hero-device__mark-x path {
  fill: var(--xp-paper-alt);
}

.xp-hero-device__word {
  font-family: var(--xp-serif);
  font-size: 30px;
  letter-spacing: 0.02em;
  fill: var(--xp-ink);
}

.xp-hero-device__rule {
  fill: var(--xp-teal);
}

.xp-hero-device__boot {
  font-family: var(--xp-mono);
  font-size: 11px;
  letter-spacing: 0.18em;
  fill: var(--xp-ink-subtle);
}

.xp-hero-device__btn {
  fill: var(--xp-surface-strong);
}

.xp-hero-device__btn-ink {
  fill: var(--xp-ink-subtle);
}
</style>
