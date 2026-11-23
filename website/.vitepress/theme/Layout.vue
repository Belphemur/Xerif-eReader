<script setup lang="ts">
import { Layout as VPLayout } from 'vitepress/theme'
import SiteFooter from './components/SiteFooter.vue'
</script>

<template>
  <VPLayout>
    <template #layout-bottom>
      <SiteFooter />
    </template>
  </VPLayout>
</template>
