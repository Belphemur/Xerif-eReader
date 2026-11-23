// Loads the vendored esp-web-tools bundle (copied to public/esp-web-tools by
// the build) so <esp-web-install-button> is defined before it can be clicked.
// The bundle is a module script; loading it once registers the element, and
// already-rendered elements upgrade automatically.
let loader = null

export function loadEspWebTools() {
  if (!loader) {
    loader = new Promise((resolve, reject) => {
      const script = document.createElement('script')
      script.type = 'module'
      script.src = '/esp-web-tools/install-button.js'
      script.onload = () => resolve()
      script.onerror = () => {
        loader = null
        reject(new Error('Failed to load the esp-web-tools installer'))
      }
      document.head.appendChild(script)
    })
  }
  return loader
}
