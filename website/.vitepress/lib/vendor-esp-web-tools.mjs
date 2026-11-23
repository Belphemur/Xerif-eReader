// Copies the esp-web-tools web component bundle into public/ so the site
// self-hosts the flasher instead of pulling it from a third-party CDN.
// Runs when the VitePress config is loaded (dev and build).
import { cpSync, existsSync, mkdirSync, rmSync } from 'node:fs'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = dirname(fileURLToPath(import.meta.url))
const WEBSITE = resolve(HERE, '..', '..')
const SRC = resolve(WEBSITE, 'node_modules', 'esp-web-tools', 'dist', 'web')
const DEST = resolve(WEBSITE, 'public', 'esp-web-tools')

export function vendorEspWebTools() {
  if (!existsSync(SRC)) {
    console.warn('[xerif] esp-web-tools is not installed; the /flash installer will be inert')
    return false
  }
  mkdirSync(dirname(DEST), { recursive: true })
  rmSync(DEST, { recursive: true, force: true })
  cpSync(SRC, DEST, { recursive: true })
  return true
}
