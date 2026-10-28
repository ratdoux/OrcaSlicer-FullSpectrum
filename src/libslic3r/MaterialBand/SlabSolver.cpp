#include "SlabSolver.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>

namespace Slic3r::MaterialBand {
namespace {
struct TriangleGradient
{
    std::array<uint32_t, 3> vertices;
    std::array<double, 3>   x, y;
    double                  area;
};

struct Halfspace
{
    std::array<uint32_t, 3> vertices;
    std::array<double, 3>   coefficients;
    double                  bound;
    double                  norm_squared;
    double                  correction = 0.;
};

bool finite_positive(double value) { return std::isfinite(value) && value > 0.; }
} // namespace

SlabResult solve_slab(const SlabProblem&           p,
                      const SolverLimits&          limits,
                      const std::function<bool()>& cancelled,
                      const std::function<bool()>& stale)
{
    const auto fail = [&](FailureCode code, const char* detail) -> SlabResult { return Failure{p.revision, code, detail, {}, {}, {}}; };
    if (cancelled && cancelled())
        return fail(FailureCode::Cancelled, "cancelled_before_solve");
    if (stale && stale())
        return fail(FailureCode::StaleInput, "revision_changed");
    if (p.interface_id == 0)
        return fail(FailureCode::InvalidInput, "missing_interface_identity");
    const size_t n = p.vertices.size();
    if (n > limits.max_vertices || p.triangles.size() > limits.max_triangles)
        return fail(FailureCode::ResourceLimit, "interface_geometry_limit");
    // Account for generated constraints and solver work arrays before allocation.
    if (n > limits.max_temporary_bytes / 160 || p.triangles.size() > (limits.max_temporary_bytes - n * 160) / 600)
        return fail(FailureCode::ResourceLimit, "solver_memory_limit");
    if (n < 3 || p.triangles.empty() || p.target_fraction_a.size() != n || p.local_fraction_error.size() != n)
        return fail(FailureCode::InvalidInput, "nodal_field_size");
    const double height = p.top_mm - p.bottom_mm;
    if (!std::isfinite(p.bottom_mm) || !std::isfinite(p.top_mm) || !finite_positive(height) ||
        !finite_positive(p.process.min_active_thickness_mm) || !finite_positive(p.process.max_active_thickness_mm) ||
        !finite_positive(p.process.max_interface_gradient) || !finite_positive(limits.convergence_mm) ||
        !std::isfinite(limits.smoothness_weight) || limits.smoothness_weight < 0. || !finite_positive(p.tolerances.interface_mm) ||
        !finite_positive(p.tolerances.absolute_volume_mm3) || !std::isfinite(p.tolerances.relative_volume) ||
        p.tolerances.relative_volume < 0. || p.tolerances.relative_volume > 1. || !std::isfinite(p.target_volume_a_mm3) ||
        !std::isfinite(p.tolerances.plateau_fraction) || p.tolerances.plateau_fraction < 0. || p.tolerances.plateau_fraction > 1.)
        return fail(FailureCode::InvalidInput, "nonfinite_or_invalid_constraint");
    if (p.discontinuous_target && p.boundary.policy == BoundaryPolicy::PreservePaintedEdge)
        return fail(FailureCode::UnsupportedTopology, "discontinuous_interface");
    if (p.boundary.policy == BoundaryPolicy::AllowTransition &&
        (!finite_positive(p.boundary.transition_width_mm) || !std::isfinite(p.boundary.transition_fraction_error) ||
         p.boundary.transition_fraction_error < 0. || p.boundary.transition_fraction_error > 1. || p.boundary_distance_mm.size() != n))
        return fail(FailureCode::InvalidInput, "invalid_transition_budget");

    std::vector<double> target(n), lower(n), upper(n), weights(n, 0.);
    const double        printable_lower = std::max(p.process.min_active_thickness_mm, height - p.process.max_active_thickness_mm);
    const double        printable_upper = std::min(p.process.max_active_thickness_mm, height - p.process.min_active_thickness_mm);
    for (size_t i = 0; i < n; ++i) {
        const double f = p.target_fraction_a[i], error = p.local_fraction_error[i];
        if (!p.vertices[i].allFinite() || !std::isfinite(f) || f < 0. || f > 1. || !std::isfinite(error) || error < 0. || error > 1.)
            return fail(FailureCode::InvalidInput, "invalid_target_field");
        double permitted_error = p.tolerances.plateau_fraction;
        if (p.boundary.policy == BoundaryPolicy::AllowTransition) {
            if (!std::isfinite(p.boundary_distance_mm[i]))
                return fail(FailureCode::InvalidInput, "invalid_boundary_distance");
            if (std::abs(p.boundary_distance_mm[i]) <= p.boundary.transition_width_mm / 2.)
                permitted_error = std::max(permitted_error, p.boundary.transition_fraction_error);
        }
        if (error > permitted_error)
            return fail(FailureCode::InvalidInput, "local_error_exceeds_declared_budget");
        if (f == 0. || f == 1.)
            return fail(FailureCode::UnsupportedTopology, "band_termination");
        target[i] = height * f;
        lower[i]  = std::max(printable_lower, height * (f - error));
        upper[i]  = std::min(printable_upper, height * (f + error));
        if (lower[i] > upper[i])
            return fail(FailureCode::InfeasibleThickness, "active_band_thickness");
    }
    std::vector<TriangleGradient>           gradients;
    std::vector<Halfspace>                  constraints;
    std::set<std::pair<uint32_t, uint32_t>> edge_set;
    gradients.reserve(p.triangles.size());
    constraints.reserve(4 * p.triangles.size());
    const double component_bound = p.process.max_interface_gradient / std::sqrt(2.);
    for (const auto& face : p.triangles) {
        for (uint32_t vertex : face)
            if (vertex >= n)
                return fail(FailureCode::InvalidInput, "triangle_vertex_index");
        const Vec2d  a = p.vertices[face[1]] - p.vertices[face[0]], b = p.vertices[face[2]] - p.vertices[face[0]];
        const double determinant = a.x() * b.y() - a.y() * b.x();
        if (!finite_positive(determinant))
            return fail(FailureCode::InvalidInput, "degenerate_or_reversed_triangle");
        TriangleGradient gradient{face,
                                  {{(a.y() - b.y()) / determinant, b.y() / determinant, -a.y() / determinant}},
                                  {{(b.x() - a.x()) / determinant, -b.x() / determinant, a.x() / determinant}},
                                  determinant / 2.};
        gradients.push_back(gradient);
        for (uint32_t vertex : face)
            weights[vertex] += gradient.area / 3.;
        for (const auto& coefficients : {gradient.x, gradient.y}) {
            const double norm = std::inner_product(coefficients.begin(), coefficients.end(), coefficients.begin(), 0.);
            if (!finite_positive(norm))
                return fail(FailureCode::InvalidInput, "ill_conditioned_triangle");
            constraints.push_back({face, coefficients, component_bound, norm, 0.});
            auto negative = coefficients;
            for (double& value : negative)
                value = -value;
            constraints.push_back({face, negative, component_bound, norm, 0.});
        }
        for (size_t i = 0; i < 3; ++i)
            edge_set.emplace(std::min(face[i], face[(i + 1) % 3]), std::max(face[i], face[(i + 1) % 3]));
    }
    if (std::any_of(weights.begin(), weights.end(), [](double weight) { return !finite_positive(weight); }))
        return fail(FailureCode::InvalidInput, "unreferenced_vertex");
    const double area = std::accumulate(weights.begin(), weights.end(), 0.);
    if (!finite_positive(area))
        return fail(FailureCode::InvalidInput, "nonfinite_domain_area");
    if (!finite_positive(area * height))
        return fail(FailureCode::InvalidInput, "nonfinite_domain_volume");
    const double min_volume       = std::inner_product(weights.begin(), weights.end(), lower.begin(), 0.);
    const double max_volume       = std::inner_product(weights.begin(), weights.end(), upper.begin(), 0.);
    const double volume_tolerance = std::max(p.tolerances.absolute_volume_mm3, p.tolerances.relative_volume * p.target_volume_a_mm3);
    if (p.target_volume_a_mm3 < min_volume - volume_tolerance || p.target_volume_a_mm3 > max_volume + volume_tolerance)
        return fail(FailureCode::MaterialToleranceExceeded, "infeasible_material_budget");
    for (double& weight : weights)
        weight /= area;
    const double                                     volume_target = p.target_volume_a_mm3 / area;
    const double                                     volume_norm = std::inner_product(weights.begin(), weights.end(), weights.begin(), 0.);
    const std::vector<std::pair<uint32_t, uint32_t>> edges(edge_set.begin(), edge_set.end());
    std::vector<size_t>                              degree(n, 0);
    for (const auto& edge : edges) {
        ++degree[edge.first];
        ++degree[edge.second];
    }
    const double        step   = 1. / (1. + 2. * limits.smoothness_weight * *std::max_element(degree.begin(), degree.end()));
    std::vector<double> x      = target, next(n), box_correction(n), before(n), objective_gradient(n);
    size_t              sweeps = 0, optimization_steps = 0;
    bool                optimized = false;
    for (size_t iteration = 0; iteration < limits.max_optimization_steps; ++iteration) {
        for (size_t i = 0; i < n; ++i)
            objective_gradient[i] = x[i] - target[i];
        if (limits.smoothness_weight > 0.)
            for (const auto& edge : edges) {
                const double difference = limits.smoothness_weight * (x[edge.first] - x[edge.second]);
                objective_gradient[edge.first] += difference;
                objective_gradient[edge.second] -= difference;
            }
        for (size_t i = 0; i < n; ++i)
            next[i] = x[i] - step * objective_gradient[i];
        std::fill(box_correction.begin(), box_correction.end(), 0.);
        for (auto& constraint : constraints)
            constraint.correction = 0.;
        double volume_correction = 0.;
        bool   projected         = false;
        for (size_t sweep = 0; sweep < limits.max_projection_sweeps; ++sweep) {
            if (cancelled && cancelled())
                return fail(FailureCode::Cancelled, "cancelled_during_solve");
            if (stale && stale())
                return fail(FailureCode::StaleInput, "revision_changed");
            before = next;
            for (size_t i = 0; i < n; ++i) {
                const double corrected = next[i] + box_correction[i];
                next[i]                = std::clamp(corrected, lower[i], upper[i]);
                box_correction[i]      = corrected - next[i];
            }
            for (auto& constraint : constraints) {
                double dot = 0.;
                for (size_t i = 0; i < 3; ++i)
                    dot += constraint.coefficients[i] * next[constraint.vertices[i]];
                const double correction = std::max(0., (dot + constraint.correction * constraint.norm_squared - constraint.bound) /
                                                           constraint.norm_squared);
                for (size_t i = 0; i < 3; ++i)
                    next[constraint.vertices[i]] += (constraint.correction - correction) * constraint.coefficients[i];
                constraint.correction = correction;
            }
            const double dot        = std::inner_product(weights.begin(), weights.end(), next.begin(), 0.);
            const double correction = (dot + volume_correction * volume_norm - volume_target) / volume_norm;
            for (size_t i = 0; i < n; ++i)
                next[i] += (volume_correction - correction) * weights[i];
            volume_correction = correction;
            ++sweeps;
            double residual = 0., change = 0.;
            for (size_t i = 0; i < n; ++i) {
                if (!std::isfinite(next[i]))
                    return fail(FailureCode::NonConverged, "nonfinite_iterate");
                residual = std::max({residual, lower[i] - next[i], next[i] - upper[i]});
                change   = std::max(change, std::abs(next[i] - before[i]));
            }
            for (const auto& constraint : constraints) {
                double value = -constraint.bound;
                for (size_t i = 0; i < 3; ++i)
                    value += constraint.coefficients[i] * next[constraint.vertices[i]];
                residual = std::max(residual, value / std::sqrt(constraint.norm_squared));
            }
            if (residual <= limits.convergence_mm && change <= limits.convergence_mm) {
                projected = true;
                break;
            }
        }
        if (!projected)
            return fail(FailureCode::NonConverged, "projection_iteration_limit");
        double change = 0.;
        for (size_t i = 0; i < n; ++i)
            change = std::max(change, std::abs(next[i] - x[i]));
        x.swap(next);
        optimization_steps = iteration + 1;
        if (limits.smoothness_weight == 0. || change / step <= limits.convergence_mm) {
            optimized = true;
            break;
        }
    }
    if (!optimized)
        return fail(FailureCode::NonConverged, "optimization_iteration_limit");

    SlabSolution solution;
    solution.revision              = p.revision;
    solution.interface.id          = p.interface_id;
    solution.interface.domain      = p.domain;
    solution.interface.macro_cycle = p.macro_cycle;
    // Patch IDs are allocated by the stage owner; never derive identity from color.
    solution.interface.surface.triangles = p.triangles;
    for (size_t i = 0; i < n; ++i) {
        solution.interface.surface.vertices.emplace_back(p.vertices[i].x(), p.vertices[i].y(), p.bottom_mm + x[i]);
        solution.max_fraction_error = std::max(solution.max_fraction_error, std::abs(x[i] / height - p.target_fraction_a[i]));
        if (x[i] < lower[i] - p.tolerances.interface_mm || x[i] > upper[i] + p.tolerances.interface_mm)
            return fail(FailureCode::InfeasibleThickness, "postsolve_nodal_residual");
    }
    // Independent post-solve triangle checks, including the actual gradient norm
    // and both complementary volumes. A small iterate change is not acceptance.
    for (const auto& gradient : gradients) {
        double gx = 0., gy = 0., mean = 0.;
        for (size_t i = 0; i < 3; ++i) {
            gx += gradient.x[i] * x[gradient.vertices[i]];
            gy += gradient.y[i] * x[gradient.vertices[i]];
            mean += x[gradient.vertices[i]] / 3.;
        }
        if (!std::isfinite(gx) || !std::isfinite(gy) || !std::isfinite(mean))
            return fail(FailureCode::NonConverged, "nonfinite_postsolve_gradient");
        solution.max_gradient = std::max(solution.max_gradient, std::hypot(gx, gy));
        solution.volume_mm3[0] += gradient.area * mean;
        solution.volume_mm3[1] += gradient.area * (height - mean);
    }
    if (solution.max_gradient > p.process.max_interface_gradient + 1e-10)
        return fail(FailureCode::NonConverged, "postsolve_gradient_residual");
    if (!std::isfinite(solution.volume_mm3[0]) || !std::isfinite(solution.volume_mm3[1]) ||
        std::abs(solution.volume_mm3[0] - p.target_volume_a_mm3) > volume_tolerance ||
        std::abs(solution.volume_mm3[0] + solution.volume_mm3[1] - area * height) > volume_tolerance)
        return fail(FailureCode::MaterialToleranceExceeded, "postsolve_volume_residual");
    solution.projection_sweeps  = sweeps;
    solution.optimization_steps = optimization_steps;
    if (cancelled && cancelled())
        return fail(FailureCode::Cancelled, "cancelled_before_publish");
    if (stale && stale())
        return fail(FailureCode::StaleInput, "revision_changed");
    return std::make_shared<const SlabSolution>(std::move(solution));
}

} // namespace Slic3r::MaterialBand
