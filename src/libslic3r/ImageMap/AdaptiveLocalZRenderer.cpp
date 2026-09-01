#include "AdaptiveLocalZRenderer.hpp"

#include "ContinuousColorSolver.hpp"
#include "Sampling.hpp"
#include "../Layer.hpp"
#include "../Model.hpp"
#include "../Print.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace Slic3r::ImageMap {

std::vector<double> allocate_adaptive_local_z_heights(const std::vector<double>& weights,
                                                      double                     total_height,
                                                      double                     configured_minimum)
{
    std::vector<double> heights(weights.size(), 0.0);
    if (weights.empty() || total_height <= EPSILON)
        return heights;

    // A valid print profile keeps the nominal layer height below this ceiling,
    // so an N-pass cadence always has enough total capacity. Preserve the
    // model envelope for a malformed profile instead of silently losing Z.
    const double maximum = std::max(ADAPTIVE_LOCAL_Z_MAX_HEIGHT_MM, total_height / double(weights.size()));
    const double minimum = std::min(maximum, std::max(0.01, configured_minimum));
    const double effective_minimum = minimum * double(weights.size()) <= total_height + EPSILON
        ? minimum
        : total_height / double(weights.size());

    const double total_weight = std::accumulate(weights.begin(), weights.end(), 0.0, [](double sum, double weight) {
        return sum + std::max(0.0, weight);
    });
    for (size_t component_idx = 0; component_idx < weights.size(); ++component_idx) {
        const double desired = total_weight > EPSILON
            ? total_height * std::max(0.0, weights[component_idx]) / total_weight
            : total_height / double(weights.size());
        heights[component_idx] = std::clamp(desired, effective_minimum, maximum);
    }

    // Clamping one component changes the height still available to its
    // siblings. Redistribute that difference without changing the fixed cycle
    // top; zero-weight siblings share unavoidable excess equally.
    for (int pass = 0; pass < 2 * int(weights.size()) + 2; ++pass) {
        const double assigned = std::accumulate(heights.begin(), heights.end(), 0.0);
        const double delta    = total_height - assigned;
        if (std::abs(delta) <= 1e-10)
            break;

        const bool increase = delta > 0.0;
        std::vector<size_t> candidates;
        double              candidate_weight = 0.0;
        for (size_t component_idx = 0; component_idx < heights.size(); ++component_idx) {
            const double capacity = increase ? maximum - heights[component_idx]
                                             : heights[component_idx] - effective_minimum;
            if (capacity <= EPSILON)
                continue;
            candidates.emplace_back(component_idx);
            candidate_weight += std::max(0.0, weights[component_idx]);
        }
        if (candidates.empty())
            break;

        double applied = 0.0;
        for (size_t component_idx : candidates) {
            const double share = candidate_weight > EPSILON
                ? std::max(0.0, weights[component_idx]) / candidate_weight
                : 1.0 / double(candidates.size());
            const double capacity = increase ? maximum - heights[component_idx]
                                             : heights[component_idx] - effective_minimum;
            const double adjustment = std::min(std::abs(delta) * share, capacity);
            heights[component_idx] += increase ? adjustment : -adjustment;
            applied += adjustment;
        }
        if (applied <= 1e-10)
            break;
    }
    return heights;
}

