#include "PerimeterEnvelopeRenderer.hpp"

#include "BoundaryModulation.hpp"
#include "ContinuousColorSolver.hpp"
#include "Sampling.hpp"
#include "../ClipperUtils.hpp"
#include "../ExtrusionEntity.hpp"
#include "../Layer.hpp"
#include "../MixedFilament.hpp"
#include "../Model.hpp"
#include "../Print.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Slic3r::ImageMap {
namespace {

bool zone_uses_perimeter_modulation(const Zone &zone)
{
    return zone.render_mode == RenderMode::PerimeterModulationV2 ||
           (zone.render_mode == RenderMode::AdaptiveLocalizedCycles &&
            zone.adaptive_modulation_mode == AdaptiveModulationMode::Perimeter);
}

bool data_has_perimeter_modulation(const VolumeData& data)
{
    return std::any_of(data.triangle_bindings.begin(), data.triangle_bindings.end(), [&data](const TriangleBinding& binding) {
        if (binding.zone_index >= data.zones.size())
            return false;
        const Zone& zone = data.zones[binding.zone_index];
        return zone.enabled && zone_uses_perimeter_modulation(zone) && !zone.palette.empty();
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
    std::shared_ptr<const TriangleMesh> mesh;
    std::shared_ptr<const VolumeData>   data;
    Transform3d                         local_to_print{Transform3d::Identity()};

    VolumeSampler(std::shared_ptr<const TriangleMesh> mesh, std::shared_ptr<const VolumeData> data, const Transform3d& transform)
        : mesh(std::move(mesh)), data(std::move(data)), local_to_print(transform)
    {}
};

struct SelectedSample
{
    LayerPlaneSample sample;
};

double extrusion_entity_length(const ExtrusionEntity& entity)
{
    if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        double length = 0.;
        for (const ExtrusionEntity* child : collection->entities)
            if (child != nullptr)
                length += extrusion_entity_length(*child);
        return length;
    }
    return entity.length();
}

struct ComponentExposure
{
    float inset_mm{0.f};
};

struct PathSample
{
    Point point;
    Vec2d outward{Vec2d::Zero()};
    float width_delta_mm{0.f};
    float smoothing_radius_mm{0.f};
    float first_smoothing_strength{1.f};
    float second_smoothing_strength{1.f};
    bool  modulated{false};
};

bool layer_slices_contain(const ExPolygons& layer_slices, const Point& point)
{
    return std::any_of(layer_slices.begin(), layer_slices.end(), [&point](const ExPolygon& slice) { return slice.contains(point, false); });
}

Vec2d fallback_outward_direction(const Points& points, size_t point_index, const ExPolygons& layer_slices, double probe_scaled)
{
    if (points.size() < 2)
        return Vec2d::Zero();

    size_t previous = point_index;
    while (previous > 0 && points[previous - 1] == points[point_index])
        --previous;
    size_t next = point_index;
    while (next + 1 < points.size() && points[next + 1] == points[point_index])
        ++next;

    const Point& before  = previous > 0 ? points[previous - 1] : points[point_index];
    const Point& after   = next + 1 < points.size() ? points[next + 1] : points[point_index];
    Vec2d        tangent = (after - before).cast<double>();
    const double length  = tangent.norm();
    if (!std::isfinite(length) || length <= EPSILON)
        return Vec2d::Zero();
    tangent /= length;

    const Vec2d normal(-tangent.y(), tangent.x());
    auto        probe = [&points, point_index, probe_scaled](const Vec2d& direction) {
        const Vec2d shifted = points[point_index].cast<double>() + direction * probe_scaled;
        return Point(coord_t(std::llround(shifted.x())), coord_t(std::llround(shifted.y())));
    };
    const bool positive_inside = layer_slices_contain(layer_slices, probe(normal));
    const bool negative_inside = layer_slices_contain(layer_slices, probe(-normal));
    if (positive_inside != negative_inside)
        return positive_inside ? -normal : normal;
    return normal;
}

Points resample_polyline(const Points& source, float spacing_mm)
{
    if (source.size() < 2)
        return source;

    const double spacing_scaled = std::max(1.0, scale_(double(std::clamp(spacing_mm, 0.02f, 2.f))));
    Points       out;
    out.reserve(source.size());
    out.emplace_back(source.front());
    for (size_t index = 0; index + 1 < source.size(); ++index) {
        const Point& a      = source[index];
        const Point& b      = source[index + 1];
        const Vec2d  delta  = (b - a).cast<double>();
        const double length = delta.norm();
        if (!std::isfinite(length) || length <= EPSILON)
            continue;
        const size_t segment_count = std::clamp<size_t>(size_t(std::ceil(length / spacing_scaled)), 1, 16384);
        for (size_t segment = 1; segment <= segment_count; ++segment) {
            const double t = double(segment) / double(segment_count);
            const Point  point(coord_t(std::llround(double(a.x()) + delta.x() * t)), coord_t(std::llround(double(a.y()) + delta.y() * t)));
            if (point != out.back())
                out.emplace_back(point);
        }
    }
    return out;
}

double path_sample_distance_mm(const PathSample& lhs, const PathSample& rhs)
{
    return unscale<double>((lhs.point - rhs.point).cast<double>().norm());
}

double cross_2d(const Vec2d& lhs, const Vec2d& rhs) { return lhs.x() * rhs.y() - lhs.y() * rhs.x(); }

struct SegmentIntersection
{
    Vec2d  point{Vec2d::Zero()};
    double first_parameter{0.};
    double second_parameter{0.};
};

std::optional<SegmentIntersection> proper_segment_intersection(const Point& first_a,
                                                               const Point& first_b,
                                                               const Point& second_a,
                                                               const Point& second_b)
{
    const Vec2d  p           = first_a.cast<double>();
    const Vec2d  r           = (first_b - first_a).cast<double>();
    const Vec2d  q           = second_a.cast<double>();
    const Vec2d  s           = (second_b - second_a).cast<double>();
    const double denominator = cross_2d(r, s);
    if (!std::isfinite(denominator) || std::abs(denominator) <= 1e-9)
        return std::nullopt;

    const double     first_parameter  = cross_2d(q - p, s) / denominator;
    const double     second_parameter = cross_2d(q - p, r) / denominator;
    constexpr double endpoint_epsilon = 1e-6;
    if (!std::isfinite(first_parameter) || !std::isfinite(second_parameter) || first_parameter <= endpoint_epsilon ||
        first_parameter >= 1. - endpoint_epsilon || second_parameter <= endpoint_epsilon || second_parameter >= 1. - endpoint_epsilon)
        return std::nullopt;

    const Vec2d intersection = p + first_parameter * r;
    if (!intersection.allFinite())
        return std::nullopt;
    return SegmentIntersection{intersection, first_parameter, second_parameter};
}

void trim_self_intersections(Points& shifted, std::vector<float>& widths, bool closed, const ExPolygons& layer_slices)
{
    if (shifted.size() != widths.size() || shifted.size() < 4)
        return;
    if (closed && shifted.front() == shifted.back()) {
        shifted.pop_back();
        widths.pop_back();
    }

    const size_t maximum_passes = shifted.size();
    for (size_t pass = 0; pass < maximum_passes && shifted.size() >= 4; ++pass) {
        bool         trimmed       = false;
        const size_t point_count   = shifted.size();
        const size_t segment_count = closed ? point_count : point_count - 1;
        const size_t maximum_span  = std::min<size_t>(32, closed ? (segment_count > 2 ? segment_count - 2 : 0) :
                                                                   (segment_count > 1 ? segment_count - 1 : 0));
        for (size_t first = 0; first < segment_count && !trimmed; ++first) {
            const size_t first_next = (first + 1) % point_count;
            for (size_t span = 2; span <= maximum_span; ++span) {
                if (!closed && first + span >= segment_count)
                    break;
                const size_t second      = (first + span) % segment_count;
                const size_t second_next = (second + 1) % point_count;

                const std::optional<SegmentIntersection> intersection = proper_segment_intersection(shifted[first], shifted[first_next],
                                                                                                    shifted[second], shifted[second_next]);
                if (!intersection)
                    continue;
                const Point quantized(coord_t(std::llround(intersection->point.x())), coord_t(std::llround(intersection->point.y())));
                if (!layer_slices_contain(layer_slices, quantized))
                    continue;

                const float first_width        = float((1. - intersection->first_parameter) * double(widths[first]) +
                                                intersection->first_parameter * double(widths[first_next]));
                const float second_width       = float((1. - intersection->second_parameter) * double(widths[second]) +
                                                 intersection->second_parameter * double(widths[second_next]));
                const float intersection_width = 0.5f * (first_width + second_width);

                size_t span_begin = first;
                size_t span_end   = second;
                if (closed && second < first) {
                    std::rotate(shifted.begin(), shifted.begin() + first, shifted.end());
                    std::rotate(widths.begin(), widths.begin() + first, widths.end());
                    span_begin = 0;
                    span_end   = span;
                }

                // Two non-adjacent shifted segments crossing means the span
                // between them is a modulation-created loop. Keep the two
                // printable segment pieces and replace only the loop with
                // their intersection. The interpolated width prevents this
                // safety repair from painting a seam down the model.
                shifted.erase(shifted.begin() + span_begin + 1, shifted.begin() + span_end + 1);
                widths.erase(widths.begin() + span_begin + 1, widths.begin() + span_end + 1);
                const size_t insertion = span_begin + 1;
                if (shifted[span_begin] != quantized && (insertion >= shifted.size() || shifted[insertion] != quantized)) {
                    shifted.insert(shifted.begin() + insertion, quantized);
                    widths.insert(widths.begin() + insertion, intersection_width);
                }
                trimmed = true;
                break;
            }
        }
        if (!trimmed)
            break;
    }

    if (closed && !shifted.empty()) {
        shifted.emplace_back(shifted.front());
        widths.emplace_back(widths.front());
    }
}

void repair_folded_corners(
    Points& shifted, const std::vector<PathSample>& samples, bool closed, const ExPolygons& layer_slices, float maximum_width_mm)
{
    const size_t count = closed && shifted.size() > 1 ? shifted.size() - 1 : shifted.size();
    if (count < 3 || samples.size() < count)
        return;

    const double max_join_distance = scale_(std::max(0.10f, 2.f * maximum_width_mm));
    for (int pass = 0; pass < 3; ++pass) {
        bool repaired = false;
        for (size_t index = 0; index < count; ++index) {
            if (!closed && (index == 0 || index + 1 == count))
                continue;
            const size_t previous = index == 0 ? count - 1 : index - 1;
            const size_t next     = (index + 1) % count;

            const Vec2d  incoming        = (shifted[index] - shifted[previous]).cast<double>();
            const Vec2d  outgoing        = (shifted[next] - shifted[index]).cast<double>();
            const double incoming_length = incoming.norm();
            const double outgoing_length = outgoing.norm();
            if (!std::isfinite(incoming_length) || !std::isfinite(outgoing_length) || incoming_length <= EPSILON ||
                outgoing_length <= EPSILON)
                continue;
            if (incoming.dot(outgoing) / (incoming_length * outgoing_length) > -0.25)
                continue;

            const Vec2d  source_previous        = samples[previous].point.cast<double>();
            const Vec2d  source_current         = samples[index].point.cast<double>();
            const Vec2d  source_next            = samples[next].point.cast<double>();
            const Vec2d  source_incoming        = source_current - source_previous;
            const Vec2d  source_outgoing        = source_next - source_current;
            const double source_incoming_length = source_incoming.norm();
            const double source_outgoing_length = source_outgoing.norm();
            if (source_incoming_length <= EPSILON || source_outgoing_length <= EPSILON)
                continue;
            const double source_turn_cosine = source_incoming.dot(source_outgoing) / (source_incoming_length * source_outgoing_length);

            // If the printable source path has a normal turn but modulation
            // folded it backwards, the source vertex itself is the safest
            // bounded bevel. Restore only that coordinate; retaining the
            // target width avoids a repeated unmodulated stripe at the same
            // corner on every layer.
            if (source_turn_cosine > -0.25) {
                shifted[index] = samples[index].point;
                repaired       = true;
                if (closed && index == 0)
                    shifted.back() = shifted.front();
                continue;
            }

            auto candidate_is_safe = [&](const Vec2d& candidate) {
                if (!candidate.allFinite() || (candidate - source_current).norm() > max_join_distance ||
                    (candidate - shifted[previous].cast<double>()).norm() > max_join_distance ||
                    (candidate - shifted[next].cast<double>()).norm() > max_join_distance)
                    return false;
                const Point quantized(coord_t(std::llround(candidate.x())), coord_t(std::llround(candidate.y())));
                if (!layer_slices_contain(layer_slices, quantized))
                    return false;

                const Vec2d  repaired_incoming        = candidate - shifted[previous].cast<double>();
                const Vec2d  repaired_outgoing        = shifted[next].cast<double>() - candidate;
                const double repaired_incoming_length = repaired_incoming.norm();
                const double repaired_outgoing_length = repaired_outgoing.norm();
                if (repaired_incoming_length <= EPSILON || repaired_outgoing_length <= EPSILON)
                    return false;
                if (repaired_incoming.dot(source_incoming) <= 0. || repaired_outgoing.dot(source_outgoing) <= 0.)
                    return false;
                return repaired_incoming.dot(repaired_outgoing) / (repaired_incoming_length * repaired_outgoing_length) > -0.25;
            };

            std::optional<Vec2d> candidate;
            const double         denominator = cross_2d(source_incoming, source_outgoing);
            if (std::abs(denominator) > 1e-9) {
                const Vec2d  p            = shifted[previous].cast<double>();
                const Vec2d  q            = shifted[next].cast<double>();
                const double parameter    = cross_2d(q - p, source_outgoing) / denominator;
                const Vec2d  intersection = p + parameter * source_incoming;
                if (candidate_is_safe(intersection))
                    candidate = intersection;
            }
            if (!candidate && candidate_is_safe(source_current))
                candidate = source_current;
            if (!candidate) {
                const Vec2d midpoint = 0.5 * (shifted[previous].cast<double>() + shifted[next].cast<double>());
                if (candidate_is_safe(midpoint))
                    candidate = midpoint;
            }
            if (!candidate)
                continue;

            shifted[index] = Point(coord_t(std::llround(candidate->x())), coord_t(std::llround(candidate->y())));
            repaired       = true;
            if (closed && index == 0)
                shifted.back() = shifted.front();
        }
        if (!repaired)
            break;
    }
}

void trim_folded_corners(Points& shifted, std::vector<float>& widths, bool closed, const ExPolygons& layer_slices, float maximum_width_mm)
{
    if (shifted.size() != widths.size() || shifted.size() < 3)
        return;
    if (closed && shifted.front() == shifted.back()) {
        shifted.pop_back();
        widths.pop_back();
    }

    auto chord_is_safe = [&](const Point& from, const Point& to) {
        const Vec2d delta = (to - from).cast<double>();
        if (!delta.allFinite() || delta.norm() > scale_(2.5 * double(maximum_width_mm)))
            return false;
        for (double t : {0.25, 0.5, 0.75}) {
            const Vec2d sample = from.cast<double>() + delta * t;
            const Point quantized(coord_t(std::llround(sample.x())), coord_t(std::llround(sample.y())));
            if (!layer_slices_contain(layer_slices, quantized))
                return false;
        }
        return true;
    };

    auto turn_is_safe = [](const Vec2d& incoming, const Vec2d& outgoing) {
        const double incoming_length = incoming.norm();
        const double outgoing_length = outgoing.norm();
        return incoming_length > EPSILON && outgoing_length > EPSILON &&
               incoming.dot(outgoing) / (incoming_length * outgoing_length) > -0.25;
    };

    const size_t maximum_passes = shifted.size();
    for (size_t pass = 0; pass < maximum_passes && shifted.size() >= 3; ++pass) {
        bool         trimmed = false;
        const size_t begin   = closed ? 0 : 1;
        const size_t end     = closed ? shifted.size() : shifted.size() - 1;
        for (size_t index = begin; index < end; ++index) {
            const size_t previous        = index == 0 ? shifted.size() - 1 : index - 1;
            const size_t next            = (index + 1) % shifted.size();
            const Vec2d  incoming        = (shifted[index] - shifted[previous]).cast<double>();
            const Vec2d  outgoing        = (shifted[next] - shifted[index]).cast<double>();
            const double incoming_length = incoming.norm();
            const double outgoing_length = outgoing.norm();
            if (incoming_length <= EPSILON || outgoing_length <= EPSILON ||
                incoming.dot(outgoing) / (incoming_length * outgoing_length) > -0.25)
                continue;

            // This is the span-aware trim used by the reference algorithm in
            // its G-code stage: remove the smallest overlap-prone span whose
            // replacement chord remains entirely in printable material.
            const size_t max_radius = std::min<size_t>(8, (shifted.size() - 1) / 2);
            for (size_t radius = 1; radius <= max_radius; ++radius) {
                if (!closed && (index < radius || index + radius >= shifted.size()))
                    break;
                const size_t span_previous      = closed ? (index + shifted.size() - radius) % shifted.size() : index - radius;
                const size_t span_next          = closed ? (index + radius) % shifted.size() : index + radius;
                const size_t before_previous    = span_previous == 0 ? shifted.size() - 1 : span_previous - 1;
                const size_t after_next         = (span_next + 1) % shifted.size();
                const Vec2d  chord              = (shifted[span_next] - shifted[span_previous]).cast<double>();
                const bool   previous_turn_safe = !closed && span_previous == 0 ?
                                                      true :
                                                      turn_is_safe((shifted[span_previous] - shifted[before_previous]).cast<double>(), chord);
                const bool   next_turn_safe     = !closed && span_next + 1 == shifted.size() ?
                                                      true :
                                                      turn_is_safe(chord, (shifted[after_next] - shifted[span_next]).cast<double>());
                if (!chord_is_safe(shifted[span_previous], shifted[span_next]) || !previous_turn_safe || !next_turn_safe)
                    continue;

                if (!closed) {
                    shifted.erase(shifted.begin() + span_previous + 1, shifted.begin() + span_next);
                    widths.erase(widths.begin() + span_previous + 1, widths.begin() + span_next);
                } else {
                    Points             repaired_points;
                    std::vector<float> repaired_widths;
                    repaired_points.reserve(shifted.size() - 2 * radius + 2);
                    repaired_widths.reserve(repaired_points.capacity());
                    repaired_points.emplace_back(shifted[span_previous]);
                    repaired_widths.emplace_back(widths[span_previous]);
                    size_t cursor = span_next;
                    while (cursor != span_previous) {
                        repaired_points.emplace_back(shifted[cursor]);
                        repaired_widths.emplace_back(widths[cursor]);
                        cursor = (cursor + 1) % shifted.size();
                    }
                    shifted = std::move(repaired_points);
                    widths  = std::move(repaired_widths);
                }
                trimmed = true;
                break;
            }
            if (trimmed)
                break;
        }
        if (!trimmed)
            break;
    }

    if (closed && !shifted.empty()) {
        shifted.emplace_back(shifted.front());
        widths.emplace_back(widths.front());
    }
}

void smooth_path_width_deltas(std::vector<PathSample>& samples, bool closed, float max_slope)
{
    const size_t count = closed && samples.size() > 1 ? samples.size() - 1 : samples.size();
    if (count < 3)
        return;

    auto smooth_once = [&samples, count, closed](float radius_scale, float minimum_radius, bool first_pass) {
        std::vector<float> smoothed(count, 0.f);
        for (size_t index = 0; index < count; ++index) {
            const float strength = std::clamp(first_pass ? samples[index].first_smoothing_strength :
                                                            samples[index].second_smoothing_strength,
                                              0.f,
                                              4.f);
            if (!samples[index].modulated || samples[index].smoothing_radius_mm <= EPSILON || strength <= EPSILON) {
                smoothed[index] = samples[index].width_delta_mm;
                continue;
            }
            const float radius =
                std::clamp(std::max(minimum_radius, samples[index].smoothing_radius_mm * radius_scale) * strength, 0.f, 2.f);
            double      weighted_sum = samples[index].width_delta_mm;
            double      weight_sum   = 1.;
            for (int direction : {-1, 1}) {
                double distance = 0.;
                size_t current  = index;
                for (size_t step = 1; step < count; ++step) {
                    if (!closed && ((direction < 0 && current == 0) || (direction > 0 && current + 1 >= count)))
                        break;
                    const size_t next = direction < 0 ? (current == 0 ? count - 1 : current - 1) : (current + 1) % count;
                    if (!samples[next].modulated)
                        break;
                    distance += path_sample_distance_mm(samples[current], samples[next]);
                    if (distance > radius)
                        break;
                    const double weight = 1. - distance / radius;
                    weighted_sum += double(samples[next].width_delta_mm) * weight;
                    weight_sum += weight;
                    current = next;
                }
            }
            // A smaller delta exposes more of the active color. Do not spread
            // that high-exposure color into a neighboring texel.
            smoothed[index] = std::max(samples[index].width_delta_mm, float(weighted_sum / std::max(weight_sum, 1.)));
        }
        for (size_t index = 0; index < count; ++index)
            samples[index].width_delta_mm = smoothed[index];
    };

    smooth_once(1.15f, 0.30f, true);
    max_slope = std::max(0.f, max_slope);
    for (int pass = 0; pass < 4; ++pass) {
        for (size_t index = 1; index < count; ++index) {
            const float limit = float(path_sample_distance_mm(samples[index - 1], samples[index])) * max_slope + 0.015f;
            if (samples[index].width_delta_mm > samples[index - 1].width_delta_mm + limit)
                samples[index].width_delta_mm = samples[index - 1].width_delta_mm + limit;
        }
        for (size_t index = count - 1; index > 0; --index) {
            const float limit = float(path_sample_distance_mm(samples[index - 1], samples[index])) * max_slope + 0.015f;
            if (samples[index - 1].width_delta_mm > samples[index].width_delta_mm + limit)
                samples[index - 1].width_delta_mm = samples[index].width_delta_mm + limit;
        }
        if (closed) {
            const float limit = float(path_sample_distance_mm(samples[count - 1], samples[0])) * max_slope + 0.015f;
            if (samples[0].width_delta_mm > samples[count - 1].width_delta_mm + limit)
                samples[0].width_delta_mm = samples[count - 1].width_delta_mm + limit;
            if (samples[count - 1].width_delta_mm > samples[0].width_delta_mm + limit)
                samples[count - 1].width_delta_mm = samples[0].width_delta_mm + limit;
        }
    }
    smooth_once(0.45f, 0.20f, false);
    if (closed)
        samples.back() = samples.front();
}

} // namespace

struct PerimeterEnvelopeRenderer::Impl
{
    static size_t color_model_index(ColorMixModel model)
    {
        return std::min<size_t>(size_t(model), size_t(ColorMixModel::FilamentMixer));
    }

