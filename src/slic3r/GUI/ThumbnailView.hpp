#pragma once

// Lightweight header declaring the named-thumbnail-viewpoint enum.
// Kept separate from GLCanvas3D.hpp so lightweight consumers (e.g.
// MixedFilamentBatchDialog.hpp) don't have to pull in the entire GLCanvas3D
// include chain just to name a viewpoint.

namespace Slic3r { namespace GUI {

// Named thumbnail viewpoints. Consumed by GLCanvas3D::render_thumbnail
// (the ThumbnailView overload) and GLCanvas3D::render_thumbnail_internal.
//
// Relationship to the legacy bool use_top_view parameter:
//   - Iso reproduces the old use_top_view=false behavior exactly.
//   - Top REPLACES the old use_top_view=true path semantically, but routes through
//     the unified named-view algorithm (Camera::select_view("top") + zoom_to_box on
//     the volumes bounding box), NOT the legacy plate-centered look_at. The framing
//     (center point + zoom factor) therefore differs slightly from the old top view.
//     The legacy use_top_view=true branch in render_thumbnail_internal is preserved
//     unchanged for existing callers (notably Plater's top_thumbnail_data for 3MF
//     export); ThumbnailView::Top is only reached via the new render_thumbnail
//     overload, which forces use_top_view=false.
enum class ThumbnailView : unsigned char {
    Iso = 0,        // Isometric (default — identical to old use_top_view=false)
    TopFront,       // Top-front
    Left,           // Left
    Right,          // Right
    Top,            // Top (replaces old use_top_view=true; uses unified named-view framing, not pixel-identical to the legacy plate-centered top view)
    Bottom,         // Bottom
    Front,          // Front
    Rear,           // Rear
};

}} // namespace Slic3r::GUI
