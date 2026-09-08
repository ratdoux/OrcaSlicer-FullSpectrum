# Snapmaker Orca FullSpectrum v0.9.13

Maintenance release fixing mixed-filament assignments when importing saved 3MF projects.

## Quick Overview

- Fixes mixed-filament assignments resetting to **filament 1** in projects with more than four physical filaments.
- Preserves the saved physical-filament palette and mixed recipes instead of applying the four-color import conversion.
- Covers opening projects and importing geometry into an empty project, including older FullSpectrum projects with mixed-filament definitions.

## Notes

- To restore affected projects, reopen the original 3MF that still contains the mixed-filament assignments.
- Release downloads: [FullSpectrum Releases](https://github.com/ratdoux/OrcaSlicer-FullSpectrum/releases).
