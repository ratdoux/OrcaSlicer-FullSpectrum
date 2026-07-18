#include "PerimeterEnvelopeRenderer.hpp"

#include "BoundaryModulation.hpp"
#include "ContinuousColorSolver.hpp"
#include "Sampling.hpp"
#include "../ClipperUtils.hpp"
#include "../Layer.hpp"
#include "../MixedFilament.hpp"
#include "../Model.hpp"
#include "../Print.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r::ImageMap {
namespace {

bool data_has_perimeter_modulation_v2(const VolumeData& data)
{
    return std::any_of(data.triangle_bindings.begin(), data.triangle_bindings.end(), [&data](const TriangleBinding& binding) {
        if (binding.zone_index >= data.zones.size())
            return false;
        const Zone& zone = data.zones[binding.zone_index];
        return zone.enabled && zone.render_mode == RenderMode::PerimeterModulationV2 && !zone.palette.empty();
    });
}

unsigned int resolve_palette_filament(const PaletteEntry& entry, const MixedFilamentManager& manager, size_t num_physical, size_t num_total)
{
    if (entry.mixed_filament_stable_id != 0) {
        const std::optional<unsigned int> stable = manager.filament_id_from_stable_id(entry.mixed_filament_stable_id, num_physical);
        if (stable && *stable <= num_total)
            return *stable;
    }
    return entry.fallback_filament_id >= 1 && entry.fallback_filament_id <= num_total ? entry.fallback_filament_id : 0u;
}

struct VolumeSampler
{
    SurfaceSampler sampler;
    Transform3d    local_to_print{Transform3d::Identity()};
    Transform3d    print_to_local{Transform3d::Identity()};
    double         minimum_scale{1.0};

    VolumeSampler(std::shared_ptr<const TriangleMesh> mesh, std::shared_ptr<const VolumeData> data, const Transform3d& transform)
        : sampler(std::move(mesh), std::move(data)), local_to_print(transform), print_to_local(transform.inverse())
    {
        minimum_scale = std::numeric_limits<double>::infinity();
        for (Eigen::Index column = 0; column < 3; ++column)
            minimum_scale = std::min(minimum_scale, local_to_print.linear().col(column).norm());
        if (!std::isfinite(minimum_scale) || minimum_scale <= EPSILON)
            minimum_scale = 1.0;
    }
};

struct SelectedSample
{
    SurfaceSample sample;
    double        squared_world_distance{std::numeric_limits<double>::infinity()};
};

} // namespace

struct PerimeterEnvelopeRenderer::Impl
{
    struct LayerCadence
    {
        unsigned int     filament_id{0};
        std::vector<int> component_percents;
    };

    const MixedFilamentManager*                   manager{nullptr};
    size_t                                        num_physical{0};
    size_t                                        num_total{0};
    float                                         reference_width_mm{0.4f};
    float                                         max_displacement_mm{0.35f};
    float                                         sample_spacing_mm{0.25f};
    std::unique_ptr<ContinuousColorSolver>        solver;
    std::vector<VolumeSampler>                    volumes;
    std::unordered_map<const Zone*, LayerCadence> cadences;

    std::optional<SelectedSample> sample(const Vec3d& print_point, double max_world_distance) const
    {
        std::optional<SelectedSample> best;
        for (const VolumeSampler& volume : volumes) {
            const Vec3d                        local_point = volume.print_to_local * print_point;
            const std::optional<SurfaceSample> candidate   = volume.sampler.sample(local_point, max_world_distance / volume.minimum_scale,
                                                                                   RenderMode::PerimeterModulationV2);
            if (!candidate)
                continue;
            const Vec3d  closest_print    = volume.local_to_print * candidate->closest_local_point;
            const double squared_distance = (closest_print - print_point).squaredNorm();
            if (!std::isfinite(squared_distance) || squared_distance > max_world_distance * max_world_distance)
                continue;
            if (!best || squared_distance < best->squared_world_distance - 1e-9 ||
                (std::abs(squared_distance - best->squared_world_distance) <= 1e-9 &&
                 candidate->zone->priority > best->sample.zone->priority))
                best = SelectedSample{*candidate, squared_distance};
        }
        return best;
    }

    float filament_offset(unsigned int filament_id, const Layer& layer) const
    {
        if (manager == nullptr || filament_id == 0)
            return 0.f;
        return std::clamp(manager->component_surface_offset(filament_id, num_physical, int(layer.id()), float(layer.print_z),
                                                            float(layer.height)),
                          -max_displacement_mm, max_displacement_mm);
    }

