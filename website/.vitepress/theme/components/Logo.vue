<script setup lang="ts">
withDefaults(defineProps<{ size?: number }>(), { size: 32 })
</script>

<template>
  <!-- Open-book mark (repo logo, traced as SVG paths), same geometry and
       layering as public/logo.svg: ink cover, paper margin around the
       pages, teal pages, paper X + rising arrow. The cover follows
       currentColor so contexts can retint the primary shape; the paper
       accents use the surface token so the mark adapts to dark mode. -->
  <svg
    xmlns="http://www.w3.org/2000/svg"
    viewBox="0 0 162 180"
    :width="size"
    :height="size"
    role="img"
    aria-label="Xerif"
    class="xp-logo"
  >
    <title>Xerif</title>
    <g transform="translate(-2.89 28.8) scale(0.7232)">
      <path fill="currentColor" d="M14 21 H218 Q228 21 228 31 V139 Q228 147 220 151.5 C198 162.5 158 170.5 124 172 Q116 172.5 108 172 C74 170.5 34 162.5 12 151.5 Q4 147 4 139 V31 Q4 21 14 21 Z" />
      <!-- Paper margin carved around the pages -->
      <path
        fill="var(--xp-paper-alt)"
        stroke="var(--xp-paper-alt)"
        stroke-width="14"
        stroke-linejoin="round"
        opacity="0.55"
        d="M36 4 H54 C80 5.5 98 14 112 28 C113.5 55 114 80 113 100 C112.3 115 111.2 128 110 136 C84 133.5 52 130 36 127 Q30 126 30 120 V10 Q30 4 36 4 Z"
      />
      <path
        fill="var(--xp-paper-alt)"
        stroke="var(--xp-paper-alt)"
        stroke-width="14"
        stroke-linejoin="round"
        opacity="0.55"
        d="M196 4 H178 C152 5.5 134 14 120 28 C118.5 55 118 80 119 100 C119.7 115 120.8 128 122 136 C148 133.5 180 130 196 127 Q202 126 202 120 V10 Q202 4 196 4 Z"
      />
      <path fill="var(--xp-teal-selected)" d="M36 4 H54 C80 5.5 98 14 112 28 C113.5 55 114 80 113 100 C112.3 115 111.2 128 110 136 C84 133.5 52 130 36 127 Q30 126 30 120 V10 Q30 4 36 4 Z" />
      <path fill="var(--xp-teal-selected)" d="M196 4 H178 C152 5.5 134 14 120 28 C118.5 55 118 80 119 100 C119.7 115 120.8 128 122 136 C148 133.5 180 130 196 127 Q202 126 202 120 V10 Q202 4 196 4 Z" />
      <!-- X + rising arrow knockout -->
      <path fill="var(--xp-paper-alt)" d="M63.6 44 H88.5 L114.5 63.7 V82.4 Z" />
      <path fill="var(--xp-paper-alt)" d="M131.6 76.6 L168.4 104.3 L159.4 116.3 L122.6 88.6 Z" />
      <path fill="var(--xp-paper-alt)" d="M50 105.5 C72 109 94 104 112 93 C126 82 138 70 146 59 L154.6 68.5 C138 80 122 94.5 108 104 C92 113.5 70 116 50 105.5 Z" />
      <path fill="var(--xp-paper-alt)" d="M130 42 L171 37.5 L159 74.5 Z" />
    </g>
  </svg>
</template>

<style scoped>
.xp-logo {
  color: var(--xp-teal);
  display: inline-block;
  flex: none;
}
</style>
