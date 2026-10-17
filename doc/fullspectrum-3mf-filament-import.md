# 3MF filament import

Interactive imports of 3MF models with a filament color palette offer two choices:

1. **Keep loaded filaments and match imported colors.** Keep the physical filament count, colors, profiles and existing mixed filaments. Match each imported color to a physical filament or a mixed filament made from the loaded palette. Reuse existing matches and create new mixes when they improve the match. This also works with one loaded filament and files containing four or fewer colors. Saved Full Spectrum mixed colors, gradients and patterns are matched by their displayed color; their original recipes are not retained in this mode.
2. **Replace loaded filaments with the ones from the 3MF.** Adopt the source filament palette and profiles. Preserve Full Spectrum mixed recipes and their assignments. The old four-filament overflow conversion does not apply to this explicit choice; the supported physical-slot limit still applies.

Bambu/Orca and Prusa project readers already load object/part assignments and multi-material facet painting. Matching remaps those assignments, including the implicit first filament used by unassigned objects. Full Spectrum image-map palette entries also receive updated fallback IDs and stable mixed-filament IDs.

Opening an interactive Bambu/Orca/Full Spectrum 3MF containing models and settings now offers a project import choice before the filament choice:

1. **Import models and colors; keep current printer and print settings.** Retain the selected printer, bed, process and machine settings.
2. **Import models, colors and compatible print settings; keep current printer.** Also transfer shared wall, shell, infill, support, brim/skirt and seam settings. Layer heights transfer only when supported by all current nozzles. Speeds, accelerations, extrusion widths, purge/AMS settings, machine G-code, printer/process preset identities and bed configuration remain unchanged.
3. **Open full project, including its printer and all settings.** Explicitly allow the source printer and full configuration to load.

The first choice is the default when the source printer differs or cannot be identified; matching printer identities default to opening the full project. Both printer-preserving choices work with either filament choice. Compatible object, part and height-range overrides follow the same filter; color assignments and painted regions remain available for remapping. Source global color assignments become object defaults rather than changing the current global filament IDs. Incompatible adaptive layer-height profiles and source custom layer G-code are discarded. Source plates and pre-sliced G-code are not loaded on these paths; geometry uses the current bed.

An explicit geometry import also retains the current printer/process settings even when replacing filament profiles. Prusa printer/process settings remain unsupported by the GUI import path. Files without a readable filament palette cannot be color-matched by this workflow. This does not add a general 3MF texture/material-extension decoder.

Recovery and silent imports keep their existing behavior and do not display the new dialog. Canceling the filament dialog does not modify the filament palette. Opening a project still uses the existing project reset/undo workflow.

## Regression checks

Automated coverage lives in the `[MixedFilament][Import]` tests and the persistent OBJ image-map test. It covers palette counts, preserving native mixed IDs on the replacement path, source mixed-color ordering, profile metadata when counts differ, implicit filament assignments, painted facets and image-map stable IDs. Project import regressions cover default selection, excluding machine-specific options, compatible process transfer, nozzle layer-height bounds, object/part/layer filtering and preserving preset snapshots with their vendor ownership.

Manual GUI checks:

- With a U1 selected and an empty bed, open a Bambu printer project. Verify the default imports models/colors and keeps the U1, bed, process, speeds and machine G-code. Repeat with Keep and Replace filament choices.
- Select compatible print settings and verify source walls/infill/support settings transfer, while U1 machine settings and speeds remain. Check object/part/height-range overrides and layer heights outside the U1 nozzle limits.
- Select full project and verify the Bambu printer/settings load as before. Repeat printer-preserving import after selecting a printer/process embedded in an earlier project.
- Import a Bambu painted 3MF with two colors absent from the loaded palette; choose Keep and verify both regions in Prepare and after slicing.
- Repeat with one loaded filament, and with more than four source colors.
- Import a Prusa painted 3MF with both choices; verify the printer/process selections remain unchanged.
- Import geometry into a nonempty project and choose Replace; verify colors and filament profiles are replaced even when the filament counts are equal.
- Open a Full Spectrum 3MF with saved mixed colors and image maps; verify both choices, then save/reopen the result.
- Cancel the dialog and verify filament settings are unchanged; verify recovery does not prompt.
