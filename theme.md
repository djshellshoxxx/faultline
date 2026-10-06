=== VISUAL IDENTITY SPEC (apply to all UI elements) ===

COLOR PALETTE (core, use across every plugin):
- Background base:      #0E1116  (near-black, slight blue undertone)
- Panel surface:        #171B22  (raised sections, module backgrounds)
- Panel edge / bevel:   #2A303A  (1px inner strokes, subtle separators)
- Primary accent:       #E8532A  (burnt orange, for active values, meter peaks, selected states)
- Secondary accent:     #4FB6C4  (muted teal, for secondary indicators, LFO/mod, links between controls)
- Text primary:         #E6E8EC  (labels, values)
- Text muted:           #8A929E  (units, hints, inactive)
- Success / signal-on:  #7BC96F
- Warning / clip:       #F2C14E
- Shadow:               rgba(0,0,0,0.55) with 8px blur, y+2 offset

Each plugin may re-tint ONE accent for its own identity (e.g. purple instead of orange), but the neutrals, layout, and control shapes stay identical.

TYPOGRAPHY:
- Primary UI font: "Inter" (fallback: "Space Grotesk", then system sans). Weight 500 for labels, 600 for section headers.
- Numeric readouts: "JetBrains Mono" or "IBM Plex Mono", weight 500, tabular-nums enabled.
- Label size: 11px, uppercase, letter-spacing +0.08em.
- Value readouts: 13-14px, mixed case, no letter-spacing.
- All text anti-aliased, never pixel fonts.

KNOBS (standard across all plugins):
- Shape: circular, flat-shaded, 48px default (36px small, 64px large).
- Body: radial gradient from #232833 (top) to #14181F (bottom), 1px inner stroke #2A303A.
- Indicator: a single 2px wide accent-color line from center to rim, rounded cap.
- Value arc: 270deg sweep (7-o'clock to 5-o'clock), 3px thick, drawn OUTSIDE the knob body with a 4px gap. Unfilled portion is #2A303A at 60% opacity, filled portion is primary accent.
- Center dot: 4px, matches value arc color when active, muted when at default.
- Double-click resets to default. Right-click opens value entry.
- Label sits below knob, value readout sits above (only visible on hover or during drag, otherwise shows label).

SLIDERS / FADERS:
- Track: 4px wide, #2A303A, rounded ends.
- Fill: primary accent, same rounding.
- Thumb: 16x24 rounded rect, same gradient as knob body, 1px accent stroke.
- Ticks: 1px muted lines every 6dB or logical interval, outside the track.

BUTTONS / TOGGLES:
- Rounded rectangles, 4px radius, 28px tall.
- Off state: panel surface color, muted text.
- On state: accent fill at 15% opacity background, accent-colored text, 1px accent border.
- Momentary presses show a brief 100ms accent flash.

METERS:
- Segmented look but rendered smoothly (no visible LED gaps). Gradient: teal (low) -> orange (nominal) -> yellow (near clip) -> red (clip).
- Peak hold: 1px line, holds 1.5s then falls at 20dB/s.
- Numeric peak readout in mono font, top-right of meter.

LAYOUT PRINCIPLES:
- 8px base grid. All spacing in multiples of 8 (or 4 for fine detail).
- Sections separated by 1px #2A303A horizontal lines, never boxes-inside-boxes.
- Section headers: uppercase, primary text color, small accent bar (2px wide, 12px tall) to the left.
- Corner radius: 6px on the main window, 4px on inner panels, 2px on small controls.
- Generous padding: minimum 16px from window edge to any control.
- No skeuomorphism, no faux wood, no faux metal textures. Flat with subtle depth from shadows and gradients only.

INTERACTION FEEL:
- All value changes animate over 80ms ease-out.
- Hover state: control brightens ~8%, cursor becomes vertical resize on knobs/sliders.
- Drag: vertical for fine control, hold Shift for coarse, hold Ctrl/Cmd for ultra-fine.
- Tooltips: dark pill with muted text, appear after 400ms hover.

PLUGIN HEADER (present on every plugin):
- Top strip 32px tall, panel surface color.
- Left: plugin name in 14px semibold, primary text.
- Right: small gear icon (settings), preset selector dropdown, A/B compare buttons.
- 1px separator line below header.

SIGNATURE ELEMENT (identity marker across all plugins):
- A 2px accent-colored diagonal notch in the top-left corner of the main window (12px long, 45deg), acts as a subtle brand mark.
- Bottom-right footer shows plugin version in 9px muted mono font.

ANIMATIONS
- a white "LED" light in the top left corner of the VST that goes from a medium gray (unlit) to white (turned on) depending on the level of output. Whenever the DB of the output goes above 0db the light will turn red until the audio goes below 0 again. -infinity is the dar gray color and gets brigher as it approaches 0db.
-If there are any empty sections of the VST there is about 10 lines of small font text that displays a rolling stream of information from the vsts internals such as midi information, audio information and control changes. The top two and bottom two lines become transparent to the point of being faded away. This should be a real stream of data but should be mostly for looks (similar to 'the matrix' scrolling text) and it should be light green, partially transparent and non intrusive. The scrolling will stop when there is no new data and continues when changes are being made to the vst. If there is no space that is suitable for this, skip it.

=== END VISUAL IDENTITY SPEC ===