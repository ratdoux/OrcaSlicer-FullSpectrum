#include "StackPlanner.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace Slic3r::MaterialBand {

MaterialFractions sample_height_gradient(const RegionIntent& intent, double progress)
{
    if (intent.height_gradient.empty())
        return intent.fractions;
    const auto& curve = intent.height_gradient;
    if (progress <= curve.front().position)
        return curve.front().fractions;
    for (size_t i = 1; i < curve.size(); ++i)
        if (progress <= curve[i].position) {
            const double      t = (progress - curve[i - 1].position) / (curve[i].position - curve[i - 1].position);
            MaterialFractions result{};
            for (size_t m = 0; m < MAX_MATERIALS; ++m)
                result[m] = (1. - t) * curve[i - 1].fractions[m] + t * curve[i].fractions[m];
            return result;
        }
    return curve.back().fractions;
}

StackDoseResult solve_stack_doses(const std::vector<StackDoseCycle>& cycles,
                                  size_t                             materials,
                                  const ProcessConstraints&          process,
                                  double                             fraction_error,
                                  const NumericTolerances&           tolerance,
                                  const RevisionKey&                 revision,
                                  const SolverLimits&                limits,
                                  const std::function<bool()>&       cancelled,
                                  const std::function<bool()>&       stale)
{
    const auto fail = [&](FailureCode code, const char* detail) -> StackDoseResult { return Failure{revision, code, detail, {}, {}, {}}; };
    const auto interrupted = [&]() -> std::optional<Failure> {
        if (cancelled && cancelled())
            return Failure{revision, FailureCode::Cancelled, "cancelled_during_stack_solve", {}, {}, {}};
        if (stale && stale())
            return Failure{revision, FailureCode::StaleInput, "revision_changed", {}, {}, {}};
        return {};
    };
    if (auto error = interrupted())
        return *error;
    if (materials < 2 || materials > MAX_MATERIALS || cycles.empty() || !std::isfinite(fraction_error) || fraction_error < 0. ||
        fraction_error > .25 || !std::isfinite(process.min_active_thickness_mm) || !std::isfinite(process.max_active_thickness_mm) ||
        process.min_active_thickness_mm <= 0. || process.max_active_thickness_mm < process.min_active_thickness_mm ||
        !std::isfinite(limits.convergence_mm) || limits.convergence_mm <= 0. || !std::isfinite(tolerance.interface_mm) ||
        tolerance.interface_mm < 0. || !std::isfinite(tolerance.absolute_volume_mm3) || tolerance.absolute_volume_mm3 < 0. ||
        !std::isfinite(tolerance.relative_volume) || tolerance.relative_volume < 0.)
        return fail(FailureCode::InvalidInput, "invalid_stack_dose_problem");
    if (cycles.size() > limits.max_vertices || cycles.size() > limits.max_temporary_bytes / 1024)
        return fail(FailureCode::ResourceLimit, "stack_dose_memory_limit");
    const size_t                   count = cycles.size();
    std::vector<MaterialFractions> x(count), lo(count), hi(count), correction(count), before(count);
    std::vector<double>            weights(count);
    MaterialFractions              target_volume{}, column_correction{};
    double                         total_volume = 0.;
    for (size_t c = 0; c < count; ++c) {
        if (auto error = interrupted())
            return *error;
        const auto& cycle = cycles[c];
        if (!std::isfinite(cycle.height_mm) || !std::isfinite(cycle.volume_mm3) || cycle.height_mm <= 0. || cycle.volume_mm3 <= 0.)
            return fail(FailureCode::InvalidInput, "invalid_stack_cycle");
        double sum = 0.;
        for (size_t m = 0; m < MAX_MATERIALS; ++m) {
            if (!std::isfinite(cycle.target[m]) || cycle.target[m] < 0. || cycle.target[m] > 1. || (m >= materials && cycle.target[m] != 0.))
                return fail(FailureCode::InvalidInput, "invalid_stack_fractions");
            sum += cycle.target[m];
            target_volume[m] += cycle.volume_mm3 * cycle.target[m];
        }
        if (std::abs(sum - 1.) > 1e-10)
            return fail(FailureCode::InvalidInput, "stack_fractions_do_not_close");
        total_volume += cycle.volume_mm3;
        weights[c]  = cycle.volume_mm3;
        double best = std::numeric_limits<double>::max();
        for (unsigned mask = 1; mask < (1u << materials); ++mask) {
            MaterialFractions lower{}, upper{}, candidate{};
            bool              feasible = true;
            for (size_t m = 0; m < materials; ++m) {
                if (mask & (1u << m)) {
                    if (cycle.target[m] == 0.) {
                        feasible = false;
                        break;
                    }
                    lower[m] = std::max(process.min_active_thickness_mm / cycle.height_mm, cycle.target[m] - fraction_error);
                    upper[m] = std::min({1., (process.max_active_thickness_mm + tolerance.interface_mm) / cycle.height_mm,
                                         cycle.target[m] + fraction_error});
                    if (lower[m] > upper[m] + 1e-12) {
                        feasible = false;
                        break;
                    }
                    lower[m] = std::min(lower[m], upper[m]);
                } else if (cycle.target[m] > fraction_error + 1e-12) {
                    feasible = false;
                    break;
                }
            }
            if (!feasible || std::accumulate(lower.begin(), lower.end(), 0.) > 1. + 1e-12 ||
                std::accumulate(upper.begin(), upper.end(), 0.) < 1. - 1e-12)
                continue;
            // Euclidean projection onto a bounded simplex. The finite active
            // masks are at most 15; no exponential search over all cycles.
            double a = -2., b = 2.;
            for (size_t iteration = 0; iteration < 64; ++iteration) {
                const double lambda = (a + b) / 2.;
                double       amount = 0.;
                for (size_t m = 0; m < materials; ++m)
                    amount += std::clamp(cycle.target[m] - lambda, lower[m], upper[m]);
                if (amount > 1.)
                    a = lambda;
                else
                    b = lambda;
            }
            double cost = 0.;
            for (size_t m = 0; m < materials; ++m) {
                candidate[m] = std::clamp(cycle.target[m] - (a + b) / 2., lower[m], upper[m]);
                cost += std::pow(candidate[m] - cycle.target[m], 2);
            }
            if (cost < best) {
                best  = cost;
                x[c]  = candidate;
                lo[c] = lower;
                hi[c] = upper;
            }
        }
        if (!std::isfinite(best) || best == std::numeric_limits<double>::max())
            return fail(FailureCode::InfeasibleThickness, "gradient_cycle_requires_more_dose_allowance_or_height");
    }
    if (!std::isfinite(total_volume) || total_volume <= 0.)
        return fail(FailureCode::InvalidInput, "invalid_stack_volume");
    for (auto& weight : weights)
        weight /= total_volume;
    const double norm = std::inner_product(weights.begin(), weights.end(), weights.begin(), 0.);
    for (size_t m = 0; m < materials; ++m) {
        double low = 0., high = 0.;
        for (size_t c = 0; c < count; ++c) {
            low += weights[c] * lo[c][m];
            high += weights[c] * hi[c][m];
        }
        if (target_volume[m] / total_volume < low - 1e-10 || target_volume[m] / total_volume > high + 1e-10)
            return fail(FailureCode::MaterialToleranceExceeded, "gradient_global_dose_infeasible");
    }
    bool   converged = false;
    size_t sweeps    = 0;
    for (; sweeps < limits.max_projection_sweeps; ++sweeps) {
        if (auto error = interrupted())
            return *error;
        before = x;
        for (size_t c = 0; c < count; ++c) {
            // Project each complete row onto its bounded simplex in one step.
            // Separate box/row projections converge poorly when most physical
            // materials are fixed at zero in long painted height gradients.
            MaterialFractions values{};
            for (size_t m = 0; m < materials; ++m) {
                values[m] = x[c][m] + correction[c][m];
            }
            double left = std::numeric_limits<double>::max(), right = -left;
            for (size_t m = 0; m < materials; ++m) {
                left  = std::min(left, values[m] - hi[c][m]);
                right = std::max(right, values[m] - lo[c][m]);
            }
            for (size_t i = 0; i < 64; ++i) {
                const double lambda = (left + right) / 2.;
                double       sum    = 0.;
                for (size_t m = 0; m < materials; ++m)
                    sum += std::clamp(values[m] - lambda, lo[c][m], hi[c][m]);
                if (sum > 1.)
                    left = lambda;
                else
                    right = lambda;
            }
            for (size_t m = 0; m < materials; ++m) {
                x[c][m]          = std::clamp(values[m] - (left + right) / 2., lo[c][m], hi[c][m]);
                correction[c][m] = values[m] - x[c][m];
            }
        }
        for (size_t m = 0; m < materials; ++m) {
            double value = 0.;
            for (size_t c = 0; c < count; ++c)
                value += weights[c] * x[c][m];
            const double delta = (value + column_correction[m] * norm - target_volume[m] / total_volume) / norm;
            for (size_t c = 0; c < count; ++c)
                x[c][m] += (column_correction[m] - delta) * weights[c];
            column_correction[m] = delta;
        }
        double residual = 0.;
        for (size_t c = 0; c < count; ++c) {
            residual = std::max(residual, std::abs(std::accumulate(x[c].begin(), x[c].end(), 0.) - 1.));
            for (size_t m = 0; m < materials; ++m) {
                if (!std::isfinite(x[c][m]))
                    return fail(FailureCode::NonConverged, "nonfinite_stack_iterate");
                residual = std::max({residual, lo[c][m] - x[c][m], x[c][m] - hi[c][m], std::abs(x[c][m] - before[c][m])});
            }
        }
        if (residual <= 1e-11) {
            converged = true;
            ++sweeps;
            break;
        }
    }
    if (!converged)
        return fail(FailureCode::NonConverged, "stack_dose_projection_limit");
    StackDoseSolution result;
    result.target_volume_mm3 = target_volume;
    result.projection_sweeps = sweeps;
    for (size_t c = 0; c < count; ++c) {
        for (size_t m = 0; m < materials; ++m) {
            if (hi[c][m] == 0.)
                x[c][m] = 0.;
            result.volume_mm3[m] += cycles[c].volume_mm3 * x[c][m];
            result.maximum_fraction_error = std::max(result.maximum_fraction_error, std::abs(x[c][m] - cycles[c].target[m]));
            if (x[c][m] != 0. && (x[c][m] * cycles[c].height_mm < process.min_active_thickness_mm - tolerance.interface_mm ||
                                  x[c][m] * cycles[c].height_mm > process.max_active_thickness_mm + tolerance.interface_mm))
                return fail(FailureCode::InfeasibleThickness, "postsolve_stack_thickness");
        }
    }
    for (size_t m = 0; m < materials; ++m)
        if (std::abs(result.volume_mm3[m] - target_volume[m]) >
            std::max(tolerance.absolute_volume_mm3, target_volume[m] * tolerance.relative_volume))
            return fail(FailureCode::MaterialToleranceExceeded, "postsolve_stack_volume");
    if (result.maximum_fraction_error > fraction_error + 1e-9)
        return fail(FailureCode::MaterialToleranceExceeded, "postsolve_gradient_fraction");
    result.fractions = std::move(x);
    if (auto error = interrupted())
        return *error;
    return std::make_shared<const StackDoseSolution>(std::move(result));
}
} // namespace Slic3r::MaterialBand