namespace {

bool zone_uses_adaptive_local_z(const Zone& zone)
{
    return zone.enabled && zone.render_mode == RenderMode::AdaptiveLocalizedCycles &&
           zone.adaptive_modulation_mode == AdaptiveModulationMode::LocalZHeight && !zone.palette.empty();
}

bool data_has_adaptive_local_z(const VolumeData& data)
{
    return std::any_of(data.triangle_bindings.begin(), data.triangle_bindings.end(), [&data](const TriangleBinding& binding) {
        return binding.zone_index < data.zones.size() && zone_uses_adaptive_local_z(data.zones[binding.zone_index]);
    });
}

struct VolumeSampler
{
    std::shared_ptr<const TriangleMesh> mesh;
    std::shared_ptr<const VolumeData>   data;
    Transform3d                         local_to_print{Transform3d::Identity()};
};

bool layer_slices_contain(const ExPolygons& layer_slices, const Point& point)
{
    return std::any_of(layer_slices.begin(), layer_slices.end(), [&point](const ExPolygon& slice) { return slice.contains(point, false); });
}

Vec2d outward_direction(const Points& points, size_t point_index, const ExPolygons& layer_slices)
{
    if (points.size() < 2)
        return Vec2d::Zero();
    const bool   closed = points.size() > 2 && points.front() == points.back();
    const size_t count  = closed ? points.size() - 1 : points.size();
    if (count < 2)
        return Vec2d::Zero();
    const size_t index = closed && point_index == count ? 0 : std::min(point_index, count - 1);
    const Point& before = index > 0 ? points[index - 1] : (closed ? points[count - 1] : points[index]);
    const Point& after  = index + 1 < count ? points[index + 1] : (closed ? points[0] : points[index]);
    Vec2d        tangent = (after - before).cast<double>();
    const double length  = tangent.norm();
    if (!std::isfinite(length) || length <= EPSILON)
        return Vec2d::Zero();
    tangent /= length;

    const Vec2d normal(-tangent.y(), tangent.x());
    const double probe_scaled = std::max(1.0, scale_(0.08));
    auto probe = [&points, index, probe_scaled](const Vec2d& direction) {
        const Vec2d shifted = points[index].cast<double>() + direction * probe_scaled;
        return Point(coord_t(std::llround(shifted.x())), coord_t(std::llround(shifted.y())));
    };
    const bool positive_inside = layer_slices_contain(layer_slices, probe(normal));
    const bool negative_inside = layer_slices_contain(layer_slices, probe(-normal));
    if (positive_inside != negative_inside)
        return positive_inside ? -normal : normal;
    return normal;
}

Points resample_polyline(const Points& source, double spacing_mm)
{
    if (source.size() < 2)
        return source;
    const double spacing_scaled = std::max(1.0, scale_(std::clamp(spacing_mm, 0.04, 0.50)));
    Points       out;
    out.reserve(source.size());
    out.emplace_back(source.front());
    for (size_t source_idx = 0; source_idx + 1 < source.size(); ++source_idx) {
        const Point& a      = source[source_idx];
        const Point& b      = source[source_idx + 1];
        const Vec2d  delta  = (b - a).cast<double>();
        const double length = delta.norm();
        if (!std::isfinite(length) || length <= EPSILON)
            continue;
        const size_t segment_count = std::clamp<size_t>(size_t(std::ceil(length / spacing_scaled)), 1, 16384);
        for (size_t segment_idx = 1; segment_idx <= segment_count; ++segment_idx) {
            const double t = double(segment_idx) / double(segment_count);
            const Point  point(coord_t(std::llround(double(a.x()) + delta.x() * t)),
                               coord_t(std::llround(double(a.y()) + delta.y() * t)));
            if (point != out.back())
                out.emplace_back(point);
        }
    }
    return out;
}

void smooth_component_heights(std::vector<std::vector<double>>& heights, const Points& points, bool closed)
{
    const size_t count = closed && heights.size() > 1 ? heights.size() - 1 : heights.size();
    if (count < 3 || heights.front().empty())
        return;
    const size_t component_count = heights.front().size();
    for (int pass = 0; pass < 3; ++pass) {
        std::vector<std::vector<double>> filtered = heights;
        for (size_t sample_idx = 0; sample_idx < count; ++sample_idx) {
            if (!closed && (sample_idx == 0 || sample_idx + 1 == count))
                continue;
            const size_t previous = sample_idx > 0 ? sample_idx - 1 : count - 1;
            const size_t next     = sample_idx + 1 < count ? sample_idx + 1 : 0;
            for (size_t component_idx = 0; component_idx < component_count; ++component_idx)
                filtered[sample_idx][component_idx] = 0.25 * heights[previous][component_idx] +
                                                       0.50 * heights[sample_idx][component_idx] +
                                                       0.25 * heights[next][component_idx];
        }
        heights.swap(filtered);
        if (closed)
            heights.back() = heights.front();
    }
    (void)points;
}

void limit_profile_slope(std::vector<double>& profile, const Points& points, bool closed, double maximum_slope)
{
    const size_t count = closed && profile.size() > 1 ? profile.size() - 1 : profile.size();
    if (count < 2 || points.size() != profile.size())
        return;
    auto distance_mm = [&points](size_t lhs, size_t rhs) {
        return unscale<double>((points[lhs] - points[rhs]).cast<double>().norm());
    };
    for (int pass = 0; pass < 8; ++pass) {
        for (size_t sample_idx = 1; sample_idx < count; ++sample_idx) {
            const double limit = maximum_slope * distance_mm(sample_idx - 1, sample_idx) + 1e-5;
            profile[sample_idx] = std::clamp(profile[sample_idx], profile[sample_idx - 1] - limit, profile[sample_idx - 1] + limit);
        }
        for (size_t sample_idx = count - 1; sample_idx > 0; --sample_idx) {
            const double limit = maximum_slope * distance_mm(sample_idx - 1, sample_idx) + 1e-5;
            profile[sample_idx - 1] = std::clamp(profile[sample_idx - 1], profile[sample_idx] - limit, profile[sample_idx] + limit);
        }
        if (closed) {
            const double limit = maximum_slope * distance_mm(count - 1, 0) + 1e-5;
            const double middle = 0.5 * (profile.front() + profile[count - 1]);
            if (std::abs(profile.front() - profile[count - 1]) > limit) {
                profile.front()     = middle;
                profile[count - 1] = middle;
            }
        }
    }
    if (closed)
        profile.back() = profile.front();
}

} // namespace

