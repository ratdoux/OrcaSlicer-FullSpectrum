#ifndef slic3r_LocalZPassScheduler_hpp_
#define slic3r_LocalZPassScheduler_hpp_
#include "libslic3r.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <vector>
namespace Slic3r {
// Geometry preparation decides which tools actually print. Scheduling consumes
// the same metadata for G-code and wipe-tower planning; indices refer to input.
struct LocalZSchedulePass
{
    size_t                    layer_to_print_idx;
    size_t                    dependency_group;
    size_t                    dependency_order;
    size_t                    pass_index;
    double                    print_z;
    std::vector<unsigned int> extruders;
};
struct LocalZScheduledPhase
{
    unsigned int        extruder_id;
    std::vector<size_t> pass_state_indices;
};
inline std::optional<std::vector<LocalZScheduledPhase>> schedule_local_z_passes(const std::vector<LocalZSchedulePass>& passes,
                                                                                int                                    start_extruder)
{
    for (const auto& pass : passes)
        if (pass.dependency_group == 0 || pass.extruders.empty() || !std::isfinite(pass.print_z))
            return std::nullopt;
    struct ChainKey
    {
        size_t layer_to_print_idx{0};
        size_t dependency_group{0};

        bool operator<(const ChainKey& rhs) const
        {
            if (layer_to_print_idx != rhs.layer_to_print_idx)
                return layer_to_print_idx < rhs.layer_to_print_idx;
            return dependency_group < rhs.dependency_group;
        }
    };
    struct PassState
    {
        const LocalZSchedulePass* pass_ref{nullptr};
        std::vector<unsigned int> remaining_extruders;
        size_t                    chain_idx{0};
        size_t                    chain_pos{0};
        bool                      ready{false};
        bool                      completed{false};
    };

    std::map<ChainKey, size_t>       chain_index_by_key;
    std::vector<std::vector<size_t>> chains;
    std::vector<PassState>           pass_states;
    pass_states.reserve(passes.size());
    for (const LocalZSchedulePass& pass_ref : passes) {
        ChainKey chain_key{pass_ref.layer_to_print_idx, pass_ref.dependency_group};
        auto [it_chain, inserted] = chain_index_by_key.emplace(chain_key, chains.size());
        if (inserted)
            chains.emplace_back();

        const size_t chain_idx      = it_chain->second;
        const size_t pass_state_idx = pass_states.size();
        pass_states.push_back(PassState{&pass_ref, pass_ref.extruders, chain_idx, 0, false, false});
        chains[chain_idx].push_back(pass_state_idx);
    }

    for (std::vector<size_t>& chain : chains) {
        std::sort(chain.begin(), chain.end(), [&pass_states](size_t lhs_idx, size_t rhs_idx) {
            const LocalZSchedulePass& lhs = *pass_states[lhs_idx].pass_ref;
            const LocalZSchedulePass& rhs = *pass_states[rhs_idx].pass_ref;
            if (lhs.dependency_order != rhs.dependency_order)
                return lhs.dependency_order < rhs.dependency_order;
            if (std::abs(lhs.print_z - rhs.print_z) > EPSILON)
                return lhs.print_z < rhs.print_z;
            return lhs.pass_index < rhs.pass_index;
        });
        for (size_t chain_pos = 0; chain_pos < chain.size(); ++chain_pos)
            pass_states[chain[chain_pos]].chain_pos = chain_pos;
        if (!chain.empty())
            pass_states[chain.front()].ready = true;
    }

    auto pass_contains_extruder = [](const PassState& pass_state, unsigned int extruder_id) {
        return std::find(pass_state.remaining_extruders.begin(), pass_state.remaining_extruders.end(), extruder_id) !=
               pass_state.remaining_extruders.end();
    };
    auto lowest_ready_print_z = [&]() {
        double print_z = std::numeric_limits<double>::infinity();
        for (const PassState& pass_state : pass_states) {
            if (!pass_state.ready || pass_state.completed)
                continue;
            print_z = std::min(print_z, pass_state.pass_ref->print_z);
        }
        return print_z;
    };
    auto choose_ready_extruder = [&](int active_extruder, double ready_print_z) -> int {
        std::vector<unsigned int> ready_extruders;
        for (const PassState& pass_state : pass_states) {
            if (!pass_state.ready || pass_state.completed || std::abs(pass_state.pass_ref->print_z - ready_print_z) > EPSILON)
                continue;
            for (unsigned int extruder_id : pass_state.remaining_extruders)
                if (std::find(ready_extruders.begin(), ready_extruders.end(), extruder_id) == ready_extruders.end())
                    ready_extruders.push_back(extruder_id);
        }
        if (ready_extruders.empty())
            return -1;
        if (active_extruder >= 0 &&
            std::find(ready_extruders.begin(), ready_extruders.end(), unsigned(active_extruder)) != ready_extruders.end()) {
            return active_extruder;
        }

        int    best_extruder     = -1;
        size_t best_ready_count  = 0;
        size_t best_future_count = 0;
        for (unsigned int extruder_id : ready_extruders) {
            size_t ready_count  = 0;
            size_t future_count = 0;
            for (const PassState& pass_state : pass_states) {
                if (pass_state.completed || !pass_contains_extruder(pass_state, extruder_id))
                    continue;
                ++future_count;
                if (pass_state.ready && std::abs(pass_state.pass_ref->print_z - ready_print_z) <= EPSILON)
                    ++ready_count;
            }

            if (best_extruder < 0 || ready_count > best_ready_count ||
                (ready_count == best_ready_count && future_count > best_future_count) ||
                (ready_count == best_ready_count && future_count == best_future_count && extruder_id < unsigned(best_extruder))) {
                best_extruder     = int(extruder_id);
                best_ready_count  = ready_count;
                best_future_count = future_count;
            }
        }

        return best_extruder;
    };

    bool                              dependency_scheduler_ok   = true;
    size_t                            completed_passes          = 0;
    int                               simulated_active_extruder = start_extruder;
    std::vector<LocalZScheduledPhase> scheduled_phases;
    while (dependency_scheduler_ok && completed_passes < pass_states.size()) {
        const double ready_print_z   = lowest_ready_print_z();
        const int    chosen_extruder = choose_ready_extruder(simulated_active_extruder, ready_print_z);
        if (chosen_extruder < 0) {
            dependency_scheduler_ok = false;
            break;
        }

        std::vector<size_t> ready_pass_indices;
        for (size_t pass_state_idx = 0; pass_state_idx < pass_states.size(); ++pass_state_idx) {
            const PassState& pass_state = pass_states[pass_state_idx];
            if (!pass_state.ready || pass_state.completed || std::abs(pass_state.pass_ref->print_z - ready_print_z) > EPSILON ||
                !pass_contains_extruder(pass_state, unsigned(chosen_extruder)))
                continue;
            ready_pass_indices.push_back(pass_state_idx);
        }
        if (ready_pass_indices.empty()) {
            dependency_scheduler_ok = false;
            break;
        }

        std::sort(ready_pass_indices.begin(), ready_pass_indices.end(), [&pass_states](size_t lhs_idx, size_t rhs_idx) {
            const LocalZSchedulePass& lhs = *pass_states[lhs_idx].pass_ref;
            const LocalZSchedulePass& rhs = *pass_states[rhs_idx].pass_ref;
            if (std::abs(lhs.print_z - rhs.print_z) > EPSILON)
                return lhs.print_z < rhs.print_z;
            if (lhs.layer_to_print_idx != rhs.layer_to_print_idx)
                return lhs.layer_to_print_idx < rhs.layer_to_print_idx;
            return lhs.pass_index < rhs.pass_index;
        });
        scheduled_phases.push_back(LocalZScheduledPhase{unsigned(chosen_extruder), ready_pass_indices});

        std::vector<size_t> newly_completed;
        for (size_t pass_state_idx : ready_pass_indices) {
            PassState& pass_state  = pass_states[pass_state_idx];
            auto       it_extruder = std::find(pass_state.remaining_extruders.begin(), pass_state.remaining_extruders.end(),
                                               unsigned(chosen_extruder));
            if (it_extruder == pass_state.remaining_extruders.end())
                continue;
            pass_state.remaining_extruders.erase(it_extruder);
            if (pass_state.remaining_extruders.empty())
                newly_completed.push_back(pass_state_idx);
        }

        for (size_t pass_state_idx : newly_completed) {
            PassState& pass_state = pass_states[pass_state_idx];
            if (pass_state.completed)
                continue;
            pass_state.ready     = false;
            pass_state.completed = true;
            ++completed_passes;

            const std::vector<size_t>& chain          = chains[pass_state.chain_idx];
            const size_t               next_chain_pos = pass_state.chain_pos + 1;
            if (next_chain_pos < chain.size())
                pass_states[chain[next_chain_pos]].ready = true;
        }

        simulated_active_extruder = chosen_extruder;
    }

    if (!dependency_scheduler_ok)
        return std::nullopt;
    return scheduled_phases;
}
} // namespace Slic3r
#endif
