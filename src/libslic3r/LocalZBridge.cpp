#include "LocalZBridge.hpp"

#include "ClipperUtils.hpp"
#include "LocalZBridgeProjector.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {

namespace {

constexpr double BRIDGE_PRINTED_HEIGHT_MM = 1e-6;

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
    const auto weights = this->row_weights_at(point);
    Span       span;
    for (size_t row = 0; row < m_rows.size(); ++row) {
        span.lower += weights[row] * m_rows[row].bounds[slot_index];
        span.upper += weights[row] * m_rows[row].bounds[slot_index + 1];
    }
    if (span.height() <= BRIDGE_PRINTED_HEIGHT_MM)
        span.upper = span.lower;
    return span;
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
            // The layer tapers with the distance to the row, so the island ends
            // where that distance reaches the band. Mitered joins would instead
            // copy every tooth of a painted edge out to where nothing prints.
            if (const double reach = this->band_reach_mm(row, other); reach > EPSILON)
                append(island, intersection_ex(offset_ex(row.masks, float(scale_(reach)), jtRound, SCALED_RESOLUTION), other.masks));
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
    return project_local_z_bridge_path(*this, path, slot_index, z_offset, tops);
}

ExPolygons bridge_island_inset(const ExPolygons& island, double inset)
{
    // Mitered joins would push a wall along the bisector of every sharp notch,
    // up to three insets deep, so walls along a painted edge would zigzag far
    // more than the edge does. A degenerate piece of the island can grow when
    // inset; the result never leaves the island.
    return intersection_ex(offset_ex(island, -float(inset), jtRound, SCALED_RESOLUTION), island);
}

ExPolygons bridge_island_wall_area(const ExPolygons& island, double inset, double spacing, ExPolygons* narrow)
{
    const ExPolygons area = bridge_island_inset(island, inset);
    // An opening keeps every part of the area that a wall-wide disk fits into,
    // so the loop around it never comes closer to itself than one spacing.
    const ExPolygons kept = opening_ex(area, float(0.5 * spacing), jtRound, SCALED_RESOLUTION);
    // The opening also rounds every convex corner. The tip it rounds off a
    // corner of 40 degrees or more spans at most 1.4 spacings, and the loop
    // around such a corner hardly overlaps itself, so the corner stays, as the
    // classic generator keeps such corners within its miter limit. Longer cuts
    // are wedge tips and necks the loop would print twice.
    const double max_span = sqr(1.4 * spacing);
    auto         is_corner = [max_span](const Polygon& contour) {
        for (const Point& a : contour.points)
            for (const Point& b : contour.points)
                if ((b - a).cast<double>().squaredNorm() > max_span)
                    return false;
        return true;
    };
    // Cut against a slightly grown area: where the opening keeps an edge, the
    // cut would otherwise run along it as a zero-width strip, joining the cut
    // corners at both of its ends into one long piece.
    ExPolygons cut;
    for (ExPolygon& part : diff_ex(area, offset_ex(kept, ClipperSafetyOffset)))
        if (!is_corner(part.contour))
            cut.emplace_back(std::move(part));
    if (cut.empty())
        return area;
    // Only the cut parts go: the opening also traces every curved edge again
    // with extra points, which would split the walls into shorter moves. They
    // go slightly grown, or the edges they share with the area would stay as
    // zero-width spikes out to the old tip.
    ExPolygons result = diff_ex(area, offset_ex(cut, ClipperSafetyOffset));
    if (narrow != nullptr)
        append(*narrow, std::move(cut));
    return result;
}

} // namespace Slic3r
