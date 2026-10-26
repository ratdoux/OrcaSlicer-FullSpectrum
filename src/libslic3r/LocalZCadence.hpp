#ifndef slic3r_LocalZCadence_hpp_
#define slic3r_LocalZCadence_hpp_

#include <cstddef>
#include <vector>

namespace Slic3r::LocalZCadence {

struct Limits
{
    double minimum = 0.;
    double maximum = 0.;
};

// Resolve these once, using the physical tool mapping of the owning region.
// Zero printer limits use Slicing.cpp defaults: .07 mm and 75% of nozzle size.
Limits resolve_limits(double local_minimum, double printer_minimum, double printer_maximum, double nozzle_diameter);

struct Component
{
    unsigned tool   = 0;
    double   weight = 0.;
    Limits   limits;
};

struct Bead
{
    unsigned tool   = 0;
    double   height = 0.;
};

enum class Status { ready, invalid_input, height_limit, pass_limit, precision_limit, search_limit };
const char* status_name(Status status);

struct Options
{
    // A real domain bound, not a nominal layer or an arbitrary planning window.
    double maximum_period = 1000.;
    double z_quantum      = .001;
    size_t maximum_beads  = 0; // Authored limit; zero means unspecified.
    size_t search_budget  = 100000;
};

struct Result
{
    Status status = Status::invalid_input;
    // Exact-ratio lower bound before coordinate quantization.
    double            minimum_period = 0.;
    double            period         = 0.;
    std::vector<Bead> beads; // Input component order; adjacent repeats satisfy a bead cap, not a new recipe.
    // Difference from the normalized target contribution, in mm, per component.
    std::vector<double> error;
    explicit            operator bool() const { return status == Status::ready; }
};

// Finds the first feasible point in the intersection of each component's union
// of printable slot-count intervals. No nominal-layer grid participates.
Result minimum(const std::vector<Component>& components, const Options& options = {});
// Debt/error indices refer to positive-weight components in input order.
Result at_period(const std::vector<Component>& components, double period, const Options& options = {}, const std::vector<double>& debt = {});

struct Tiling
{
    Status              status           = Status::invalid_input;
    double              minimum_period   = 0.;
    bool                closure_adjusted = false;
    std::vector<Result> cycles;
    std::vector<double> residual_debt;
    size_t              constrained_corrections = 0;
    explicit            operator bool() const { return status == Status::ready; }
};

// Close a real finite domain using the greatest feasible count of complete
// recipes. Preserve the minimum-period prefix and solve the actual endpoint
// remainder using feasible periods. Only the caller can identify a real end.
Tiling tile(const std::vector<Component>& components, double height, const Options& options = {});

} // namespace Slic3r::LocalZCadence
#endif
