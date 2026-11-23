// Build-time data loader for the /changelog page.
import { getReleases } from './.vitepress/lib/releases.mjs'

/** Light markdown strip: headers, emphasis, inline code, list bullets. */
function plainLine(line) {
  return line
    .replace(/^#{1,6}\s+/, '')
    .replace(/^[-*]\s+/, '')
    .replace(/^(\d+\.)\s+/, '$1 ')
    .replace(/\*\*([^*]+)\*\*/g, '$1')
    .replace(/`([^`]+)`/g, '$1')
    .trim()
}

function summarize(body) {
  if (!body) return []
  const lines = body
    .split(/\r?\n/)
    .filter((l) => l.trim() && !/^<\/?details|^<summary|^<\/?img/i.test(l.trim()))
    .map(plainLine)
    .filter(Boolean)
  return lines.slice(0, 40)
}

export default {
  async load() {
    const releases = await getReleases()
    return {
      releases: releases.map((r) => ({
        tag: r.tag,
        version: r.version,
        name: r.name,
        url: r.url,
        publishedAt: r.publishedAt,
        date: r.publishedAt ? r.publishedAt.slice(0, 10) : '',
        highlights: summarize(r.body),
        upstream: /upstream/i.test(r.body),
        flashable: r.assets.some((a) => /-x4pro\.bin$/.test(a.name)),
      })),
    }
  },
}