    struct LayerCadence
    {
        unsigned int                                 filament_id{0};
        unsigned int                                 direct_component_id{0};
        std::vector<int>                             component_percents;
        std::vector<unsigned int>                    layer_sequence;
        std::shared_ptr<const ContinuousColorSolver> restricted_solver;
        std::vector<size_t>                          solver_to_physical;
    };

    const MixedFilamentManager*                                                   manager{nullptr};
    size_t                                                                        num_physical{0};
    size_t                                                                        num_total{0};
    float                                                                         max_displacement_mm{0.63f};
    float                                                                         carrier_width_mm{0.95f};
    float                                                                         minimum_visibility_width_mm{0.32f};
    float                                                                         line_width_fraction{0.f};
    float                                                                         smoothing_base_mm{0.95f};
    float                                                                         sample_spacing_mm{0.16f};
    bool                                                                          adaptive_material_islands{false};
    std::array<std::unique_ptr<ContinuousColorSolver>, 2>                         solvers;
    std::array<std::unique_ptr<ContinuousColorRecipeSolver>, 2>                   adaptive_recipe_solvers;
    std::vector<ContinuousColorComponent>                                         solver_components;
    std::unordered_map<std::string, std::shared_ptr<const ContinuousColorSolver>> restricted_solvers;
    std::vector<VolumeSampler>                                                    volumes;
    std::unordered_map<const Zone*, LayerCadence>                                 shared_cadences;
    std::unordered_map<const PaletteEntry*, LayerCadence>                         adaptive_cadences;
    std::unordered_set<unsigned int>                                              adaptive_island_filament_ids;
    mutable std::mutex                                                            adaptive_dynamic_cadences_mutex;
    mutable std::unordered_map<std::string, std::shared_ptr<const LayerCadence>>  adaptive_dynamic_cadences_by_mask;
    mutable std::unordered_map<const Zone*,
                               std::unordered_map<uint32_t, std::shared_ptr<const LayerCadence>>>
        adaptive_dynamic_cadences_by_color;

    void ensure_solvers(ColorMixModel model)
    {
        const size_t index = color_model_index(model);
        if (!solvers[index])
            solvers[index] = std::make_unique<ContinuousColorSolver>(solver_components, model);
        if (!adaptive_recipe_solvers[index])
            adaptive_recipe_solvers[index] = std::make_unique<ContinuousColorRecipeSolver>(solver_components, 4, model);
    }

    const ContinuousColorSolver* solver_for(ColorMixModel model) const
    {
        const std::unique_ptr<ContinuousColorSolver>& solver = solvers[color_model_index(model)];
        return solver && solver->valid() ? solver.get() : nullptr;
    }

    const ContinuousColorRecipeSolver* adaptive_recipe_solver_for(ColorMixModel model) const
    {
        const std::unique_ptr<ContinuousColorRecipeSolver>& solver = adaptive_recipe_solvers[color_model_index(model)];
        return solver && solver->valid() ? solver.get() : nullptr;
    }

    float path_displacement(float requested_displacement_mm) const { return requested_displacement_mm * (1.f - line_width_fraction); }

    float line_width_reduction(float requested_displacement_mm) const { return requested_displacement_mm * line_width_fraction; }

    struct CadenceRef
    {
        const LayerCadence*                 cadence{nullptr};
        std::shared_ptr<const LayerCadence> lifetime;

        explicit operator bool() const { return cadence != nullptr; }
    };

    struct AdaptiveTextureCadenceMap
    {
        uint32_t                width{0};
        uint32_t                height{0};
        std::vector<uint16_t>   labels;
        std::vector<CadenceRef> cadences{CadenceRef{}};
    };

    struct AdaptiveTextureCadenceMapKey
    {
        const Zone*         zone{nullptr};
        const TextureAsset* asset{nullptr};

        bool operator==(const AdaptiveTextureCadenceMapKey& other) const { return zone == other.zone && asset == other.asset; }
    };

    struct AdaptiveTextureCadenceMapKeyHash
    {
        size_t operator()(const AdaptiveTextureCadenceMapKey& key) const
        {
            const size_t zone_hash  = std::hash<const void*>{}(key.zone);
            const size_t asset_hash = std::hash<const void*>{}(key.asset);
            return zone_hash ^ (asset_hash + size_t(0x9e3779b9) + (zone_hash << 6) + (zone_hash >> 2));
        }
    };