struct AdaptiveLocalZRenderer::Impl
{
    std::vector<VolumeSampler>                    volumes;
    std::vector<ContinuousColorComponent>         physical_components;
    double                                        sample_spacing_mm{0.12};
    mutable std::mutex                            solver_mutex;
    mutable std::unordered_map<std::string, std::shared_ptr<const ContinuousColorSolver>> restricted_solvers;

    std::shared_ptr<const ContinuousColorSolver> solver_for(const std::vector<unsigned int>& component_ids,
                                                            ColorMixModel                    color_mix_model) const
    {
        std::string key(1, char('0' + std::min<size_t>(size_t(color_mix_model), size_t(ColorMixModel::FilamentMixer))));
        key += ':';
        key.reserve(component_ids.size() * 4);
        for (unsigned int component_id : component_ids)
            key += std::to_string(component_id) + ',';
        std::lock_guard<std::mutex> lock(solver_mutex);
        if (const auto cached = restricted_solvers.find(key); cached != restricted_solvers.end())
            return cached->second;

        std::vector<ContinuousColorComponent> selected;
        selected.reserve(component_ids.size());
        for (unsigned int component_id : component_ids) {
            if (component_id < 1 || component_id > physical_components.size())
                return {};
            selected.emplace_back(physical_components[component_id - 1]);
        }
        auto solver = std::make_shared<ContinuousColorSolver>(std::move(selected), color_mix_model);
        if (!solver->valid())
            return {};
        restricted_solvers.emplace(std::move(key), solver);
        return solver;
    }
};

