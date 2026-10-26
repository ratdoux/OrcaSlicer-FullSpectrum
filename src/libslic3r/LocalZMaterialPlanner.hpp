#ifndef slic3r_LocalZMaterialPlanner_hpp_
#define slic3r_LocalZMaterialPlanner_hpp_

#include "LocalZMaterialGraph.hpp"
#include "LocalZMixBridge.hpp"
#include <string>

namespace Slic3r {

// Material ownership is finalized before perimeter generation. The legacy
// bridge is an interface/flow adapter, not the authority for recipe cadence.
class LocalZMaterialPlanner
{
public:
    struct Options
    {
        double                 transition_factor  = 1.;
        double                 nozzle_diameter    = .4;
        bool                   join_equal_periods = true;
        LocalZCadence::Options cadence;
    };
    struct Diagnostic
    {
        size_t              domain = 0;
        std::string         reason;
        double              bottom = 0., top = 0., ideal_period = 0., chosen_period = 0.;
        std::vector<double> residual_debt;
        std::vector<double> nominal_volume_debt; // mm^3; never mixed with cadence height debt.
        size_t              source_domain = 0;   // Persistent graph identity shared by contact-bounded planning runs.
    };
    enum class ContactKind { equal_period_join, separate_islands, unequal_period, moving_boundary, vertical_handover };
    struct Contract
    {
        size_t      a = 0, b = 0;
        ContactKind kind = ContactKind::separate_islands;
    };
    struct Cell
    {
        std::shared_ptr<const LocalZMixBridge> geometry;
        size_t                                 configuration = 0;
        std::vector<size_t>                    domains;
    };
    struct Plan
    {
        std::vector<Cell>       cells;
        std::vector<Diagnostic> diagnostics;
        std::vector<Contract>   contacts;
        size_t                  minimum_cycles = 0, closure_cycles = 0, nominal_cells = 0, joined_cycles = 0;
        double                  mixed_volume = 0., minimum_volume = 0., closure_volume = 0., nominal_volume = 0., joined_volume = 0.;
    };

    static Plan build(const LocalZMaterialGraph& graph, const Options& options, const std::function<void()>& cancel = {});
};
} // namespace Slic3r
#endif
