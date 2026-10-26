# Continuous color islands for SML

SML (Subdivide Mix Layers) reproduces mixed colors by stacking thin layers of their component filaments. This improvement lets neighboring mixtures share continuous islands of the same physical filament, even when their layers have different thicknesses or Z heights. The aim is to retain SML's color reproduction while reducing visible boundaries between mixed-color regions.

Previously, each mixture generated its own stack. A magenta-rich region and a cyan-rich region could both contain cyan, but their cyan layers ended independently at the recipe boundary. Those repeated stops created a visible seam.

The improved planner connects compatible same-color regions and varies both extrusion height and Z along the connecting paths. A thin black layer can gradually meet a thicker black layer at a different height, forming one non-planar island. Each island has its own perimeter and, when Local-Z infill is enabled, matching interior paths. Connections require an available path through eligible material regions.

This works with compatible two-, three-, and four-filament mixtures. For example, neighboring cyan–magenta and cyan–magenta–yellow recipes can share cyan and magenta islands, while yellow forms its own closed island and tapers inside the region containing it. Shared colors can continue across recipe boundaries without introducing yellow into a region that does not contain it.

At a boundary with pure cyan, the pure region retains its nominal layers. Those smaller cyan layers can extend into the transition as yellow retreats, avoiding unnecessary thickening of magenta or a temporary bulge in the final cyan bead. The original blend proportions are retained away from the transition; the narrow transition itself redistributes material to make the connection.

The intended result is fewer abrupt layer endings and a smoother surface between color zones. In the reference project, the yellow taper was shortened from approximately 1.81 mm to 0.68 mm. Its actual length depends on the recipe and layer heights.

The **Mix transition length** setting controls the transition distance: 100% keeps the standard length, 50% halves it, and 200% doubles it. The preview groups non-planar moves into the configured nominal layer height, such as 0.08 mm or 0.12 mm, instead of treating every small Z change as a new layer.

Curved and tapered models are checked locally over each short joining cycle. A shared island reserves the same footprint through its entire height, preventing another color from being placed inside the lower part of a thick bead. Gradient recipes are sampled within each cycle. Where a joined island meets an ordinary flat region, its interfaces return to their scheduled heights.

With full-domain subdivision and independent multicolor heights, unsupported curved regions are divided into short blocks. Stable recipe cores retain variable-height layers; moving edges use bounded flat sublayers within each nominal interval. Unused color-volume contributions carry into later blocks so small recipe components are retained over time. This fallback can leave local boundaries and adds slicing work; it does not connect every painted patch. It currently requires the nominal layer height to meet the minimum component height. The feature remains experimental, with physical print quality still to be validated.

The **Continuous mixed-color islands** setting is saved with the project. Infill modifiers retain their own density, pattern, speed and flow even when their perimeter is shared. Non-planar travel lifts above the printed surface before moving sideways; the wipe tower keeps its planned height. The engine still requires an experimental build or developer enable switch, whose effective value is recorded in exported G-code.
