#ifndef slic3r_LocalZBridgeProjector_hpp_
#define slic3r_LocalZBridgeProjector_hpp_
#include "LocalZBridge.hpp"
namespace Slic3r {
// Project a seam-selected path onto a slot and partition its volume by layer.
// The field remains immutable; this operation owns sampling, cuts and flow.
std::vector<ExtrusionPathSloped> project_local_z_bridge_path(
    const LocalZBridgeField& field, const ExtrusionPath& path, size_t slot_index, double z_offset, const LocalZBridgeField::Span& tops);
} // namespace Slic3r
#endif
