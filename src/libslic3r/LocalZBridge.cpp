#include "LocalZBridge.hpp"

#include "ClipperUtils.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {

namespace {

// Resampling only matters inside the transition band; flat runs collapse back
// to their source vertices, so a fine spacing does not inflate flat G-code.
constexpr double BRIDGE_SAMPLE_SPACING_MM = 0.20;
constexpr double BRIDGE_FLAT_TOLERANCE_MM = 1e-5;
constexpr double BRIDGE_PRINTED_HEIGHT_MM = 1e-6;
// Length of the overlap in which a scarf joins two parts of a cut path.
constexpr double BRIDGE_SCARF_LENGTH_MM = 1.0;

// Odd smoothstep on [-1, 1] with zero slope at both ends, so the blend meets
// the constant row heights without a crease at the band edge.
double smooth_signed_step(double t)
{
    t = std::clamp(t, -1., 1.);
    return 0.5 * t * (3. - t * t);
}

} // namespace

LocalZBridgeField::LocalZBridgeField(double                    transition_width_mm,
                                     std::vector<unsigned int> slot_filaments,
                                     std::vector<Row>          rows,
                                     const Options&            options)
    : m_half_width_mm(0.5 * std::max(0., transition_width_mm)), m_options(options), m_slot_filaments(std::move(slot_filaments))
{
    const size_t                           slot_count = m_slot_filaments.size();
    std::vector<std::vector<unsigned int>> filaments_by_row;
    for (Row& row : rows) {
        if (row.masks.empty() || row.bounds.size() != slot_count + 1 || slot_count == 0 || !std::isfinite(row.bounds.front()))
            continue;
        // Interfaces never descend; a malformed one collapses onto the slot below.
        for (size_t bound_idx = 1; bound_idx < row.bounds.size(); ++bound_idx)
            if (!std::isfinite(row.bounds[bound_idx]) || row.bounds[bound_idx] < row.bounds[bound_idx - 1])
                row.bounds[bound_idx] = row.bounds[bound_idx - 1];
        std::vector<unsigned int> filaments = row.filaments;
        for (size_t slot_idx = 0; slot_idx < slot_count; ++slot_idx)
            if (this->row_height(row, slot_idx) > BRIDGE_PRINTED_HEIGHT_MM)
                filaments.push_back(m_slot_filaments[slot_idx]);
        std::sort(filaments.begin(), filaments.end());
        filaments.erase(std::unique(filaments.begin(), filaments.end()), filaments.end());
        filaments_by_row.emplace_back(std::move(filaments));
        m_distancers.emplace_back(to_lines(row.masks));
        m_rows.emplace_back(std::move(row));
    }

    m_shares.assign(m_rows.size(), std::vector<bool>(m_rows.size(), false));
    for (size_t lhs = 0; lhs < m_rows.size(); ++lhs)
        for (size_t rhs = 0; rhs < m_rows.size(); ++rhs) {
            const std::vector<unsigned int>& a = filaments_by_row[lhs];
            const std::vector<unsigned int>& b = filaments_by_row[rhs];
            m_shares[lhs][rhs] = lhs == rhs || std::find_first_of(a.begin(), a.end(), b.begin(), b.end()) != a.end();
        }
}

double LocalZBridgeField::band_reach_mm(const Row& from, const Row& into) const
{
    // A mix reaches through a whole band into a single-filament zone and half a
    // band into another mix; a single-filament zone never reaches into a mix.
    if (from.absorbs_transition == into.absorbs_transition)
        return m_half_width_mm;
    return from.absorbs_transition ? 0. : 2. * m_half_width_mm;
}

double LocalZBridgeField::band_shift_mm(const Row& row, const Row& other) const
{
    if (row.absorbs_transition == other.absorbs_transition)
        return 0.;
    return row.absorbs_transition ? m_half_width_mm : -m_half_width_mm;
}

