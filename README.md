
<h1> <p "font-size:200px;"> Snapmaker Orca FullSpectrum</p> </h1>

### A Snapmaker Orca Fork with Mixed-Color Filament Support

[![Build all](https://github.com/Snapmaker/OrcaSlicer/actions/workflows/build_all.yml/badge.svg?branch=main)](https://github.com/Snapmaker/OrcaSlicer/actions/workflows/build_all.yml)

---

## ☕ Support Development

If you find this fun or interesting!

<a href="https://www.buymeacoffee.com/ratdoux" target="_blank"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me A Coffee" style="height: 60px !important;width: 217px !important;" ></a>

---

## ⚠️ **IMPORTANT DISCLAIMER** ⚠️

**This fork is currently in active development and has been tested on actual hardware.**

- **Use at Your Own Risk**: As with any slicer fork, please review critical prints and generated G-code before production use
- **Project Compatibility Warning**: Some `.3mf` files created with older FullSpectrum builds may not open or migrate cleanly in newer versions because mixed-filament data and project serialization have changed over time

---

**Snapmaker Orca FullSpectrum** is an open source slicer for FDM printers based on Snapmaker Orca and OrcaSlicer, optimized for Snapmaker's U1 multi-color 3D printer with independent tool heads. This fork adds support for virtual mixed-color filaments, enabling you to create new colors by alternating layers between physical filaments.
 


# Download

### Stable Release
📥 **[Download the Latest Stable Release](https://github.com/ratdoux/OrcaSlicer-FullSpectrum/releases)**  
Visit our GitHub Releases page for the latest stable version of Snapmaker Orca FullSpectrum, recommended for most users.

# Features

## Mixed-Color Filaments
Snapmaker Orca FullSpectrum includes support for **virtual mixed-color filaments** designed for the Snapmaker U1 multi-color printer with independent print heads.

### How It Works
- **Create new colors by mixing**: Combine two physical filaments to create a new color appearance through layer alternation
- **Example**: One layer of red + one layer of green = apparent yellow color
- **Customizable ratios**: Adjust the alternation pattern (e.g., 2:1 ratio = two layers of filament A, one layer of filament B)

### Features
- Automatic generation of all possible color combinations from your loaded filaments
- Visual preview showing the additive color blend
- Enable/disable individual mixed filaments
- Per-layer resolution control with customizable ratios
- Optional per-pair Bias control for slightly recessing one component to push the apparent color toward the other
- Seamless integration with the existing filament management system

### Using Mixed Filaments
1. Load 2 or more physical filaments in your printer
2. The "Mixed Colors" panel will automatically appear in the sidebar
3. Each combination shows:
   - Color preview swatch
   - Component filaments (e.g., "Filament 1 + Filament 2")
   - Layer ratio controls (spin controls for fine-tuning)
   - Enable/disable checkbox
4. Mixed filaments can be assigned to objects just like physical filaments
5. During slicing, the mixed filament resolves to alternating layers of its components

### Bias Control
Snapmaker Orca FullSpectrum also includes an optional **Bias** control for mixed filament pairs. When enabled in **Print Settings -> Others -> Mixed Filaments**, each mixed row gets a compact inline Bias value:

- **Positive Bias** recesses the second filament in the pair
- **Negative Bias** recesses the first filament in the pair
- This lets you shift the apparent color without changing the nominal layer cadence
- The inline preview updates to show the estimated apparent mix shift

Example: for a pair like `F1 + F2`, a positive bias makes `F2` sit slightly lower, so `F1` visually dominates more. A negative bias does the opposite and recesses `F1`.

### Dithering Settings
Snapmaker Orca FullSpectrum includes advanced dithering controls to fine-tune the layer alternation behavior for mixed filaments. These settings are found in **Others → Dithering** in the print settings:

#### Dithering Cadence Height A & B
- **What it does**: Controls the height (in mm) of each alternating segment for the two component filaments
- **Cadence Height A**: The height of layers using the first filament in the mix
- **Cadence Height B**: The height of layers using the second filament in the mix
- **Example**: Setting A=0.3mm and B=0.15mm creates a 2:1 ratio pattern where you get twice as much of filament A as filament B
- **Use case**: Fine-tune color intensity by adjusting the relative amounts of each component color

#### Dithering Step Size
- **What it does**: Defines the Z-height increment (in mm) for each dithering step
- **Purpose**: Controls the resolution of the layer alternation pattern
- **Default**: Typically matches your layer height setting
- **Advanced usage**: Set smaller values for smoother color transitions, or larger values for more distinct color banding
- **Compatibility**: Must be compatible with your printer's Z-axis resolution

These settings give you precise control over how your mixed colors appear in the final print, allowing you to achieve different visual effects from the same filament combinations.

### Technical Details
- Virtual filament IDs start after physical filaments (e.g., with 4 physical filaments, first mixed ID is 5)
- Layer-based alternation is computed during tool ordering
- Works with all existing features: supports, infill, and multi-material painting

# How to install
**Windows**: 
1.  Download the installer for your preferred version from the [releases page](https://github.com/ratdoux/OrcaSlicer-FullSpectrum/releases).
    - *For convenience there is also a portable build available.*
    - *If you have troubles to run the build, you might need to install following runtimes:*
      - [MicrosoftEdgeWebView2RuntimeInstallerX64](https://github.com/SoftFever/OrcaSlicer/releases/download/v1.0.10-sf2/MicrosoftEdgeWebView2RuntimeInstallerX64.exe)
          - [Details of this runtime](https://aka.ms/webview2)
          - [Alternative Download Link Hosted by Microsoft](https://go.microsoft.com/fwlink/p/?LinkId=2124703)
      - [vcredist2019_x64](https://github.com/SoftFever/OrcaSlicer/releases/download/v1.0.10-sf2/vcredist2019_x64.exe)
          -  [Alternative Download Link Hosted by Microsoft](https://aka.ms/vs/17/release/vc_redist.x64.exe)
          -  This file may already be available on your computer if you've installed visual studio.  Check the following location: `%VCINSTALLDIR%Redist\MSVC\v142`

**Mac**:
1. Download the DMG for your computer: `arm64` version for Apple Silicon and `x86_64` for Intel CPU.  
2. Drag Snapmaker_Orca.app to Application folder. 
3. *If you want to run a build from a PR, you also need to follow the instructions below:*  
    <details quarantine>
    - Option 1 (You only need to do this once. After that the app can be opened normally.):
      - Step 1: Hold _cmd_ and right click the app, from the context menu choose **Open**.
      - Step 2: A warning window will pop up, click _Open_  
      
    - Option 2:  
      Execute this command in terminal: `xattr -dr com.apple.quarantine /Applications/Snapmaker_Orca.app`
      ```console
          softfever@mac:~$ xattr -dr com.apple.quarantine /Applications/Snapmaker_Orca.app
      ```
    - Option 3:  
        - Step 1: open the app, a warning window will pop up  
            ![image](./SoftFever_doc/mac_cant_open.png)  
        - Step 2: in `System Settings` -> `Privacy & Security`, click `Open Anyway`:  
            ![image](./SoftFever_doc/mac_security_setting.png)  
    </details>
    
**Linux (Ubuntu)**:
 1. If you run into trouble executing it, try this command in the terminal:  
    `chmod +x /path_to_appimage/Snapmaker_Orca_Linux.AppImage`
    
# How to compile
- Windows 64-bit  
  - Tools needed: Visual Studio 2019, Cmake, git, git-lfs, Strawberry Perl.
      - You will require cmake version 3.14 or later, which is available [on their website](https://cmake.org/download/).
      - Strawberry Perl is [available on their GitHub repository](https://github.com/StrawberryPerl/Perl-Dist-Strawberry/releases/).
  - Run `build_release.bat` in `x64 Native Tools Command Prompt for VS 2019`
  - Note: Don't forget to run `git lfs pull` after cloning the repository to download tools on Windows

- Mac 64-bit  
  - Tools needed: Xcode, Cmake, git, gettext, libtool, automake, autoconf, texinfo
      - You can install most of them by running `brew install cmake gettext libtool automake autoconf texinfo`
  - run `build_release_macos.sh`
  - To build and debug in Xcode:
      - run `Xcode.app`
      - open ``build_`arch`/Snapmaker_Orca.Xcodeproj``
      - menu bar: Product => Scheme => Snapmaker_Orca
      - menu bar: Product => Scheme => Edit Scheme...
          - Run => Info tab => Build Configuration: `RelWithDebInfo`
          - Run => Options tab => Document Versions: uncheck `Allow debugging when browsing versions`
      - menu bar: Product => Run

- Ubuntu 
  - Dependencies **Will be auto-installed with the shell script**: `libmspack-dev libgstreamerd-3-dev libsecret-1-dev libwebkit2gtk-4.0-dev libosmesa6-dev libssl-dev libcurl4-openssl-dev eglexternalplatform-dev libudev-dev libdbus-1-dev extra-cmake-modules libgtk2.0-dev libglew-dev libudev-dev libdbus-1-dev cmake git texinfo`
  - run 'sudo ./BuildLinux.sh -u'
  - run './BuildLinux.sh -dsir'


# Note: 
If you're running Klipper, it's recommended to add the following configuration to your `printer.cfg` file.
```
# Enable object exclusion
[exclude_object]

# Enable arcs support
[gcode_arcs]
resolution: 0.1
```

# Troubleshooting

Snapmaker Orca FullSpectrum is still evolving, and some mixed-filament workflows remain experimental. If something looks off, always review the sliced result and generated G-code before starting an important print.

### The app does not start

- **Windows:** If the app does not launch, especially on systems where the username contains special characters, update to the latest build.
- **Linux:** If the AppImage will not run, your system may be missing a compatible glibc version.
- **macOS:** If macOS blocks the app, follow the Gatekeeper workaround steps in the installation section above.

### The preview color does not match the printed result

- The color shown in the UI is only an approximation of the final printed appearance.
- Different filament brands, finishes, and materials can produce very different real-world results even when the preview looks similar.
- If a mixed filament swatch looks wrong in the interface, update to the latest version before troubleshooting further.
- For best results, test each filament combination on a small calibration model before using it on a larger print.

### The prime tower is missing or overlaps the model

- In some cases, a plate that uses only a single mixed color may not show the prime tower correctly in the **Prepare** view.
- If that happens, temporarily add a second material so the tower becomes visible, move it to a safe location, and then continue preparing the print.
- Always double-check prime tower placement before exporting G-code.

### A project opens with incorrect mixed patterns

- After updating to a newer version, older project files may occasionally load with incorrect mixed-filament mappings.
- If a project opens with the wrong gradients, patterns, or filament assignments, recreate the affected mixed entries and verify all assignments before slicing.
- Recheck painted regions and tool mappings whenever opening an older `.3mf` file in a newer release.

### The printer cannot reprint a FullSpectrum file from its screen

- Some users have reported that certain FullSpectrum-sliced files can print normally once, but are not recognized correctly when loaded again from the printer interface.
- If that happens, keep the original project file and resend the job from the slicer or host instead of relying on printer-side reprint.

### Temperatures or tool behavior look wrong in the G-code

- Mixed-filament support is still experimental, so edge cases can produce incorrect or unexpected G-code.
- Before long or important prints, inspect:
  - tool changes
  - bed and nozzle temperature commands
  - wipe and purge behavior
  - first-layer material order
- This is especially important when using custom firmware, non-standard printer profiles, or non-U1 machines.

### Mixed colors look banded or uneven on sloped surfaces

- Because FullSpectrum mixing works by alternating layers or segments, gently curved or sloped surfaces can show visible contour-like banding.
- This effect is more noticeable on organic shapes, domes, and top surfaces where slight Z differences affect the visible color pattern.
- If this happens:
  - try simpler mix patterns
  - reduce complexity in multi-perimeter mixes
  - test smaller step sizes
  - use adaptive layers where appropriate

### Local-Z dithering behaves unexpectedly

- Local-Z dithering has improved across recent releases, but complex paint regions and advanced combinations can still behave unpredictably.
- If a Local-Z print looks wrong:
  - simplify the paint setup
  - reduce the number of overlapping mixed regions
  - disable extra wipe-related overrides
  - test on a small model first

### Colors are shifted to one side of the print

- If one color appears biased to one side, the issue may be mechanical rather than slicer-related.
- Check the following:
  - nozzle and toolhead calibration
  - alignment after nozzle swaps
  - nozzle cleanliness
  - hardware looseness
  - possible nozzle offset issues
- Recalibrating the printer is a good first step before changing slicer settings.

### Before starting a long FullSpectrum print

- Update to the latest available release.
- Reopen older project files carefully and verify all filament mappings.
- Inspect the **Prepare** view for prime tower placement, top-surface appearance, and suspicious pattern behavior.
- Run a small test print whenever using a new filament pair, new pattern, or new release version.
- For critical prints, review the generated G-code before printing.

## Some background
**Snapmaker Orca FullSpectrum** is forked from Snapmaker Orca, which is originally forked from Orca Slicer by SoftFever.

Orca Slicer was originally forked from Bambu Studio, it was previously known as BambuStudio-SoftFever.
Bambu Studio is forked from [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research, which is from [Slic3r](https://github.com/Slic3r/Slic3r) by Alessandro Ranellucci and the RepRap community. 
Orca Slicer incorporates a lot of features from SuperSlicer by @supermerill
Orca Slicer's logo is designed by community member Justin Levine(@freejstnalxndr)

## Acknowledgements
Special thanks to [u/Aceman11100](https://www.reddit.com/user/Aceman11100/) for the inspiration and idea behind the mixed-color filament feature!  


# License
Snapmaker Orca FullSpectrum is licensed under the GNU Affero General Public License, version 3. Snapmaker Orca FullSpectrum is based on Snapmaker Orca.

Snapmaker Orca is licensed under the GNU Affero General Public License, version 3. Snapmaker Orca is based on Orca Slicer by SoftFever.

Orca Slicer is licensed under the GNU Affero General Public License, version 3. Orca Slicer is based on Bambu Studio by BambuLab.

Bambu Studio is licensed under the GNU Affero General Public License, version 3. Bambu Studio is based on PrusaSlicer by PrusaResearch.

PrusaSlicer is licensed under the GNU Affero General Public License, version 3. PrusaSlicer is owned by Prusa Research. PrusaSlicer is originally based on Slic3r by Alessandro Ranellucci.

Slic3r is licensed under the GNU Affero General Public License, version 3. Slic3r was created by Alessandro Ranellucci with the help of many other contributors.

The GNU Affero General Public License, version 3 ensures that if you use any part of this software in any way (even behind a web server), your software must be released under the same license.

Orca Slicer includes a pressure advance calibration pattern test adapted from Andrew Ellis' generator, which is licensed under GNU General Public License, version 3. Ellis' generator is itself adapted from a generator developed by Sineos for Marlin, which is licensed under GNU General Public License, version 3.

The Bambu networking plugin is based on non-free libraries from BambuLab. It is optional to the Orca Slicer and provides extended functionalities for Bambulab printer users.

Filament color blending is powered by [FilamentMixer](https://github.com/justinh-rahb/filament-mixer), an openly licensed library.

# Feedback & Contribution
We greatly value feedback and contributions from our users. Your feedback will help us to further develop Snapmaker Orca FullSpectrum for our community.
- To submit a bug or feature request, file an issue in GitHub Issues.
- To contribute some code, make sure you have read and followed our guidelines for contributing.
