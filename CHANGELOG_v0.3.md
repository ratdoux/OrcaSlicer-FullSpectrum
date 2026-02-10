## ⚠️ EXPERIMENTAL BUILD - NOT TESTED ⚠️

Based on Snapmaker Orca v2.2.4

This is an alpha release of Full Spectrum with mixed-color filament support.

**THIS HAS NOT BEEN TESTED ON ACTUAL HARDWARE!**

### What's New
- **Mixed Filaments UI**: Renamed the feature from Dithering to Mixed Filaments and improved row labeling.
- **Auto + Custom Mixed Filaments**: Auto-generated mixed rows from physical filaments plus manual custom rows via `+`.
- **Gradient Selector**: Added visual gradient picking for custom mixed rows to control A/B blend ratio.
- **Cadence Controls**: Added Layer cycle cadence, Height-weighted cadence toggle, and Advanced dithering mode.
- **Mixed Height Bounds**: Added lower/upper mixed filament height bounds and cycle controls in Process -> Others.
- **Z Step Controls**: Added dithering Z step size and painted-zone behavior options.

### Changelog
- Added mixed filament dependency cleanup when a physical filament is removed.
- Added Process -> Others controls for mixed filament cadence and height behavior.
- Fixed multiple startup/UI crashes related to mixed filament rows and Others-tab interactions.
- Fixed gradient selector state handling and crash on color change.
- Improved sidebar behavior with compact mixed-row spacing and scroll handling for many mixed rows.
- Improved mixed preview colors with weighted RYB pigment-style blending and normalization.
- Added invalidation propagation so mixed-setting changes correctly require re-slicing.
- Updated mixed-setting tooltips with experimental disclaimers and a wiki-availability note.

### Installation
1. Download `Snapmaker_Orca.exe` (or the full package)
2. Extract to a folder
3. Run the executable

### ⚠️ Warning
- Use at your own risk
- May produce incorrect G-code
- Seeking testers with U1 printers

### Known Issues
- Untested on real hardware
- Mixed filament behavior may require per-material calibration
- Advanced dithering is highly experimental and may not match normal dithering color output
- Height-weighted cadence operates at the layer plane level, not independent per-color subregions in the same XY plane