    mutable std::unordered_map<AdaptiveTextureCadenceMapKey,
                               std::shared_ptr<const AdaptiveTextureCadenceMap>,
                               AdaptiveTextureCadenceMapKeyHash>
        adaptive_texture_cadence_maps;

    static uint32_t adaptive_color_key(const RGBA& color)
    {
        auto quantize = [](float channel) {
            return uint32_t(std::lround(std::clamp(channel, 0.f, 1.f) * 31.f));
        };
        return quantize(color[0]) | (quantize(color[1]) << 5) | (quantize(color[2]) << 10);
    }

    std::shared_ptr<const LayerCadence> dynamic_adaptive_cadence_locked(const Zone* zone, const RGBA& color) const
    {
        const ContinuousColorRecipeSolver* recipe_solver = zone != nullptr ? adaptive_recipe_solver_for(zone->color_mix_model) : nullptr;
        if (zone == nullptr || zone->palette.size() != 1 || recipe_solver == nullptr)
            return {};

        const uint32_t color_key   = adaptive_color_key(color);
        auto&          color_cache = adaptive_dynamic_cadences_by_color[zone];
        if (const auto cached = color_cache.find(color_key); cached != color_cache.end())
            return cached->second;

        const RGBA quantized_color{float(color_key & 31u) / 31.f,
                                   float((color_key >> 5) & 31u) / 31.f,
                                   float((color_key >> 10) & 31u) / 31.f,
                                   1.f};
        const ContinuousColorRecipe recipe = recipe_solver->solve(quantized_color, zone->minimum_component_percent);
        if (!recipe.valid()) {
            color_cache.emplace(color_key, nullptr);
            return {};
        }

        std::string component_mask(1, char('0' + color_model_index(zone->color_mix_model)));
        component_mask.append(num_physical, '0');
        for (size_t component_index : recipe.component_indices)
            if (component_index < num_physical)
                component_mask[component_index + 1] = '1';
        if (const auto cached = adaptive_dynamic_cadences_by_mask.find(component_mask);
            cached != adaptive_dynamic_cadences_by_mask.end()) {
            color_cache.emplace(color_key, cached->second);
            return cached->second;
        }

        auto cadence = std::make_shared<LayerCadence>();
        cadence->component_percents.assign(num_physical, 0);
        for (size_t recipe_index = 0; recipe_index < recipe.component_indices.size(); ++recipe_index) {
            const size_t physical_index = recipe.component_indices[recipe_index];
            if (physical_index < cadence->component_percents.size())
                cadence->component_percents[physical_index] = recipe.component_percents[recipe_index];
        }
        for (size_t physical_index : recipe.layer_sequence)
            if (physical_index < num_physical)
                cadence->layer_sequence.emplace_back(unsigned(physical_index + 1));

        const size_t active_component_count = size_t(std::count_if(cadence->component_percents.begin(),
                                                                    cadence->component_percents.end(),
                                                                    [](int percent) { return percent > 0; }));
        if (active_component_count == 1) {
            const auto active = std::find_if(cadence->component_percents.begin(), cadence->component_percents.end(),
                                             [](int percent) { return percent > 0; });
            cadence->direct_component_id = unsigned(std::distance(cadence->component_percents.begin(), active) + 1);
        } else if (active_component_count >= 2 && active_component_count < num_physical) {
            std::vector<ContinuousColorComponent> restricted_components;
            restricted_components.reserve(active_component_count);
            for (size_t component_index = 0; component_index < cadence->component_percents.size(); ++component_index) {
                if (cadence->component_percents[component_index] <= 0 || component_index >= solver_components.size())
                    continue;
                cadence->solver_to_physical.emplace_back(component_index);
                restricted_components.emplace_back(solver_components[component_index]);
            }
            auto restricted_solver =
                std::make_shared<ContinuousColorSolver>(std::move(restricted_components), zone->color_mix_model);
            if (!restricted_solver->valid()) {
                color_cache.emplace(color_key, nullptr);
                return {};
            }
            cadence->restricted_solver = std::move(restricted_solver);
        }
        if (cadence->layer_sequence.empty() || active_component_count == 0) {
            color_cache.emplace(color_key, nullptr);
            return {};
        }

        const std::shared_ptr<const LayerCadence> stored = cadence;
        adaptive_dynamic_cadences_by_mask.emplace(std::move(component_mask), stored);
        color_cache.emplace(color_key, stored);
        return stored;
    }

    CadenceRef adaptive_cadence_for_color_locked(const Zone* zone, const RGBA& color) const
    {
        if (zone == nullptr)
            return {};
        if (zone->palette.size() == 1) {
            if (std::shared_ptr<const LayerCadence> dynamic = dynamic_adaptive_cadence_locked(zone, color))
                return {dynamic.get(), std::move(dynamic)};
        }
        const PaletteEntry* entry = nearest_palette_entry(*zone, color);
        if (entry == nullptr)
            return {};
        const auto cadence = adaptive_cadences.find(entry);
        return cadence == adaptive_cadences.end() ? CadenceRef{} : CadenceRef{&cadence->second, {}};
    }

    static std::string cadence_component_mask(const CadenceRef& cadence)
    {
        if (!cadence)
            return {};
        std::string mask(cadence.cadence->component_percents.size(), '0');
        for (size_t component = 0; component < cadence.cadence->component_percents.size(); ++component)
            if (cadence.cadence->component_percents[component] > 0)
                mask[component] = '1';
        return mask;
    }

    std::shared_ptr<const AdaptiveTextureCadenceMap> build_adaptive_texture_cadence_map_locked(const Zone*         zone,
                                                                                                 const TextureAsset* asset) const
    {
        if (zone == nullptr || asset == nullptr || !asset->valid())
            return {};

        // Recipe ownership is categorical. Sampling the source bilinearly and
        // solving every wall point independently creates artificial mixture
        // colors along every texel edge; adjacent layers then choose different
        // component subsets and grow horizontal fingers. Build one bounded 2D
        // ownership field from source texels instead, then regularize that
        // field before any perimeter layer queries it.
        constexpr uint32_t maximum_map_dimension = 512;
        const double scale = std::min(1.0,
                                      double(maximum_map_dimension) /
                                          double(std::max<uint32_t>(1, std::max(asset->width, asset->height))));
        auto map    = std::make_shared<AdaptiveTextureCadenceMap>();
        map->width  = std::max<uint32_t>(1, uint32_t(std::lround(double(asset->width) * scale)));
        map->height = std::max<uint32_t>(1, uint32_t(std::lround(double(asset->height) * scale)));
        map->labels.resize(size_t(map->width) * size_t(map->height), 0);

        std::unordered_map<std::string, uint16_t> label_by_mask;
        for (uint32_t y = 0; y < map->height; ++y) {
            const uint32_t source_y = std::min(asset->height - 1,
                                               uint32_t((uint64_t(2 * y + 1) * asset->height) /
                                                        uint64_t(2 * map->height)));
            for (uint32_t x = 0; x < map->width; ++x) {
                const uint32_t source_x = std::min(asset->width - 1,
                                                   uint32_t((uint64_t(2 * x + 1) * asset->width) /
                                                            uint64_t(2 * map->width)));
                const size_t offset = (size_t(source_y) * size_t(asset->width) + size_t(source_x)) * 4;
                const float  alpha  = float(asset->rgba[offset + 3]) / 255.f;
                const RGBA   color{float(asset->rgba[offset]) / 255.f * alpha + (1.f - alpha),
                                 float(asset->rgba[offset + 1]) / 255.f * alpha + (1.f - alpha),
                                 float(asset->rgba[offset + 2]) / 255.f * alpha + (1.f - alpha),
                                 1.f};
                CadenceRef cadence = adaptive_cadence_for_color_locked(zone, color);
                if (!cadence)
                    continue;

                const std::string mask = cadence_component_mask(cadence);
                auto              label = label_by_mask.find(mask);
                if (label == label_by_mask.end()) {
                    if (map->cadences.size() >= size_t(std::numeric_limits<uint16_t>::max()))
                        continue;
                    const uint16_t new_label = uint16_t(map->cadences.size());
                    map->cadences.emplace_back(std::move(cadence));
                    label = label_by_mask.emplace(mask, new_label).first;
                }
                map->labels[size_t(y) * size_t(map->width) + size_t(x)] = label->second;
            }
        }

        // Two conservative majority passes remove sub-bead spikes while
        // retaining compact authored islands. The cadence table is keyed by
        // component subset, so weights may still vary continuously inside an
        // island without changing which tools own its layers.
        constexpr int radius = 2;
        std::vector<uint16_t> filtered(map->labels.size(), 0);
        std::vector<size_t>   counts(map->cadences.size(), 0);
        for (int pass = 0; pass < 2; ++pass) {
            for (uint32_t y = 0; y < map->height; ++y) {
                for (uint32_t x = 0; x < map->width; ++x) {
                    std::fill(counts.begin(), counts.end(), size_t(0));
                    size_t sample_count = 0;
                    for (int dy = -radius; dy <= radius; ++dy) {
                        const uint32_t sample_y = uint32_t(std::clamp<int>(int(y) + dy, 0, int(map->height) - 1));
                        for (int dx = -radius; dx <= radius; ++dx) {
                            const uint32_t sample_x = uint32_t(std::clamp<int>(int(x) + dx, 0, int(map->width) - 1));
                            const uint16_t label = map->labels[size_t(sample_y) * size_t(map->width) + size_t(sample_x)];
                            if (label < counts.size())
                                ++counts[label];
                            ++sample_count;
                        }
                    }
                    const uint16_t center = map->labels[size_t(y) * size_t(map->width) + size_t(x)];
                    uint16_t       best   = center;
                    for (uint16_t label = 0; label < counts.size(); ++label)
                        if (counts[label] > counts[best])
                            best = label;
                    filtered[size_t(y) * size_t(map->width) + size_t(x)] =
                        best != center && counts[best] > sample_count / 2 ? best : center;
                }
            }
            map->labels.swap(filtered);
        }
        return map;
    }

    CadenceRef adaptive_texture_cadence(const LayerPlaneSample& sample) const
    {
        if (sample.zone == nullptr || sample.data == nullptr || sample.binding == nullptr ||
            sample.binding->source.kind != SourceKind::Texture || sample.binding->source.texture_asset_index < 0 ||
            size_t(sample.binding->source.texture_asset_index) >= sample.data->texture_assets.size())
            return {};

        const TextureAsset* asset = &sample.data->texture_assets[size_t(sample.binding->source.texture_asset_index)];
        const AdaptiveTextureCadenceMapKey key{sample.zone, asset};
        std::lock_guard<std::mutex>         lock(adaptive_dynamic_cadences_mutex);
        auto                               cached = adaptive_texture_cadence_maps.find(key);
        if (cached == adaptive_texture_cadence_maps.end()) {
            std::shared_ptr<const AdaptiveTextureCadenceMap> built = build_adaptive_texture_cadence_map_locked(sample.zone, asset);
            cached = adaptive_texture_cadence_maps.emplace(key, std::move(built)).first;
        }
        const std::shared_ptr<const AdaptiveTextureCadenceMap>& map = cached->second;
        if (!map || map->labels.empty() || map->cadences.size() <= 1)
            return {};

        Vec2f uv = sample.binding->source.uvs[0] * sample.barycentric.x() +
                   sample.binding->source.uvs[1] * sample.barycentric.y() +
                   sample.binding->source.uvs[2] * sample.barycentric.z();
        if ((sample.binding->source.wrap_u == WrapMode::Transparent && (uv.x() < 0.f || uv.x() > 1.f)) ||
            (sample.binding->source.wrap_v == WrapMode::Transparent && (uv.y() < 0.f || uv.y() > 1.f)))
            return {};
        auto wrap = [](float coordinate, WrapMode mode) {
            if (!std::isfinite(coordinate))
                return 0.f;
            if (mode == WrapMode::Clamp || mode == WrapMode::Transparent)
                return std::clamp(coordinate, 0.f, 1.f);
            const float wrapped = coordinate - std::floor(coordinate);
            return wrapped < 0.f ? wrapped + 1.f : wrapped;
        };
        uv.x() = wrap(uv.x(), sample.binding->source.wrap_u);
        uv.y() = wrap(uv.y(), sample.binding->source.wrap_v);
        const uint32_t x = std::min(map->width - 1, uint32_t(std::lround(uv.x() * float(map->width - 1))));
        const uint32_t y = std::min(map->height - 1, uint32_t(std::lround(uv.y() * float(map->height - 1))));
        const uint16_t label = map->labels[size_t(y) * size_t(map->width) + size_t(x)];
        return label < map->cadences.size() ? map->cadences[label] : CadenceRef{};
    }

    std::shared_ptr<const LayerCadence> dynamic_adaptive_cadence(const LayerPlaneSample& sample) const
    {
        if (sample.zone == nullptr || sample.zone->palette.size() != 1)
            return {};
        std::lock_guard<std::mutex> lock(adaptive_dynamic_cadences_mutex);
        return dynamic_adaptive_cadence_locked(sample.zone, sample.color);
    }

