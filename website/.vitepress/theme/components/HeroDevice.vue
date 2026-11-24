<template>
  <!-- Dedicated 512x512 hero visual: bezel-only e-reader with the Xerif
       monogram as an e-ink boot splash and floating paper particles. -->
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

    <!-- Boot splash: logo over a rules line, e-ink paper tone -->
    <g class="xp-hero-device__mark" transform="translate(240.5 150) scale(0.19)">
      <path d="M141.133 1.93774C150.102 -3.44192 161.517 3.01665 161.521 13.4749V166.526C161.517 176.984 150.102 183.442 141.133 178.063L80.7588 141.837L20.3867 178.063C11.4176 183.442 0.00328866 176.984 0 166.526V13.4749C0.00337257 3.01685 11.4176 -3.44137 20.3867 1.93774L80.7598 38.1614L141.133 1.93774ZM121.275 117.524L92.3848 134.864L147.281 167.811C148.277 168.408 149.542 167.685 149.545 166.526V100.567L121.275 117.524ZM14.2393 12.1897C13.2438 11.5924 11.979 12.3152 11.9756 13.4749V86.6125L74.7725 124.29V48.51L14.2393 12.1897Z" />
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

.xp-hero-device__mark path {
  fill: var(--xp-teal);
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
