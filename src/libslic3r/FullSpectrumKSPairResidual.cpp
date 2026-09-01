#include "FullSpectrumKSPairResidual.hpp"
#include "FullSpectrumICCPolynomialEstimator.hpp"
#include "FullSpectrumMaterialDatabaseProfile.h"
#include "FullSpectrumMaterialHigherOrderProfile.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>

namespace Slic3r {

namespace {

namespace MaterialDatabaseData = FullSpectrumMaterialDatabaseProfileData;
namespace HigherOrderData     = FullSpectrumMaterialHigherOrderProfileData;

using Spectrum = std::array<double, MaterialDatabaseData::SPECTRUM_SIZE>;

constexpr bool material_database_grid_matches_estimator()
{
    if (MaterialDatabaseData::SPECTRUM_SIZE != FullSpectrumICCPolynomialEstimator::SPECTRUM_SIZE)
        return false;
    for (size_t i = 0; i < MaterialDatabaseData::SPECTRUM_SIZE; ++i) {
        const int expected_wavelength = FullSpectrumICCPolynomialEstimator::FIRST_WAVELENGTH_NM +
                                        static_cast<int>(i) * FullSpectrumICCPolynomialEstimator::WAVELENGTH_STEP_NM;
        if (MaterialDatabaseData::WAVELENGTH_NM[i] != expected_wavelength)
            return false;
    }
    return MaterialDatabaseData::WAVELENGTH_NM.back() == FullSpectrumICCPolynomialEstimator::LAST_WAVELENGTH_NM;
}

static_assert(material_database_grid_matches_estimator());
static_assert(HigherOrderData::SPECTRUM_SIZE == MaterialDatabaseData::SPECTRUM_SIZE);

constexpr double EPSILON                        = 1e-9;
constexpr double DEFAULT_REFERENCE_TD_MM        = 6.0;
constexpr double MIN_INFERRED_TD_TOLERANCE_MM   = 0.75;
constexpr double INFERRED_TD_TOLERANCE_FRACTION = 0.25;

struct Lab
{
    double L = 0.0;
    double a = 0.0;
    double b = 0.0;
};

struct CieObserverSample
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct MaterialKS
{
    Spectrum              ks {};
    std::optional<size_t> material_index;
    double                weight = 0.0;
};

static double clamp01(double value)
{
    return std::clamp(value, 0.0, 1.0);
}

static std::optional<std::string> normalize_hex_color(const std::string &hex)
{
    if (hex.size() != 7 || hex[0] != '#')
        return std::nullopt;

    std::string normalized = "#";
    normalized.reserve(7);
    for (size_t i = 1; i < 7; ++i) {
        const unsigned char ch = static_cast<unsigned char>(hex[i]);
        if (!std::isxdigit(ch))
            return std::nullopt;
        normalized.push_back(static_cast<char>(std::toupper(ch)));
    }
    return normalized;
}

static bool is_generic_ui_color(const std::string &normalized)
{
    static constexpr std::array<const char *, 8> generic_colors {{
        "#000000", "#FFFFFF", "#FF0000", "#00FF00", "#0000FF", "#00FFFF", "#FF00FF", "#FFFF00"
    }};
    return std::find_if(generic_colors.begin(), generic_colors.end(), [&normalized](const char *color) {
        return normalized == color;
    }) != generic_colors.end();
}

static std::optional<size_t> material_index_for_id(const std::optional<std::string> &material_id)
{
    if (!material_id || material_id->empty())
        return std::nullopt;

    for (size_t material = 0; material < MaterialDatabaseData::MATERIAL_COUNT; ++material) {
        if (*material_id == MaterialDatabaseData::MATERIAL_ID[material])
            return material;
    }
    return std::nullopt;
}

static std::optional<size_t> material_index_for_color(const std::string &hex,
                                                      const std::optional<double> &td_mm = std::nullopt,
                                                      const std::optional<std::string> &material_id = std::nullopt)
{
    const std::optional<std::string> normalized = normalize_hex_color(hex);
    if (!normalized)
        return std::nullopt;

    if (material_id && !material_id->empty()) {
        // A supplied ID is an explicit identity contract. Unknown/stale IDs
        // must fail closed instead of silently regaining a color-only match.
        return material_index_for_id(material_id);
    }

    const bool valid_td = td_mm && std::isfinite(*td_mm) && *td_mm > EPSILON;
    std::optional<size_t> first_match;
    std::optional<size_t> closest_td_match;
    double                closest_td_distance = std::numeric_limits<double>::max();
    for (size_t material = 0; material < MaterialDatabaseData::MATERIAL_COUNT; ++material) {
        if (*normalized != MaterialDatabaseData::MATERIAL_HEX[material])
            continue;

        // Pure UI colors are too common to identify a calibrated material.
        // They remain usable with an exact MATERIAL_ID, but never by color inference.
        if (is_generic_ui_color(*normalized))
            continue;

        if (!first_match)
            first_match = material;
        if (!valid_td)
            continue;

        const double native_td = MaterialDatabaseData::MATERIAL_TD_MM[material];
        const double distance  = std::abs(*td_mm - native_td);
        const double tolerance = std::max(MIN_INFERRED_TD_TOLERANCE_MM,
                                          INFERRED_TD_TOLERANCE_FRACTION * native_td);
        if (distance <= tolerance && distance < closest_td_distance) {
            closest_td_distance = distance;
            closest_td_match    = material;
        }
    }
    return valid_td ? closest_td_match : first_match;
}

static const Spectrum& material_ks(size_t material_index)
{
    return MaterialDatabaseData::MATERIAL_KS[material_index];
}

static double material_td_mm(size_t material_index)
{
    return MaterialDatabaseData::MATERIAL_TD_MM[material_index];
}

static double ks_from_reflectance(double reflectance)
{
    const double r = std::clamp(reflectance, 0.001, 0.999);
    return ((1.0 - r) * (1.0 - r)) / (2.0 * r);
}

static Spectrum estimated_ks_from_hex(const std::string &hex)
{
    Spectrum ks {};
    const std::optional<FullSpectrumICCPolynomialEstimator::Spectrum> reflectance =
        FullSpectrumICCPolynomialEstimator::estimate_reflectance_from_srgb_hex(hex);
    if (!reflectance) {
        ks.fill(ks_from_reflectance(0.02));
        return ks;
    }

    for (size_t i = 0; i < ks.size(); ++i)
        ks[i] = ks_from_reflectance((*reflectance)[i]);

    return ks;
}

static std::optional<std::vector<MaterialKS>> materials_from_colors(
    const std::vector<FullSpectrumKSPairResidualColorInput> &color_percents)
{
    std::vector<MaterialKS> materials;
    materials.reserve(color_percents.size());
    double total = 0.0;

    for (const FullSpectrumKSPairResidualColorInput &input : color_percents) {
        const int pct = input.percent;
        if (pct <= 0)
            continue;

        if (!normalize_hex_color(input.color_hex))
            return std::nullopt;

        const std::optional<size_t> material_index = material_index_for_color(input.color_hex, input.td_mm, input.material_id);

        double optical_strength = 1.0;
        if (input.td_mm && std::isfinite(*input.td_mm) && *input.td_mm > EPSILON) {
            // Pair residuals are trained at each material's measured profile TD.
            // Preserve the printed recipe at that native TD and apply only a
            // dimensionless relative correction when the runtime TD differs.
            const double reference_td = material_index ? material_td_mm(*material_index) : DEFAULT_REFERENCE_TD_MM;
            optical_strength          = reference_td / *input.td_mm;
        }

        const double weighted = static_cast<double>(pct) * optical_strength;
        MaterialKS material;
        material.material_index = material_index;
        material.weight       = weighted;
        if (material_index) {
            material.ks = material_ks(*material_index);
        } else {
            material.ks = estimated_ks_from_hex(input.color_hex);
        }
        materials.emplace_back(std::move(material));
        total += weighted;
    }

    if (total <= EPSILON)
        return std::nullopt;

    for (MaterialKS &material : materials)
        material.weight /= total;

    if (materials.size() < 2)
        return std::nullopt;

    return materials;
}

static double reflectance_from_ks(double ks)
{
    const double f = std::max(0.0, ks);
    return clamp01(1.0 + f - std::sqrt(f * f + 2.0 * f));
}

template <class PairResiduals>
static void apply_pair_residuals(const PairResiduals &pair_residuals,
                                 const std::array<double, MaterialDatabaseData::MATERIAL_COUNT> &composition,
                                 Spectrum &ks)
{
    for (const auto &pair : pair_residuals) {
        const double pa = composition[pair.material_a];
        const double pb = composition[pair.material_b];
        if (pa <= EPSILON || pb <= EPSILON)
            continue;

        const double d = (pa - pb) / (pa + pb);
        const double product = pa * pb;
        for (size_t wave = 0; wave < ks.size(); ++wave)
            ks[wave] += product * (pair.b0[wave] + pair.b1[wave] * d + pair.b2[wave] * d * d);
    }
}

static void apply_triple_residuals(
    const std::array<double, MaterialDatabaseData::MATERIAL_COUNT> &composition,
    Spectrum &ks)
{
    for (const HigherOrderData::TripleResidualCoefficients &triple : HigherOrderData::TRIPLE_RESIDUALS) {
        const double pa = composition[triple.materials[0]];
        const double pb = composition[triple.materials[1]];
        const double pc = composition[triple.materials[2]];
        if (pa <= EPSILON || pb <= EPSILON || pc <= EPSILON)
            continue;

        const std::array<double, HigherOrderData::TRIPLE_BASIS_SIZE> basis {{
            pa,
            pb,
            pc
        }};
        const double product = pa * pb * pc;
        for (size_t wave = 0; wave < ks.size(); ++wave) {
            double residual = 0.0;
            for (size_t term = 0; term < basis.size(); ++term)
                residual += basis[term] * triple.coefficients[term][wave];
            ks[wave] += product * residual;
        }
    }
}

static void apply_quadruple_residuals(
    const std::array<double, MaterialDatabaseData::MATERIAL_COUNT> &composition,
    Spectrum &ks)
{
    for (const HigherOrderData::QuadrupleResidualCoefficients &quadruple : HigherOrderData::QUADRUPLE_RESIDUALS) {
        const double pa = composition[quadruple.materials[0]];
        const double pb = composition[quadruple.materials[1]];
        const double pc = composition[quadruple.materials[2]];
        const double pd = composition[quadruple.materials[3]];
        if (pa <= EPSILON || pb <= EPSILON || pc <= EPSILON || pd <= EPSILON)
            continue;

        const std::array<double, HigherOrderData::QUADRUPLE_BASIS_SIZE> basis {{
            pa,
            pb,
            pc,
            pd
        }};
        const double product = pa * pb * pc * pd;
        for (size_t wave = 0; wave < ks.size(); ++wave) {
            double residual = 0.0;
            for (size_t term = 0; term < basis.size(); ++term)
                residual += basis[term] * quadruple.coefficients[term][wave];
            ks[wave] += product * residual;
        }
    }
}

static Spectrum predict_reflectance_spectrum(const std::vector<MaterialKS> &materials)
{
    Spectrum ks {};
    std::array<double, MaterialDatabaseData::MATERIAL_COUNT> material_composition {};

    for (const MaterialKS &material : materials) {
        if (material.weight <= EPSILON)
            continue;
        if (material.material_index)
            material_composition[*material.material_index] += material.weight;
        for (size_t wave = 0; wave < ks.size(); ++wave)
            ks[wave] += material.weight * material.ks[wave];
    }

    apply_pair_residuals(MaterialDatabaseData::PAIR_RESIDUALS, material_composition, ks);
    apply_triple_residuals(material_composition, ks);
    apply_quadruple_residuals(material_composition, ks);

    Spectrum reflectance {};
    for (size_t wave = 0; wave < reflectance.size(); ++wave)
        reflectance[wave] = reflectance_from_ks(ks[wave]);

    return reflectance;
}

static const std::array<double, MaterialDatabaseData::SPECTRUM_SIZE>& cie_d65_400_700_10nm()
{
    static const std::array<double, MaterialDatabaseData::SPECTRUM_SIZE> values = {
        82.7549, 91.4860, 93.4318, 86.6823, 104.8650, 117.0080, 117.8120, 114.8610,
        115.9230, 108.8110, 109.3540, 107.8020, 104.7900, 107.6890, 104.4050, 104.0460,
        100.0000, 96.3342, 95.7880, 88.6856, 90.0062, 89.5991, 87.6987, 83.2886,
        83.6992, 80.0268, 80.2146, 82.2778, 78.2842, 69.7213, 71.6091
    };
    return values;
}

static const std::array<CieObserverSample, MaterialDatabaseData::SPECTRUM_SIZE>& cie_observer_10deg_400_700_10nm()
{
    static const std::array<CieObserverSample, MaterialDatabaseData::SPECTRUM_SIZE> values = {{
        {0.019110, 0.002004, 0.086011}, {0.084736, 0.008756, 0.389366},
        {0.204492, 0.021391, 0.972542}, {0.314679, 0.038676, 1.553480},
        {0.383734, 0.062077, 1.967280}, {0.370702, 0.089456, 1.994800},
        {0.302273, 0.128201, 1.745370}, {0.195618, 0.185190, 1.317560},
        {0.080507, 0.253589, 0.772125}, {0.016172, 0.339133, 0.415254},
        {0.003816, 0.460777, 0.218502}, {0.037465, 0.606741, 0.112044},
        {0.117749, 0.761757, 0.060709}, {0.236491, 0.875211, 0.030451},
        {0.376772, 0.961988, 0.013676}, {0.529826, 0.991761, 0.003988},
        {0.705224, 0.997340, 0.000000}, {0.878655, 0.955552, 0.000000},
        {1.014160, 0.868934, 0.000000}, {1.118520, 0.777405, 0.000000},
        {1.124000, 0.658341, 0.000000}, {1.030480, 0.527963, 0.000000},
        {0.856297, 0.398057, 0.000000}, {0.647467, 0.283493, 0.000000},
        {0.431567, 0.179828, 0.000000}, {0.268329, 0.107633, 0.000000},
        {0.152568, 0.060281, 0.000000}, {0.081261, 0.031800, 0.000000},
        {0.040851, 0.015905, 0.000000}, {0.019941, 0.007749, 0.000000},
        {0.009577, 0.003718, 0.000000}
    }};
    return values;
}

static double lab_pivot_xyz(double value)
{
    constexpr double delta = 6.0 / 29.0;
    constexpr double delta3 = delta * delta * delta;
    return value > delta3 ? std::cbrt(value) : value / (3.0 * delta * delta) + 4.0 / 29.0;
}

static Lab lab_from_reflectance_spectrum(
    const std::array<double, MaterialDatabaseData::SPECTRUM_SIZE> &spectrum)
{
    const auto &d65 = cie_d65_400_700_10nm();
    const auto &cmf = cie_observer_10deg_400_700_10nm();

    double y_weight = 0.0;
    double xn_weight = 0.0;
    double zn_weight = 0.0;
    for (size_t i = 0; i < spectrum.size(); ++i) {
        y_weight += d65[i] * cmf[i].y;
        xn_weight += d65[i] * cmf[i].x;
        zn_weight += d65[i] * cmf[i].z;
    }

    const double k = 100.0 / y_weight;
    const double white_x = k * xn_weight;
    const double white_y = 100.0;
    const double white_z = k * zn_weight;

    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    for (size_t i = 0; i < spectrum.size(); ++i) {
        const double reflectance = std::max(0.0, spectrum[i]);
        x += reflectance * d65[i] * cmf[i].x;
        y += reflectance * d65[i] * cmf[i].y;
        z += reflectance * d65[i] * cmf[i].z;
    }
    x *= k;
    y *= k;
    z *= k;

    const double fx = lab_pivot_xyz(x / white_x);
    const double fy = lab_pivot_xyz(y / white_y);
    const double fz = lab_pivot_xyz(z / white_z);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

static double pivot_lab_to_xyz(double value)
{
    const double cubed = value * value * value;
    return cubed > 0.008856 ? cubed : (value - 16.0 / 116.0) / 7.787;
}

static double linear_to_srgb(double value)
{
    return value <= 0.0031308 ? 12.92 * value : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

static std::string lab_to_hex(const Lab &lab)
{
    constexpr double d65_10_x = 94.811;
    constexpr double d65_10_y = 100.0;
    constexpr double d65_10_z = 107.304;

    const double fy = (lab.L + 16.0) / 116.0;
    const double fx = lab.a / 500.0 + fy;
    const double fz = fy - lab.b / 200.0;

    const double x = d65_10_x * pivot_lab_to_xyz(fx) / 100.0;
    const double y = d65_10_y * pivot_lab_to_xyz(fy) / 100.0;
    const double z = d65_10_z * pivot_lab_to_xyz(fz) / 100.0;

    const double lr = 3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
    const double lg = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
    const double lb = 0.0556434 * x - 0.2040259 * y + 1.0572252 * z;

    const int r = std::clamp(static_cast<int>(std::round(clamp01(linear_to_srgb(lr)) * 255.0)), 0, 255);
    const int g = std::clamp(static_cast<int>(std::round(clamp01(linear_to_srgb(lg)) * 255.0)), 0, 255);
    const int b = std::clamp(static_cast<int>(std::round(clamp01(linear_to_srgb(lb)) * 255.0)), 0, 255);

    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
    return std::string(buf);
}

static std::optional<std::string> blend_from_colors(
    const std::vector<FullSpectrumKSPairResidualColorInput> &color_percents)
{
    const auto materials = materials_from_colors(color_percents);
    if (!materials)
        return std::nullopt;

    const auto spectrum = predict_reflectance_spectrum(*materials);
    return lab_to_hex(lab_from_reflectance_spectrum(spectrum));
}

} // namespace

std::optional<std::string> full_spectrum_ks_blend_color_multi(
    const std::vector<std::pair<std::string, int>> &color_percents)
{
    std::vector<FullSpectrumKSPairResidualColorInput> inputs;
    inputs.reserve(color_percents.size());
    for (const auto &[hex, pct] : color_percents)
        inputs.push_back({hex, pct, std::nullopt, std::nullopt});
    return blend_from_colors(inputs);
}

std::optional<std::string> full_spectrum_ks_blend_color_multi(
    const std::vector<FullSpectrumKSPairResidualColorInput> &color_percents)
{
    return blend_from_colors(color_percents);
}

std::optional<std::string> full_spectrum_ks_blend_color(const std::string &color_a,
                                                        const std::string &color_b,
                                                        int                ratio_a,
                                                        int                ratio_b)
{
    return full_spectrum_ks_blend_color(color_a, color_b, ratio_a, ratio_b, std::nullopt, std::nullopt);
}

std::optional<std::string> full_spectrum_ks_blend_color(const std::string           &color_a,
                                                        const std::string           &color_b,
                                                        int                          ratio_a,
                                                        int                          ratio_b,
                                                        const std::optional<double> &td_a_mm,
                                                        const std::optional<double> &td_b_mm)
{
    return blend_from_colors({
        {color_a, std::max(0, ratio_a), td_a_mm, std::nullopt},
        {color_b, std::max(0, ratio_b), td_b_mm, std::nullopt}
    });
}

bool full_spectrum_ks_profile_matches_color(const std::string &hex)
{
    return material_index_for_color(hex).has_value();
}

std::optional<double> full_spectrum_ks_profile_td_mm_for_color(const std::string &hex)
{
    const std::optional<size_t> material_index = material_index_for_color(hex);
    if (!material_index)
        return std::nullopt;
    return material_td_mm(*material_index);
}

const char* full_spectrum_ks_profile_id()
{
    return MaterialDatabaseData::PROFILE_ID;
}

std::size_t full_spectrum_ks_profile_material_count()
{
    return MaterialDatabaseData::MATERIAL_COUNT;
}

std::size_t full_spectrum_ks_profile_pair_count()
{
    return MaterialDatabaseData::PAIR_COUNT;
}

std::size_t full_spectrum_ks_profile_triple_count()
{
    return HigherOrderData::TRIPLE_COUNT;
}

std::size_t full_spectrum_ks_profile_quadruple_count()
{
    return HigherOrderData::QUADRUPLE_COUNT;
}

std::size_t full_spectrum_ks_profile_higher_order_sample_count()
{
    return HigherOrderData::TRIPLE_SAMPLE_COUNT + HigherOrderData::QUADRUPLE_SAMPLE_COUNT;
}

std::size_t full_spectrum_ks_profile_mixture_sample_count()
{
    return HigherOrderData::TOTAL_MIXTURE_SAMPLE_COUNT;
}

const char* full_spectrum_ks_profile_specular_mode()
{
    return MaterialDatabaseData::SPECULAR_MODE;
}

const char* full_spectrum_ks_profile_backing_condition()
{
    return MaterialDatabaseData::BACKING_CONDITION;
}

} // namespace Slic3r