std::unique_ptr<AdaptiveLocalZRenderer> AdaptiveLocalZRenderer::create(const PrintObject& print_object)
{
    const Print*       print        = print_object.print();
    const ModelObject* model_object = print_object.model_object();
    if (print == nullptr || model_object == nullptr)
        return nullptr;

    auto impl = std::make_unique<Impl>();
    const size_t num_physical = print->config().filament_colour.size();
    impl->physical_components.reserve(num_physical);
    for (size_t component_idx = 0; component_idx < num_physical; ++component_idx) {
        ContinuousColorComponent component;
        component.color_hex = print->config().filament_colour.values[component_idx];
        if (MixedFilamentManager::use_td_for_color_prediction() &&
            component_idx < print->config().filament_transmission_distance.values.size() &&
            print->config().filament_transmission_distance.values[component_idx] > EPSILON)
            component.transmission_distance_mm = print->config().filament_transmission_distance.values[component_idx];
        if (component_idx < print->config().filament_full_spectrum_material_id.values.size() &&
            !print->config().filament_full_spectrum_material_id.values[component_idx].empty())
            component.material_id = print->config().filament_full_spectrum_material_id.values[component_idx];
        impl->physical_components.emplace_back(std::move(component));
    }

    const Transform3d object_to_print = print_object.trafo_centered();
    for (const ModelVolume* volume : model_object->volumes) {
        if (volume == nullptr || !volume->is_model_part())
            continue;
        const std::shared_ptr<const VolumeData> source_data = volume->image_map_data();
        if (!source_data || !data_has_adaptive_local_z(*source_data) || !source_data->validate(volume->mesh()).valid)
            continue;
        auto filtered_data = std::make_shared<VolumeData>(*source_data);
        for (Zone& zone : filtered_data->zones) {
            if (!zone_uses_adaptive_local_z(zone)) {
                zone.enabled = false;
                continue;
            }
            impl->sample_spacing_mm = std::min(impl->sample_spacing_mm,
                                               double(std::clamp(zone.modulation_sample_spacing_mm, 0.04f, 0.50f)));
        }
        const Transform3d local_to_print = object_to_print * volume->get_matrix();
        if (std::abs(local_to_print.linear().determinant()) <= EPSILON)
            continue;
        impl->volumes.push_back({volume->mesh_ptr(), std::move(filtered_data), local_to_print});
    }
    if (impl->volumes.empty() || impl->physical_components.empty())
        return nullptr;
    return std::unique_ptr<AdaptiveLocalZRenderer>(new AdaptiveLocalZRenderer(std::move(impl)));
}

