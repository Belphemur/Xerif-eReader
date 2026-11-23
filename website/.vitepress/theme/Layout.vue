<script setup lang="ts">
import DefaultTheme from 'vitepress/theme'
import SiteFooter from './components/SiteFooter.vue'

const VPLayout = DefaultTheme.Layout
</script>

<template>
  <VPLayout>
    <template #layout-bottom>
      <SiteFooter />
    </template>
  </VPLayout>
</template>
