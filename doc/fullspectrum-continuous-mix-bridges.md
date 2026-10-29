# Continuous mix bridges

Continuous mix bridges let neighboring Local-Z color zones share continuous layers of the same physical filament, even when those layers have different thicknesses or sit at different heights.

## Problem

Local-Z creates a mixed color by stacking thin sublayers of its component filaments inside one nominal layer. Take two neighboring painted regions that both mix cyan and magenta:

- the left region needs mostly cyan, so its cyan sublayer is thick and its magenta sublayer is thin;
- the right region needs mostly magenta, so the thicknesses are reversed.

With independent stacks, each region is its own print region with its own perimeters, and each region chooses its own stacking order. Every sublayer ends at the painted boundary. Those repeated endings can leave a visible seam. The same happens where a mix meets a single-filament area, such as the object's own color, or where a two-filament mix meets a three-filament mix.

## Behavior

With **Continuous mix bridges** enabled, the slicer groups neighboring zones that share at least one physical filament. A zone is a Local-Z mix, a painted single-filament area, or an island of the object's own filament. A group needs at least one mix.

- Every zone keeps the passes its own Local-Z cadence prints on each nominal layer, at the same heights as without bridges. A mix restacks that layer's passes in ascending filament order; a single-filament zone prints one pass over the whole layer.
- On each layer, the group's slots are its filaments in ascending order. Every zone's pass of a filament shares that filament's slot; a zone without the filament gives the slot zero height at that point of its stack. A filament gets as many slots as the most passes any zone prints with it on that layer.
- Every slot prints as its own island: the zones that use it plus the part of the band its layer reaches. Each island gets its own walls along its outline, at least two, and the region's infill inside them, so a layer ends in perimeters instead of clipped-off beads. The bridged zones also share one region, so the merged infill follows the group outline instead of every painted boundary.
- Away from boundaries, every zone looks exactly as it would unbridged. Within a band at a boundary between zones that share a filament, each slot interface blends between the two sides' absolute heights. A shared layer therefore moves and thins toward its neighbor's layer, while the other layers thicken or thin around it.
- Between two mixes the band is centered on the boundary. Next to a single-filament zone the whole band lies on the single-filament side, so a mix keeps its exact ratio up to its edge. A thin painted shell over the object's body therefore keeps its color at the surface, and the transition happens inside the body.
- A layer that only one side has tapers to nothing across the band, filling the space the other layers leave instead of ending abruptly at a thickness.
- Zones that share no filament keep a sharp boundary to each other, even inside one group.

Paths inside the band print non-planar: each short segment carries its own Z and bead height, and flow scales with the local height. Outside the band, paths stay flat and keep their original vertices.

Bridges never change which Local-Z planner a mix uses or the heights it prints.

### Per-layer Local-Z

With normal Local-Z, every mix stacks all its filaments inside each nominal layer. Every zone of a layer then spans the whole layer, shared filaments line up in every zone, and neighboring layers alternate filaments the way an unbridged mix does. This needs a layer thick enough for each mix's filaments at the minimum sublayer height; thinner layers print the mix unsplit, as in normal Local-Z.

### Independent layer heights: phase-locked windows

Rows planned with **Use independent Local-Z layer heights** run cycles longer than one nominal layer, each with its own length. Connecting only passes that happen to print on the same nominal layer leaves almost every layer tapering out from both sides, because neighboring cycles fall out of step. Bridges therefore phase-lock those rows:

- **Windows.** The object is divided into windows of whole nominal layers, counted from the first layer Local-Z subdivides. A window spans as few layers as hold the longest cycle used on the object, for example 5 layers of 0.08 mm for a 0.40 mm cycle. The last window also takes the layers left over at the top.
- **Cycles.** In each window every bridged mix repeats its own cycle as many whole times as fit and stretches it to fill the window, with its filaments in ascending order. A mix keeps its exact ratio; its sublayers are at most twice their independent heights, and a mix keeps the same heights everywhere on the object. A sublayer above the printer's maximum layer height splits into equal passes.
- **Matching.** Sublayers match across mixes by cycle and filament. A mix that repeats fewer times maps each cycle to the nearest cycle of the mix that repeats most; cycles left without a partner taper out. A single-filament zone keeps one pass per nominal layer and matches each layer to at most one sublayer of its filament, in order, choosing the matches that overlap the window's consensus stack most. Unmatched layers sit between the sublayers around their middle.
- **Printing.** A matched sublayer spans different nominal layers in different zones. Every nominal layer prints the parts of each slot whose top lies on that layer, cutting paths exactly where the top crosses a layer boundary, so the parts printed on neighboring layers meet. A zone therefore prints its own sublayers on the same layers whether or not it bridges, the nozzle never prints more than one layer below its neighbors, and zones can join or leave groups between layers.
- A row whose paint starts or ends inside a window prints only from the bottom of its first layer to the top of its last one.

Gradient rows and adaptive image-map rows keep their own cadence and do not bridge. With normal Local-Z, bridges stack once per nominal layer as described above.

## Settings

| Key | Label | Default | Meaning |
| --- | --- | --- | --- |
| `dithering_local_z_continuous_bridges` | Continuous mix bridges | off | Enables bridge planning for Local-Z zones. |
| `dithering_local_z_bridge_transition_width` | Bridge transition width | 2.0 mm | Width of the blend band at each bridged boundary: centered between two mixes, entirely on the single-filament side next to a single filament. `0` switches at the boundary but keeps each shared layer connected. |

Both are project-level Color Mixing settings, like the other `dithering_local_z_*` options. Changing either invalidates slicing.

## Implementation

