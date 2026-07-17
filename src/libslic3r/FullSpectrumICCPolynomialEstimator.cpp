#include "FullSpectrumICCPolynomialEstimator.hpp"

#include "FullSpectrumICCPolynomialProfile.h"

#include <array>
#include <cmath>

namespace Slic3r::FullSpectrumICCPolynomialEstimator {

namespace {

namespace ProfileData = FullSpectrumICCPolynomialProfileData;

static_assert(SPECTRUM_SIZE == ProfileData::SPECTRUM_SIZE);
static_assert(FIRST_WAVELENGTH_NM == ProfileData::FIRST_WAVELENGTH_NM);
static_assert(LAST_WAVELENGTH_NM == ProfileData::LAST_WAVELENGTH_NM);
static_assert(WAVELENGTH_STEP_NM == ProfileData::WAVELENGTH_STEP_NM);

struct Xyz
{
    double x;
    double y;
    double z;
};

static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    return -1;
}

static std::optional<std::array<unsigned char, 3>> rgb_from_hex(const std::string& hex)
{
    if (hex.size() != 7 || hex.front() != '#')
        return std::nullopt;

    std::array<unsigned char, 3> rgb{};
    for (size_t channel = 0; channel < rgb.size(); ++channel) {
        const int high = hex_nibble(hex[1 + 2 * channel]);
        const int low  = hex_nibble(hex[2 + 2 * channel]);
        if (high < 0 || low < 0)
            return std::nullopt;
        rgb[channel] = static_cast<unsigned char>((high << 4) | low);
    }
    return rgb;
}

static double srgb_to_linear(unsigned char value)
{
    const double srgb = static_cast<double>(value) / 255.0;
    return srgb <= 0.04045 ? srgb / 12.92 : std::pow((srgb + 0.055) / 1.055, 2.4);
}

static Xyz d50_xyz_from_srgb(const std::array<unsigned char, 3>& rgb)
{
    const double red   = srgb_to_linear(rgb[0]);
    const double green = srgb_to_linear(rgb[1]);
    const double blue  = srgb_to_linear(rgb[2]);

    // ICC sRGB colorimetric profile: linear sRGB to D65 XYZ.
    const Xyz d65{
        0.412348487282463 * red + 0.357601378848728 * green + 0.180450133868809 * blue,
        0.212617188755020 * red + 0.715202757697456 * green + 0.072180053547523 * blue,
        0.019328835341365 * red + 0.119200459616243 * green + 0.950370705042392 * blue,
    };

    // ICC c2sp linearized Bradford adaptation from D65 to the D50 PCS.
    return {
        100.0 * (1.047907381710167 * d65.x + 0.022933384554211 * d65.y - 0.050201634798010 * d65.z),
        100.0 * (0.029605959417717 * d65.x + 0.990456039910784 * d65.y - 0.017075529195870 * d65.z),
        100.0 * (-0.009246794326782 * d65.x + 0.015062680140149 * d65.y + 0.751791232609078 * d65.z),
    };
}

static std::array<double, ProfileData::TERM_COUNT> polynomial_terms(const Xyz& xyz)
{
    const double x2 = xyz.x * xyz.x;
    const double y2 = xyz.y * xyz.y;
    const double z2 = xyz.z * xyz.z;

    return {{
        1.0,        xyz.x,      xyz.y,         xyz.z,         x2,
        y2,         z2,         xyz.x * xyz.y, xyz.x * xyz.z, xyz.y * xyz.z,
        x2 * xyz.x, y2 * xyz.y, z2 * xyz.z,    x2 * xyz.y,    x2 * xyz.z,
        xyz.x * y2, xyz.x * z2, y2 * xyz.z,    xyz.y * z2,    xyz.x * xyz.y * xyz.z,
    }};
}

} // namespace

std::optional<Spectrum> estimate_reflectance_from_srgb_hex(const std::string& hex)
{
    const std::optional<std::array<unsigned char, 3>> rgb = rgb_from_hex(hex);
    if (!rgb)
        return std::nullopt;

    const std::array<double, ProfileData::TERM_COUNT> terms = polynomial_terms(d50_xyz_from_srgb(*rgb));

    Spectrum reflectance{};
    for (size_t wavelength = 0; wavelength < reflectance.size(); ++wavelength) {
        double estimate = 0.0;
        for (size_t term = 0; term < terms.size(); ++term)
            estimate += static_cast<double>(ProfileData::COEFFICIENTS[wavelength][term]) * terms[term];
        reflectance[wavelength] = estimate;
    }
    return reflectance;
}

} // namespace Slic3r::FullSpectrumICCPolynomialEstimator