    CadenceRef cadence_for_sample(const LayerPlaneSample& sample) const
    {
        if (sample.zone == nullptr)
            return {};
        if (sample.zone->render_mode == RenderMode::AdaptiveLocalizedCycles && sample.palette_entry != nullptr) {
            if (CadenceRef texture = adaptive_texture_cadence(sample))
                return texture;
            if (std::shared_ptr<const LayerCadence> dynamic = dynamic_adaptive_cadence(sample))
                return {dynamic.get(), std::move(dynamic)};
            const auto cadence = adaptive_cadences.find(sample.palette_entry);
            return cadence == adaptive_cadences.end() ? CadenceRef{} : CadenceRef{&cadence->second, {}};
        }
        const auto cadence = shared_cadences.find(sample.zone);
        return cadence == shared_cadences.end() ? CadenceRef{} : CadenceRef{&cadence->second, {}};
    }

    unsigned int active_component(const LayerCadence& cadence, const Layer& layer) const
    {
        return !cadence.layer_sequence.empty() ? cadence.layer_sequence[layer.id() % cadence.layer_sequence.size()] :
                                                 manager->resolve(cadence.filament_id,
                                                                  num_physical,
                                                                  int(layer.id()),
                                                                  float(layer.print_z),
                                                                  float(layer.height));
    }

    std::optional<SelectedSample> sample(const std::vector<LayerPlaneSampler>& layer_samplers,
                                         const Vec2d&                          print_point,
                                         const Vec2d&                          outward,
                                         double                                max_world_distance,
                                         std::optional<RenderMode>              required_mode = std::nullopt) const
    {
        std::optional<SelectedSample> best;
        for (const LayerPlaneSampler& sampler : layer_samplers) {
            for (const RenderMode mode : {RenderMode::PerimeterModulationV2, RenderMode::AdaptiveLocalizedCycles}) {
                if (required_mode && mode != *required_mode)
                    continue;
                const std::optional<LayerPlaneSample> candidate = sampler.sample(print_point, outward, max_world_distance, mode);
                if (!candidate)
                    continue;
                if (!best || candidate->squared_distance < best->sample.squared_distance - 1e-9 ||
                    (std::abs(candidate->squared_distance - best->sample.squared_distance) <= 1e-9 &&
                     candidate->zone->priority > best->sample.zone->priority))
                    best = SelectedSample{*candidate};
            }
        }
        return best;
    }

    struct ComponentWeightSample
    {
        unsigned int       active_component{0};
        std::vector<double> weights;
    };

    struct ComponentWeightFieldPoint
    {
        Vec2d               point_mm{Vec2d::Zero()};
        Vec2d               outward{Vec2d::Zero()};
        const Zone*         zone{nullptr};
        unsigned int        active_component{0};
        double              integration_weight_mm{0.0};
        std::vector<double> weights;
    };

    struct ComponentWeightField
    {
        double                                             bucket_size_mm{0.12};
        std::vector<ComponentWeightFieldPoint>             points;
        std::unordered_map<uint64_t, std::vector<uint32_t>> buckets;

        static uint64_t bucket_key(int x, int y) { return (uint64_t(uint32_t(x)) << 32) | uint64_t(uint32_t(y)); }
    };

    std::optional<ComponentWeightSample> raw_component_weights(
        const LayerPlaneSample& sample, const Layer& layer, std::optional<unsigned int> active_component_override = std::nullopt) const
    {
        if (manager == nullptr || sample.zone == nullptr)
            return std::nullopt;
        // A synchronized projection owns the object's layer cadence, but only
        // opaque image coverage owns its perimeter modulation. Outside the
        // projected rectangle (or under transparent pixels), leave the wall
        // envelope unchanged so the shared cadence remains visible there.
        if (sample.zone->synchronize_whole_object_cadence && sample.data != nullptr && sample.binding != nullptr &&
            sample_source_opacity(*sample.data, *sample.binding, sample.barycentric) <= 0.001f)
            return std::nullopt;
        const CadenceRef cadence = cadence_for_sample(sample);
        if (!cadence)
            return std::nullopt;
        const unsigned int selected_component = active_component_override ? *active_component_override :
                                                                             active_component(*cadence.cadence, layer);
        if (selected_component < 1 || selected_component > num_physical)
            return std::nullopt;

        ComponentWeightSample result;
        result.active_component = selected_component;
        result.weights.assign(num_physical, 0.0);
        if (cadence.cadence->direct_component_id != 0) {
            if (selected_component != cadence.cadence->direct_component_id)
                return std::nullopt;
            result.weights[selected_component - 1] = 1.0;
            return result;
        }

        const RGBA target_color = adjusted_modulation_target_color(sample.color, *sample.zone);

        if (cadence.cadence->restricted_solver && cadence.cadence->restricted_solver->valid()) {
            std::vector<double> restricted_weights = cadence.cadence->restricted_solver->solve(target_color);
            if (restricted_weights.size() != cadence.cadence->solver_to_physical.size())
                return std::nullopt;
            apply_modulation_component_contrast(restricted_weights, *sample.zone);
            for (size_t index = 0; index < cadence.cadence->solver_to_physical.size(); ++index) {
                const size_t physical_index = cadence.cadence->solver_to_physical[index];
                if (physical_index >= cadence.cadence->component_percents.size() || physical_index >= result.weights.size())
                    return std::nullopt;
                result.weights[physical_index] = restricted_weights[index];
            }
            return result;
        }

        const ContinuousColorSolver* full_solver = solver_for(sample.zone->color_mix_model);
        if (full_solver == nullptr)
            return std::nullopt;
        result.weights = full_solver->solve(target_color);
        if (result.weights.size() != num_physical)
            return std::nullopt;
        apply_modulation_component_contrast(result.weights, *sample.zone);
        return result;
    }

    std::optional<ComponentExposure> compact_exposure(ComponentWeightSample sample) const
    {
        if (sample.active_component < 1 || sample.active_component > sample.weights.size())
            return std::nullopt;
        sample.weights = compact_modulation_weights(std::move(sample.weights));
        const float active_strength = float(std::clamp(sample.weights[sample.active_component - 1], 0.0, 1.0));
        return ComponentExposure{max_displacement_mm * (1.f - active_strength)};
    }

    ComponentWeightField build_component_weight_field(
        const Layer&                          layer,
        const std::vector<LayerPlaneSampler>& layer_samplers,
        std::optional<RenderMode>              required_mode = std::nullopt,
        std::optional<unsigned int>            active_component_override = std::nullopt) const
    {
        ComponentWeightField field;
        const double         field_pitch_mm = std::clamp(double(sample_spacing_mm), 0.02, 0.08);
        field.bucket_size_mm                = std::max(0.12, 1.5 * field_pitch_mm);

        for (const LayerPlaneSampler& sampler : layer_samplers) {
            std::vector<LayerPlaneFieldSample> surface_samples = sampler.field_samples(field_pitch_mm, required_mode);
            for (LayerPlaneFieldSample& surface_sample : surface_samples) {
                std::optional<ComponentWeightSample> weights =
                    raw_component_weights(surface_sample.sample, layer, active_component_override);
                if (!weights || weights->weights.size() != num_physical || weights->active_component == 0 ||
                    !surface_sample.print_point.allFinite() || !surface_sample.outward.allFinite() ||
                    !std::isfinite(surface_sample.integration_weight_mm) || surface_sample.integration_weight_mm <= EPSILON)
                    continue;

                ComponentWeightFieldPoint point;
                point.point_mm              = surface_sample.print_point;
                point.outward               = surface_sample.outward;
                point.zone                  = surface_sample.sample.zone;
                point.active_component      = weights->active_component;
                point.integration_weight_mm = surface_sample.integration_weight_mm;
                point.weights               = std::move(weights->weights);
                if (field.points.size() >= size_t(std::numeric_limits<uint32_t>::max()))
                    break;
                const uint32_t point_index = uint32_t(field.points.size());
                field.points.emplace_back(std::move(point));
                const int bucket_x = int(std::floor(field.points.back().point_mm.x() / field.bucket_size_mm));
                const int bucket_y = int(std::floor(field.points.back().point_mm.y() / field.bucket_size_mm));
                field.buckets[ComponentWeightField::bucket_key(bucket_x, bucket_y)].emplace_back(point_index);
            }
        }
        return field;
    }

    std::optional<ComponentExposure> spatially_filtered_exposure(
        const SelectedSample&       selected,
        const Vec2d&                point_mm,
        const Vec2d&                outward,
        const Layer&                layer,
        const ComponentWeightField& weight_field,
        std::optional<unsigned int> active_component_override = std::nullopt) const
    {
        std::optional<ComponentWeightSample> center = raw_component_weights(selected.sample, layer, active_component_override);
        if (!center || center->weights.size() != num_physical)
            return std::nullopt;

        // Match ImageMap V2's high-resolution reconstruction: solve printable
        // component recipes on every mapped mesh/layer intersection first,
        // then sample that world-space field through an isotropic XY Gaussian.
        // The former tangent-only walk discarded coherent detail approaching
        // the perimeter from another direction and varied with path/triangle
        // orientation.
        const double gaussian_strength = std::clamp(double(selected.sample.zone->gaussian_smoothing_strength), 0.0, 4.0);
        if (gaussian_strength <= EPSILON || weight_field.points.empty() || !point_mm.allFinite() || !outward.allFinite())
            return compact_exposure(std::move(*center));
        const double base_sigma_mm = std::max(0.04, 0.45 * double(sample_spacing_mm));
        const double sigma_mm      = base_sigma_mm * gaussian_strength;
        const double radius_mm =
            std::min(std::max(0.16, 1.75 * double(sample_spacing_mm)), 3.0 * base_sigma_mm) * gaussian_strength;
        const double radius_squared            = radius_mm * radius_mm;
        const double inverse_two_sigma_squared = 1.0 / std::max(2.0 * sigma_mm * sigma_mm, 1e-12);
        const int    min_bucket_x = int(std::floor((point_mm.x() - radius_mm) / weight_field.bucket_size_mm));
        const int    max_bucket_x = int(std::floor((point_mm.x() + radius_mm) / weight_field.bucket_size_mm));
        const int    min_bucket_y = int(std::floor((point_mm.y() - radius_mm) / weight_field.bucket_size_mm));
        const int    max_bucket_y = int(std::floor((point_mm.y() + radius_mm) / weight_field.bucket_size_mm));

        std::vector<double> accumulated(num_physical, 0.0);
        double              kernel_sum = 0.0;
        for (int bucket_y = min_bucket_y; bucket_y <= max_bucket_y; ++bucket_y) {
            for (int bucket_x = min_bucket_x; bucket_x <= max_bucket_x; ++bucket_x) {
                const auto bucket = weight_field.buckets.find(ComponentWeightField::bucket_key(bucket_x, bucket_y));
                if (bucket == weight_field.buckets.end())
                    continue;
                for (const uint32_t point_index : bucket->second) {
                    if (point_index >= weight_field.points.size())
                        continue;
                    const ComponentWeightFieldPoint& field_point = weight_field.points[point_index];
                    if (field_point.zone != selected.sample.zone || field_point.active_component != center->active_component ||
                        field_point.weights.size() != accumulated.size())
                        continue;
                    // Preserve direct sampling's wall-axis rejection. Absolute
                    // alignment tolerates mixed triangle winding while keeping
                    // a perpendicular face around a corner out of this field.
                    const double alignment = std::abs(outward.dot(field_point.outward));
                    if (!std::isfinite(alignment) || alignment < 0.25)
                        continue;
                    const double distance_squared = (point_mm - field_point.point_mm).squaredNorm();
                    if (!std::isfinite(distance_squared) || distance_squared > radius_squared)
                        continue;
                    const double kernel = std::exp(-distance_squared * inverse_two_sigma_squared) *
                                          field_point.integration_weight_mm;
                    if (!std::isfinite(kernel) || kernel <= EPSILON)
                        continue;
                    for (size_t component_index = 0; component_index < accumulated.size(); ++component_index)
                        accumulated[component_index] += kernel * field_point.weights[component_index];
                    kernel_sum += kernel;
                }
            }
        }

        if (kernel_sum <= EPSILON)
            return compact_exposure(std::move(*center));
        for (double& weight : accumulated)
            weight /= kernel_sum;
        center->weights = std::move(accumulated);
        return compact_exposure(std::move(*center));
    }