std::vector<double> LocalZBridgeField::row_weights_at(const Point& point) const
{
    const size_t        row_count = m_rows.size();
    std::vector<double> weights(row_count, 0.);
    if (row_count == 0)
        return weights;
    if (row_count == 1) {
        weights.front() = 1.;
        return weights;
    }

    // Distance from the point to each row's region, zero inside that region.
    // Using only distances to regions keeps the blend independent of the
    // object's outer contour, which also bounds every row.
    std::vector<double> outside_mm(row_count, std::numeric_limits<double>::infinity());
    size_t              owner = 0;
    for (size_t row_idx = 0; row_idx < row_count; ++row_idx) {
        const double signed_distance = m_distancers[row_idx].distance_from_lines<true>(point);
        if (std::isfinite(signed_distance))
            outside_mm[row_idx] = std::max(0., unscale<double>(signed_distance));
        if (outside_mm[row_idx] < outside_mm[owner])
            owner = row_idx;
    }
    if (m_half_width_mm <= EPSILON) {
        weights[owner] = 1.;
        return weights;
    }

    // Rows that share no filament with the owner keep a sharp boundary to it.
    // A participating row's signed depth is its distance outside, measured
    // against the owner, or minus the distance to the closest other
    // participant when inside. Two rows of the same kind then weigh 1/2 each
    // at their boundary and reach 1/0 half a band away. Between a mix and a
    // single-filament zone, both depths shift by half a band so the whole band
    // lies in the single-filament zone.
    auto shift_mm = [this](size_t row_idx, size_t other_idx) { return this->band_shift_mm(m_rows[row_idx], m_rows[other_idx]); };
    double total = 0.;
    for (size_t row_idx = 0; row_idx < row_count; ++row_idx) {
        if (!m_shares[owner][row_idx])
            continue;
        double depth   = outside_mm[row_idx];
        size_t against = owner;
        if (depth <= 0.) {
            depth   = -std::numeric_limits<double>::infinity();
            against = row_idx;
            for (size_t other_idx = 0; other_idx < row_count; ++other_idx)
                if (other_idx != row_idx && m_shares[owner][other_idx] && -outside_mm[other_idx] > depth) {
                    depth   = -outside_mm[other_idx];
                    against = other_idx;
                }
        }
        if (against != row_idx)
            depth += shift_mm(row_idx, against);
        weights[row_idx] = 0.5 * (1. - smooth_signed_step(depth / m_half_width_mm));
        total += weights[row_idx];
    }
    if (total <= EPSILON) {
        std::fill(weights.begin(), weights.end(), 0.);
        weights[owner] = 1.;
        return weights;
    }
    for (double& weight : weights)
        weight /= total;
    return weights;
}

std::vector<double> LocalZBridgeField::stack_at(const Point& point) const
{
    std::vector<double>       stack(m_slot_filaments.size() + 1, 0.);
    const std::vector<double> weights = this->row_weights_at(point);
    for (size_t row_idx = 0; row_idx < m_rows.size(); ++row_idx) {
        if (weights[row_idx] <= 0.)
            continue;
        for (size_t bound_idx = 0; bound_idx < stack.size(); ++bound_idx)
            stack[bound_idx] += weights[row_idx] * m_rows[row_idx].bounds[bound_idx];
    }
    for (size_t bound_idx = 1; bound_idx < stack.size(); ++bound_idx)
        stack[bound_idx] = std::max(stack[bound_idx], stack[bound_idx - 1]);
    return stack;
}

std::vector<double> LocalZBridgeField::heights_at(const Point& point) const
{
    const std::vector<double> stack = this->stack_at(point);
    std::vector<double>       heights(m_slot_filaments.size(), 0.);
    for (size_t slot_idx = 0; slot_idx < heights.size(); ++slot_idx)
        heights[slot_idx] = stack[slot_idx + 1] - stack[slot_idx];
    return heights;
}

LocalZBridgeField::Span LocalZBridgeField::slot_span_at(const Point& point, size_t slot_index) const
{
    if (slot_index >= m_slot_filaments.size())
        return {};
    const std::vector<double> stack = this->stack_at(point);
    if (stack[slot_index + 1] - stack[slot_index] <= BRIDGE_PRINTED_HEIGHT_MM)
        return {stack[slot_index], stack[slot_index]};
    return {stack[slot_index], stack[slot_index + 1]};
}

