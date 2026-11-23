---
layout: page
title: Flash Xerif
description: Install Xerif in the browser — stable installer for the Xteink X4 Pro, beta installers for other PSRAM boards, or use an SD card.
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
  :beta-boards="data.betaBoards"
/>
