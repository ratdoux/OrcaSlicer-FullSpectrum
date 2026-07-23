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
    explicit ContinuousColorSolver(std::vector<ContinuousColorComponent> components);
    ~ContinuousColorSolver();
    ContinuousColorSolver(ContinuousColorSolver&&) noexcept;
    ContinuousColorSolver& operator=(ContinuousColorSolver&&) noexcept;
    ContinuousColorSolver(const ContinuousColorSolver&)            = delete;
    ContinuousColorSolver& operator=(const ContinuousColorSolver&) = delete;

    bool                valid() const;
    size_t              component_count() const;
    size_t              candidate_count() const;
    std::vector<double> solve(const RGBA& target_color) const;
    std::optional<RGBA> predict_color(const RGBA& target_color) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

int    continuous_color_solver_total_units(size_t component_count);
size_t continuous_color_solver_candidate_count(size_t component_count, int total_units = 0);

} // namespace Slic3r::ImageMap

#endif
