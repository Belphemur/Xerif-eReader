---
layout: page
title: Flash Xerif
description: Install Xerif on your Xteink X4 Pro — flash it in the browser or use an SD card. Stable channel only.
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
