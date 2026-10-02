# Procyon UI components

The shared component vocabulary for every Procyon UI. The macOS implementation lives in
`apps/macos/Sources/ProcyonDesign`. The Windows (Win32) and Linux (GTK) UIs must implement the
same components with the same anatomy, tokens and behaviour, so the app looks and feels the same everywhere.

**Source of truth for values:** `design/tokens.json` → `scripts/gen-tokens.py` → platform files
(`Tokens.generated.swift` today; Win32 header and GTK CSS emitters go in the same script).
Never hard-code a color, spacing, radius or font size in a screen.

## Rules

1. **Unknown is not zero.** The core reports unknown values as `-1` or `PC_PROC_RESTRICTED`.
   Render `—` (`Format.unavailable`) and dim the row. Never show 0.
2. **Capabilities decide visibility.** If `pc_capabilities()` lacks a flag (e.g. `PC_CAP_PROCESS_NETWORK`
   on macOS), hide the column or section. Only add an `InfoBanner` where the absence would confuse the user.
3. **No per-tick animation.** Live values change without animation. Any animation tied to data updates
   re-runs the render loop and costs several percent CPU (measured on macOS: numeric-text transitions, gauge
   easing and a SwiftUI pulse cost ~70% CPU in total). Use OS compositor animations
   (Core Animation, DirectComposition, GTK CSS) for decoration that must move.
4. **No blur on live content.** Shadows go on static background shapes. A glow is drawn as wider
   translucent strokes, not a blur filter.
5. **Metric identity is fixed:** CPU = blue→cyan, Memory = violet→pink, Disk = amber→orange,
   Network = emerald→lime, GPU = rose→orange, Battery = lime→green, Energy = yellow→lime (`metric` tokens). A secondary
   series (write, upload, system) uses the gradient's end color.

## Foundation

| Name | Purpose |
| --- | --- |
| `Tokens.Palette` | Semantic colors with light/dark pairs (`background`, `surface`, `surfaceRaised`, `surfaceSunken`, `border`, `text*`, `accent`, `success`, `warning`, `danger`, `chartGrid`, `track`). |
| `Metric` / `MetricStyle` | Resource identity: gradient start/end and symbol. |
| `Format` | Formats percentages, CPU (100% = one core), bytes (base 1024), rates, durations and counts. Unknown values become `—`. |
| `cardSurface()` | Card: `surface` fill, optional tint gradient (top-left, 14% dark / 8% light), 1px `border`, `shadow.card` on the background only. |
| `glassChrome()` | Floating control chrome: Liquid Glass on macOS 26+, material elsewhere. |

## Components

| Component | Anatomy | Notes |
| --- | --- | --- |
| `MetricIcon` | Rounded square (radius 28% of size), metric gradient, white symbol, soft colored shadow | Static, so the shadow is fine. |
| `Sparkline` | Time-series line + gradient area, optional dashed grid, end-point dot, optional stroke halo | Newest sample on the right; `window` seconds visible (60). Auto-scale rounds up to 1/2/2.5/5×10ⁿ. |
| `LiveChart` | Legend row (capsule swatch, label, value) → `Sparkline` with 4 grid lines + right axis labels (max, half, 0) → "60 seconds … now" | Used for every resource's main chart. |
| `RingGauge` | Track circle, gradient arc from 12 o'clock, translucent halo arc, centre slot | 0…1, no easing. |
| `UsageBar` | Capsule track + gradient fill | Table cells, volumes, top lists. |
| `StackedBar` | Proportional segments (2pt gaps) + legend grid (swatch, label, value) | Memory composition. |
| `MetricCard` | Icon + title, big value + unit, caption, optional ring gauge, full-bleed sparkline at the bottom | Dashboard tiles; card tinted with the metric color. |
| `CoreGrid` | Adaptive grid of sunken tiles: label, %, mini sparkline, 2pt usage bar at the bottom | Per-core CPU. |
| `PageHeader` | Optional 40pt `MetricIcon`, title (`title`), subtitle, trailing slot | Every screen. |
| `Panel` | Uppercase tracked caption + symbol, accessory slot, content, inside a card | Sections. |
| `StatGrid` / `StatItem` | Adaptive grid of label, value (17pt rounded semibold), optional tint dot and detail | Key/value facts. |
| `Badge` | Capsule, 12% tone fill, 20% tone hairline, caption font, optional symbol | Tones: neutral, accent, success, warning, danger. |
| `LiveIndicator` | 7pt dot + compositor-driven pulse ring | Grey and still when paused. |
| `SearchField` | Magnifier, plain text field, clear button or `⌘F` key cap, accent ring when focused | Escape clears the field and leaves it. |
| `ValueText` | Monospaced-digit value + smaller secondary unit | Big numbers. |
| `InfoBanner`, `EmptyState`, `KeyCap` | Notices, empty results, shortcut hints | |

## Screen patterns

- **Window:** sidebar (brand, Overview, Processes, a Performance section with live sparkline rows,
  System, live/pause controls pinned to the bottom) plus a detail page on `background`.
- **Processes:** header with a view-mode segmented control (All / By App / Tree), a search field, Expand/Collapse
  all and a red **End Task** button. Below them, a table in a card. CPU, Memory and Disk cells are heat cells:
  the metric color at 8–50% opacity, scaled by load.
- **End task:** plain End Task on a user process runs at once. Force quit, end process tree, group actions
  and anything flagged `PC_PROC_SYSTEM` ask for confirmation first. `PC_PROC_PROTECTED` disables the actions.
