#ifndef slic3r_ImageMap_SimplePmCalibration_hpp_
#define slic3r_ImageMap_SimplePmCalibration_hpp_

#include "ContinuousColorSolver.hpp"
#include "VolumeData.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::ImageMap {

inline constexpr uint32_t SIMPLE_PM_CALIBRATION_SCHEMA_VERSION = 1;

struct SimplePmCalibrationChartSettings
{
    // One continuous, upright plaque. The colored cells and their guard
    // gutters are all pixels of the same front-face image map.
    // Sized for a 19 x 14 field of the enlarged guarded cells below.
    float    plaque_width_mm{150.f};
    float    plaque_height_mm{110.f};
    float    plaque_thickness_mm{6.f};
    float    base_depth_mm{18.f};
    float    base_slope_height_mm{8.f};
    // The photo reader discards 28% around every edge to avoid transition
    // contamination. These dimensions leave a 2.55 x 2.20 mm clean center,
    // nearly four times the sampled area of the previous 3.0 x 2.5 mm cells.
    float    cell_width_mm{5.8f};
    float    cell_height_mm{5.0f};
    // The path-direction guard is deliberately wider than the combined
    // default Gaussian and broad-smoothing footprint (~1.1 mm for a 0.6 mm
    // carrier), so the recipe settles before the next measured cell.
    float    horizontal_gutter_mm{1.2f};
    float    vertical_gutter_mm{0.8f};
    float    side_margin_mm{8.f};
    float    bottom_margin_mm{8.f};
    // Reserve a quiet area for the 32 x 16 mm low-density identity code.
    float    top_margin_mm{20.f};
    size_t   maximum_patch_count{266};
    uint32_t texture_width{2048};
    uint32_t texture_height{1536};
};

struct SimplePmCalibrationPatch
{
    uint32_t             patch_id{0};
    std::vector<uint8_t> component_units;
    RGBA                 target_color{0.f, 0.f, 0.f, 1.f};
    RGBA                 predicted_color{0.f, 0.f, 0.f, 1.f};
    // UV-space rectangle in the bottom-up convention used by TextureAsset.
    std::array<float, 4> uv_rect{0.f, 0.f, 0.f, 0.f};
    bool                 solid_anchor{false};
};

struct SimplePmCalibrationChart
{
    uint32_t                            schema_version{SIMPLE_PM_CALIBRATION_SCHEMA_VERSION};
    uint64_t                            signature{0};
    int                                 total_units{0};
    size_t                              total_recipe_count{0};
    size_t                              capacity{0};
    size_t                              columns{0};
    size_t                              rows{0};
    RGBA                                guard_color{1.f, 1.f, 1.f, 1.f};
    RGBA                                marker_dark{0.f, 0.f, 0.f, 1.f};
    RGBA                                marker_light{1.f, 1.f, 1.f, 1.f};
    TextureAsset                        texture;
    std::vector<SimplePmCalibrationPatch> patches;
    SimplePmCalibrationChartSettings    settings;

    bool valid() const;
};

struct SimplePmCalibrationObservation
{
    std::vector<uint8_t> component_units;
    RGBA                 expected_color{0.f, 0.f, 0.f, 1.f};
    RGBA                 measured_color{0.f, 0.f, 0.f, 1.f};
    float                confidence{0.f};
};

struct SimplePmCalibrationProfile
{
    uint32_t                                   schema_version{SIMPLE_PM_CALIBRATION_SCHEMA_VERSION};
    uint64_t                                   signature{0};
    ColorMixModel                              color_mix_model{ColorMixModel::FullSpectrumKmKs};
    int                                        total_units{0};
    std::vector<ContinuousColorComponent>      components;
    std::vector<SimplePmCalibrationObservation> observations;
};

struct SimplePmPhotoAnalysis
{
    bool                       success{false};
    std::string                error;
    float                      registration_confidence{0.f};
    size_t                     accepted_patch_count{0};
    size_t                     rejected_patch_count{0};
    SimplePmCalibrationProfile profile;

    explicit operator bool() const { return success; }
};

uint64_t simple_pm_calibration_signature(const std::vector<ContinuousColorComponent>& components,
                                         ColorMixModel color_mix_model);

// Builds the largest useful recipe set that fits on one continuous plaque.
// The complete candidate lattice is reduced with deterministic farthest-point
// sampling when it exceeds physical cell capacity.
SimplePmCalibrationChart make_simple_pm_calibration_chart(
    const std::vector<ContinuousColorComponent>& components,
    ColorMixModel                                color_mix_model = ColorMixModel::FullSpectrumKmKs,
    const SimplePmCalibrationChartSettings&      settings = {});

// The input buffer follows TextureAsset's bottom-up RGBA convention (the same
// convention returned by decode_imported_texture_rgba_from_file()).
SimplePmPhotoAnalysis analyze_simple_pm_calibration_photo(
    const std::vector<uint8_t>&                  photo_rgba,
    uint32_t                                     photo_width,
    uint32_t                                     photo_height,
    const std::vector<ContinuousColorComponent>& components,
    ColorMixModel                                color_mix_model = ColorMixModel::FullSpectrumKmKs,
    const SimplePmCalibrationChartSettings&      settings = {});

bool save_simple_pm_calibration_profile(const SimplePmCalibrationProfile& profile,
                                        std::string*                      saved_path = nullptr,
                                        std::string*                      error = nullptr);

// Clears the process-local profile cache after a photo has been imported.
void     reload_simple_pm_calibration_profiles();
uint64_t simple_pm_calibration_revision();

// Applies a locally saved measured residual field. The function is a no-op
// when no profile exactly matches the material tuple and color model.
RGBA apply_simple_pm_calibration(const std::vector<ContinuousColorComponent>& components,
                                 ColorMixModel                                color_mix_model,
                                 const std::vector<double>&                    normalized_weights,
                                 const RGBA&                                   predicted_color);

} // namespace Slic3r::ImageMap

#endif