    std::optional<float> continuous_offset(const SurfaceSample& sample, const Layer& layer) const
    {
        if (!solver || !solver->valid() || sample.zone == nullptr)
            return std::nullopt;
        const auto cadence_it = cadences.find(sample.zone);
        if (cadence_it == cadences.end())
            return std::nullopt;
        const LayerCadence& cadence          = cadence_it->second;
        const unsigned int  active_component = manager->resolve(cadence.filament_id, num_physical, int(layer.id()), float(layer.print_z),
                                                                float(layer.height));
        if (active_component < 1 || active_component > num_physical)
            return std::nullopt;

        const std::vector<double> target_weights = solver->solve(sample.color);
        const std::vector<float>  offsets = mixed_filament_surface_offsets_for_apparent_weights(cadence.component_percents, target_weights,
                                                                                                reference_width_mm);
        if (offsets.size() != num_physical)
            return std::nullopt;
        return std::clamp(offsets[active_component - 1], -max_displacement_mm, max_displacement_mm);
    }

    BoundaryModulationResult modulate(const ExPolygons& source_envelope, const Layer& layer) const
    {
        BoundaryModulationOptions options;
        options.sample_spacing_mm           = sample_spacing_mm;
        options.max_abs_displacement_mm     = max_displacement_mm;
        const double max_sample_distance_mm = double(max_displacement_mm) + std::max(0.5, double(layer.height) * 2.0);
        return modulate_boundary(source_envelope, options,
                                 [this, &layer, max_sample_distance_mm](const Vec2d& point_mm,
                                                                        const Vec2d&) -> std::optional<BoundaryDisplacement> {
                                     const std::optional<SelectedSample> selected = sample(Vec3d(point_mm.x(), point_mm.y(),
                                                                                                 double(layer.slice_z)),
                                                                                           max_sample_distance_mm);
                                     if (!selected || selected->sample.zone == nullptr)
                                         return std::nullopt;
                                     if (const std::optional<float> offset = continuous_offset(selected->sample, layer))
                                         return BoundaryDisplacement{*offset, selected->sample.zone->corner_smoothing_radius_mm};

                                     // Preserve older V2 projects whose sequence definition is
                                     // missing or no longer resolvable.
                                     if (selected->sample.palette_entry == nullptr)
                                         return std::nullopt;
                                     const unsigned int filament_id = resolve_palette_filament(*selected->sample.palette_entry, *manager,
                                                                                               num_physical, num_total);
                                     return BoundaryDisplacement{filament_offset(filament_id, layer),
                                                                 selected->sample.zone->corner_smoothing_radius_mm};
                                 });
    }
};

bool model_has_perimeter_modulation_v2(const ModelObject& model_object)
{
    for (const ModelVolume* volume : model_object.volumes) {
        if (volume == nullptr || !volume->is_model_part())
            continue;
        const std::shared_ptr<const VolumeData> data = volume->image_map_data();
        if (data && data_has_perimeter_modulation_v2(*data))
            return true;
    }
    return false;
}

bool model_uses_perimeter_modulation_v2_filament(const ModelObject& model_object,
                                                 uint64_t           mixed_filament_stable_id,
                                                 unsigned int       fallback_filament_id)
{
    if (mixed_filament_stable_id == 0 && fallback_filament_id == 0)
        return false;
    for (const ModelVolume* volume : model_object.volumes) {
        if (volume == nullptr || !volume->is_model_part())
            continue;
        const std::shared_ptr<const VolumeData> data = volume->image_map_data();
        if (!data)
            continue;
        for (const Zone& zone : data->zones) {
            if (!zone.enabled || zone.render_mode != RenderMode::PerimeterModulationV2)
                continue;
            if (std::any_of(zone.palette.begin(), zone.palette.end(), [&](const PaletteEntry& entry) {
                    return (mixed_filament_stable_id != 0 && entry.mixed_filament_stable_id == mixed_filament_stable_id) ||
                           (entry.mixed_filament_stable_id == 0 && entry.fallback_filament_id == fallback_filament_id);
                }))
                return true;
        }
    }
    return false;
}

