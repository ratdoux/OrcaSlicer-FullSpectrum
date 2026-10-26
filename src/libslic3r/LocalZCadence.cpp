#include "LocalZCadence.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <set>

namespace Slic3r::LocalZCadence {
namespace {
constexpr double TOLERANCE = 1e-9;

bool normalize(const std::vector<Component>& input, const Options& options, std::vector<Component>& output)
{
    if (input.empty() || !std::isfinite(options.maximum_period) || options.maximum_period <= 0. || !std::isfinite(options.z_quantum) ||
        options.z_quantum < 0. || options.search_budget == 0)
        return false;
    double             sum = 0.;
    std::set<unsigned> tools;
    for (const auto& c : input) {
        if (!std::isfinite(c.weight) || c.weight < 0.)
            return false;
        if (c.weight == 0.)
            continue;
        if (c.tool == 0 || !tools.insert(c.tool).second || !std::isfinite(c.limits.minimum) || !std::isfinite(c.limits.maximum) ||
            c.limits.minimum <= 0. || c.limits.maximum < c.limits.minimum)
            return false;
        sum += c.weight;
        output.push_back(c);
    }
    if (output.empty() || !std::isfinite(sum) || sum <= 0.)
        return false;
    for (auto& c : output)
        c.weight /= sum;
    return true;
}

size_t count_for(double total, double maximum)
{
    const double count = std::ceil(total / maximum - TOLERANCE);
    if (!std::isfinite(count) || count > double(std::numeric_limits<size_t>::max() / 2))
        return std::numeric_limits<size_t>::max();
    return std::max<size_t>(1, size_t(std::max(0., count)));
}

Result allocate(const std::vector<Component>& components, double period, const Options& options, const std::vector<double>& debt)
{
    Result result;
    result.period = period;
    if (!std::isfinite(period) || period <= 0. || period > options.maximum_period + TOLERANCE ||
        (!debt.empty() &&
         (debt.size() != components.size() || std::any_of(debt.begin(), debt.end(), [](double v) { return !std::isfinite(v); }))))
        return result;

    struct Choice
    {
        double total;
        size_t count;
        double cost;
    };
    std::vector<std::vector<Choice>> choices(components.size());
    const double                     quantum = options.z_quantum;
    if (quantum > 0. && std::abs(period / quantum - std::round(period / quantum)) > 1e-6) {
        result.status = Status::precision_limit;
        return result;
    }
    for (size_t i = 0; i < components.size(); ++i) {
        const auto&  c      = components[i];
        const double ideal  = c.weight * period;
        const double target = ideal + (debt.empty() ? 0. : debt[i]);
        const double lower  = quantum > 0. ? std::ceil(c.limits.minimum / quantum - TOLERANCE) * quantum : c.limits.minimum;
        const double upper  = quantum > 0. ? std::floor(c.limits.maximum / quantum + TOLERANCE) * quantum : c.limits.maximum;
        if (lower > upper + TOLERANCE || upper <= 0.) {
            result.status = Status::precision_limit;
            return result;
        }
        // Rounding choices stay within one coordinate tick of the requested
        // contribution. The debt corrects drift only in this connected domain.
        const double lo = quantum > 0. ? std::floor(target / quantum) * quantum : ideal;
        const double hi = quantum > 0. ? std::ceil(target / quantum) * quantum : ideal;
        for (double total : {lo, hi}) {
            const size_t count = count_for(total, upper);
            if (total <= 0. || count == std::numeric_limits<size_t>::max() || count * lower > total + TOLERANCE ||
                (options.maximum_beads && count > options.maximum_beads))
                continue;
            if (!choices[i].empty() && std::abs(choices[i].back().total - total) < TOLERANCE)
                continue;
            choices[i].push_back({total, count, std::abs(total - target)});
        }
        if (choices[i].empty()) {
            result.status = Status::height_limit;
            return result;
        }
    }
    std::vector<Choice>                                 selected, best;
    double                                              best_cost        = std::numeric_limits<double>::infinity();
    size_t                                              visits           = 0;
    bool                                                budget_exhausted = false, pass_limited = false;
    std::function<void(size_t, double, size_t, double)> search = [&](size_t index, double sum, size_t count, double cost) {
        if (++visits > options.search_budget) {
            budget_exhausted = true;
            return;
        }
        if (options.maximum_beads && count > options.maximum_beads) {
            pass_limited = true;
            return;
        }
        if (sum > period + TOLERANCE || cost >= best_cost)
            return;
        if (index == choices.size()) {
            if (std::abs(sum - period) <= TOLERANCE) {
                best      = selected;
                best_cost = cost;
            }
            return;
        }
        for (const auto& choice : choices[index]) {
            selected.push_back(choice);
            search(index + 1, sum + choice.total, count + choice.count, cost + choice.cost);
            selected.pop_back();
        }
    };
    search(0, 0., 0, 0.);
    if (best.empty()) {
        result.status = budget_exhausted ? Status::search_limit : pass_limited ? Status::pass_limit : Status::precision_limit;
        return result;
    }
    for (size_t i = 0; i < best.size(); ++i) {
        const auto& choice = best[i];
        if (choice.count > options.search_budget || result.beads.size() > options.search_budget - choice.count) {
            result.status = Status::search_limit;
            result.beads.clear();
            return result;
        }
        double previous = 0.;
        for (size_t j = 1; j <= choice.count; ++j) {
            const double exact = choice.total * double(j) / double(choice.count);
            const double end   = quantum > 0. ? std::round(exact / quantum) * quantum : exact;
            result.beads.push_back({components[i].tool, end - previous});
            previous = end;
        }
        result.error.push_back(choice.total - components[i].weight * period);
    }
    result.status = Status::ready;
    return result;
}
} // namespace

Limits resolve_limits(double local_minimum, double printer_minimum, double printer_maximum, double nozzle_diameter)
{
    if (!std::isfinite(local_minimum) || !std::isfinite(printer_minimum) || !std::isfinite(printer_maximum) ||
        !std::isfinite(nozzle_diameter) || local_minimum < 0. || printer_minimum < 0. || printer_maximum < 0. || nozzle_diameter <= 0.)
        return {};
    return {std::max({.01, local_minimum, printer_minimum == 0. ? .07 : printer_minimum}),
            printer_maximum == 0. ? .75 * nozzle_diameter : printer_maximum};
}

const char* status_name(Status status)
{
    switch (status) {
    case Status::ready: return "ready";
    case Status::invalid_input: return "invalid_input";
    case Status::height_limit: return "height_limit";
    case Status::pass_limit: return "pass_limit";
    case Status::precision_limit: return "precision_limit";
    case Status::search_limit: return "search_limit";
    }
    return "invalid_input";
}

Result at_period(const std::vector<Component>& input, double period, const Options& options, const std::vector<double>& debt)
{
    std::vector<Component> components;
    return normalize(input, options, components) ? allocate(components, period, options, debt) : Result{};
}

Result minimum(const std::vector<Component>& input, const Options& options)
{
    std::vector<Component> components;
    if (!normalize(input, options, components))
        return {};
    Result result;
    double period = 0.;
    for (const auto& c : components)
        period = std::max(period, c.limits.minimum / c.weight);
    for (size_t iteration = 0; iteration < options.search_budget; ++iteration) {
        if (!std::isfinite(period) || period > options.maximum_period + TOLERANCE) {
            result.status = Status::height_limit;
            return result;
        }
        double next  = period;
        size_t beads = 0;
        for (const auto& c : components) {
            const size_t count = count_for(period * c.weight, c.limits.maximum);
            if (count == std::numeric_limits<size_t>::max() || count > std::numeric_limits<size_t>::max() - beads) {
                result.status = Status::search_limit;
                return result;
            }
            beads += count;
            next = std::max(next, count * c.limits.minimum / c.weight);
        }
        if (options.maximum_beads && beads > options.maximum_beads) {
            result.status = Status::pass_limit;
            return result;
        }
        if (next > period + TOLERANCE) {
            period = next;
            continue;
        }
        const double ideal = period;
        if (options.z_quantum > 0.)
            period = std::ceil(period / options.z_quantum - TOLERANCE) * options.z_quantum;
        // Coordinate rounding can straddle a slot-count gap. Search the emitted
        // periods too, rather than returning a bead below its minimum.
        for (; iteration < options.search_budget && period <= options.maximum_period + TOLERANCE; ++iteration) {
            result                = allocate(components, period, options, {});
            result.minimum_period = ideal;
            if (result || result.status == Status::search_limit || result.status == Status::pass_limit)
                return result;
            if (options.z_quantum == 0.)
                return result;
            period += options.z_quantum;
        }
        result.status = period > options.maximum_period + TOLERANCE ? Status::height_limit : Status::search_limit;
        return result;
    }
    result.status = Status::search_limit;
    return result;
}

Tiling tile(const std::vector<Component>& components, double height, const Options& options)
{
    Tiling result;
    if (!std::isfinite(height) || height <= 0.)
        return result;
    Options bounded        = options;
    bounded.maximum_period = std::min(height, options.maximum_period);
    const auto first       = minimum(components, bounded);
    result.status          = first.status;
    result.minimum_period  = first.minimum_period;
    if (!first)
        return result;
    if (options.z_quantum > 0. && std::abs(height / options.z_quantum - std::round(height / options.z_quantum)) > 1e-6) {
        result.status = Status::precision_limit;
        return result;
    }
    const size_t maximum_count = size_t(std::floor(height / first.period + TOLERANCE));
    if (maximum_count > options.search_budget) {
        result.status = Status::search_limit;
        return result;
    }
    size_t attempts = 0;
    for (size_t count = maximum_count; count > 0; --count) {
        // Solve the excess over count minimum periods. A minimum-period prefix
        // stays phase-stable; only real endpoint cycles receive extra ticks.
        // Unlike uniform stretching this also handles disjoint feasible ranges.
        std::vector<double> periods(count, first.period);
        const double        excess = std::max(0., height - count * first.period);
        if (excess < TOLERANCE) {
            // Already closed at the minimum cadence.
        } else if (at_period(components, first.period + excess, bounded)) {
            periods.back() += excess;
        } else if (options.z_quantum > 0.) {
            const size_t ticks = size_t(std::llround(excess / options.z_quantum));
            if (ticks > options.search_budget) {
                result.status = Status::search_limit;
                return result;
            }
            std::vector<size_t> steps(ticks + 1, count + 1), previous(ticks + 1, 0);
            steps[0] = 0;
            for (size_t delta = 1; delta <= ticks; ++delta) {
                if (++attempts > options.search_budget) {
                    result.status = Status::search_limit;
                    return result;
                }
                const double period = first.period + delta * options.z_quantum;
                if (period > bounded.maximum_period + TOLERANCE || !at_period(components, period, bounded))
                    continue;
                for (size_t total = delta; total <= ticks; ++total) {
                    if (++attempts > options.search_budget) {
                        result.status = Status::search_limit;
                        return result;
                    }
                    if (steps[total - delta] < count && steps[total - delta] + 1 < steps[total]) {
                        steps[total]    = steps[total - delta] + 1;
                        previous[total] = delta;
                    }
                }
            }
            if (steps[ticks] > count)
                continue;
            size_t tail = count;
            for (size_t rest = ticks; rest > 0; rest -= previous[rest])
                periods[--tail] += previous[rest] * options.z_quantum;
        } else {
            // Continuous mode has no coordinate lattice. Try the unchanged
            // prefix first, then bounded evenly distributed endpoint closures.
            bool closed = excess < TOLERANCE;
            for (size_t tail = 1; !closed && tail <= count; ++tail) {
                if (++attempts > options.search_budget) {
                    result.status = Status::search_limit;
                    return result;
                }
                const double period = first.period + excess / double(tail);
                if (at_period(components, period, bounded)) {
                    std::fill(periods.end() - tail, periods.end(), period);
                    closed = true;
                }
            }
            if (!closed)
                continue;
        }
        std::vector<Result> cycles;
        std::vector<double> debt(first.error.size(), 0.);
        size_t              constrained = 0;
        for (size_t i = 1; i <= count; ++i) {
            if (++attempts > options.search_budget) {
                result.status = Status::search_limit;
                return result;
            }
            auto cycle = at_period(components, periods[i - 1], bounded, debt);
            if (!cycle) {
                // A minimum-height component may constrain error correction.
                // Retry without debt, keeping that unresolved debt local.
                cycle = at_period(components, periods[i - 1], bounded);
                if (!cycle)
                    break;
                ++constrained;
            }
            cycle.minimum_period = first.minimum_period;
            for (size_t j = 0; j < debt.size(); ++j)
                debt[j] -= cycle.error[j];
            cycles.push_back(std::move(cycle));
        }
        if (cycles.size() == count) {
            result.status                  = Status::ready;
            result.closure_adjusted        = std::any_of(cycles.begin(), cycles.end(),
                                                         [&](const auto& c) { return std::abs(c.period - first.period) > TOLERANCE; });
            result.cycles                  = std::move(cycles);
            result.residual_debt           = std::move(debt);
            result.constrained_corrections = constrained;
            return result;
        }
    }
    result.status = Status::height_limit;
    return result;
}
} // namespace Slic3r::LocalZCadence
