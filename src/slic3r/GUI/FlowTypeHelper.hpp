#ifndef slic3r_GUI_FlowTypeHelper_hpp_
#define slic3r_GUI_FlowTypeHelper_hpp_

#include "libslic3r/PrintConfig.hpp"

#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace FlowType {

// Snapmaker requirement 7.1: all functions read/write wxGetApp().preset_bundle state.
//
// The per-nozzle flow combo (nozzle_volume_type), the grouping mode chosen in the
// slice-button hover popup (filament_grouping_mode) and the per-filament mapping
// edited in the grouping dialog (filament_volume_type) are three independent
// pieces of state: changing one never rewrites another. The only shared gate is
// printer_supports_high_flow() -- whether the edited printer preset declares
// "high_flow" in printer_flow_support at all.

// True when the edited printer preset declares "high_flow" in printer_flow_support.
bool printer_supports_high_flow();

// True when at least one selected filament preset declares "high_flow" in its
// filament_flow_support. The custom grouping popup/dialog only makes sense when some
// filament can actually use the high-flow variant.
bool any_filament_supports_high_flow();

// Number of DISTINCT flow variants currently selected across the nozzles (from the
// per-nozzle flow combos, via nozzle_volume_types()). 1 when every nozzle uses the
// same type. The standard / custom slice-mode popup is only shown when this is >= 2
// -- i.e. the nozzles actually mix flow types; if they are all the same there is
// nothing to group and slicing routes every filament to that one type.
size_t distinct_nozzle_flow_type_count();

// Per-nozzle flow types from project config, resized to the nozzle count and
// normalized (unknown entries -> standard; everything standard when the printer
// preset does not support high flow).
std::vector<std::string> nozzle_volume_types();

// Writes one nozzle's flow type and marks the project dirty.
void set_nozzle_volume_type(size_t nozzle_idx, const std::string &volume_type);

// Resets every nozzle's flow type to standard (one entry per nozzle), sized to the
// current nozzle count. Used on New Project: the printer preset carries no nozzle
// flow-type field, so the per-nozzle flow types must fall back to standard instead
// of inheriting whatever the previous project left in the shared project config.
void reset_nozzle_volume_types_to_standard();

// Batch-writes all nozzle flow types from connected machine data.
// Silently ignores mismatches between the vector size and the nozzle count.
void set_nozzle_volume_types(const std::vector<std::string> &volume_types);

// Restores the flow types saved by the setters above ("nozzle_volume_types"
// section, keyed by printer preset). No-op without memory; callers rebuild the UI.
void restore_nozzle_volume_types_from_app_config();

// Grouping mode (FILAMENT_GROUPING_STANDARD / FILAMENT_GROUPING_CUSTOM).
std::string grouping_mode();

// Writes the grouping mode and marks the project dirty.
void set_grouping_mode(const std::string &mode);

// Writes the dialog's per-filament mapping and invalidates the slice result.
void apply_custom_mapping(const std::vector<FilamentVolumeType> &mapping);

// Normalizes the per-filament flow mapping for slicing when the custom grouping
// does not apply: when the nozzles are not mixing flow types every filament follows
// that single nozzle type (all standard -> standard, all high flow -> high flow); in
// standard mode with mixed nozzles everything falls back to standard. No-op when the
// mapping already matches. Only meaningful outside the custom+mixed case.
void sync_filament_volume_types_for_slice();

}}} // namespace Slic3r::GUI::FlowType

#endif // slic3r_GUI_FlowTypeHelper_hpp_
