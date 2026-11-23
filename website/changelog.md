---
layout: page
title: Changelog
description: Every stable Xerif release — highlights, notes, and one-click flash per version.
search: false
---

<script setup>
import ChangelogPage from './.vitepress/theme/components/ChangelogPage.vue'
import { data } from './changelog.data.js'
</script>

<ChangelogPage :releases="data.releases" />