    BoundaryModulationResult modulate_slices(const ExPolygons&                     source,
                                             const Layer&                          layer,
                                             const std::vector<LayerPlaneSampler>& layer_samplers,
                                             const ComponentWeightField&            weight_field,
                                             bool&                                 mapped,
                                             bool&                                 requires_wide_carrier) const
    {
        const float path_displacement_range_mm = path_displacement(max_displacement_mm);
        const bool  width_only_modulation = path_displacement_range_mm <= EPSILON && line_width_fraction > EPSILON;
        BoundaryModulationOptions options;
        options.sample_spacing_mm       = sample_spacing_mm;
        // BoundaryModulation normally skips a zero-displacement request. A
        // width-only carrier still has to sample the boundary so its region is
        // marked for the maximum-width perimeter before per-segment narrowing.
        // Give that discovery pass a harmless non-zero range, then restore the
        // exact authored geometry below.
        options.max_abs_displacement_mm = width_only_modulation ? 0.001f : path_displacement_range_mm;
        // Keep the authored slice as the stable outer envelope. V2 expresses
        // weaker component visibility by recessing that layer into the model;
        // it must never push a strong component outside the source surface.
        options.center_displacement_on_boundary = false;

        // The queried points are on the pre-modulation slice boundary. A
        // modest tolerance covers XY and elephant-foot compensation without
        // allowing a distant parallel face to paint this wall.
        const double max_sample_distance_mm = std::clamp(std::max(0.45, double(max_displacement_mm) + 0.20), 0.45, 1.0);
        BoundaryModulationResult result =
            modulate_boundary(source, options,
                              [this, &layer, &layer_samplers, &weight_field, &mapped, &requires_wide_carrier,
                               max_sample_distance_mm](const Vec2d& point_mm,
                                                       const Vec2d& inward) -> std::optional<BoundaryDisplacement> {
                                     const Vec2d                         outward  = -inward;
                                     const std::optional<SelectedSample> selected = sample(layer_samplers,
                                                                                           point_mm,
                                                                                           outward,
                                                                                            max_sample_distance_mm);
                                     if (!selected || selected->sample.zone == nullptr)
                                         return std::nullopt;
                                     const std::optional<ComponentExposure> exposure =
                                         spatially_filtered_exposure(*selected, point_mm, outward, layer, weight_field);
                                     if (!exposure)
                                         return std::nullopt;
                                     mapped                                = true;
                                     requires_wide_carrier                 = true;
                                     const float first_smoothing_strength  = selected->sample.zone->disable_broad_path_smoothing ?
                                                                                 0.f :
                                                                                 selected->sample.zone->first_path_smoothing_strength;
                                     const float second_smoothing_strength = selected->sample.zone->disable_broad_path_smoothing ?
                                                                                 0.f :
                                                                                 selected->sample.zone->second_path_smoothing_strength;
                                     return BoundaryDisplacement{
                                         path_displacement(std::clamp(exposure->inset_mm, 0.f, max_displacement_mm)),
                                                                 std::max(selected->sample.zone->corner_smoothing_radius_mm,
                                                                          smoothing_base_mm),
                                                                 first_smoothing_strength, second_smoothing_strength};
                              });
        if (width_only_modulation) {
            result.geometry = source;
            result.changed  = false;
        }
        return result;
    }

    ExtrusionPaths modulate_path(const ExtrusionPath&                    path,
                                 const Layer&                            layer,
                                 const ExPolygons&                       layer_slices,
                                 const std::vector<LayerPlaneSampler>& layer_samplers,
                                 const ComponentWeightField&             weight_field,
                                 bool&                                   changed,
                                 std::optional<RenderMode>               required_mode,
                                 std::optional<unsigned int>             active_component_override,
                                 bool                                    apply_path_displacement,
                                 bool                                    use_carrier_width) const
    {
        changed = false;
        if (!is_external_perimeter(path.role()) || path.polyline.points.size() < 2 || layer_slices.empty())
            return {path};
        Points points = resample_polyline(path.polyline.points, sample_spacing_mm);
        if (points.size() < 2)
            return {path};
        const bool closed = points.front() == points.back();

        const double            max_sample_distance_mm   = std::clamp(0.5 * double(path.width) + 0.20, 0.45, 1.0);
        const double            max_boundary_distance_mm = std::max(1.0, double(smoothing_base_mm + max_displacement_mm));
        const double            max_boundary_distance    = scale_(max_boundary_distance_mm);
        const double            probe_scaled             = scale_(0.10);
        std::vector<PathSample> samples;
        samples.reserve(points.size());
        bool has_modulation = false;
        for (size_t index = 0; index < points.size(); ++index) {
            PathSample path_sample;
            path_sample.point = points[index];
            const Point boundary  = projection_onto(layer_slices, points[index]);
            path_sample.outward   = (boundary - points[index]).cast<double>();
            const double distance = path_sample.outward.norm();
            if (!std::isfinite(distance) || distance > max_boundary_distance) {
                samples.emplace_back(path_sample);
                continue;
            }
            if (distance <= EPSILON)
                path_sample.outward = fallback_outward_direction(points, index, layer_slices, probe_scaled);
            else
                path_sample.outward /= distance;
            if (!path_sample.outward.allFinite() || path_sample.outward.squaredNorm() <= EPSILON * EPSILON) {
                samples.emplace_back(path_sample);
                continue;
            }

            const Vec2d                         boundary_mm(unscale<double>(boundary.x()), unscale<double>(boundary.y()));
            const std::optional<SelectedSample> selected = sample(layer_samplers,
                                                                  boundary_mm,
                                                                  path_sample.outward,
                                                                  max_sample_distance_mm,
                                                                  required_mode);
            if (selected && selected->sample.zone != nullptr) {
                if (const std::optional<ComponentExposure> exposure = spatially_filtered_exposure(*selected,
                                                                                                  boundary_mm,
                                                                                                  path_sample.outward,
                                                                                                  layer,
                                                                                                  weight_field,
                                                                                                  active_component_override)) {
                    path_sample.width_delta_mm            = std::clamp(exposure->inset_mm, 0.f, max_displacement_mm);
                    path_sample.smoothing_radius_mm       = std::max(selected->sample.zone->corner_smoothing_radius_mm, smoothing_base_mm);
                    path_sample.first_smoothing_strength  = selected->sample.zone->disable_broad_path_smoothing ?
                                                                0.f :
                                                                selected->sample.zone->first_path_smoothing_strength;
                    path_sample.second_smoothing_strength = selected->sample.zone->disable_broad_path_smoothing ?
                                                                0.f :
                                                                selected->sample.zone->second_path_smoothing_strength;
                    path_sample.modulated                 = true;
                    has_modulation                        = true;
                }
            }
            samples.emplace_back(path_sample);
        }
        if (!has_modulation)
            return {path};

        smooth_path_width_deltas(samples, closed, 0.35f);

        const float source_width_mm   = std::max(0.01f, path.width);
        const float mapped_width_mm   = use_carrier_width ? std::max(source_width_mm, carrier_width_mm) : source_width_mm;
        const float layer_height_mm   = std::max(0.01f, path.height);
        const auto  cross_section_mm2 = [layer_height_mm](float width_mm) {
            return double(layer_height_mm) * double(std::max(0.001f, width_mm - layer_height_mm * float(1. - 0.25 * PI)));
        };
        const double       source_cross_section_mm2 = cross_section_mm2(source_width_mm);
        Points             shifted;
        std::vector<float> target_widths;
        shifted.reserve(samples.size());
        target_widths.reserve(samples.size());
        for (const PathSample& path_sample : samples) {
            Point quantized       = path_sample.point;
            float target_width_mm = source_width_mm;
            if (path_sample.modulated) {
                const float width_reduction_mm = std::min(line_width_reduction(path_sample.width_delta_mm),
                                                          std::max(0.f, mapped_width_mm - minimum_visibility_width_mm));
                target_width_mm = std::clamp(mapped_width_mm - width_reduction_mm, minimum_visibility_width_mm, mapped_width_mm);
                // Keep the inner edge anchored as the hybrid narrows the
                // carrier. Moving the centerline by half the width reduction
                // makes the outer edge retreat by the full reduction; the
                // geometry-first pass supplies the remainder. Arachne may
                // have regenerated a normal-width path after an adaptive
                // material split; half of the restored carrier delta is also
                // inset here so its outer edge remains on the authored shell.
                const float requested_path_displacement =
                    required_mode == RenderMode::AdaptiveLocalizedCycles ? path_sample.width_delta_mm :
                                                                           path_displacement(path_sample.width_delta_mm);
                const float centerline_inset_mm =
                    (apply_path_displacement ? requested_path_displacement : 0.f) +
                    0.5f * (mapped_width_mm - source_width_mm + width_reduction_mm);
                const Vec2d moved = path_sample.point.cast<double>() - path_sample.outward * scale_(double(centerline_inset_mm));
                quantized         = Point(coord_t(std::llround(moved.x())), coord_t(std::llround(moved.y())));
            }
            shifted.emplace_back(quantized);
            target_widths.emplace_back(target_width_mm);
            changed |= quantized != path_sample.point || std::abs(target_width_mm - source_width_mm) > 0.0005f;
        }
        if (closed && shifted.size() > 1)
            shifted.back() = shifted.front();
        repair_folded_corners(shifted, samples, closed, layer_slices, mapped_width_mm);
        trim_self_intersections(shifted, target_widths, closed, layer_slices);
        trim_folded_corners(shifted, target_widths, closed, layer_slices, mapped_width_mm);

        ExtrusionPaths out;
        out.reserve(shifted.size() - 1);
        for (size_t index = 0; index + 1 < shifted.size(); ++index) {
            if (shifted[index] == shifted[index + 1])
                continue;
            const float target_width_mm = 0.5f * (target_widths[index] + target_widths[index + 1]);
            if (!out.empty() && out.back().last_point() == shifted[index] && std::abs(out.back().width - target_width_mm) <= 0.002f) {
                out.back().polyline.points.emplace_back(shifted[index + 1]);
                continue;
            }

            ExtrusionPath segment(path);
            segment.polyline = Polyline{shifted[index], shifted[index + 1]};
            segment.width    = target_width_mm;
            if (path.mm3_per_mm > EPSILON && source_cross_section_mm2 > EPSILON)
                segment.mm3_per_mm = path.mm3_per_mm * cross_section_mm2(target_width_mm) / source_cross_section_mm2;
            out.emplace_back(std::move(segment));
        }
        if (out.empty()) {
            changed = false;
            return {path};
        }
        if (!changed)
            return {path};
        return out;
    }

    bool repair_path_join(ExtrusionPath& previous,
                          ExtrusionPath& next,
                          const Point& source_joint,
                          const ExPolygons& layer_slices,
                          float join_width_mm) const
    {
        if (previous.polyline.points.size() < 2 || next.polyline.points.size() < 2)
            return false;

        const Point& previous_before = previous.polyline.points[previous.polyline.points.size() - 2];
        const Point& previous_end    = previous.polyline.points.back();
        const Point& next_start      = next.polyline.points.front();
        const Point& next_after      = next.polyline.points[1];
        if (previous_end == next_start)
            return false;

        const Vec2d  p        = previous_before.cast<double>();
        const Vec2d  r        = (previous_end - previous_before).cast<double>();
        const Vec2d  q        = next_start.cast<double>();
        const Vec2d  s        = (next_after - next_start).cast<double>();
        const double r_length = r.norm();
        const double s_length = s.norm();
        if (!std::isfinite(r_length) || !std::isfinite(s_length) || r_length <= EPSILON || s_length <= EPSILON)
            return false;

        const double max_join_distance = scale_(std::max(0.10f, 2.f * join_width_mm));
        auto         candidate_is_safe = [&](const Vec2d& candidate) {
            if (!candidate.allFinite())
                return false;
            const Point quantized(coord_t(std::llround(candidate.x())), coord_t(std::llround(candidate.y())));
            if (!layer_slices_contain(layer_slices, quantized))
                return false;
            if ((candidate - source_joint.cast<double>()).norm() > max_join_distance ||
                (candidate - previous_end.cast<double>()).norm() > max_join_distance ||
                (candidate - next_start.cast<double>()).norm() > max_join_distance)
                return false;

            // A join must stay ahead of the incoming segment and behind the
            // outgoing segment. This rejects the line intersections which
            // turn an acute corner into a folded-back spike.
            const double minimum_progress = scale_(0.001);
            return (candidate - p).dot(r / r_length) > minimum_progress &&
                   (next_after.cast<double>() - candidate).dot(s / s_length) > minimum_progress;
        };

        std::optional<Vec2d> joined;
        const double         denominator = cross_2d(r, s);
        if (std::abs(denominator) > 1e-9) {
            const Vec2d  delta        = q - p;
            const double parameter_t  = cross_2d(delta, s) / denominator;
            const double parameter_u  = cross_2d(delta, r) / denominator;
            const Vec2d  intersection = p + parameter_t * r;
            // A valid inward corner trims the end of the incoming span and
            // the beginning of the outgoing span. Intersections which extend
            // the incoming span past its end or the outgoing span backwards
            // are the triangular edge loops seen in the preview.
            constexpr double endpoint_epsilon = 1e-6;
            if (parameter_t > endpoint_epsilon && parameter_t <= 1. + endpoint_epsilon && parameter_u > endpoint_epsilon &&
                parameter_u < 1. - endpoint_epsilon && candidate_is_safe(intersection))
                joined = intersection;
        }

        // Collinear transitions and rejected acute miters get a bounded
        // bevel. If even that would leave the material, fall back to the
        // original printable centerline joint; it is always preferable to a
        // gap or a self-intersection.
        if (!joined) {
            const Vec2d midpoint = 0.5 * (previous_end.cast<double>() + next_start.cast<double>());
            if (candidate_is_safe(midpoint))
                joined = midpoint;
        }
        if (!joined && candidate_is_safe(source_joint.cast<double>()))
            joined = source_joint.cast<double>();
        if (!joined)
            return false;

        const Point quantized(coord_t(std::llround(joined->x())), coord_t(std::llround(joined->y())));
        previous.polyline.points.back() = quantized;
        next.polyline.points.front()    = quantized;
        return true;
    }

