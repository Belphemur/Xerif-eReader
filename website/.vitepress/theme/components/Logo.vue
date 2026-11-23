<script setup lang="ts">
withDefaults(defineProps<{ size?: number }>(), { size: 32 })
</script>

<template>
  <svg
    xmlns="http://www.w3.org/2000/svg"
    viewBox="0 0 64 64"
    :width="size"
    :height="size"
    role="img"
    aria-label="Xerif monogram"
    class="xp-logo"
  >
    <g stroke-linejoin="round" stroke-linecap="round">
      <g fill="currentColor">
        <path d="M10 10 L54 54 L48 60 Q19 45 4 16 Z" />
        <path d="M10 10 L54 54 L60 48 Q45 19 16 4 Z" />
      </g>
      <path d="M10 10 L54 54" stroke="var(--xp-paper)" stroke-width="1.6" />
      <g fill="currentColor" stroke="var(--xp-paper)" stroke-width="2.4">
        <path d="M54 10 L10 54 L16 60 Q45 45 60 16 Z" />
        <path d="M54 10 L10 54 L4 48 Q19 19 48 4 Z" />
      </g>
      <path d="M54 10 L10 54" stroke="var(--xp-paper)" stroke-width="1.6" />
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
