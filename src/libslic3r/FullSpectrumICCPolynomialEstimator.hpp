#ifndef slic3r_FullSpectrumICCPolynomialEstimator_hpp_
#define slic3r_FullSpectrumICCPolynomialEstimator_hpp_

#include <array>
#include <cstddef>
#include <optional>
#include <string>

namespace Slic3r::FullSpectrumICCPolynomialEstimator {

static constexpr std::size_t SPECTRUM_SIZE       = 31;
static constexpr int         FIRST_WAVELENGTH_NM = 400;
static constexpr int         LAST_WAVELENGTH_NM  = 700;
static constexpr int         WAVELENGTH_STEP_NM  = 10;
using Spectrum                                   = std::array<double, SPECTRUM_SIZE>;

// Estimates 400-700 nm reflectance at 10 nm intervals. Values are the raw
// polynomial result and may fall outside the physical [0, 1] range for colors
// outside the estimator's Munsell Glossy training domain.
std::optional<Spectrum> estimate_reflectance_from_srgb_hex(const std::string& hex);

} // namespace Slic3r::FullSpectrumICCPolynomialEstimator

#endif // slic3r_FullSpectrumICCPolynomialEstimator_hpp_