    bool modulate_paths(ExtrusionPaths&                              paths,
                        const Layer&                                 layer,
                        const ExPolygons&                            layer_slices,
                        const std::vector<LayerPlaneSampler>& layer_samplers,
                        const ComponentWeightField&                  weight_field,
                        std::optional<RenderMode>                    required_mode,
                        std::optional<unsigned int>                  active_component_override,
                        bool                                         apply_path_displacement,
                        bool                                         use_carrier_width,
                        bool                                         closed = false) const
    {
        struct PendingJoin
        {
            size_t previous_path_index{0};
            size_t next_path_index{0};
            Point  source_joint;
        };

        bool           changed = false;
        ExtrusionPaths source  = std::move(paths);
        float          join_width_mm = use_carrier_width ? carrier_width_mm : 0.f;
        for (const ExtrusionPath& path : source)
            join_width_mm = std::max(join_width_mm, path.width);
        ExtrusionPaths output;
        output.reserve(source.size());
        std::vector<PendingJoin> joins;
        joins.reserve(source.size());
        for (size_t source_index = 0; source_index < source.size(); ++source_index) {
            const ExtrusionPath& path         = source[source_index];
            const size_t         output_begin = output.size();
            bool                 path_changed = false;
            ExtrusionPaths modulated = modulate_path(path,
                                                     layer,
                                                     layer_slices,
                                                     layer_samplers,
                                                     weight_field,
                                                     path_changed,
                                                     required_mode,
                                                     active_component_override,
                                                     apply_path_displacement,
                                                     use_carrier_width);
            changed |= path_changed;
            output.insert(output.end(), std::make_move_iterator(modulated.begin()), std::make_move_iterator(modulated.end()));
            if (source_index > 0 && output_begin < output.size() && source[source_index - 1].last_point() == path.first_point())
                joins.push_back({output_begin - 1, output_begin, path.first_point()});
        }

        if (closed && source.size() > 1 && output.size() > 1 && source.back().last_point() == source.front().first_point())
            joins.push_back({output.size() - 1, 0, source.front().first_point()});

        for (const PendingJoin& join : joins)
            changed |= repair_path_join(output[join.previous_path_index],
                                        output[join.next_path_index],
                                        join.source_joint,
                                        layer_slices,
                                        join_width_mm);
        paths = std::move(output);
        return changed;
    }

    bool modulate_entity(ExtrusionEntity&                           entity,
                         const Layer&                               layer,
                         const ExPolygons&                          layer_slices,
                         const std::vector<LayerPlaneSampler>& layer_samplers,
                         const ComponentWeightField&                weight_field,
                         std::optional<RenderMode>                  required_mode,
                         std::optional<unsigned int>                active_component_override,
                         bool                                       apply_path_displacement,
                         bool                                       use_carrier_width) const
    {
        bool changed = false;
        if (auto* multipath = dynamic_cast<ExtrusionMultiPath*>(&entity)) {
            changed |= modulate_paths(multipath->paths,
                                      layer,
                                      layer_slices,
                                      layer_samplers,
                                      weight_field,
                                      required_mode,
                                      active_component_override,
                                      apply_path_displacement,
                                      use_carrier_width);
        } else if (auto* loop = dynamic_cast<ExtrusionLoop*>(&entity)) {
            changed |= modulate_paths(loop->paths,
                                      layer,
                                      layer_slices,
                                      layer_samplers,
                                      weight_field,
                                      required_mode,
                                      active_component_override,
                                      apply_path_displacement,
                                      use_carrier_width,
                                      true);
        } else if (auto* collection = dynamic_cast<ExtrusionEntityCollection*>(&entity)) {
            for (ExtrusionEntity*& child : collection->entities) {
                if (auto* path = dynamic_cast<ExtrusionPath*>(child)) {
                    bool           path_changed = false;
                    ExtrusionPaths modulated    = modulate_path(*path,
                                                                layer,
                                                                layer_slices,
                                                                layer_samplers,
                                                                weight_field,
                                                                path_changed,
                                                                required_mode,
                                                                active_component_override,
                                                                apply_path_displacement,
                                                                use_carrier_width);
                    if (path_changed) {
                        auto* replacement      = new ExtrusionMultiPath();
                        replacement->paths     = std::move(modulated);
                        replacement->inset_idx = path->inset_idx;
                        if (!path->can_reverse())
                            replacement->set_reverse();
                        delete child;
                        child   = replacement;
                        changed = true;
                    }
                } else if (child != nullptr) {
                    changed |= modulate_entity(*child,
                                               layer,
                                               layer_slices,
                                               layer_samplers,
                                               weight_field,
                                               required_mode,
                                               active_component_override,
                                               apply_path_displacement,
                                               use_carrier_width);
                }
            }
        } else if (auto* path = dynamic_cast<ExtrusionPath*>(&entity)) {
            bool           path_changed = false;
            ExtrusionPaths modulated    = modulate_path(*path,
                                                        layer,
                                                        layer_slices,
                                                        layer_samplers,
                                                        weight_field,
                                                        path_changed,
                                                        required_mode,
                                                        active_component_override,
                                                        apply_path_displacement,
                                                        use_carrier_width);
            if (path_changed && modulated.size() == 1)
                *path = std::move(modulated.front());
            changed = path_changed;
        }
        return changed;
    }

    struct AdaptiveSegment
    {
        const ExtrusionPath* source{nullptr};
        Point                a;
        Point                b;
        CadenceRef           cadence;
        unsigned int         component{0};
        double               length_mm{0.};
    };

    std::optional<CadenceRef> adaptive_cadence_for_segment(const Point&                          a,
                                                           const Point&                          b,
                                                           const ExPolygons&                     layer_slices,
                                                           const std::vector<LayerPlaneSampler>& layer_samplers,
                                                           double                                max_sample_distance_mm) const
    {
        const Vec2d tangent = (b - a).cast<double>();
        const double tangent_length = tangent.norm();
        if (!std::isfinite(tangent_length) || tangent_length <= EPSILON)
            return std::nullopt;

        const Vec2d midpoint = 0.5 * (a.cast<double>() + b.cast<double>());
        const Point midpoint_point(coord_t(std::llround(midpoint.x())), coord_t(std::llround(midpoint.y())));
        const Point boundary = projection_onto(layer_slices, midpoint_point);
        Vec2d       outward  = (boundary - midpoint_point).cast<double>();
        const double boundary_distance = outward.norm();
        if (std::isfinite(boundary_distance) && boundary_distance > EPSILON) {
            outward /= boundary_distance;
        } else {
            const Vec2d direction = tangent / tangent_length;
            const Vec2d normal(-direction.y(), direction.x());
            const double probe_scaled = scale_(0.10);
            auto probe = [&boundary, probe_scaled](const Vec2d& probe_direction) {
                const Vec2d shifted = boundary.cast<double>() + probe_direction * probe_scaled;
                return Point(coord_t(std::llround(shifted.x())), coord_t(std::llround(shifted.y())));
            };
            const bool positive_inside = layer_slices_contain(layer_slices, probe(normal));
            const bool negative_inside = layer_slices_contain(layer_slices, probe(-normal));
            outward = positive_inside != negative_inside ? (positive_inside ? -normal : normal) : normal;
        }
        if (!outward.allFinite() || outward.squaredNorm() <= EPSILON * EPSILON)
            return std::nullopt;

        const Vec2d boundary_mm(unscale<double>(boundary.x()), unscale<double>(boundary.y()));
        const std::optional<SelectedSample> selected =
            sample(layer_samplers, boundary_mm, outward, max_sample_distance_mm, RenderMode::AdaptiveLocalizedCycles);
        if (!selected)
            return std::nullopt;
        CadenceRef cadence = cadence_for_sample(selected->sample);
        if (!cadence)
            return std::nullopt;
        return cadence;
    }

    void stabilize_adaptive_runs(std::vector<AdaptiveSegment>& segments, bool closed) const
    {
        if (segments.size() < 2)
            return;

        struct Run
        {
            size_t              begin{0};
            size_t              end{0};
            const LayerCadence* cadence{nullptr};
            double              length_mm{0.};
        };
        const double minimum_run_mm = std::max(double(carrier_width_mm), 3. * double(sample_spacing_mm));
        auto build_runs = [&segments]() {
            std::vector<Run> runs;
            size_t begin = 0;
            while (begin < segments.size()) {
                size_t end = begin + 1;
                double length_mm = segments[begin].length_mm;
                while (end < segments.size() && segments[end].cadence.cadence == segments[begin].cadence.cadence) {
                    length_mm += segments[end].length_mm;
                    ++end;
                }
                runs.push_back({begin, end, segments[begin].cadence.cadence, length_mm});
                begin = end;
            }
            return runs;
        };
        auto replace_run = [&segments](const Run& run, const CadenceRef& cadence) {
            for (size_t index = run.begin; index < run.end; ++index)
                segments[index].cadence = cadence;
        };
        auto cadence_for_run = [&segments](const Run& run) { return segments[run.begin].cadence; };

        for (size_t pass = 0; pass < segments.size(); ++pass) {
            const std::vector<Run> runs = build_runs();
            if (runs.size() < 2)
                break;

            bool merged = false;
            if (closed && runs.front().cadence == runs.back().cadence &&
                runs.front().length_mm + runs.back().length_mm < minimum_run_mm) {
                const Run& after  = runs[1];
                const Run& before = runs[runs.size() - 2];
                if (runs.front().cadence == nullptr && after.cadence != before.cadence)
                    break;
                const CadenceRef replacement = after.cadence == before.cadence ? cadence_for_run(after) :
                                                   (after.length_mm >= before.length_mm ? cadence_for_run(after) : cadence_for_run(before));
                replace_run(runs.front(), replacement);
                replace_run(runs.back(), replacement);
                continue;
            }

            for (size_t run_index = 0; run_index < runs.size(); ++run_index) {
                const Run& run = runs[run_index];
                const bool wraps = closed && runs.front().cadence == runs.back().cadence &&
                                   (run_index == 0 || run_index + 1 == runs.size());
                if (wraps || run.length_mm >= minimum_run_mm)
                    continue;

                const Run* before = run_index > 0 ? &runs[run_index - 1] : (closed ? &runs.back() : nullptr);
                const Run* after  = run_index + 1 < runs.size() ? &runs[run_index + 1] : (closed ? &runs.front() : nullptr);
                if (before == nullptr && after == nullptr)
                    continue;
                if (run.cadence == nullptr && before != nullptr && after != nullptr && before->cadence != after->cadence)
                    continue;
                const CadenceRef replacement = before == nullptr ? cadence_for_run(*after) :
                                                   after == nullptr ? cadence_for_run(*before) :
                                                   before->cadence == after->cadence ? cadence_for_run(*before) :
                                                   (before->length_mm >= after->length_mm ? cadence_for_run(*before) :
                                                                                          cadence_for_run(*after));
                replace_run(run, replacement);
                merged = true;
                break;
            }
            if (!merged)
                break;
        }
    }

    bool route_adaptive_paths(const ExtrusionPaths&                   paths,
                              bool                                    closed,
                              unsigned int                            source_component,
                              const Layer&                            layer,
                              const ExPolygons&                       layer_slices,
                              const std::vector<LayerPlaneSampler>&   layer_samplers,
                              std::vector<ExtrusionEntitiesPtr>&      assigned) const
    {
        std::vector<AdaptiveSegment> segments;
        for (const ExtrusionPath& path : paths) {
            if (!is_external_perimeter(path.role()) || path.polyline.points.size() < 2) {
                assigned[source_component].emplace_back(path.clone());
                continue;
            }
            const Points points = resample_polyline(path.polyline.points, sample_spacing_mm);
            const double max_sample_distance_mm = std::clamp(0.5 * double(path.width) + 0.20, 0.45, 1.0);
            for (size_t point_index = 0; point_index + 1 < points.size(); ++point_index) {
                if (points[point_index] == points[point_index + 1])
                    continue;
                CadenceRef cadence;
                if (std::optional<CadenceRef> selected = adaptive_cadence_for_segment(points[point_index],
                                                                                       points[point_index + 1],
                                                                                       layer_slices,
                                                                                       layer_samplers,
                                                                                       max_sample_distance_mm))
                    cadence = std::move(*selected);
                segments.push_back({&path,
                                    points[point_index],
                                    points[point_index + 1],
                                    std::move(cadence),
                                    source_component,
                                    unscale<double>((points[point_index + 1] - points[point_index]).cast<double>().norm())});
            }
        }
        if (segments.empty())
            return false;

        stabilize_adaptive_runs(segments, closed);
        for (AdaptiveSegment& segment : segments) {
            if (!segment.cadence)
                continue;
            const unsigned int component = active_component(*segment.cadence.cadence, layer);
            if (component >= 1 && component <= num_physical)
                segment.component = component;
        }
        bool changed = false;
        size_t begin = 0;
        while (begin < segments.size()) {
            size_t end = begin + 1;
            while (end < segments.size() && segments[end].component == segments[begin].component &&
                   segments[end].source == segments[begin].source && segments[end - 1].b == segments[end].a)
                ++end;

            ExtrusionPath run(*segments[begin].source);
            run.polyline.points.clear();
            run.polyline.points.reserve(end - begin + 1);
            run.polyline.points.emplace_back(segments[begin].a);
            for (size_t segment_index = begin; segment_index < end; ++segment_index)
                run.polyline.points.emplace_back(segments[segment_index].b);
            assigned[segments[begin].component].emplace_back(new ExtrusionPath(std::move(run)));
            changed |= segments[begin].component != source_component;
            begin = end;
        }
        return changed;
    }

    bool route_adaptive_entity(const ExtrusionEntity&                    entity,
                               unsigned int                              source_component,
                               const Layer&                              layer,
                               const ExPolygons&                         layer_slices,
                               const std::vector<LayerPlaneSampler>&     layer_samplers,
                               std::vector<ExtrusionEntitiesPtr>&        assigned) const
    {
        if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
            bool changed = false;
            for (const ExtrusionEntity* child : collection->entities)
                if (child != nullptr)
                    changed |= route_adaptive_entity(*child, source_component, layer, layer_slices, layer_samplers, assigned);
            return changed;
        }

