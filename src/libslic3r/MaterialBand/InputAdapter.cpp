#include "InputAdapter.hpp"

#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <map>
#include <limits>
#include <cmath>

namespace Slic3r::MaterialBand {
namespace {
// Visit leaf paint facets once, including splits. Per-state get_facets() would
// repeatedly scan the full selector as paint complexity grows.
class SnapshotSelector : public TriangleSelector
{
public:
    using TriangleSelector::TriangleSelector;
    bool visit(const std::function<bool(unsigned int, const std::array<Vec3f, 3>&)>& visitor) const
    {
        for (const auto& triangle : m_triangles)
            if (triangle.valid() && !triangle.is_split()) {
                const std::array<Vec3f, 3> vertices{
                    {m_vertices[triangle.verts_idxs[0]].v, m_vertices[triangle.verts_idxs[1]].v, m_vertices[triangle.verts_idxs[2]].v}};
                if (!visitor(static_cast<unsigned int>(triangle.get_state()), vertices))
                    return false;
            }
        return true;
    }
};
} // namespace

InputResult snapshot_input(const ModelObject&           object,
                           const DynamicPrintConfig&    config,
                           const InputContext&          context,
                           const std::function<bool()>& cancelled)
{
    const auto fail = [&](FailureCode code, const char* detail) -> InputResult {
        return Failure{context.revision, code, detail, {}, {}, {}};
    };
    if (cancelled && cancelled())
        return fail(FailureCode::Cancelled, "cancelled_before_snapshot");
    if (object.volumes.size() != 1 || !object.volumes.front()->is_model_part())
        return fail(FailureCode::UnsupportedTopology, "single_model_part_required");
    const auto& volume = *object.volumes.front();
    if (volume.has_image_map_data())
        return fail(FailureCode::UnsupportedProfile, "image_map_intent_requires_adapter");
    const auto* colors = config.option<ConfigOptionStrings>("filament_colour");
    if (!colors || colors->values.size() < 2 || colors->values.size() > MAX_MATERIALS)
        return fail(FailureCode::UnsupportedProfile, "two_to_four_physical_materials_required");
    const size_t material_count = colors->values.size();
    if (context.phase.order[0].value < 1 || context.phase.order[0].value > 2 || context.phase.order[1].value < 1 ||
        context.phase.order[1].value > 2 || context.phase.order[0].value == context.phase.order[1].value ||
        context.phase.cadence != std::array<uint32_t, 2>{{1, 1}} || context.phase.slot > 1)
        return fail(FailureCode::IncompatibleOrder, "unsupported_stack_phase");
    const auto& source   = volume.mesh().its;
    const auto& painting = volume.mmu_segmentation_facets.get_data();
    // Serialized color IDs use several bits per leaf, even without splitting.
    // Bound actual expansion before deserialization rather than treating bits
    // as triangles (which rejected ordinary densely painted meshes).
    if (context.max_triangles > std::numeric_limits<uint32_t>::max() / 3 || source.indices.size() > context.max_triangles ||
        source.vertices.size() > 3 * context.max_triangles)
        return fail(FailureCode::ResourceLimit, "input_geometry_limit");
    if (painting.triangles_to_split.size() > source.indices.size() || painting.bitstream.size() > 80 * context.max_triangles)
        return fail(FailureCode::ResourceLimit, "paint_stream_limit");
    size_t            leaves = source.indices.size(), nodes = 0;
    std::vector<bool> seen(source.indices.size(), false);
    for (const auto& mapping : painting.triangles_to_split) {
        if (cancelled && cancelled())
            return fail(FailureCode::Cancelled, "cancelled_during_paint_preflight");
        if (mapping.triangle_idx < 0 || size_t(mapping.triangle_idx) >= seen.size() || seen[mapping.triangle_idx] ||
            mapping.bitstream_start_idx < 0)
            return fail(FailureCode::InvalidInput, "invalid_paint_stream_mapping");
        seen[mapping.triangle_idx] = true;
        size_t     bit = size_t(mapping.bitstream_start_idx), pending = 1;
        const auto nibble = [&]() -> int {
            if (bit > painting.bitstream.size() || painting.bitstream.size() - bit < 4)
                return -1;
            int value = 0;
            for (int i = 0; i < 4; ++i)
                value |= int(painting.bitstream[bit++]) << i;
            return value;
        };
        while (pending) {
            if (++nodes > 2 * context.max_triangles)
                return fail(FailureCode::ResourceLimit, "paint_node_limit");
            const int code = nibble();
            if (code < 0)
                return fail(FailureCode::InvalidInput, "truncated_paint_stream");
            --pending;
            const int splits = code & 3;
            if (splits) {
                if ((code >> 2) > 2)
                    return fail(FailureCode::InvalidInput, "invalid_paint_split_side");
                pending += splits + 1;
                leaves += splits;
                if (leaves > context.max_triangles)
                    return fail(FailureCode::ResourceLimit, "paint_leaf_limit");
            } else if ((code >> 2) == 3) {
                int state = 3, next = 0;
                do {
                    next = nibble();
                    if (next < 0)
                        return fail(FailureCode::InvalidInput, "truncated_paint_state");
                    state += next;
                    if (state > int(EnforcerBlockerType::ExtruderMax))
                        return fail(FailureCode::InvalidInput, "invalid_paint_state");
                } while (next == 15);
            }
        }
    }
    const auto transform = volume.get_matrix();
    if (!transform.matrix().allFinite() || std::abs(transform.linear().determinant()) < 1e-12)
        return fail(FailureCode::InvalidInput, "invalid_volume_transform");
    const bool  mirrored = transform.linear().determinant() < 0.;
    KernelInput input;
    input.revision                = context.revision;
    input.source_revision         = context.source_revision;
    input.boundary                = context.boundary;
    input.process                 = context.process;
    input.tolerances              = context.tolerances;
    input.material_count          = material_count;
    const auto* gradient_error    = config.option<ConfigOptionPercent>("material_band_gradient_error");
    input.gradient_fraction_error = gradient_error ? gradient_error->value / 100. : 0.;
    input.solid.vertices.reserve(source.vertices.size());
    input.solid.triangles.reserve(source.indices.size());
    for (const auto& vertex : source.vertices) {
        const Vec3d point = transform * vertex.cast<double>();
        if (!point.allFinite())
            return fail(FailureCode::InvalidInput, "nonfinite_solid_vertex");
        input.solid.vertices.push_back(point);
    }
    for (const auto& face : source.indices) {
        for (int vertex : {face[0], face[1], face[2]})
            if (vertex < 0 || size_t(vertex) >= source.vertices.size())
                return fail(FailureCode::InvalidInput, "solid_vertex_index");
        const Vec3d edge_a = input.solid.vertices[face[1]] - input.solid.vertices[face[0]];
        const Vec3d edge_b = input.solid.vertices[face[2]] - input.solid.vertices[face[0]];
        if (!edge_a.cross(edge_b).allFinite() || !std::isfinite(edge_a.cross(edge_b).squaredNorm()))
            return fail(FailureCode::InvalidInput, "nonfinite_solid_triangle");
        // Exactly collinear facets have no surface area or signed volume.
        // Keep source facet indices intact for paint decoding, but omit these
        // facets from the owned geometric snapshot. Never discard a small
        // nonzero facet by an arbitrary mesh-resolution threshold.
        if (edge_a.cross(edge_b).squaredNorm() == 0.) {
            ++input.discarded_degenerate_faces;
            continue;
        }
        input.solid.triangles.push_back({uint32_t(face[0]), uint32_t(face[mirrored ? 2 : 1]), uint32_t(face[mirrored ? 1 : 2])});
    }
    if (input.solid.triangles.empty())
        return fail(FailureCode::InvalidInput, "empty_nondegenerate_solid");
    MixedFilamentManager manager;
    manager.auto_generate(colors->values);
    manager.load_custom_entries(config.opt_string("mixed_filament_definitions"), colors->values);
    const auto       definitions = manager.mixed_filament_definitions(material_count);
    SnapshotSelector selector(volume.mesh());
    selector.deserialize(painting, false);
    std::map<unsigned int, RegionIntent> regions;
    size_t                               triangles = 0;
    Failure                              failure{context.revision, FailureCode::InvalidInput, {}, {}, {}, {}};
    bool                                 initialize_base = true;
    const auto                           append_facet    = [&](unsigned int state, const std::array<Vec3f, 3>& vertices) {
        failure.region = state;
        if (!initialize_base)
            failure.witness_mm = transform * vertices.front().cast<double>();
        if (cancelled && cancelled()) {
            failure.code   = FailureCode::Cancelled;
            failure.detail = "cancelled_during_snapshot";
            return false;
        }
        if (!initialize_base && ++triangles > context.max_triangles) {
            failure.code   = FailureCode::ResourceLimit;
            failure.detail = "paint_leaf_limit";
            return false;
        }
        if (!initialize_base && (vertices[1].cast<double>() - vertices[0].cast<double>())
                                        .cross(vertices[2].cast<double>() - vertices[0].cast<double>())
                                        .squaredNorm() == 0.)
            return true;
        auto found = regions.find(state);
        if (found == regions.end()) {
            RegionIntent intent;
            // Region IDs are paint-channel provenance within a domain. Domain
            // and revision supply the volume/object namespace.
            intent.id     = state;
            intent.domain = volume.id().id;
            intent.scope  = state == 0 ? DepositionScope::WholeSolid : context.painted_scope;
            intent.phase  = context.phase;
            const unsigned int filament = state == 0 ? static_cast<unsigned int>(std::max(1, volume.extruder_id())) : state;
            if (filament <= material_count) {
                intent.fractions[filament - 1] = 1.;
                intent.fraction_a              = filament == context.phase.order[0].value ? 1. : 0.;
            } else {
                const int index = manager.mixed_index_from_filament_id(filament, material_count);
                if (index < 0 || size_t(index) >= definitions.size()) {
                    failure.detail = "unresolved_virtual_recipe";
                    return false;
                }
                const auto& recipe     = definitions[size_t(index)];
                const auto& components = recipe.recipe.blend.components;
                if (components.size() < 2 || components.size() > material_count || recipe.behavior.surface_bias.perimeter_modulation) {
                    failure.code   = FailureCode::UnsupportedProfile;
                    failure.detail = "unsupported_spatial_recipe";
                    return false;
                }
                const auto& bias = recipe.behavior.surface_bias;
                if (bias.component_a_offset_mm != 0. || bias.component_b_offset_mm != 0. ||
                    std::any_of(bias.component_offsets_mm.begin(), bias.component_offsets_mm.end(),
                                                             [](float offset) { return offset != 0.; })) {
                    failure.code   = FailureCode::UnsupportedProfile;
                    failure.detail = "surface_bias_requires_domain_adapter";
                    return false;
                }
                if (recipe.recipe.manual_pattern || recipe.behavior.layer_cadence.component_a_layers != 1 ||
                    recipe.behavior.layer_cadence.component_b_layers != 1 ||
                    (material_count == 2 && !recipe.behavior.gradient.enabled &&
                     (components[0].filament.id != context.phase.order[0].value ||
                      components[1].filament.id != context.phase.order[1].value))) {
                    failure.code   = FailureCode::IncompatibleOrder;
                    failure.detail = "recipe_stack_or_cadence";
                    return false;
                }
                int                             total = 0;
                std::array<bool, MAX_MATERIALS> seen{};
                for (const auto& component : components) {
                    if (component.filament.id < 1 || component.filament.id > material_count || component.percent < 0 ||
                        component.percent > 100 || seen[component.filament.id - 1]) {
                        failure.detail = "invalid_recipe_weights";
                        return false;
                    }
                    seen[component.filament.id - 1]             = true;
                    intent.fractions[component.filament.id - 1] = component.percent / 100.;
                    total += component.percent;
                }
                if (total != 100) {
                    failure.detail = "invalid_recipe_weights";
                    return false;
                }
                intent.recipe     = recipe.identity.stable_id;
                intent.fraction_a = components[0].percent / 100.;
                if (recipe.behavior.gradient.enabled) {
                    const auto ids   = mixed_gradient_components(recipe, material_count);
                    const auto stops = mixed_gradient_stops(recipe, material_count);
                    // Decode saved intent only. Legacy Local-Z process settings
                    // must not influence this kernel's curve or geometry.
                    const auto widths = mixed_gradient_solid_half_widths(recipe, material_count, .03);
                    const auto knot   = [&](double x, unsigned int a, unsigned int b, double ratio) {
                        GradientKnot k;
                        k.position = x;
                        k.fractions[a - 1] += 1. - ratio;
                        k.fractions[b - 1] += ratio;
                        intent.height_gradient.push_back(k);
                    };
                    for (size_t i = 0; i < ids.size(); ++i) {
                        knot(stops[2 * i] - widths[i], ids[i], ids[i], 0.);
                        if (widths[i] > 0.)
                            knot(stops[2 * i] + widths[i], ids[i], ids[i], 0.);
                        if (i + 1 < ids.size())
                            knot(stops[2 * i + 1], ids[i], ids[i + 1], .5);
                    }
                }
            }
            found = regions.emplace(state, std::move(intent)).first;
        }
        if (initialize_base)
            return true;
        auto&      mesh  = found->second.painted_surface;
        const auto index = uint32_t(mesh.vertices.size());
        for (const auto& vertex : vertices)
            mesh.vertices.emplace_back(transform * vertex.cast<double>());
        mesh.triangles.push_back({index, index + (mirrored ? 2u : 1u), index + (mirrored ? 1u : 2u)});
        return true;
    };
    // Object/volume assignment still governs the interior when every visible
    // facet is painted. Preserve it even if there are no unpainted leaf facets.
    if (!append_facet(0, {}))
        return failure;
    initialize_base = false;
    if (!selector.visit(append_facet))
        return failure;
    for (auto& region : regions)
        input.regions.push_back(std::move(region.second));
    if (cancelled && cancelled())
        return fail(FailureCode::Cancelled, "cancelled_before_publish");
    return std::make_shared<const KernelInput>(std::move(input));
}

} // namespace Slic3r::MaterialBand
