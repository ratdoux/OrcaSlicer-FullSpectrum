#ifndef slic3r_FullSpectrumKSPairResidual_hpp_
#define slic3r_FullSpectrumKSPairResidual_hpp_

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

struct FullSpectrumKSPairResidualColorInput
{
    std::string                color_hex;
    int                        percent = 0;
    std::optional<double>      td_mm;
    std::optional<std::string> material_id;
};

// Spectral sidewall predictor. Exact stable material IDs and unambiguous measured
// colors in the embedded 0.08 mm SCE black-backed database use measured anchors
// and learned pair residuals. Generic or unknown valid hex colors are converted to
// estimated anchor spectra and mixed with plain KM/K-S so changing one filament
// color does not silently fall back to the legacy RGB mixer.
std::optional<std::string> full_spectrum_ks_blend_color_multi(
    const std::vector<std::pair<std::string, int>> &color_percents);

std::optional<std::string> full_spectrum_ks_blend_color_multi(
    const std::vector<FullSpectrumKSPairResidualColorInput> &color_percents);

std::optional<std::string> full_spectrum_ks_blend_color(const std::string &color_a,
                                                        const std::string &color_b,
                                                        int                ratio_a,
                                                        int                ratio_b);

std::optional<std::string> full_spectrum_ks_blend_color(const std::string           &color_a,
                                                        const std::string           &color_b,
                                                        int                          ratio_a,
                                                        int                          ratio_b,
                                                        const std::optional<double> &td_a_mm,
                                                        const std::optional<double> &td_b_mm);

bool full_spectrum_ks_profile_matches_color(const std::string &hex);

std::optional<double> full_spectrum_ks_profile_td_mm_for_color(const std::string &hex);

const char* full_spectrum_ks_profile_id();
std::size_t full_spectrum_ks_profile_material_count();
std::size_t full_spectrum_ks_profile_pair_count();
const char* full_spectrum_ks_profile_specular_mode();
const char* full_spectrum_ks_profile_backing_condition();

} // namespace Slic3r

#endif // slic3r_FullSpectrumKSPairResidual_hpp_