        if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
            if (!std::all_of(loop->paths.begin(), loop->paths.end(), [](const ExtrusionPath& path) {
                    return is_external_perimeter(path.role());
                })) {
                assigned[source_component].emplace_back(entity.clone());
                return false;
            }
            return route_adaptive_paths(loop->paths, true, source_component, layer, layer_slices, layer_samplers, assigned);
        }

        if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
            bool all_external = !multipath->paths.empty() &&
                                std::all_of(multipath->paths.begin(), multipath->paths.end(), [](const ExtrusionPath& path) {
                                    return is_external_perimeter(path.role());
                                });
            if (all_external) {
                return route_adaptive_paths(multipath->paths, false, source_component, layer, layer_slices, layer_samplers, assigned);
            }

            bool changed = false;
            for (const ExtrusionPath& path : multipath->paths) {
                if (is_external_perimeter(path.role())) {
                    changed |= route_adaptive_paths(ExtrusionPaths{path},
                                                    path.polyline.is_closed(),
                                                    source_component,
                                                    layer,
                                                    layer_slices,
                                                    layer_samplers,
                                                    assigned);
                } else {
                    assigned[source_component].emplace_back(path.clone());
                }
            }
            return changed;
        }

        if (const auto* path = dynamic_cast<const ExtrusionPath*>(&entity)) {
            if (!is_external_perimeter(path->role())) {
                assigned[source_component].emplace_back(entity.clone());
                return false;
            }
            return route_adaptive_paths(ExtrusionPaths{*path},
                                        path->polyline.is_closed(),
                                        source_component,
                                        layer,
                                        layer_slices,
                                        layer_samplers,
                                        assigned);
        }

        assigned[source_component].emplace_back(entity.clone());
        return false;
    }
};

bool model_has_perimeter_modulation(const ModelObject& model_object)
{
    for (const ModelVolume* volume : model_object.volumes) {
        if (volume == nullptr || !volume->is_model_part())
            continue;
        const std::shared_ptr<const VolumeData> data = volume->image_map_data();
        if (data && data_has_perimeter_modulation(*data))
            return true;
    }
    return false;
}