std::unique_ptr<PerimeterEnvelopeRenderer> PerimeterEnvelopeRenderer::create(const PrintObject& print_object)
{
    const Print*       print        = print_object.print();
    const ModelObject* model_object = print_object.model_object();
    if (print == nullptr || model_object == nullptr || !print->config().mixed_filament_component_bias_enabled.value ||
        !model_has_perimeter_modulation_v2(*model_object))
        return nullptr;

    auto impl                 = std::make_unique<Impl>();
    impl->manager             = &print->mixed_filament_manager();
    impl->num_physical        = print->config().filament_colour.size();
    impl->num_total           = impl->manager->total_filaments(impl->num_physical);
    float reference_nozzle_mm = 0.4f;
    if (!print->config().nozzle_diameter.values.empty()) {
        const size_t count = std::min(impl->num_physical, print->config().nozzle_diameter.values.size());
        if (count != 0)
            reference_nozzle_mm = float(std::accumulate(print->config().nozzle_diameter.values.begin(),
                                                        print->config().nozzle_diameter.values.begin() + count, 0.0) /
                                        double(count));
    }
    impl->reference_width_mm  = reference_nozzle_mm;
    impl->max_displacement_mm = MixedFilamentManager::max_component_surface_offset_mm(reference_nozzle_mm);

    std::vector<ContinuousColorComponent> solver_components;
    solver_components.reserve(impl->num_physical);
    for (size_t component_index = 0; component_index < impl->num_physical; ++component_index) {
        ContinuousColorComponent component;
        component.color_hex = print->config().filament_colour.values[component_index];
        if (component_index < print->config().filament_transmission_distance.values.size() &&
            print->config().filament_transmission_distance.values[component_index] > EPSILON)
            component.transmission_distance_mm = print->config().filament_transmission_distance.values[component_index];
        if (component_index < print->config().filament_full_spectrum_material_id.values.size() &&
            !print->config().filament_full_spectrum_material_id.values[component_index].empty())
            component.material_id = print->config().filament_full_spectrum_material_id.values[component_index];
        solver_components.emplace_back(std::move(component));
    }
    impl->solver = std::make_unique<ContinuousColorSolver>(std::move(solver_components));

    const Transform3d object_to_print = print_object.trafo_centered();
    for (const ModelVolume* volume : model_object->volumes) {
        if (volume == nullptr || !volume->is_model_part())
            continue;
        const std::shared_ptr<const VolumeData> data = volume->image_map_data();
        if (!data || !data_has_perimeter_modulation_v2(*data) || !data->validate(volume->mesh()).valid)
            continue;
        for (const Zone& zone : data->zones) {
            if (zone.enabled && zone.render_mode == RenderMode::PerimeterModulationV2) {
                impl->sample_spacing_mm  = std::min(impl->sample_spacing_mm, std::clamp(zone.modulation_sample_spacing_mm, 0.03f, 2.f));
                unsigned int filament_id = 0;
                for (const PaletteEntry& entry : zone.palette) {
                    filament_id = resolve_palette_filament(entry, *impl->manager, impl->num_physical, impl->num_total);
                    if (filament_id != 0)
                        break;
                }
                const std::optional<MixedFilamentDefinition> definition =
                    impl->manager->mixed_filament_definition_from_id(filament_id, impl->num_physical);
                if (definition && definition->behavior.surface_bias.perimeter_modulation) {
                    std::vector<int> component_percents(impl->num_physical, 0);
                    for (const MixedFilamentWeightedComponent& component : definition->recipe.blend.components) {
                        if (component.filament.id >= 1 && component.filament.id <= impl->num_physical)
                            component_percents[component.filament.id - 1] += std::max(0, component.percent);
                    }
                    if (std::all_of(component_percents.begin(), component_percents.end(), [](int percent) { return percent > 0; }))
                        impl->cadences.emplace(&zone, Impl::LayerCadence{filament_id, std::move(component_percents)});
                }
            }
        }
        const Transform3d local_to_print = object_to_print * volume->get_matrix();
        if (std::abs(local_to_print.linear().determinant()) <= EPSILON)
            continue;
        impl->volumes.emplace_back(volume->mesh_ptr(), data, local_to_print);
    }
    if (impl->volumes.empty())
        return nullptr;
    return std::unique_ptr<PerimeterEnvelopeRenderer>(new PerimeterEnvelopeRenderer(std::move(impl)));
}

