---
layout: page
title: Flash Xerif
description: Pick your device and install Xerif in the browser over USB, or use an SD card. The web installer serves stable releases; beta boards are supported best-effort.
---

<script setup>
import FlashPage from './.vitepress/theme/components/FlashPage.vue'
import { data } from './flash.data.js'
</script>

<FlashPage
  :version="data.version"
  :bin-url="data.binUrl"
  :bin-name="data.binName"
  :size-m-b="data.sizeMB"
  :release-url="data.releaseUrl"
/>