LocalZBridgeField::Span LocalZBridgeField::slot_bounds(size_t slot_index) const
{
    if (slot_index >= m_slot_filaments.size() || m_rows.empty())
        return {};
    Span bounds{std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    for (const Row& row : m_rows) {
        bounds.lower = std::min(bounds.lower, row.bounds[slot_index]);
        bounds.upper = std::max(bounds.upper, row.bounds[slot_index + 1]);
    }
    return bounds;
}

double LocalZBridgeField::slot_max_height(size_t slot_index) const
{
    double max_height = 0.;
    if (slot_index >= m_slot_filaments.size())
        return max_height;
    for (const Row& row : m_rows)
        max_height = std::max(max_height, this->row_height(row, slot_index));
    return max_height;
}

bool LocalZBridgeField::slot_used(size_t slot_index) const
{
    return slot_index < m_slot_filaments.size() &&
           std::any_of(m_rows.begin(), m_rows.end(),
                       [this, slot_index](const Row& row) { return this->row_height(row, slot_index) > BRIDGE_PRINTED_HEIGHT_MM; });
}

ExPolygons LocalZBridgeField::slot_island(size_t slot_index, const ExPolygons& outline) const
{
    ExPolygons island;
    if (slot_index >= m_slot_filaments.size())
        return island;
    for (size_t row_idx = 0; row_idx < m_rows.size(); ++row_idx) {
        const Row& row = m_rows[row_idx];
        if (this->row_height(row, slot_index) <= BRIDGE_PRINTED_HEIGHT_MM)
            continue;
        append(island, row.masks);
        for (size_t other_idx = 0; other_idx < m_rows.size(); ++other_idx) {
            const Row& other = m_rows[other_idx];
            if (other_idx == row_idx || !m_shares[row_idx][other_idx] || this->row_height(other, slot_index) > BRIDGE_PRINTED_HEIGHT_MM)
                continue;
            if (const double reach = this->band_reach_mm(row, other); reach > EPSILON)
                append(island, intersection_ex(offset_ex(row.masks, float(scale_(reach))), other.masks));
        }
    }
    island = union_ex(island);
    return outline.empty() ? island : intersection_ex(island, outline);
}

std::vector<ExtrusionPathSloped> LocalZBridgeField::modulate_path(const ExtrusionPath& path,
                                                                  size_t               slot_index,
                                                                  double               z_offset,
                                                                  const Span&          tops) const
{
    std::vector<ExtrusionPathSloped> output;
    const Points&                    source = path.polyline.points;
    if (source.size() < 2 || slot_index >= m_slot_filaments.size() || m_rows.empty())
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
        samples.push_back({point, source_vertex, this->slot_span_at(point, slot_index)});
    };

    // Fine sampling never steps over a band, then refines wherever neighboring
    // samples differ, so flat runs cost no more than before.
    const double transition_mm  = this->transition_width_mm();
    const bool   fine           = m_options.fine_sampling && transition_mm > EPSILON;
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
                    refined.push_back({point, false, this->slot_span_at(point, slot_index)});
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
    auto top_kept = [&tops](double top) { return top_in(tops, top); };
    std::vector<std::vector<double>> splits(samples.size());
    std::vector<double>              cuts;
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
            if (!std::isfinite(limit) || (a.span.upper > limit + TOP_TOLERANCE_MM) == (b.span.upper > limit + TOP_TOLERANCE_MM))
                continue;
            const double t = std::clamp((limit - a.span.upper) / top_delta, 0., 1.);
            if (t > 1e-9 && t < 1. - 1e-9)
                splits[sample_idx].push_back(t);
            cuts.push_back(lengths[sample_idx] + t * (lengths[sample_idx + 1] - lengths[sample_idx]));
        }
    }
    std::sort(cuts.begin(), cuts.end());

    // Scarf joints: within half a scarf of a cut, both layers print the bead,
    // with flow ramping from one half at the cut to all of it on the side the
    // layer keeps and to nothing on the other, so the joint adds up to one bead.
    const double half_scarf = m_options.scarf_cuts && !cuts.empty() ? scale_(0.5 * BRIDGE_SCARF_LENGTH_MM) : 0.;
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
    auto flow_at = [&](double position, bool kept) {
        if (half_scarf <= 0.)
            return kept ? 1. : 0.;
        const auto   next     = std::lower_bound(cuts.begin(), cuts.end(), position);
        double       distance = std::numeric_limits<double>::infinity();
        if (next != cuts.end())
            distance = *next - position;
        if (next != cuts.begin())
            distance = std::min(distance, position - *std::prev(next));
        const double ramp = 0.5 * distance / half_scarf;
        return kept ? std::min(1., 0.5 + ramp) : std::max(0., 0.5 - ramp);
    };

    // Flat runs at full flow merge into one path. Resampled points on a flat
    // run lie on their source segment, so only source vertices are kept.
    std::vector<std::pair<Point, bool>> run;
    double                              run_top    = 0.;
    double                              run_height = 0.;
    auto flush_run = [&]() {
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
        const bool  flat  = same_level(a.span, b.span);
        const Vec2d delta = (b.point - a.point).cast<double>();
        auto point_at = [&](double t) {
            if (t <= 0.)
                return a.point;
            if (t >= 1.)
                return b.point;
            return Point(coord_t(std::llround(double(a.point.x()) + delta.x() * t)), coord_t(std::llround(double(a.point.y()) + delta.y() * t)));
        };
        auto top_at    = [&](double t) { return a.span.upper + (b.span.upper - a.span.upper) * t; };
        auto height_at = [&](double t) { return a.span.height() + (b.span.height() - a.span.height()) * t; };
        for (size_t piece_idx = 0; piece_idx + 1 < ts.size(); ++piece_idx) {
            const double t_begin = ts[piece_idx];
            const double t_end   = ts[piece_idx + 1];
            const double t_mid   = 0.5 * (t_begin + t_end);
            const double flow    = flow_at(lengths[sample_idx] + t_mid * (lengths[sample_idx + 1] - lengths[sample_idx]), top_kept(top_at(t_mid)));
            const Point  begin_point = point_at(t_begin);
            const Point  end_point   = point_at(t_end);
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
