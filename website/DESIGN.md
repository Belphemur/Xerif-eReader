---
name: Tactile E-Paper Editorial
colors:
  surface: '#fcf9f2'
  surface-dim: '#dcdad3'
  surface-bright: '#fcf9f2'
  surface-container-lowest: '#ffffff'
  surface-container-low: '#f6f3ec'
  surface-container: '#f1eee7'
  surface-container-high: '#ebe8e1'
  surface-container-highest: '#e5e2db'
  on-surface: '#1c1c18'
  on-surface-variant: '#3f4947'
  inverse-surface: '#31312c'
  inverse-on-surface: '#f3f0e9'
  outline: '#6f7977'
  outline-variant: '#bec9c7'
  surface-tint: '#1a6964'
  primary: '#004945'
  on-primary: '#ffffff'
  primary-container: '#0e625d'
  on-primary-container: '#93dbd4'
  inverse-primary: '#8cd3cd'
  secondary: '#5f5e5e'
  on-secondary: '#ffffff'
  secondary-container: '#e2dfde'
  on-secondary-container: '#636262'
  tertiary: '#004945'
  on-tertiary: '#ffffff'
  tertiary-container: '#00635d'
  on-tertiary-container: '#83ded5'
  error: '#ba1a1a'
  on-error: '#ffffff'
  error-container: '#ffdad6'
  on-error-container: '#93000a'
  primary-fixed: '#a8f0e9'
  primary-fixed-dim: '#8cd3cd'
  on-primary-fixed: '#00201e'
  on-primary-fixed-variant: '#00504c'
  secondary-fixed: '#e5e2e1'
  secondary-fixed-dim: '#c8c6c5'
  on-secondary-fixed: '#1c1b1b'
  on-secondary-fixed-variant: '#474746'
  tertiary-fixed: '#98f2e9'
  tertiary-fixed-dim: '#7bd6cd'
  on-tertiary-fixed: '#00201e'
  on-tertiary-fixed-variant: '#00504b'
  background: '#fcf9f2'
  on-background: '#1c1c18'
  surface-variant: '#e5e2db'
typography:
  display:
    fontFamily: Newsreader
    fontSize: 56px
    fontWeight: '400'
    lineHeight: 64px
    letterSpacing: -0.02em
  display-mobile:
    fontFamily: Newsreader
    fontSize: 38px
    fontWeight: '400'
    lineHeight: 46px
    letterSpacing: -0.015em
  headline-lg:
    fontFamily: Newsreader
    fontSize: 40px
    fontWeight: '400'
    lineHeight: 48px
    letterSpacing: -0.015em
  headline-lg-mobile:
    fontFamily: Newsreader
    fontSize: 28px
    fontWeight: '400'
    lineHeight: 36px
    letterSpacing: -0.01em
  headline-md:
    fontFamily: Newsreader
    fontSize: 28px
    fontWeight: '400'
    lineHeight: 36px
    letterSpacing: -0.01em
  headline-sm:
    fontFamily: Newsreader
    fontSize: 22px
    fontWeight: '500'
    lineHeight: 28px
    letterSpacing: 0em
  body-lg:
    fontFamily: Inter
    fontSize: 18px
    fontWeight: '400'
    lineHeight: 28px
    letterSpacing: -0.005em
  body-md:
    fontFamily: Inter
    fontSize: 15px
    fontWeight: '400'
    lineHeight: 24px
    letterSpacing: 0em
  body-sm:
    fontFamily: Inter
    fontSize: 13px
    fontWeight: '400'
    lineHeight: 20px
    letterSpacing: 0.01em
  label-lg:
    fontFamily: JetBrains Mono
    fontSize: 13px
    fontWeight: '500'
    lineHeight: 18px
    letterSpacing: 0.04em
  label-md:
    fontFamily: JetBrains Mono
    fontSize: 11px
    fontWeight: '500'
    lineHeight: 16px
    letterSpacing: 0.06em
  label-sm:
    fontFamily: JetBrains Mono
    fontSize: 10px
    fontWeight: '600'
    lineHeight: 14px
    letterSpacing: 0.08em