PerimeterEnvelopeRenderer::PerimeterEnvelopeRenderer(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
PerimeterEnvelopeRenderer::~PerimeterEnvelopeRenderer()                                               = default;
PerimeterEnvelopeRenderer::PerimeterEnvelopeRenderer(PerimeterEnvelopeRenderer&&) noexcept            = default;
PerimeterEnvelopeRenderer& PerimeterEnvelopeRenderer::operator=(PerimeterEnvelopeRenderer&&) noexcept = default;

bool PerimeterEnvelopeRenderer::apply(Layer& layer) const
{
    if (!m_impl || layer.regions().empty())
        return false;

    struct RegionGeometry
    {
        LayerRegion* region{nullptr};
        ExPolygons   original;
        ExPolygons   adjusted;
        unsigned int filament_id{0};
        float        outward_growth_mm{0.f};
    };
    std::vector<RegionGeometry> regions;
    ExPolygons                  source_envelope;
    for (LayerRegion* region : layer.regions()) {
        RegionGeometry item;
        item.region            = region;
        item.original          = to_expolygons(region->slices.surfaces);
        item.filament_id       = unsigned(std::max(0, region->region().config().wall_filament.value));
        item.outward_growth_mm = std::max(0.f, -m_impl->filament_offset(item.filament_id, layer));
        append(source_envelope, item.original);
        regions.emplace_back(std::move(item));
    }
    if (source_envelope.empty())
        return false;
    source_envelope = union_ex(std::move(source_envelope));

    const BoundaryModulationResult modulated = m_impl->modulate(source_envelope, layer);
    if (!modulated.changed)
        return false;

    for (RegionGeometry& region : regions)
        region.adjusted = intersection_ex(region.original, modulated.geometry, ApplySafetyOffset::Yes);

    ExPolygons expansion = diff_ex(modulated.geometry, source_envelope, ApplySafetyOffset::Yes);
    if (!expansion.empty()) {
        std::vector<size_t> ownership_order(regions.size());
        std::iota(ownership_order.begin(), ownership_order.end(), size_t(0));
        std::stable_sort(ownership_order.begin(), ownership_order.end(),
                         [&regions](size_t lhs, size_t rhs) { return regions[lhs].outward_growth_mm > regions[rhs].outward_growth_mm; });
        for (size_t index : ownership_order) {
            RegionGeometry& region = regions[index];
            if (region.original.empty() || region.outward_growth_mm <= EPSILON || expansion.empty())
                continue;
            const ExPolygons grown   = offset_ex(region.original, float(scale_(region.outward_growth_mm)));
            ExPolygons       claimed = intersection_ex(grown, expansion, ApplySafetyOffset::Yes);
            if (claimed.empty())
                continue;
            append(region.adjusted, claimed);
            expansion = diff_ex(expansion, claimed, ApplySafetyOffset::Yes);
        }

        // Sampling is more precise than the transient facet segmentation, so
        // a very small ownership remainder is possible at palette boundaries.
        // Claim it by nearest existing region while preserving the envelope.
        for (RegionGeometry& region : regions) {
            if (region.original.empty() || expansion.empty())
                continue;
            const ExPolygons neighborhood = offset_ex(region.original, float(scale_(m_impl->max_displacement_mm)));
            ExPolygons       claimed      = intersection_ex(neighborhood, expansion, ApplySafetyOffset::Yes);
            if (claimed.empty())
                continue;
            append(region.adjusted, claimed);
            expansion = diff_ex(expansion, claimed, ApplySafetyOffset::Yes);
        }
        if (!expansion.empty()) {
            const auto owner = std::find_if(regions.begin(), regions.end(),
                                            [](const RegionGeometry& region) { return !region.original.empty(); });
            if (owner != regions.end())
                append(owner->adjusted, std::move(expansion));
        }
    }

    for (RegionGeometry& region : regions) {
        if (region.adjusted.size() > 1)
            region.adjusted = union_ex(std::move(region.adjusted));
        region.region->slices.set(std::move(region.adjusted), stInternal);
    }
    return true;
}

bool PerimeterEnvelopeRenderer::apply_to_envelope(ExPolygons& envelope, const Layer& layer) const
{
    if (!m_impl || envelope.empty())
        return false;
    BoundaryModulationResult modulated = m_impl->modulate(envelope, layer);
    if (!modulated.changed)
        return false;
    envelope = std::move(modulated.geometry);
    return true;
}

} // namespace Slic3r::ImageMap
