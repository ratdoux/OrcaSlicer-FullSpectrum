#include "LocalZBridgeProjector.hpp"
#include <algorithm>
#include <cmath>
namespace Slic3r {
namespace {
// Resampling only matters inside the transition band; flat runs collapse back
// to their source vertices, so a fine spacing does not inflate flat G-code.
constexpr double BRIDGE_SAMPLE_SPACING_MM = 0.20;
constexpr double BRIDGE_FLAT_TOLERANCE_MM = 1e-5;
constexpr double BRIDGE_PRINTED_HEIGHT_MM = 1e-6;
// Length of the overlap in which a scarf joins two parts of a cut path.
constexpr double BRIDGE_SCARF_LENGTH_MM = 1.0;

} // namespace
std::vector<ExtrusionPathSloped> project_local_z_bridge_path(
    const LocalZBridgeField& field, const ExtrusionPath& path, size_t slot_index, double z_offset, const LocalZBridgeField::Span& tops)
{
    using Span = LocalZBridgeField::Span;
    std::vector<ExtrusionPathSloped> output;
    const Points&                    source = path.polyline.points;
    if (source.size() < 2 || slot_index >= field.slot_count() || field.rows().empty())
        return output;

    struct Sample
    {
        Point point;
        bool  source_vertex;
        Span  span;
        bool  printed() const { return span.height() > BRIDGE_PRINTED_HEIGHT_MM; }
    };
    std::vector<Sample> samples;
    samples.reserve(source.size());
    auto add_sample = [&](const Point& point, bool source_vertex) {
        if (!samples.empty() && samples.back().point == point) {
            samples.back().source_vertex = samples.back().source_vertex || source_vertex;
            return;
        }
        samples.push_back({point, source_vertex, field.slot_span_at(point, slot_index)});
    };

    // Fine sampling never steps over a band, then refines wherever neighboring
    // samples differ, so flat runs cost no more than before.
    const double transition_mm  = field.transition_width_mm();
    const bool   fine           = field.options().fine_sampling && transition_mm > EPSILON;
    const double spacing_scaled = scale_(fine ? std::clamp(transition_mm, 0.05, BRIDGE_SAMPLE_SPACING_MM) : BRIDGE_SAMPLE_SPACING_MM);
    add_sample(source.front(), true);
    for (size_t source_idx = 0; source_idx + 1 < source.size(); ++source_idx) {
        const Point& a      = source[source_idx];
        const Point& b      = source[source_idx + 1];
        const Vec2d  delta  = (b - a).cast<double>();
        const double length = delta.norm();
        if (!std::isfinite(length) || length <= EPSILON)
            continue;
        const size_t segment_count = std::clamp<size_t>(size_t(std::ceil(length / spacing_scaled)), 1, 16384);
        for (size_t segment_idx = 1; segment_idx < segment_count; ++segment_idx) {
            const double t = double(segment_idx) / double(segment_count);
            add_sample(Point(coord_t(std::llround(double(a.x()) + delta.x() * t)), coord_t(std::llround(double(a.y()) + delta.y() * t))),
                       false);
        }
        add_sample(b, true);
    }
    if (samples.size() < 2)
        return output;

    auto same_level = [](const Span& lhs, const Span& rhs) {
        return std::abs(lhs.upper - rhs.upper) <= BRIDGE_FLAT_TOLERANCE_MM &&
               std::abs(lhs.height() - rhs.height()) <= BRIDGE_FLAT_TOLERANCE_MM;
    };
    if (fine) {
        const double        fine_spacing_scaled = scale_(std::max(0.01, transition_mm / 8.));
        std::vector<Sample> refined;
        refined.reserve(samples.size());
        for (size_t sample_idx = 0; sample_idx + 1 < samples.size(); ++sample_idx) {
            const Sample& a = samples[sample_idx];
            const Sample& b = samples[sample_idx + 1];
            refined.push_back(a);
            if (same_level(a.span, b.span))
                continue;
            const Vec2d  delta = (b.point - a.point).cast<double>();
            const size_t count = std::clamp<size_t>(size_t(std::ceil(delta.norm() / fine_spacing_scaled)), 1, 4096);
            for (size_t step = 1; step < count; ++step) {
                const double t = double(step) / double(count);
                const Point  point(coord_t(std::llround(double(a.point.x()) + delta.x() * t)),
                                   coord_t(std::llround(double(a.point.y()) + delta.y() * t)));
                if (point != refined.back().point && point != b.point)
                    refined.push_back({point, false, field.slot_span_at(point, slot_index)});
            }
        }
        refined.push_back(samples.back());
        samples = std::move(refined);
    }

    auto emit = [&](Points&& points, double z_begin, double z_end, double height, double flow) {
        ExtrusionPath segment(Polyline(std::move(points)), path);
        segment.polyline.fitting_result.clear();
        // A layer that tapers to nothing still needs a positive bead height.
        height = std::max(1e-3, height);
        if (path.height > EPSILON)
            segment.mm3_per_mm *= height / double(path.height);
        // A scarf shares one bead between two nominal layers.
        segment.mm3_per_mm *= flow;
        segment.height = float(height);
        ExtrusionPathSloped sloped(std::move(segment), ExtrusionPathSloped::Slope{0., 1.}, ExtrusionPathSloped::Slope{1., 1.});
        sloped.local_z_modulation = true;
        sloped.absolute_z_begin   = z_begin + z_offset;
        sloped.absolute_z_end     = z_end + z_offset;
        output.emplace_back(std::move(sloped));
    };

    std::vector<double> lengths(samples.size(), 0.);
    for (size_t sample_idx = 1; sample_idx < samples.size(); ++sample_idx)
        lengths[sample_idx] = lengths[sample_idx - 1] + (samples[sample_idx].point - samples[sample_idx - 1].point).cast<double>().norm();

    // Cuts: where the top crosses a limit of the kept range between two
    // printed samples. They depend only on the path, so the parts kept on
    // neighboring layers meet exactly.
    auto                             top_kept = [&tops](double top) { return LocalZBridgeField::top_in(tops, top); };
    std::vector<std::vector<double>> splits(samples.size());
    std::vector<double>              cuts;
    // Signed steps of this layer's ownership along the path. Smoothing the
    // steps is linear, so adjacent layers cancel the same boundary exactly,
    // including when several boundaries lie within one scarf length.
    std::vector<std::pair<double, double>> ownership_steps;
    for (size_t sample_idx = 0; sample_idx + 1 < samples.size(); ++sample_idx) {
        const Sample& a         = samples[sample_idx];
        const Sample& b         = samples[sample_idx + 1];
        const double  top_delta = b.span.upper - a.span.upper;
        if (!a.printed() || !b.printed() || std::abs(top_delta) <= 1e-12)
            continue;
        // The top changes sides of a limit here, possibly exactly at a sample.
        // Sides shift by the tolerance of top_in, so rounding error around a
        // limit never cuts; the cut itself lies on the limit.
        for (const double limit : {tops.lower, tops.upper}) {
            if (!std::isfinite(limit) || (a.span.upper > limit + LocalZBridgeField::TOP_TOLERANCE_MM) ==
                                             (b.span.upper > limit + LocalZBridgeField::TOP_TOLERANCE_MM))
                continue;
            const double t = std::clamp((limit - a.span.upper) / top_delta, 0., 1.);
            if (t > 1e-9 && t < 1. - 1e-9)
                splits[sample_idx].push_back(t);
            cuts.push_back(lengths[sample_idx] + t * (lengths[sample_idx + 1] - lengths[sample_idx]));
            ownership_steps.emplace_back(cuts.back(), (limit == tops.lower ? 1. : -1.) * (top_delta > 0. ? 1. : -1.));
        }
    }
    std::sort(cuts.begin(), cuts.end());
    sort_remove_duplicates(cuts);
    sort_remove_duplicates(ownership_steps);

    // Scarf joints: within half a scarf of a cut, both layers print the bead,
    // with flow ramping from one half at the cut to all of it on the side the
    // layer keeps and to nothing on the other, so the joint adds up to one bead.
    // Only walls are scarfed: they follow the island outline, so the parts on
    // neighboring layers lie on one line. Infill turns from layer to layer, so
    // its two ramps would land on crossing lines and thin out both.
    const bool   closed     = source.front() == source.back();
    const double half_scarf = field.options().scarf_cuts && is_perimeter(path.role()) && !cuts.empty() ?
                                  std::min(double(scale_(0.5 * BRIDGE_SCARF_LENGTH_MM)), closed ? 0.5 * lengths.back() : lengths.back()) :
                                  0.;
    if (closed && half_scarf > 0.) {
        // A seam is only the start of a closed path, not the end of its scarf.
        const auto steps = ownership_steps;
        for (const auto& [position, change] : steps)
            for (const double shift : {-lengths.back(), lengths.back()}) {
                ownership_steps.emplace_back(position + shift, change);
                cuts.push_back(position + shift);
            }
        sort_remove_duplicates(cuts);
        sort_remove_duplicates(ownership_steps);
    }
    if (half_scarf > 0.) {
        const double ramp_step = half_scarf / 4.;
        for (size_t sample_idx = 0; sample_idx + 1 < samples.size(); ++sample_idx) {
            const double begin = lengths[sample_idx];
            const double end   = lengths[sample_idx + 1];
            if (end - begin <= EPSILON || !samples[sample_idx].printed() || !samples[sample_idx + 1].printed())
                continue;
            for (const double cut : cuts) {
                const double ramp_begin = std::max(begin, cut - half_scarf);
                const double ramp_end   = std::min(end, cut + half_scarf);
                if (ramp_end <= ramp_begin)
                    continue;
                for (double position = ramp_begin;; position += ramp_step) {
                    const double clamped = std::min(position, ramp_end);
                    const double t       = (clamped - begin) / (end - begin);
                    if (t > 1e-9 && t < 1. - 1e-9)
                        splits[sample_idx].push_back(t);
                    if (clamped >= ramp_end)
                        break;
                }
            }
        }
    }
    auto flow_at = [&](double position, bool kept, double piece_middle) {
        if (half_scarf <= 0.)
            return kept ? 1. : 0.;
        double flow = kept ? 1. : 0.;
        for (const auto& [cut, change] : ownership_steps) {
            const double ramp = std::clamp(0.5 + (position - cut) / (2. * half_scarf), 0., 1.);
            flow += change * (ramp - (piece_middle > cut ? 1. : 0.));
        }
        return std::clamp(flow, 0., 1.);
    };

    // Flat runs at full flow merge into one path. Resampled points on a flat
    // run lie on their source segment, so only source vertices are kept.
    std::vector<std::pair<Point, bool>> run;
    double                              run_top    = 0.;
    double                              run_height = 0.;
    auto                                flush_run  = [&]() {
        if (run.size() >= 2) {
            Points points;
            for (size_t run_idx = 0; run_idx < run.size(); ++run_idx)
                if (run_idx == 0 || run_idx + 1 == run.size() || run[run_idx].second)
                    points.emplace_back(run[run_idx].first);
            emit(std::move(points), run_top, run_top, run_height, 1.);
        }
        run.clear();
    };
    for (size_t sample_idx = 0; sample_idx + 1 < samples.size(); ++sample_idx) {
        const Sample& a = samples[sample_idx];
        const Sample& b = samples[sample_idx + 1];
        if (!a.printed() || !b.printed()) {
            // The slot starts or ends between these samples, where its layer
            // has already tapered to nothing.
            flush_run();
            continue;
        }
        std::vector<double>& ts = splits[sample_idx];
        ts.push_back(0.);
        ts.push_back(1.);
        sort_remove_duplicates(ts);
        const bool  flat     = same_level(a.span, b.span);
        const Vec2d delta    = (b.point - a.point).cast<double>();
        auto        point_at = [&](double t) {
            if (t <= 0.)
                return a.point;
            if (t >= 1.)
                return b.point;
            return Point(coord_t(std::llround(double(a.point.x()) + delta.x() * t)),
                                coord_t(std::llround(double(a.point.y()) + delta.y() * t)));
        };
        auto top_at    = [&](double t) { return a.span.upper + (b.span.upper - a.span.upper) * t; };
        auto height_at = [&](double t) { return a.span.height() + (b.span.height() - a.span.height()) * t; };
        for (size_t piece_idx = 0; piece_idx + 1 < ts.size(); ++piece_idx) {
            const double t_begin      = ts[piece_idx];
            const double t_end        = ts[piece_idx + 1];
            const double t_mid        = 0.5 * (t_begin + t_end);
            const double piece_length = lengths[sample_idx + 1] - lengths[sample_idx];
            const double middle       = lengths[sample_idx] + t_mid * piece_length;
            const bool   kept         = top_kept(top_at(t_mid));
            const double begin_flow   = flow_at(lengths[sample_idx] + t_begin * piece_length, kept, middle);
            const double end_flow     = flow_at(lengths[sample_idx] + t_end * piece_length, kept, middle);
            const double height       = 0.5 * (height_at(t_begin) + height_at(t_end));
            // Integrate the product of linear height and linear flow, rather
            // than multiplying their averages. Different layer partitions
            // then preserve the same extrusion volume.
            const double flow = (height_at(t_begin) * (2. * begin_flow + end_flow) + height_at(t_end) * (begin_flow + 2. * end_flow)) /
                                (6. * height);
            const Point begin_point = point_at(t_begin);
            const Point end_point   = point_at(t_end);
            if (flow <= 1e-6 || begin_point == end_point) {
                flush_run();
                continue;
            }
            if (flat && flow >= 1. - 1e-9) {
                const bool continues = !run.empty() && run.back().first == begin_point &&
                                       std::abs(run_top - a.span.upper) <= BRIDGE_FLAT_TOLERANCE_MM &&
                                       std::abs(run_height - a.span.height()) <= BRIDGE_FLAT_TOLERANCE_MM;
                if (!continues) {
                    flush_run();
                    run.emplace_back(begin_point, t_begin <= 0. ? a.source_vertex : false);
                    run_top    = a.span.upper;
                    run_height = a.span.height();
                }
                run.emplace_back(end_point, t_end >= 1. ? b.source_vertex : false);
                continue;
            }
            flush_run();
            emit(Points{begin_point, end_point}, top_at(t_begin), top_at(t_end), 0.5 * (height_at(t_begin) + height_at(t_end)), flow);
        }
    }
    flush_run();
    return output;
}
} // namespace Slic3r