spacing:
  gutter: 1.5rem
  gutter-mobile: 1rem
  margin: 3rem
  margin-mobile: 1.25rem
  space-xs: 0.25rem
  space-sm: 0.5rem
  space-md: 1rem
  space-lg: 1.75rem
  space-xl: 3rem
rounded:
  sm: 0px
  md: 0px
  lg: 0px
---

## Brand & Style

The brand personality is contemplative, literary, and mechanically precise. It caters to discerning readers, software engineers, typographers, and digital minimalists who seek refuge from glossy, attention-extractive OLED screens. The emotional response should be one of immediate sensory relief—a digital experience that feels dry, unhurried, tangible, and permanent like fresh ink on cotton-pressed stock.

The design movement combines **Minimalism** and **Tactile / Skeuomorphic Hardware Realism** rooted in physical e-ink ergonomics:
- Surface rendering references high-grade newsprint and raw unbleached bond paper.
- Interactions eschew simulated motion blur and hyper-fluid spring physics in favor of crisp, decisive state shifts that evoke optical e-ink micro-refreshes.
- High-contrast editorial authority is balanced with structural engineering precision: dense typographic technical manifests sit comfortably beside vast literary negative space.

## Colors

The palette is anchored in raw newsprint substrates and viscous carbon pigment, punctuated by a solitary archival bookcloth teal.

- **Background Canvas & Surface Paper:** `#fbf9f5` serves as the primary canvas tint, with `#f5f2eb` serving as stacked paper sheets, surface cards, and secondary modules.
- **Deep Ink Black (`#1a1a1a`):** Primary text, major structural rules, high-contrast badges, and interactive focal points. Not pure zero-black, retaining the soft absorption of ink struck onto fibrous paper.
- **Muted Carbon Charcoal (`#525252`):** Body copy, auxiliary descriptions, and secondary metadata.
- **Delicate Ink Rules (`#e5e0d8`):** Thin tectonic separators, structural hairline borders, and table matrices.
- **Archival Deep Teal (`#0e625d`):** Primary accent reserved for active system calls, interactive focal toggles, firmware version highlights, and primary CTAs.
- **Deepened Pine (`#0a4d49`) & Bright Verdigris (`#168079`):** Used strictly for hover states, focused keyboard rings, and selected state chips.

## Typography

The typographic hierarchy establishes a deliberate tension between classical book publishing and technical device architecture.

- **Newsreader (Serif):** Employed for editorial presence, page narratives, chapter markers, and hero hooks. Newsreader features optical sizing attributes that preserve delicacy in large sizes and physical legibility at scale.
- **Inter (Sans):** Neutral, utilitarian, and distraction-free for general prose, explanatory summaries, and long-form change logs.
- **JetBrains Mono (Monospace):** Dedicated to firmware build hashes, hardware register specs, battery voltage readings, dithering algorithms, and device telemetry. Rendered predominantly in uppercase for micro-labels.

## Layout & Spacing

The layout model draws inspiration from broadsheet newspapers and architectural specimen sheets.

- **Grid Architecture:** A 12-column fluid grid system across desktop screens, adapting to a 6-column grid on tablet, and a 4-column column layout on mobile devices.
- **Margins & Rhythm:** Desktop layouts maintain expansive margins (`margin: 3rem`) to frame the page like generous unprinted book margins. Element margins compress linearly down to `1.25rem` on mobile to optimize screen real estate.
- **Column Separation:** Sections and structural columns are demarcated through continuous hairline dividers rather than arbitrary whitespace or dropped shadows, reinforcing mechanical clarity.
- **Reflow Rules:** Data comparison grids and firmware release specifications collapse into vertical index-card stacks on viewport widths beneath 768px.

## Elevation & Depth

