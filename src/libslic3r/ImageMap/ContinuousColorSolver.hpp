#ifndef slic3r_ImageMap_ContinuousColorSolver_hpp_
#define slic3r_ImageMap_ContinuousColorSolver_hpp_

#include "../Color.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::ImageMap {

struct ContinuousColorComponent
{
    std::string                color_hex;
    std::optional<double>      transmission_distance_mm;
    std::optional<std::string> material_id;
};

// Full-resolution texture colors are solved against a finite, dense set of
// physical-filament weight combinations. The candidate colors are predicted
// once with FullSpectrum's KM/K-S sidewall model, then queried concurrently by
// the slice-time perimeter renderer and viewport result preview.
class ContinuousColorSolver
{
public:
    explicit ContinuousColorSolver(std::vector<ContinuousColorComponent> components, bool prepare_modulation = false);
    ~ContinuousColorSolver();
    ContinuousColorSolver(ContinuousColorSolver&&) noexcept;
    ContinuousColorSolver& operator=(ContinuousColorSolver&&) noexcept;
    ContinuousColorSolver(const ContinuousColorSolver&)            = delete;
    ContinuousColorSolver& operator=(const ContinuousColorSolver&) = delete;

    bool                valid() const;
    size_t              component_count() const;
    size_t              candidate_count() const;
    std::vector<double> solve(const RGBA& target_color) const;
    // Perimeter modulation needs a continuous transfer function: a hard
    // nearest-candidate boundary turns imperceptible texture changes into
    // visible geometric bands. These methods softly project onto the physical
    // gamut and interpolate a small RGB lookup table so neighboring source
    // colors always produce neighboring mixture weights.
    std::vector<double> solve_modulation(const RGBA& target_color) const;
    std::optional<RGBA> predict_color(const RGBA& target_color) const;
    std::optional<RGBA> predict_modulation_color(const RGBA& target_color) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

int    continuous_color_solver_total_units(size_t component_count);
size_t continuous_color_solver_candidate_count(size_t component_count, int total_units = 0);
size_t continuous_color_solver_max_component_count();

// Selects the physical components that are materially used by a bounded set of
// representative source colors. requested_count == 0 enables automatic mode;
// otherwise exactly that many components are returned (within solver limits).
// Returned indices are zero-based and sorted in physical-filament order.
std::vector<size_t> select_continuous_color_components(const std::vector<ContinuousColorComponent>& components,
                                                       const std::vector<RGBA>&                     representative_colors,
                                                       size_t                                       requested_count          = 0,
                                                       double                                       minimum_component_weight = 0.15);

} // namespace Slic3r::ImageMap

#endif