AdaptiveLocalZRenderer::AdaptiveLocalZRenderer(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
AdaptiveLocalZRenderer::~AdaptiveLocalZRenderer()                                      = default;
AdaptiveLocalZRenderer::AdaptiveLocalZRenderer(AdaptiveLocalZRenderer&&) noexcept      = default;
AdaptiveLocalZRenderer& AdaptiveLocalZRenderer::operator=(AdaptiveLocalZRenderer&&) noexcept = default;

std::vector<ExtrusionPathSloped> AdaptiveLocalZRenderer::modulate_path(
    const ExtrusionPath&             path,
    const Layer&                     layer,
    const ExPolygons&                layer_slices,
    const std::vector<unsigned int>& component_ids,
    const std::vector<int>&          fallback_component_weights,
    size_t                           component_index,
    double                           nominal_z_lo,
    double                           nominal_z_hi,
    double                           sample_z,
    double                           minimum_height) const
{
    std::vector<ExtrusionPathSloped> output;
    if (!m_impl || path.polyline.points.size() < 2 || component_ids.size() < 2 ||
        component_index >= component_ids.size() || nominal_z_hi <= nominal_z_lo + EPSILON)
        return output;

    const Points points = resample_polyline(path.polyline.points, m_impl->sample_spacing_mm);
    if (points.size() < 2)
        return output;
    const bool closed = points.size() > 2 && points.front() == points.back();

    std::vector<LayerPlaneSampler> samplers;
    samplers.reserve(m_impl->volumes.size());
    const double source_z = std::isfinite(sample_z) ? sample_z : double(layer.slice_z);
    for (const VolumeSampler& volume : m_impl->volumes)
        samplers.emplace_back(volume.mesh, volume.data, volume.local_to_print, source_z);
    std::vector<double> fallback_weights(component_ids.size(), 1.0);
    if (fallback_component_weights.size() == component_ids.size())
        for (size_t idx = 0; idx < component_ids.size(); ++idx)
            fallback_weights[idx] = std::max(0, fallback_component_weights[idx]);

    const double total_height = nominal_z_hi - nominal_z_lo;
    std::vector<std::vector<double>> local_heights;
    local_heights.reserve(points.size());
    for (size_t point_idx = 0; point_idx < points.size(); ++point_idx) {
        const Vec2d scaled_point = points[point_idx].cast<double>();
        const Vec2d print_point  = unscale(scaled_point);
        const Vec2d outward     = outward_direction(points, point_idx, layer_slices);
        std::optional<LayerPlaneSample> selected;
        for (const LayerPlaneSampler& sampler : samplers) {
            const std::optional<LayerPlaneSample> candidate =
                sampler.sample(print_point, outward, 0.85, RenderMode::AdaptiveLocalizedCycles);
            if (!candidate || candidate->zone == nullptr || !zone_uses_adaptive_local_z(*candidate->zone))
                continue;
            if (!selected || candidate->squared_distance < selected->squared_distance - 1e-9 ||
                (std::abs(candidate->squared_distance - selected->squared_distance) <= 1e-9 &&
                 candidate->zone->priority > selected->zone->priority))
                selected = *candidate;
        }

        std::vector<double> weights = fallback_weights;
        const std::shared_ptr<const ContinuousColorSolver> solver =
            selected ? m_impl->solver_for(component_ids, selected->zone->color_mix_model) : nullptr;
        if (selected && solver && solver->valid()) {
            std::vector<double> solved = solver->solve_modulation(selected->color);
            if (solved.size() == component_ids.size())
                weights = std::move(solved);
        }
        local_heights.emplace_back(allocate_adaptive_local_z_heights(weights, total_height, minimum_height));
    }
    smooth_component_heights(local_heights, points, closed);

    std::vector<std::vector<double>> cumulative(component_ids.size(), std::vector<double>(points.size(), nominal_z_lo));
    auto rebuild_cumulative = [&]() {
        for (size_t point_idx = 0; point_idx < points.size(); ++point_idx) {
            double z = nominal_z_lo;
            for (size_t component_idx = 0; component_idx < component_ids.size(); ++component_idx) {
                z += local_heights[point_idx][component_idx];
                cumulative[component_idx][point_idx] = component_idx + 1 == component_ids.size() ? nominal_z_hi : z;
            }
        }
    };
    rebuild_cumulative();
    constexpr double maximum_z_slope = 0.12;
    for (int projection_pass = 0; projection_pass < 4; ++projection_pass) {
        for (size_t component_idx = 0; component_idx + 1 < cumulative.size(); ++component_idx)
            limit_profile_slope(cumulative[component_idx], points, closed, maximum_z_slope);
        for (size_t point_idx = 0; point_idx < points.size(); ++point_idx) {
            std::vector<double> projected_weights(component_ids.size(), 0.0);
            double              lower = nominal_z_lo;
            for (size_t component_idx = 0; component_idx < component_ids.size(); ++component_idx) {
                projected_weights[component_idx] = cumulative[component_idx][point_idx] - lower;
                lower = cumulative[component_idx][point_idx];
            }
            local_heights[point_idx] =
                allocate_adaptive_local_z_heights(projected_weights, total_height, minimum_height);
        }
        rebuild_cumulative();
    }

    output.reserve(points.size() - 1);
    for (size_t point_idx = 0; point_idx + 1 < points.size(); ++point_idx) {
        if (points[point_idx] == points[point_idx + 1])
            continue;
        const double lower_begin = component_index == 0 ? nominal_z_lo : cumulative[component_index - 1][point_idx];
        const double lower_end   = component_index == 0 ? nominal_z_lo : cumulative[component_index - 1][point_idx + 1];
        const double upper_begin = cumulative[component_index][point_idx];
        const double upper_end   = cumulative[component_index][point_idx + 1];
        const double local_height = std::max(0.01, 0.5 * ((upper_begin - lower_begin) + (upper_end - lower_end)));

        ExtrusionPath segment(Polyline{points[point_idx], points[point_idx + 1]}, path);
        if (path.height > EPSILON)
            segment.mm3_per_mm *= local_height / double(path.height);
        segment.height = float(local_height);
        segment.polyline.fitting_result.clear();
        ExtrusionPathSloped sloped(std::move(segment), ExtrusionPathSloped::Slope{0., 1.}, ExtrusionPathSloped::Slope{1., 1.});
        sloped.local_z_modulation = true;
        sloped.absolute_z_begin   = upper_begin;
        sloped.absolute_z_end     = upper_end;
        output.emplace_back(std::move(sloped));
    }
    return output;
}

} // namespace Slic3r::ImageMap