This system avoids ambient blur-heavy drop shadows, colored halos, and translucent glass layers. Depth is constructed purely through flat material layering, structural borders, and paper tone shifts:

- **Surface Tiers:**
  - Base canvas is `#fbf9f5`.
  - Level 1 elevated surfaces (cards, sidebars, modal panels) use `#f5f2eb` bordered by a crisp `1px solid #e5e0d8`.
  - Floating overlays use `#fbf9f5` bounded by a distinct `1px solid #1a1a1a` outer stroke paired with a secondary `1px` inner offset border.
- **Paper Relief:** Selected elements and elevated floating tooltips utilize a strict, unblurred hard shadow: `box-shadow: 2px 2px 0px #1a1a1a`. This produces the effect of punch-cut card stock resting directly on a drafting table.
- **Dithering & Texture:** Surface transitions and image placeholders incorporate 1-bit or 2-bit geometric ordered dither stippling patterns instead of smooth linear alpha gradients, evoking physical electrophoretic display capsules.

## Shapes

The geometry is uncompromisingly sharp (`roundedness: 0`). 

- All card panels, input fields, tags, buttons, and alert modules employ crisp right-angle corners (`0px` border-radius).
- The sharp silhouette mirrors sheet paper, metal stamping, and the precise square pixel grids of monochrome display panels.
- Micro-elements (such as status pips and circular mode-switchers) may use pure circles (`50%` radius) strictly when representing physical dials, toggles, or mechanical battery indicators.

## Components

### Buttons
- **Primary:** Solid `#1a1a1a` background, `#fbf9f5` text, zero radius, `1px solid #1a1a1a`. Hover shifts to `#0e625d` with instantaneous color switching (zero transition duration to simulate e-ink refresh cycles).
- **Secondary:** Surface `#fbf9f5`, text `#1a1a1a`, with a hairline border of `1px solid #1a1a1a`. Hover inverts to `#1a1a1a` text on `#f5f2eb` with a hard `2px 2px 0px #1a1a1a` offset shadow.
- **Teal Accent CTA:** Background `#0e625d`, text `#fbf9f5`, bordered in `#0a4d49`. Used sparingly for primary firmware download actions.

### Chips & Badges
- Constructed with `#JetBrains Mono`, uppercase, `label-sm`.
- Bounded by `1px solid #e5e0d8` on `#f5f2eb` surface with `padding: 0.25rem 0.5rem`.
- Active or firmware-status chips invert with `#0e625d` background and crisp `#ffffff` text.

### Lists & Technical Specs
- Technical manifests present specs as ledger lines: key on the left in `#525252` (`JetBrains Mono`), trailing dot leaders (`.....`), and value on the right in `#1a1a1a` bold.
- Editorial lists use fine `1px solid #e5e0d8` horizontal dividers spanning the full column width.

### Checkboxes & Radio Controls
- **Checkboxes:** Square boxes (`16px x 16px`), `1px solid #1a1a1a`, fill `#fbf9f5`. Checked state displays an uncompromising solid `#1a1a1a` interior square with a 2px paper inset margin.
- **Radios:** Circular rings (`16px x 16px`), `1px solid #1a1a1a`. Selected state features a solid `#1a1a1a` concentric disc.

### Input Fields
- Flat background `#fbf9f5`, bordered with `1px solid #e5e0d8`.
- Focus state updates border to `1px solid #1a1a1a` with an instant, sharp `2px 2px 0px #0e625d` offset accent. Text input renders in `JetBrains Mono` or `Inter`.

### Cards & Module Panels
- Set against `#f5f2eb` with crisp `1px solid #e5e0d8` outlines.
- Feature header bands separated by a persistent horizontal hairline. Optional upper-right index markers (e.g., `SEC_01 // 300_DPI`) rendered in `label-sm`.

### Hardware Simulator & Dithering Containers
- Visual frames showcasing firmware screenshots must include a simulated `2px` bezel with outer grounding rule `#1a1a1a`, simulating the flush glass window of dedicated e-readers.