| Stage | Location | Responsibility |
| --- | --- | --- |
| Field | `src/libslic3r/LocalZBridge.*` | `LocalZBridgeField` holds member masks, each member's absolute slot interfaces, and whether it absorbs the band. It blends interfaces at a point, returns each slot's island outline, and turns an extrusion path into `ExtrusionPathSloped` segments with absolute Z, dropping parts the slot does not print. |
| Zones and groups | `PrintObjectSlice.cpp` | `local_z_bridge_row_passes()`, `collect_local_z_bridge_single_filament_zones()` and `group_local_z_bridge_zones()` build zones and group touching zones that share a filament. Single-filament zones come from the full paint segmentation, because the planner's own segmentation omits single-filament channels. |
| Alignment | `align_local_z_bridge_zones()`, `align_local_z_bridge_window_zones()` | Builds the group's slots, in ascending filament order per layer or in window key order, and each member's absolute slot interfaces from its base height and passes. |
| Windows | `build_local_z_bridge_window()` | Builds a window's phase-locked row cycles, the template keys and consensus stack, and the single-filament layer matching. |
| Planning | `build_local_z_plan()` | Rows first plan their own passes, so cadence state advances as without bridges. With per-layer Local-Z, a lone mix also plans in isolated multi-row mode so it can bridge. Independent intervals print phase-locked rows from their windows. `build_local_z_bridge_layer()` then replaces grouped rows' passes with one pass per slot; in a window it keeps the slots that can have a top on the layer and gives each pass that layer's top range, and its island only the zones whose own top lies there plus the transition bands. |
| Regions | `merge_local_z_bridge_regions()` | Before `apply_mm_segmentation()`, moves bridged mixes and single-filament islands into the largest zone's channel, so perimeters and infill follow the merged island. |
| Export | `GCode::process_layer()` and `_extrude()` | `make_local_z_bridge_island()` builds wall loops from each bridge pass's island outline, with the flows and wall count of the region covering most of it, at least two walls. The region's infill is clipped to the area inside those walls; its own walls never print in bridge passes. Infill inside `LocalZInterval::bridge_masks` always prints in bridge passes, even with **Apply subdivision to infill** off, so merged single-filament zones keep their own filament. While a bridge pass is emitted, every path that is not already sloped goes through the field. Seam scarfs, wipes and the inward move after loops are disabled for bridge passes, and the pass skips its planar Z move. |
| Tool ordering | `ToolOrdering` | Registers every filament a Local-Z pass prints, since a bridged filament can print only inside passes once its region is merged away. |

Blending uses only distances to member regions, never to the object's outer contour, so the band has the same profile at the outer wall as in the interior. At a point inside member `r`, its signed depth is minus the distance to the closest other participating member; outside, it is the distance to `r`. A smoothstep of the depth over half the band width gives each member's weight, and the weights are normalized. Two members of the same kind therefore weigh 1/2 each on their boundary and reach 1/0 half a band width away. Between a mix and a single-filament zone both depths shift by half a band, which moves the whole band onto the single-filament side. Blended interfaces are convex combinations of the members' interfaces, so they stay ordered and every layer stays between its two sides.

A bridge pass sets `print_z` to the highest top of its slot over all members. Pass ordering, wipe-tower tool-change planning and travel use that plane, so the nozzle stays above the material it meets.

## Scope and limits

- Adaptive image-map Local-Z rows never bridge.
- With independent heights, phase-locking stretches each mix's cycle by up to twice its independent height. Mixes that repeat a different number of times per window connect their nearest cycles and taper the rest. Inside the band, a connected layer slopes between its two sides' heights within the window.
- A connected layer printed across several nominal layers is cut where its top crosses a layer boundary, so its wall loops print in pieces with a start and stop at each cut.
- With per-layer Local-Z, a layer too thin for a mix's filaments at the minimum sublayer height prints that mix unsplit and without bridges, as in normal Local-Z. Use independent heights for thinner layers.
- Painted boundaries that move between layers, and groups that form or end, can leave small overlaps or gaps inside the band where neighboring cadences differ, like independent Local-Z at any moving boundary.
- Outside Local-Z passes, the merged region uses the configuration of the largest zone, for example its wall count.
- A large single-filament zone, such as the object body, prints entirely in bridge passes on layers where it bridges. Away from the band its beads are flat and identical to normal printing.
- A layer that tapers to nothing prints its last beads far thinner than the Local-Z minimum sublayer height; their flow scales down with them.
- Island walls are evenly spaced offset loops, like the classic wall generator, not Arachne's variable-width walls.
- Z slopes inside the band equal the interface change divided by the band width, reaching up to 1.5 times the mean slope at the boundary. Keep the band wide enough for the printer's nozzle geometry.

## Tests

- `tests/libslic3r/test_local_z_bridge.cpp` covers the field: member heights, boundary blending, independence from the outer contour, monotonic interfaces, connected segments, flow scaling, volume preservation, the zero-width switch, a mix next to a single-filament zone, a thin painted shell keeping its ratio, island outlines, tapering instead of pockets, mixes with different filament sets, stacks that start and end at different heights, tapering onto a mix that prints nothing on a layer, printing each part of a slot on the layer holding its top, and sharp boundaries between zones that share nothing.
- `tests/fff_print/test_print.cpp` slices painted cubes with bridges for two mixes of the same filaments, a mix next to the object's own filament, a two-filament mix next to a three-filament mix, phase-locked independent-height mixes with different filament sets on layers thinner than their cycles, and a phase-locked mix next to the object's own filament. The tests check bridge passes, stack order, per-layer stacking, per-layer top ranges, connections between phase-locked mixes, kept ratios, whole body layers, unchanged mix ratios next to other zones, merged regions, outer and inner island walls in non-bridged filaments, non-planar G-code and blended bead heights.