bool model_uses_perimeter_modulation_filament(const ModelObject& model_object,
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
            if (!zone.enabled || !zone_uses_perimeter_modulation(zone))
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

std::optional<unsigned int> model_whole_object_cadence_filament(const ModelObject&          model_object,
                                                                const MixedFilamentManager& manager,
                                                                size_t                      num_physical,
                                                                int                         layer_index,
                                                                float                       layer_print_z,
                                                                float                       layer_height)
{
    if (num_physical == 0)
        return std::nullopt;

    const size_t                num_total = manager.total_filaments(num_physical);
    std::optional<unsigned int> selected_physical;
    int                         selected_priority = std::numeric_limits<int>::lowest();
    for (const ModelVolume* volume : model_object.volumes) {
        if (volume == nullptr || !volume->is_model_part())
            continue;
        const std::shared_ptr<const VolumeData> data = volume->image_map_data();
        if (!data)
            continue;
        for (size_t zone_index = 0; zone_index < data->zones.size(); ++zone_index) {
            const Zone& zone = data->zones[zone_index];
            if (!zone.enabled || zone.render_mode != RenderMode::PerimeterModulationV2 ||
                !zone.synchronize_whole_object_cadence || zone.palette.empty() || zone.priority <= selected_priority)
                continue;
            const bool zone_is_bound = std::any_of(data->triangle_bindings.begin(), data->triangle_bindings.end(),
                                                   [zone_index](const TriangleBinding& binding) {
                                                       return binding.zone_index == zone_index;
                                                   });
            if (!zone_is_bound)
                continue;

            const unsigned int filament_id = resolve_palette_filament(zone.palette.front(), manager, num_physical, num_total);
            const std::optional<MixedFilamentDefinition> definition =
                manager.mixed_filament_definition_from_id(filament_id, num_physical);
            if (!definition || !definition->behavior.surface_bias.perimeter_modulation ||
                definition->recipe.blend.component_ids(num_physical).size() < 2)
                continue;
            const unsigned int physical = manager.resolve(filament_id, num_physical, layer_index, layer_print_z, layer_height);
            if (physical < 1 || physical > num_physical)
                continue;
            selected_physical = physical;
            selected_priority = zone.priority;
        }
    }
    return selected_physical;
}

std::unique_ptr<PerimeterEnvelopeRenderer> PerimeterEnvelopeRenderer::create(const PrintObject& print_object)
{
    const Print*       print        = print_object.print();
    const ModelObject* model_object = print_object.model_object();
    if (print == nullptr || model_object == nullptr || !print->config().mixed_filament_component_bias_enabled.value ||
        print->config().texture_mapping_outer_wall_gradient_global_strength.value <= EPSILON ||
        !model_has_perimeter_modulation(*model_object))
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
    const float maximum_width = std::max(0.05f, float(print->config().texture_mapping_outer_wall_gradient_max_line_width.value));
    const float minimum_width = std::clamp(float(print->config().texture_mapping_outer_wall_gradient_min_line_width.value), 0.05f,
                                           maximum_width);
    const float strength = std::clamp(float(print->config().texture_mapping_outer_wall_gradient_global_strength.value) / 100.f, 0.f, 1.f);
    impl->max_displacement_mm                             = std::min((maximum_width - minimum_width) * strength,
                                                                     2.f * MixedFilamentManager::max_component_surface_offset_mm(reference_nozzle_mm));
    impl->minimum_visibility_width_mm                     = minimum_width;
    const ImageMapPerimeterModulationMode modulation_mode = print->config().image_map_perimeter_modulation_mode.value;
    if (modulation_mode == ImageMapPerimeterModulationMode::ReferenceWidePath) {
        impl->carrier_width_mm    = maximum_width;
        impl->line_width_fraction = 0.f;
    } else if (modulation_mode == ImageMapPerimeterModulationMode::ImageControlledWidth) {
        // Generate enough room for the configured maximum bead, then express
        // the complete exposure range through per-segment width. The
        // centerline correction in modulate_path() holds the bead's inner edge
        // fixed, so only image zones requesting maximum coverage reach the
        // maximum width.
        impl->carrier_width_mm    = maximum_width;
        impl->line_width_fraction = 1.f;
    } else {
        impl->carrier_width_mm = std::clamp(float(print->config().image_map_perimeter_printable_width.value), minimum_width, maximum_width);
        if (modulation_mode == ImageMapPerimeterModulationMode::HybridPathWidth && impl->max_displacement_mm > EPSILON) {
            const float printable_width_range = std::max(0.f, impl->carrier_width_mm - minimum_width);
            impl->line_width_fraction         = std::clamp(printable_width_range / impl->max_displacement_mm, 0.f, 1.f);
        }
    }
    impl->smoothing_base_mm = impl->carrier_width_mm;

    std::vector<ContinuousColorComponent> solver_components;
    solver_components.reserve(impl->num_physical);
    for (size_t component_index = 0; component_index < impl->num_physical; ++component_index) {
        ContinuousColorComponent component;
        component.color_hex = print->config().filament_colour.values[component_index];
        if (MixedFilamentManager::use_td_for_color_prediction() &&
            component_index < print->config().filament_transmission_distance.values.size() &&
            print->config().filament_transmission_distance.values[component_index] > EPSILON)
            component.transmission_distance_mm = print->config().filament_transmission_distance.values[component_index];
        if (component_index < print->config().filament_full_spectrum_material_id.values.size() &&
            !print->config().filament_full_spectrum_material_id.values[component_index].empty())
            component.material_id = print->config().filament_full_spectrum_material_id.values[component_index];
        solver_components.emplace_back(std::move(component));
    }
    impl->solver_components = solver_components;

    const Transform3d object_to_print = print_object.trafo_centered();
    for (const ModelVolume* volume : model_object->volumes) {
        if (volume == nullptr || !volume->is_model_part())
            continue;
        const std::shared_ptr<const VolumeData> source_data = volume->image_map_data();
        if (!source_data || !data_has_perimeter_modulation(*source_data) || !source_data->validate(volume->mesh()).valid)
            continue;
        auto filtered_data = std::make_shared<VolumeData>(*source_data);
        for (Zone &zone : filtered_data->zones)
            if (!zone_uses_perimeter_modulation(zone))
                zone.enabled = false;
        const std::shared_ptr<const VolumeData> data = std::move(filtered_data);
        for (const Zone& zone : data->zones) {
            if (zone.enabled && zone_uses_perimeter_modulation(zone)) {
                impl->ensure_solvers(zone.color_mix_model);
                impl->sample_spacing_mm = std::min(impl->sample_spacing_mm, std::clamp(zone.modulation_sample_spacing_mm, 0.02f, 2.f));
                auto cadence_for_entry  = [&impl, &solver_components, &zone](
                                              const PaletteEntry& entry,
                                              bool shared_sequence) -> std::optional<Impl::LayerCadence> {
                    const unsigned int filament_id = resolve_palette_filament(entry, *impl->manager, impl->num_physical, impl->num_total);
                    std::vector<int> component_percents(impl->num_physical, 0);
                    std::vector<unsigned int> layer_sequence;
                    if (shared_sequence) {
                        const std::optional<MixedFilamentDefinition> definition =
                            impl->manager->mixed_filament_definition_from_id(filament_id, impl->num_physical);
                        if (!definition || !definition->behavior.surface_bias.perimeter_modulation)
                            return std::nullopt;
                        for (const MixedFilamentWeightedComponent& component : definition->recipe.blend.components) {
                            if (component.filament.id >= 1 && component.filament.id <= impl->num_physical)
                                component_percents[component.filament.id - 1] += std::max(0, component.percent);
                        }
                    } else {
                        ContinuousColorRecipe recipe;
                        if (filament_id >= 1 && filament_id <= impl->num_physical) {
                            recipe.component_indices  = {size_t(filament_id - 1)};
                            recipe.component_percents = {100};
                            recipe.layer_sequence     = recipe.component_indices;
                        } else if (const std::optional<MixedFilamentDefinition> definition =
                                       impl->manager->mixed_filament_definition_from_id(filament_id, impl->num_physical);
                                   definition && definition->behavior.surface_bias.perimeter_modulation &&
                                   !definition->recipe.manual_pattern) {
                            for (const MixedFilamentWeightedComponent& component : definition->recipe.blend.components) {
                                if (component.filament.id < 1 || component.filament.id > impl->num_physical || component.percent <= 0)
                                    continue;
                                const size_t physical_index = size_t(component.filament.id - 1);
                                if (std::find(recipe.component_indices.begin(), recipe.component_indices.end(), physical_index) !=
                                    recipe.component_indices.end())
                                    continue;
                                recipe.component_indices.emplace_back(physical_index);
                                recipe.component_percents.emplace_back(component.percent);
                            }
                            if (recipe.component_indices.size() <= 4)
                                recipe.layer_sequence = recipe.component_indices;
                        }
                        if (!recipe.valid()) {
                            const ContinuousColorRecipeSolver* recipe_solver =
                                impl->adaptive_recipe_solver_for(zone.color_mix_model);
                            if (recipe_solver != nullptr)
                                recipe = recipe_solver->solve(entry.target_color, zone.minimum_component_percent);
                        }
                        if (!recipe.valid())
                            return std::nullopt;
                        for (size_t recipe_index = 0; recipe_index < recipe.component_indices.size(); ++recipe_index) {
                            const size_t physical_index = recipe.component_indices[recipe_index];
                            if (physical_index < component_percents.size())
                                component_percents[physical_index] = recipe.component_percents[recipe_index];
                        }
                        layer_sequence.reserve(recipe.layer_sequence.size());
                        for (const size_t physical_index : recipe.layer_sequence)
                            if (physical_index < impl->num_physical)
                                layer_sequence.emplace_back(unsigned(physical_index + 1));
                    }
                    const bool   has_components         = std::any_of(component_percents.begin(), component_percents.end(),
                                                                       [](int percent) { return percent > 0; });
                    const bool   has_all_components     = std::all_of(component_percents.begin(), component_percents.end(),
                                                                       [](int percent) { return percent > 0; });
                    const size_t active_component_count = size_t(
                        std::count_if(component_percents.begin(), component_percents.end(), [](int percent) { return percent > 0; }));
                    if (!has_components || (shared_sequence && active_component_count < 2))
                        return std::nullopt;

                    Impl::LayerCadence cadence;
                    cadence.filament_id        = filament_id;
                    cadence.component_percents = std::move(component_percents);
                    cadence.layer_sequence     = std::move(layer_sequence);
                    if (!shared_sequence && active_component_count == 1) {
                        const auto active = std::find_if(cadence.component_percents.begin(), cadence.component_percents.end(),
                                                         [](int percent) { return percent > 0; });
                        cadence.direct_component_id = unsigned(std::distance(cadence.component_percents.begin(), active) + 1);
                        return cadence;
                    }
                    if (!has_all_components) {
                        std::string mask(1, char('0' + Impl::color_model_index(zone.color_mix_model)));
                        mask.append(impl->num_physical, '0');
                        std::vector<ContinuousColorComponent> restricted_components;
                        for (size_t component_idx = 0; component_idx < cadence.component_percents.size(); ++component_idx) {
                            if (cadence.component_percents[component_idx] <= 0 || component_idx >= solver_components.size())
                                continue;
                            mask[component_idx + 1] = '1';
                            cadence.solver_to_physical.emplace_back(component_idx);
                            restricted_components.emplace_back(solver_components[component_idx]);
                        }
                        if (restricted_components.size() < 2)
                            return std::nullopt;
                        const auto cached = impl->restricted_solvers.find(mask);
                        if (cached != impl->restricted_solvers.end()) {
                            cadence.restricted_solver = cached->second;
                        } else {
                            auto restricted_solver =
                                std::make_shared<ContinuousColorSolver>(std::move(restricted_components), zone.color_mix_model);
                            if (!restricted_solver->valid())
                                return std::nullopt;
                            cadence.restricted_solver = restricted_solver;
                            impl->restricted_solvers.emplace(std::move(mask), std::move(restricted_solver));
                        }
                    }
                    return cadence;
                };

                if (zone.render_mode == RenderMode::PerimeterModulationV2) {
                    for (const PaletteEntry& entry : zone.palette) {
                        if (std::optional<Impl::LayerCadence> cadence = cadence_for_entry(entry, true)) {
                            impl->shared_cadences.emplace(&zone, std::move(*cadence));
                            break;
                        }
                    }
                } else {
                    impl->adaptive_material_islands = true;
                    for (const PaletteEntry& entry : zone.palette) {
                        if (std::optional<Impl::LayerCadence> cadence = cadence_for_entry(entry, false)) {
                            if (cadence->filament_id > impl->num_physical && cadence->filament_id <= impl->num_total)
                                impl->adaptive_island_filament_ids.emplace(cadence->filament_id);
                            impl->adaptive_cadences.emplace(&entry, std::move(*cadence));
                        }
                    }
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

bool PerimeterEnvelopeRenderer::apply_to_slices(Layer& layer) const
{
    if (!m_impl || layer.regions().empty())
        return false;

    std::vector<LayerPlaneSampler> layer_samplers;
    layer_samplers.reserve(m_impl->volumes.size());
    for (const VolumeSampler& volume : m_impl->volumes)
        layer_samplers.emplace_back(volume.mesh, volume.data, volume.local_to_print, double(layer.slice_z));
    const Impl::ComponentWeightField weight_field = m_impl->build_component_weight_field(layer, layer_samplers);

    if (m_impl->adaptive_material_islands) {
        struct RegionGeometry
        {
            LayerRegion* region{nullptr};
            ExPolygons   geometry;
        };
        std::vector<RegionGeometry> region_geometries;
        ExPolygons                 complete_envelope;
        region_geometries.reserve(layer.regions().size());
        for (LayerRegion* region : layer.regions()) {
            if (region == nullptr)
                continue;
            region->image_map_external_perimeter_width_mm = 0.f;
            region->image_map_adaptive_perimeter_island   = false;
            region->image_map_perimeter_fallback_slices.clear();
            region->image_map_has_perimeter_fallback_slices = false;
            if (region->image_map_unmodulated_raw_slices.empty()) {
                if (!region->raw_slices.empty())
                    region->image_map_unmodulated_raw_slices = region->raw_slices;
                else
                    region->image_map_unmodulated_raw_slices = to_expolygons(region->slices.surfaces);
            }
            ExPolygons source = region->image_map_unmodulated_raw_slices;
            if (source.empty())
                continue;
            append(complete_envelope, source);
            region_geometries.push_back({region, std::move(source)});
        }
        complete_envelope = union_ex(std::move(complete_envelope));
        if (!complete_envelope.empty()) {
            bool                           mapped                = false;
            bool                           requires_wide_carrier = false;
            const BoundaryModulationResult result =
                m_impl->modulate_slices(complete_envelope, layer, layer_samplers, weight_field, mapped, requires_wide_carrier);
            const ExPolygons &modulated_envelope = result.geometry.empty() ? complete_envelope : result.geometry;

            // Adaptive tool ownership was already segmented into mutually
            // exclusive closed regions. Modulate their shared exterior once,
            // then clip every region by that same envelope. Independently
            // displacing each region would move both sides of an internal
            // material interface and create either overlap or a gap.
            for (RegionGeometry &item : region_geometries) {
                ExPolygons clipped = intersection_ex(item.geometry, modulated_envelope, ApplySafetyOffset::No);
                item.region->slices.set(clipped, stInternal);
                item.region->raw_slices = std::move(clipped);
                const unsigned int filament_id = unsigned(std::max(0, item.region->region().config().wall_filament.value));
                if (m_impl->adaptive_island_filament_ids.count(filament_id) != 0) {
                    item.region->image_map_external_perimeter_width_mm = m_impl->carrier_width_mm;
                    item.region->image_map_adaptive_perimeter_island   = true;
                }
            }
            if (mapped) {
                layer.make_slices();
                layer.lslices_bboxes.clear();
                layer.lslices_bboxes.reserve(layer.lslices.size());
                for (const ExPolygon& expolygon : layer.lslices)
                    layer.lslices_bboxes.emplace_back(get_extents(expolygon));
            }
            return result.changed || mapped;
        }
    }

    bool any_mapping = false;
    bool changed     = false;
    for (LayerRegion* region : layer.regions()) {
        if (region == nullptr)
            continue;

        region->image_map_external_perimeter_width_mm = 0.f;
        region->image_map_adaptive_perimeter_island   = false;
        region->image_map_perimeter_fallback_slices.clear();
        region->image_map_has_perimeter_fallback_slices = false;
        if (region->image_map_unmodulated_raw_slices.empty()) {
            if (!region->raw_slices.empty())
                region->image_map_unmodulated_raw_slices = region->raw_slices;
            else
                region->image_map_unmodulated_raw_slices = to_expolygons(region->slices.surfaces);
        }
        if (!region->image_map_unmodulated_raw_slices.empty())
            region->slices.set(region->image_map_unmodulated_raw_slices, stInternal);

        SurfaceCollection output;
        output.surfaces.reserve(region->slices.surfaces.size());
        bool region_mapped                = false;
        bool region_requires_wide_carrier = false;
        for (const Surface& surface : region->slices.surfaces) {
            bool                           surface_mapped                = false;
            bool                           surface_requires_wide_carrier = false;
            const BoundaryModulationResult result =
                m_impl->modulate_slices(ExPolygons{surface.expolygon},
                                        layer,
                                        layer_samplers,
                                        weight_field,
                                        surface_mapped,
                                        surface_requires_wide_carrier);
            region_mapped |= surface_mapped;
            region_requires_wide_carrier |= surface_requires_wide_carrier;
            changed |= result.changed;
            if (!result.geometry.empty())
                output.append(result.geometry, surface);
            else
                output.surfaces.emplace_back(surface);
        }

        if (!output.empty())
            region->slices = std::move(output);
        region->raw_slices = to_expolygons(region->slices.surfaces);
        if (region_requires_wide_carrier)
            region->image_map_external_perimeter_width_mm = m_impl->carrier_width_mm;
        any_mapping |= region_mapped;
    }

    if (any_mapping) {
        layer.make_slices();
        layer.lslices_bboxes.clear();
        layer.lslices_bboxes.reserve(layer.lslices.size());
        for (const ExPolygon& expolygon : layer.lslices)
            layer.lslices_bboxes.emplace_back(get_extents(expolygon));
    }
    return changed || any_mapping;
}

bool PerimeterEnvelopeRenderer::apply_to_perimeters(Layer& layer) const
{
    if (!m_impl)
        return false;

    bool changed = false;

    std::vector<LayerPlaneSampler> layer_samplers;
    if (m_impl->line_width_fraction > EPSILON) {
        layer_samplers.reserve(m_impl->volumes.size());
        for (const VolumeSampler& volume : m_impl->volumes)
            layer_samplers.emplace_back(volume.mesh, volume.data, volume.local_to_print, double(layer.slice_z));
        const Impl::ComponentWeightField weight_field = m_impl->build_component_weight_field(layer, layer_samplers);

        // Width-based modes adjust the generated bead while it is still owned
        // by its original region. This never changes perimeter topology.
        for (LayerRegion* region : layer.regions()) {
            if (region == nullptr || region->image_map_external_perimeter_width_mm <= EPSILON ||
                region->image_map_unmodulated_raw_slices.empty())
                continue;
            for (ExtrusionEntity* entity : region->perimeters.entities)
                if (entity != nullptr)
                    changed |= m_impl->modulate_entity(*entity,
                                                       layer,
                                                       region->image_map_unmodulated_raw_slices,
                                                       layer_samplers,
                                                       weight_field,
                                                       std::nullopt,
                                                       std::nullopt,
                                                       false,
                                                       true);
        }
    }

    // Adaptive ownership is a polygon partition created by multi-material
    // segmentation before walls are generated. Splitting the finished wall a
    // second time would turn its closed islands back into open color runs.
    if (m_impl->adaptive_material_islands || m_impl->adaptive_cadences.empty())
        return changed;

    if (layer_samplers.empty()) {
        layer_samplers.reserve(m_impl->volumes.size());
        for (const VolumeSampler& volume : m_impl->volumes)
            layer_samplers.emplace_back(volume.mesh, volume.data, volume.local_to_print, double(layer.slice_z));
    }

    // Generate the authored wall once, then split it at adaptive ownership
    // boundaries. Each resulting entity is stored in exactly one physical
    // filament region, so the G-code planner must perform a toolchange between
    // differently owned runs. Do not turn these runs into closed material
    // islands: doing so creates a second wall along every texture boundary.
    ExPolygons authored_layer_slices;
    for (const LayerRegion* region : layer.regions()) {
        if (region == nullptr)
            continue;
        if (!region->image_map_unmodulated_raw_slices.empty())
            append(authored_layer_slices, region->image_map_unmodulated_raw_slices);
        else
            append(authored_layer_slices, to_expolygons(region->slices.surfaces));
    }
    authored_layer_slices = union_ex(std::move(authored_layer_slices));
    if (authored_layer_slices.empty())
        return changed;

    std::vector<LayerRegion*> region_by_component(m_impl->num_physical + 1, nullptr);
    for (LayerRegion* region : layer.regions()) {
        if (region == nullptr)
            continue;
        const unsigned int component = unsigned(std::max(0, region->region().config().wall_filament.value));
        if (component >= 1 && component <= m_impl->num_physical && region_by_component[component] == nullptr)
            region_by_component[component] = region;
    }

    // Stage complete printable islands before clearing any source region. A
    // single layer may contain many disconnected perimeter islands; combining
    // every run of one component into one collection lets path ordering treat
    // unrelated islands as a single object and may manufacture long crossing
    // paths. Keeping the original top-level island granularity also gives us a
    // safe fallback when a physical destination region is unavailable.
    std::unordered_map<LayerRegion*, ExtrusionEntitiesPtr> staged_by_region;
    for (LayerRegion* region : layer.regions()) {
        if (region == nullptr || region->perimeters.empty())
            continue;

        const unsigned int configured_component = unsigned(std::max(0, region->region().config().wall_filament.value));
        const unsigned int source_component = configured_component >= 1 && configured_component <= m_impl->num_physical ?
                                                  configured_component :
                                                  m_impl->manager->resolve(configured_component,
                                                                           m_impl->num_physical,
                                                                           int(layer.id()),
                                                                           float(layer.print_z),
                                                                           float(layer.height));
        for (const ExtrusionEntity* entity : region->perimeters.entities) {
            if (entity == nullptr)
                continue;

            if (source_component < 1 || source_component > m_impl->num_physical) {
                staged_by_region[region].emplace_back(entity->clone());
                continue;
            }

            std::vector<ExtrusionEntitiesPtr> island_runs(m_impl->num_physical + 1);
            // Adaptive path displacement is geometry-first: apply_to_slices()
            // already moved the complete boundary before perimeter generation.
            // The generated entity is therefore the single canonical,
            // continuous modulated wall. Moving it again here applied the same
            // exposure twice, which recessed walls behind infill and made
            // adjacent cadence layers appear to overlap. Route this existing
            // geometry without changing its points; neighbouring material
            // runs then inherit the exact same joint from one printable path.
            const bool route_changed = m_impl->route_adaptive_entity(*entity,
                                                                      source_component,
                                                                      layer,
                                                                      authored_layer_slices,
                                                                      layer_samplers,
                                                                      island_runs);

            bool   missing_destination = false;
            double routed_length       = 0.;
            for (unsigned int component = 1; component <= m_impl->num_physical; ++component) {
                if (island_runs[component].empty())
                    continue;
                missing_destination |= region_by_component[component] == nullptr;
                for (const ExtrusionEntity* run : island_runs[component])
                    if (run != nullptr)
                        routed_length += extrusion_entity_length(*run);
            }

            const double routed_source_length = extrusion_entity_length(*entity);
            const bool   conserved = routed_source_length <= EPSILON ||
                                   std::abs(routed_length - routed_source_length) <= 0.005 * routed_source_length;
            if (missing_destination || !conserved) {
                // Ownership is optional; structural walls are not. If this
                // layer does not contain every physical destination region,
                // or resampling failed to reproduce the complete source
                // island, retain the original printable island verbatim.
                // Never publish a partially routed wall set.
                staged_by_region[region].emplace_back(entity->clone());
                continue;
            }

            changed |= route_changed;
            for (unsigned int component = 1; component <= m_impl->num_physical; ++component) {
                if (island_runs[component].empty())
                    continue;

                LayerRegion* target = region_by_component[component];
                assert(target != nullptr);

                auto* island = new ExtrusionEntityCollection();
                island->append(std::move(island_runs[component]));
                staged_by_region[target].emplace_back(island);
            }
        }
    }

    for (LayerRegion* region : layer.regions()) {
        if (region == nullptr)
            continue;
        region->perimeters.clear();
        auto staged = staged_by_region.find(region);
        if (staged != staged_by_region.end())
            region->perimeters.append(std::move(staged->second));
    }
    return changed;
}

} // namespace Slic3r::ImageMap
