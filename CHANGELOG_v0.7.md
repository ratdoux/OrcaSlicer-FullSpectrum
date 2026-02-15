## v0.7-alpha Pre-release

EXPERIMENTAL BUILD - NOT TESTED
Based on Snapmaker Orca v2.2.4

v0.7 full spectrum alpha focuses on Local Z reliability, better mixed-filament control, and a cleaner custom Gradient/Pattern workflow.

This has had limited testing on the Snapmaker U1 via the community.

### What's New in v0.7

#### Local Z Dithering Reliability (v0.7)
- Improved handling for overlapping mixed-painted zones on the same XY plane.
- Fixed cases where layer-height modulation could flatten or become equal when a second zone starts.
- Fixed stale first-zone behavior where editing one gradient did not correctly update resulting layer heights.
- Reduced repeated same-color layer runs in complex multi-zone transitions.
- Mixed passes and base passes are separated more cleanly in perimeter clipping paths.

#### Manual Pattern Mixed Filaments (v0.6, refined in v0.7)
- Pattern add button in Mixed Filaments UI (alongside Gradient add).
- Custom repeating layer patterns (for example `11112222` for a 4:4 ratio).
- Pattern parser accepts `1/2` or `A/B` with separators (`/`, `-`, `_`, `|`, etc.).
- Manual pattern takes precedence; blend percentage is auto-derived from the pattern.
- Backward-compatible parsing for older custom mixed-row formats.

#### Local Z Dithering Mode (v0.4, refined in v0.7)
- Local Z dithering mode in Process -> Others (`dithering_local_z_mode`).
- Mixed-painted zones are split into local sub-passes with pass-specific flow height.
- Runtime guard remains active when wipe tower or wiping overrides are enabled.

#### Selective Expansion/Contraction (v0.7)
- Added selective expansion/contraction controls for mixed-color behavior tuning in challenging transition regions.
- Intended to help with edge consistency where mixed passes interact with neighboring toolpaths.

#### Mixed Filaments UI Refresh (v0.7)
- More modern and compact layout for custom Gradient and Pattern mixed filaments.
- Reduced spacing and tighter row cards for better readability with many mixed rows.
- Cleaner preview area with summary inline for faster scanning.

#### Config/Save/Load Reliability (v0.5 + v0.7 hardening)
- Config comparison checks type and value.
- Improved dirty-state detection, diffing, and unsaved-changes behavior.
- Mixed filament project settings load more consistently.
- Stronger logging for mixed-row parsing and config application.

#### Same-layer Pointillisme (new in v0.7, experimental)
- What it is: a same-layer color interleaving mode that tries to distribute multiple filaments inside a single layer instead of alternating by layer.
- Goal: test whether very small in-layer color "dots" can produce smoother visual blends than normal layer cycling.
- Status: extremely experimental and may produce unusable results.
- Reality check: this is likely a dead end for production use on current hardware and may be removed or replaced in a future release.

### Installation
1. Download `Snapmaker_Orca.zip`.
2. Extract to a folder.
3. Run the executable.

### Warning
- Use at your own risk.
- May produce incorrect G-code.
- Local Z behavior is still experimental.
- Seeking testers with U1 printers.

### Features Not Yet Tested
1. Local Z height dithering
2. Patterns
3. Selective expansion/contraction
4. Advanced dithering
5. Height-weight cadence

### Known Issues
- Untested on real hardware.
- Local Z mode still needs validation with real prints.
- Advanced dithering output may require calibration.
- Pointillisme is research-grade only right now and is likely not viable for stable, repeatable prints.
- Startup crash may still occur in some setups and needs further debugging.
