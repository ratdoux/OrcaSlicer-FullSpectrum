#ifndef slic3r_ImageMap_ContinuousColorSolver_hpp_
#define slic3r_ImageMap_ContinuousColorSolver_hpp_

#include "../Color.hpp"
#include "VolumeData.hpp"

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

struct ContinuousColorCandidate
{
    // Normalized physical recipe. Values sum to one and use the same dense
    // exposure lattice as solve().
    std::vector<double> weights;
    RGBA                predicted_color{0.f, 0.f, 0.f, 1.f};
};

// Scales a physical recipe uniformly so its strongest component reaches full
// surface exposure. Relative component ratios are preserved.
std::vector<double> compact_modulation_weights(std::vector<double> weights);

// Full-resolution texture colors are solved against a finite, dense set of
// physical-filament weight combinations. Candidate colours are predicted once
// with the selected mixing model, then queried concurrently by the slice-time
// perimeter renderer and viewport result preview. Every model uses the same
// closest-candidate lookup and Oklab SoftCap4Dark4 score.
class ContinuousColorSolver
{
public:
    explicit ContinuousColorSolver(std::vector<ContinuousColorComponent> components,
                                   ColorMixModel color_mix_model = ColorMixModel::FullSpectrumKmKs,
                                   bool apply_saved_calibration = true);
    ~ContinuousColorSolver();
    ContinuousColorSolver(ContinuousColorSolver&&) noexcept;
    ContinuousColorSolver& operator=(ContinuousColorSolver&&) noexcept;
    ContinuousColorSolver(const ContinuousColorSolver&)            = delete;
    ContinuousColorSolver& operator=(const ContinuousColorSolver&) = delete;

    bool                valid() const;
    size_t              component_count() const;
    size_t              candidate_count() const;
    std::optional<ContinuousColorCandidate> candidate(size_t candidate_index) const;
    std::vector<double> solve(const RGBA& target_color) const;
    // Preview and texture sampling quantize source colours to five bits per
    // channel. Cache that finite 32^3 lookup in the shared solver so live
    // image-processing edits do not repeat thousands of KD-tree searches.
    // Entries are populated lock-free and are safe to reuse concurrently.
    std::vector<double> solve_quantized_5bit(const RGBA& target_color) const;
    // Perimeter modulation uses the closest printable physical recipe, then
    // compacts its exposure strengths so the dominant component reaches the
    // authored outer envelope. Slice-time spatial reconstruction blends these
    // discrete recipes along the wall before this compaction is applied.
    std::vector<double> solve_modulation(const RGBA& target_color) const;
    std::optional<RGBA> predict_color(const RGBA& target_color) const;
    std::optional<RGBA> predict_modulation_color(const RGBA& target_color) const;
    // Predict the physical appearance of an already reconstructed exposure
    // vector. This is used by the viewport after Gaussian/path filtering.
    std::optional<RGBA> predict_weights(const std::vector<double>& weights) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

struct ContinuousColorRecipe
{
    // Zero-based indices into the physical component array supplied to the
    // recipe solver. A recipe intentionally contains no virtual filament IDs.
    std::vector<size_t> component_indices;
    // Continuous target exposure. These percentages control modulation depth;
    // they do not control how often a material appears in the layer cadence.
    std::vector<int>    component_percents;
    // Equal, repetition-free ownership cycle. Each selected component occurs
    // exactly once (except the unavoidable one-component solid-color case).
    std::vector<size_t> layer_sequence;
    std::optional<RGBA> predicted_color;
    float               perceptual_error{0.f};

    bool                  valid() const;
    std::optional<size_t> component_index_for_layer(size_t layer_index) const;
};

// Finds the smallest physical-material subset which reproduces a target
// colour within a small perceptual tolerance of the best available subset.
// The subset solvers are prepared once and reused for every palette entry, so
// adaptive contour sampling remains cheap at slice time.
class ContinuousColorRecipeSolver
{
public:
    explicit ContinuousColorRecipeSolver(std::vector<ContinuousColorComponent> components,
                                         size_t max_components = 3,
                                         ColorMixModel color_mix_model = ColorMixModel::FullSpectrumKmKs);
    ~ContinuousColorRecipeSolver();
    ContinuousColorRecipeSolver(ContinuousColorRecipeSolver&&) noexcept;
    ContinuousColorRecipeSolver& operator=(ContinuousColorRecipeSolver&&) noexcept;
    ContinuousColorRecipeSolver(const ContinuousColorRecipeSolver&)            = delete;
    ContinuousColorRecipeSolver& operator=(const ContinuousColorRecipeSolver&) = delete;

    bool                  valid() const;
    ContinuousColorRecipe solve(const RGBA& target_color, int minimum_component_percent = 15) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

int    continuous_color_solver_total_units(size_t component_count);
size_t continuous_color_solver_candidate_count(size_t component_count, int total_units = 0);
size_t continuous_color_solver_max_component_count();

// Selects the physical components used by the shared perimeter cadence.
// requested_count == 0 preserves every physical component supported by the
// solver, matching ImageMap's default component cadence. Otherwise exactly
// that many source-relevant components are returned (within solver limits).
// Returned indices are zero-based and sorted in physical-filament order.
std::vector<size_t> select_continuous_color_components(const std::vector<ContinuousColorComponent>& components,
                                                       const std::vector<RGBA>&                     representative_colors,
                                                       size_t                                       requested_count = 0,
                                                       ColorMixModel                                color_mix_model =
                                                           ColorMixModel::FullSpectrumKmKs);

} // namespace Slic3r::ImageMap

#endif
