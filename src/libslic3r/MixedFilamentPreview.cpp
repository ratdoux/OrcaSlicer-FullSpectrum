#include "MixedFilamentPreview.hpp"
#include "filament_mixer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <numeric>
#include <sstream>

namespace Slic3r {

namespace {

constexpr double TD99_OPTICAL_DEPTH = 4.6051701859880918; // ln(100)
constexpr double SIDEWALL_SURFACE_FILTER_FALLOFF_MM = 0.22;
constexpr size_t SIDEWALL_SURFACE_STRENGTH_NEIGHBOR_COUNT = 2;

struct SurfaceFilterParams
{
    double gain = 1.0;
    double max_strength = 0.60;
};

struct RGB
{
    int r = 0;
    int g = 0;
    int b = 0;
};

static double clamp01(double v) { return std::max(0.0, std::min(1.0, v)); }

static double srgb_to_linear(double c)
{
    c = clamp01(c);
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

static double linear_to_srgb(double c)
{
    c = clamp01(c);
    return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

static double linear_luminance(double r, double g, double b)
{
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

static double surface_filter_absorption(double opacity)
{
    return std::sqrt(clamp01(opacity));
}

static double surface_carrier_visibility(double opacity)
{
    return std::sqrt(1.0 - clamp01(opacity));
}

static RGB parse_hex_color(const std::string& hex)
{
    RGB c;
    if (hex.size() >= 7 && hex[0] == '#') {
        try {
            c.r = std::stoi(hex.substr(1, 2), nullptr, 16);
            c.g = std::stoi(hex.substr(3, 2), nullptr, 16);
            c.b = std::stoi(hex.substr(5, 2), nullptr, 16);
        } catch (...) {
            c = {};
        }
    }
    return c;
}

static std::string rgb_to_hex(const RGB& c)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", std::clamp(c.r, 0, 255), std::clamp(c.g, 0, 255), std::clamp(c.b, 0, 255));
    return std::string(buf);
}

static bool valid_material_id(unsigned int filament_id, const std::vector<MixedFilamentOpticalMaterial>& materials)
{
    return filament_id >= 1 && filament_id <= materials.size();
}

static double layer_opacity(const MixedFilamentSidewallSample& sample, const std::vector<MixedFilamentOpticalMaterial>& materials)
{
    if (!valid_material_id(sample.filament_id, materials))
        return 0.0;
    return mixed_filament_td99_opacity(sample.height_mm, materials[sample.filament_id - 1].td99_mm);
}

static double optical_depth_between_centers(const MixedFilamentSidewallSample&               target,
                                            const MixedFilamentSidewallSample&               contributor,
                                            const std::vector<MixedFilamentSidewallSample>&  region_samples,
                                            const std::vector<MixedFilamentOpticalMaterial>& materials)
{
    const double target_center      = target.z_bottom_mm + 0.5 * std::max(0.0, target.height_mm);
    const double contributor_center = contributor.z_bottom_mm + 0.5 * std::max(0.0, contributor.height_mm);
    const double lo                 = std::min(target_center, contributor_center);
    const double hi                 = std::max(target_center, contributor_center);

    if (hi <= lo)
        return 0.0;

    double depth = 0.0;
    for (const MixedFilamentSidewallSample& sample : region_samples) {
        if (!valid_material_id(sample.filament_id, materials))
            continue;

        const double td = materials[sample.filament_id - 1].td99_mm;
        if (td <= 0.0)
            continue;

        const double sample_lo = sample.z_bottom_mm;
        const double sample_hi = sample.z_bottom_mm + std::max(0.0, sample.height_mm);
        const double overlap   = std::min(hi, sample_hi) - std::max(lo, sample_lo);
        if (overlap > 0.0)
            depth += TD99_OPTICAL_DEPTH * overlap / td;
    }
    return depth;
}

static std::string blend_filament_mixer(const std::vector<std::pair<std::string, double>>& weighted_colors)
{
    double total_weight = 0.0;
    for (const auto& item : weighted_colors)
        total_weight += std::max(0.0, item.second);

    if (total_weight <= 0.0)
        return "#000000";

    unsigned char out_r              = 0;
    unsigned char out_g              = 0;
    unsigned char out_b              = 0;
    double        accumulated_weight = 0.0;
    bool          has_color          = false;
    double        avg_r_linear       = 0.0;
    double        avg_g_linear       = 0.0;
    double        avg_b_linear       = 0.0;

    for (const auto& [hex, raw_weight] : weighted_colors) {
        const double weight = std::max(0.0, raw_weight);
        if (weight <= 0.0)
            continue;

        const RGB           rgb = parse_hex_color(hex);
        const unsigned char r   = static_cast<unsigned char>(std::clamp(rgb.r, 0, 255));
        const unsigned char g   = static_cast<unsigned char>(std::clamp(rgb.g, 0, 255));
        const unsigned char b   = static_cast<unsigned char>(std::clamp(rgb.b, 0, 255));
        const double normalized_weight = weight / total_weight;
        avg_r_linear += normalized_weight * srgb_to_linear(double(r) / 255.0);
        avg_g_linear += normalized_weight * srgb_to_linear(double(g) / 255.0);
        avg_b_linear += normalized_weight * srgb_to_linear(double(b) / 255.0);

        if (!has_color) {
            out_r              = r;
            out_g              = g;
            out_b              = b;
            accumulated_weight = weight;
            has_color          = true;
            continue;
        }

        const double new_total = accumulated_weight + weight;
        if (new_total <= 0.0)
            continue;

        const float t = float(weight / new_total);
        filament_mixer_lerp(out_r, out_g, out_b, r, g, b, t, &out_r, &out_g, &out_b);
        accumulated_weight = new_total;
    }

    if (!has_color)
        return "#000000";

    // FilamentMixer is useful for pigment hue, but weighted optical stacks can
    // become much darker than the contributing reflectances imply. Keep the hue
    // while preventing an excessive luminance collapse.
    double out_r_linear = srgb_to_linear(double(out_r) / 255.0);
    double out_g_linear = srgb_to_linear(double(out_g) / 255.0);
    double out_b_linear = srgb_to_linear(double(out_b) / 255.0);
    const double avg_luminance = linear_luminance(avg_r_linear, avg_g_linear, avg_b_linear);
    const double out_luminance = linear_luminance(out_r_linear, out_g_linear, out_b_linear);
    const double min_luminance = 0.82 * avg_luminance;
    if (out_luminance > 1e-6 && out_luminance < min_luminance) {
        const double scale = std::min(3.0, min_luminance / out_luminance);
        out_r_linear = clamp01(out_r_linear * scale);
        out_g_linear = clamp01(out_g_linear * scale);
        out_b_linear = clamp01(out_b_linear * scale);
    }

    return rgb_to_hex({
        std::clamp(int(std::lround(linear_to_srgb(out_r_linear) * 255.0)), 0, 255),
        std::clamp(int(std::lround(linear_to_srgb(out_g_linear) * 255.0)), 0, 255),
        std::clamp(int(std::lround(linear_to_srgb(out_b_linear) * 255.0)), 0, 255)
    });
}

static std::string blend_yule_nielsen(const std::vector<std::pair<std::string, double>>& weighted_colors, double n_value)
{
    double total_weight = 0.0;
    for (const auto& item : weighted_colors)
        total_weight += std::max(0.0, item.second);

    if (total_weight <= 0.0)
        return "#000000";

    const double n = std::max(0.25, n_value);
    double       r = 0.0;
    double       g = 0.0;
    double       b = 0.0;

    for (const auto& [hex, raw_weight] : weighted_colors) {
        const double weight = std::max(0.0, raw_weight);
        if (weight <= 0.0)
            continue;

        const RGB    rgb               = parse_hex_color(hex);
        const double normalized_weight = weight / total_weight;
        r += normalized_weight * std::pow(srgb_to_linear(double(rgb.r) / 255.0), 1.0 / n);
        g += normalized_weight * std::pow(srgb_to_linear(double(rgb.g) / 255.0), 1.0 / n);
        b += normalized_weight * std::pow(srgb_to_linear(double(rgb.b) / 255.0), 1.0 / n);
    }

    auto to_u8 = [n](double v) {
        return std::clamp(int(std::lround(linear_to_srgb(std::pow(clamp01(v), n)) * 255.0)), 0, 255);
    };

    return rgb_to_hex({to_u8(r), to_u8(g), to_u8(b)});
}

static std::string blend_weighted_colors(const std::vector<std::pair<std::string, double>>& weighted_colors,
                                         const MixedFilamentSidewallPredictionSettings&     settings)
{
    if (settings.blend_model == MixedFilamentSidewallBlendModel::TdYuleNielsen)
        return blend_yule_nielsen(weighted_colors, settings.yule_nielsen_n);
    return blend_filament_mixer(weighted_colors);
}

static SurfaceFilterParams surface_filter_params(MixedFilamentSidewallBlendModel model)
{
    switch (model) {
    case MixedFilamentSidewallBlendModel::TdFilamentMixer:
        return { 1.70, 0.74 };
    case MixedFilamentSidewallBlendModel::TdYuleNielsen:
        return { 1.45, 0.68 };
    case MixedFilamentSidewallBlendModel::Legacy:
    default:
        return {};
    }
}

static std::string blend_carrier_with_weighted_neighbors(const std::string& carrier,
                                                          const std::vector<std::pair<std::string, double>>& weighted_filters,
                                                          double strength_weight,
                                                          MixedFilamentSidewallBlendModel model)
{
    const RGB carrier_rgb = parse_hex_color(carrier);

    double total_weight = 0.0;
    double filter_r     = 0.0;
    double filter_g     = 0.0;
    double filter_b     = 0.0;
    for (const auto& [hex, raw_weight] : weighted_filters) {
        const double weight = std::max(0.0, raw_weight);
        if (weight <= 0.0)
            continue;

        const RGB rgb = parse_hex_color(hex);
        filter_r += weight * srgb_to_linear(double(rgb.r) / 255.0);
        filter_g += weight * srgb_to_linear(double(rgb.g) / 255.0);
        filter_b += weight * srgb_to_linear(double(rgb.b) / 255.0);
        total_weight += weight;
    }

    if (total_weight <= 0.0)
        return carrier;

    filter_r /= total_weight;
    filter_g /= total_weight;
    filter_b /= total_weight;

    // Sidewall bleed is a reflected/scattered apparent-color contribution, not
    // a pure absorption filter. A pure filter cannot add missing channels, so
    // saturated colors such as magenta would never visibly pick up yellow.
    const SurfaceFilterParams params = surface_filter_params(model);
    constexpr double carrier_surface_weight = 1.0;
    const double strength_base = std::max(0.0, strength_weight);
    const double strength      = std::clamp(params.gain * strength_base / (carrier_surface_weight + strength_base),
                                           0.0,
                                           params.max_strength);
    const double carrier_r = srgb_to_linear(double(carrier_rgb.r) / 255.0);
    const double carrier_g = srgb_to_linear(double(carrier_rgb.g) / 255.0);
    const double carrier_b = srgb_to_linear(double(carrier_rgb.b) / 255.0);
    const double r         = carrier_r * (1.0 - strength) + filter_r * strength;
    const double g         = carrier_g * (1.0 - strength) + filter_g * strength;
    const double b         = carrier_b * (1.0 - strength) + filter_b * strength;

    return rgb_to_hex({
        std::clamp(int(std::lround(linear_to_srgb(r) * 255.0)), 0, 255),
        std::clamp(int(std::lround(linear_to_srgb(g) * 255.0)), 0, 255),
        std::clamp(int(std::lround(linear_to_srgb(b) * 255.0)), 0, 255)
    });
}

static std::vector<MixedFilamentOpticalMaterial> make_materials(const std::vector<std::string>& physical_colors,
                                                                const std::vector<double>&      physical_td99_mm)
{
    std::vector<MixedFilamentOpticalMaterial> materials;
    materials.reserve(physical_colors.size());
    for (size_t i = 0; i < physical_colors.size(); ++i) {
        const double td = i < physical_td99_mm.size() ? physical_td99_mm[i] : 0.0;
        materials.push_back({physical_colors[i], td});
    }
    return materials;
}

struct SequenceOptics
{
    std::vector<std::string> colors;
    std::vector<double>      center_depths;
    std::vector<double>      center_z_mm;
    std::vector<double>      heights_mm;
    std::vector<double>      opacities;
    bool                     valid { false };
};

static SequenceOptics build_sequence_optics(const std::vector<unsigned int>& sequence,
                                            const std::vector<std::string>&  physical_colors,
                                            const std::vector<double>&       physical_td99_mm,
                                            double                           layer_height_mm,
                                            const std::vector<double>*       layer_heights_mm = nullptr)
{
    SequenceOptics optics;
    if (sequence.empty())
        return optics;

    const double safe_layer_height = std::max(0.001, layer_height_mm);
    optics.colors.reserve(sequence.size());
    optics.center_depths.reserve(sequence.size());
    optics.center_z_mm.reserve(sequence.size());
    optics.heights_mm.reserve(sequence.size());
    optics.opacities.reserve(sequence.size());

    double accumulated_depth  = 0.0;
    double accumulated_height = 0.0;
    for (size_t idx = 0; idx < sequence.size(); ++idx) {
        const unsigned int filament_id = sequence[idx];
        if (filament_id == 0 || filament_id > physical_colors.size() || filament_id > physical_td99_mm.size() ||
            physical_td99_mm[filament_id - 1] <= 0.0)
            return {};

        const double sample_height = layer_heights_mm != nullptr && idx < layer_heights_mm->size() && (*layer_heights_mm)[idx] > 0.0 ?
                                         std::max(0.001, (*layer_heights_mm)[idx]) :
                                         safe_layer_height;
        const double layer_depth = TD99_OPTICAL_DEPTH * sample_height / physical_td99_mm[filament_id - 1];
        optics.colors.emplace_back(physical_colors[filament_id - 1]);
        optics.center_depths.emplace_back(accumulated_depth + 0.5 * layer_depth);
        optics.center_z_mm.emplace_back(accumulated_height + 0.5 * sample_height);
        optics.heights_mm.emplace_back(sample_height);
        optics.opacities.emplace_back(1.0 - std::exp(-layer_depth));
        accumulated_depth += layer_depth;
        accumulated_height += sample_height;
    }

    optics.valid = true;
    return optics;
}

static std::string predict_sequence_color_at(const SequenceOptics&                         optics,
                                             size_t                                        target_index,
                                             const MixedFilamentSidewallPredictionSettings& settings,
                                             const std::string&                            fallback)
{
    if (!optics.valid || target_index >= optics.colors.size())
        return fallback;

    const double target_depth = optics.center_depths[target_index];
    std::vector<std::pair<std::string, double>> weighted_colors;
    weighted_colors.reserve(optics.colors.size());

    for (size_t i = 0; i < optics.colors.size(); ++i) {
        const double weight = optics.opacities[i] * std::exp(-std::abs(optics.center_depths[i] - target_depth));
        if (weight < settings.min_contribution)
            continue;
        weighted_colors.emplace_back(optics.colors[i], weight);
    }

    return weighted_colors.empty() ? fallback : blend_weighted_colors(weighted_colors, settings);
}

static std::string predict_sequence_surface_color_at(const std::vector<unsigned int>&               sequence,
                                                     const SequenceOptics&                         optics,
                                                     size_t                                        target_index,
                                                     const MixedFilamentSidewallPredictionSettings& settings,
                                                     const std::string&                            fallback)
{
    if (!optics.valid || target_index >= optics.colors.size() || target_index >= sequence.size())
        return fallback;

    const unsigned int target_filament_id = sequence[target_index];
    const std::string& carrier_color      = optics.colors[target_index];
    const double       target_z           = target_index < optics.center_z_mm.size() ? optics.center_z_mm[target_index] : 0.0;
    const double       target_visibility =
        target_index < optics.opacities.size() ? surface_carrier_visibility(optics.opacities[target_index]) : 1.0;

    std::vector<std::pair<std::string, double>> weighted_filters;
    weighted_filters.reserve(optics.colors.size());
    std::vector<double> strength_weights;
    strength_weights.reserve(optics.colors.size());
    double tint_weight_total = 0.0;

    for (size_t i = 0; i < optics.colors.size() && i < sequence.size(); ++i) {
        if (i == target_index || sequence[i] == target_filament_id)
            continue;

        const double contributor_z      = i < optics.center_z_mm.size() ? optics.center_z_mm[i] : target_z;
        const double layer_distance_mm  = std::abs(contributor_z - target_z);
        const double weight =
            target_visibility * surface_filter_absorption(optics.opacities[i]) *
            std::exp(-layer_distance_mm / SIDEWALL_SURFACE_FILTER_FALLOFF_MM);
        if (weight < settings.min_contribution)
            continue;

        tint_weight_total += weight;
        strength_weights.emplace_back(weight);
        weighted_filters.emplace_back(optics.colors[i], weight);
    }

    if (weighted_filters.empty() || tint_weight_total <= 0.0)
        return carrier_color.empty() ? fallback : carrier_color;

    std::sort(strength_weights.begin(), strength_weights.end(), std::greater<double>());
    double tint_strength_weight = 0.0;
    for (size_t i = 0; i < std::min(SIDEWALL_SURFACE_STRENGTH_NEIGHBOR_COUNT, strength_weights.size()); ++i)
        tint_strength_weight += strength_weights[i];

    return blend_carrier_with_weighted_neighbors(
        carrier_color.empty() ? fallback : carrier_color, weighted_filters, tint_strength_weight, settings.blend_model);
}

static bool materials_cover_sequence(const std::vector<unsigned int>& sequence, const std::vector<double>& physical_td99_mm)
{
    if (sequence.empty())
        return false;
    for (const unsigned int id : sequence) {
        if (id == 0 || id > physical_td99_mm.size() || physical_td99_mm[id - 1] <= 0.0)
            return false;
    }
    return true;
}

static std::string normalized_model_value(const std::string& value)
{
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char ch : value) {
        if (std::isalnum(ch))
            normalized.push_back(static_cast<char>(std::tolower(ch)));
    }
    return normalized;
}

} // namespace

MixedFilamentSidewallBlendModel mixed_filament_sidewall_blend_model_from_string(const std::string& value)
{
    const std::string normalized = normalized_model_value(value);
    if (normalized == "tdfilamentmixer")
        return MixedFilamentSidewallBlendModel::TdFilamentMixer;
    if (normalized == "tdyulenielsen")
        return MixedFilamentSidewallBlendModel::TdYuleNielsen;
    return MixedFilamentSidewallBlendModel::Legacy;
}

std::string mixed_filament_sidewall_blend_model_to_string(MixedFilamentSidewallBlendModel model)
{
    switch (model) {
    case MixedFilamentSidewallBlendModel::TdFilamentMixer: return "td_filament_mixer";
    case MixedFilamentSidewallBlendModel::TdYuleNielsen: return "td_yule_nielsen";
    case MixedFilamentSidewallBlendModel::Legacy: break;
    }
    return "legacy";
}

double mixed_filament_td99_transmittance(double thickness_mm, double td99_mm)
{
    if (td99_mm <= 0.0)
        return 1.0;
    return std::exp(-TD99_OPTICAL_DEPTH * std::max(0.0, thickness_mm) / td99_mm);
}

double mixed_filament_td99_opacity(double thickness_mm, double td99_mm)
{
    return 1.0 - mixed_filament_td99_transmittance(thickness_mm, td99_mm);
}

bool mixed_filament_sidewall_prediction_available(const std::vector<unsigned int>& sequence,
                                                  const std::vector<double>&       physical_td99_mm,
                                                  MixedFilamentSidewallBlendModel  model)
{
    return model != MixedFilamentSidewallBlendModel::Legacy && materials_cover_sequence(sequence, physical_td99_mm);
}

std::vector<std::string> predict_mixed_filament_sidewall_sample_colors(const std::vector<MixedFilamentSidewallSample>&  samples,
                                                                       const std::vector<MixedFilamentOpticalMaterial>& materials,
                                                                       const MixedFilamentSidewallPredictionSettings&   settings)
{
    std::vector<std::string> out;
    out.reserve(samples.size());

    if (settings.blend_model == MixedFilamentSidewallBlendModel::Legacy) {
        for (const MixedFilamentSidewallSample& sample : samples)
            out.push_back(valid_material_id(sample.filament_id, materials) ? materials[sample.filament_id - 1].color : "#000000");
        return out;
    }

    for (const MixedFilamentSidewallSample& target : samples) {
        if (!valid_material_id(target.filament_id, materials) || materials[target.filament_id - 1].td99_mm <= 0.0) {
            out.push_back(valid_material_id(target.filament_id, materials) ? materials[target.filament_id - 1].color : "#000000");
            continue;
        }

        std::vector<MixedFilamentSidewallSample> region_samples;
        region_samples.reserve(samples.size());
        for (const MixedFilamentSidewallSample& sample : samples) {
            if (sample.spatial_region_id == target.spatial_region_id)
                region_samples.push_back(sample);
        }
        std::sort(region_samples.begin(), region_samples.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.z_bottom_mm != rhs.z_bottom_mm)
                return lhs.z_bottom_mm < rhs.z_bottom_mm;
            return lhs.layer_index < rhs.layer_index;
        });

        std::vector<std::pair<std::string, double>> weighted_colors;
        weighted_colors.reserve(region_samples.size());

        for (const MixedFilamentSidewallSample& contributor : region_samples) {
            if (!valid_material_id(contributor.filament_id, materials))
                continue;

            const double opacity = layer_opacity(contributor, materials);
            if (opacity <= 0.0)
                continue;

            const double depth  = optical_depth_between_centers(target, contributor, region_samples, materials);
            const double weight = opacity * std::exp(-depth);
            if (weight < settings.min_contribution)
                continue;

            weighted_colors.emplace_back(materials[contributor.filament_id - 1].color, weight);
        }

        if (weighted_colors.empty())
            out.push_back(materials[target.filament_id - 1].color);
        else
            out.push_back(blend_weighted_colors(weighted_colors, settings));
    }

    return out;
}

std::vector<std::string> predict_mixed_filament_sequence_apparent_colors(const std::vector<unsigned int>&               sequence,
                                                                         const std::vector<std::string>&                physical_colors,
                                                                         const std::vector<double>&                     physical_td99_mm,
                                                                         double                                         layer_height_mm,
                                                                         const MixedFilamentSidewallPredictionSettings& settings)
{
    if (!mixed_filament_sidewall_prediction_available(sequence, physical_td99_mm, settings.blend_model))
        return {};

    const SequenceOptics optics = build_sequence_optics(sequence, physical_colors, physical_td99_mm, layer_height_mm);
    if (!optics.valid)
        return {};

    std::vector<std::string> out;
    out.reserve(sequence.size());
    for (size_t target_index = 0; target_index < sequence.size(); ++target_index) {
        const unsigned int filament_id = sequence[target_index];
        const std::string fallback =
            filament_id >= 1 && filament_id <= physical_colors.size() ? physical_colors[filament_id - 1] : "#000000";
        out.push_back(predict_sequence_color_at(optics, target_index, settings, fallback));
    }
    return out;
}

std::string predict_mixed_filament_sequence_apparent_color_at(const std::vector<unsigned int>&               sequence,
                                                              size_t                                         target_index,
                                                              const std::vector<std::string>&                physical_colors,
                                                              const std::vector<double>&                     physical_td99_mm,
                                                              double                                         layer_height_mm,
                                                              const MixedFilamentSidewallPredictionSettings& settings,
                                                              const std::string&                             fallback)
{
    if (target_index >= sequence.size() ||
        !mixed_filament_sidewall_prediction_available(sequence, physical_td99_mm, settings.blend_model))
        return fallback;

    const SequenceOptics optics = build_sequence_optics(sequence, physical_colors, physical_td99_mm, layer_height_mm);
    if (!optics.valid)
        return fallback;

    const unsigned int filament_id = sequence[target_index];
    const std::string target_fallback =
        filament_id >= 1 && filament_id <= physical_colors.size() ? physical_colors[filament_id - 1] : fallback;
    return predict_sequence_color_at(optics, target_index, settings, target_fallback);
}

std::string predict_mixed_filament_sequence_surface_color_at(const std::vector<unsigned int>&               sequence,
                                                             size_t                                         target_index,
                                                             const std::vector<std::string>&                physical_colors,
                                                             const std::vector<double>&                     physical_td99_mm,
                                                             double                                         layer_height_mm,
                                                             const MixedFilamentSidewallPredictionSettings& settings,
                                                             const std::string&                             fallback)
{
    if (target_index >= sequence.size() ||
        !mixed_filament_sidewall_prediction_available(sequence, physical_td99_mm, settings.blend_model))
        return fallback;

    const SequenceOptics optics = build_sequence_optics(sequence, physical_colors, physical_td99_mm, layer_height_mm);
    if (!optics.valid)
        return fallback;

    const unsigned int filament_id = sequence[target_index];
    const std::string target_fallback =
        filament_id >= 1 && filament_id <= physical_colors.size() ? physical_colors[filament_id - 1] : fallback;
    return predict_sequence_surface_color_at(sequence, optics, target_index, settings, target_fallback);
}

std::string predict_mixed_filament_sequence_surface_color_at(const std::vector<unsigned int>&               sequence,
                                                             const std::vector<double>&                     layer_heights_mm,
                                                             size_t                                         target_index,
                                                             const std::vector<std::string>&                physical_colors,
                                                             const std::vector<double>&                     physical_td99_mm,
                                                             double                                         layer_height_mm,
                                                             const MixedFilamentSidewallPredictionSettings& settings,
                                                             const std::string&                             fallback)
{
    if (target_index >= sequence.size() ||
        !mixed_filament_sidewall_prediction_available(sequence, physical_td99_mm, settings.blend_model))
        return fallback;

    const SequenceOptics optics =
        build_sequence_optics(sequence, physical_colors, physical_td99_mm, layer_height_mm, &layer_heights_mm);
    if (!optics.valid)
        return fallback;

    const unsigned int filament_id = sequence[target_index];
    const std::string target_fallback =
        filament_id >= 1 && filament_id <= physical_colors.size() ? physical_colors[filament_id - 1] : fallback;
    return predict_sequence_surface_color_at(sequence, optics, target_index, settings, target_fallback);
}

std::string predict_mixed_filament_sequence_aggregate_color(const std::vector<unsigned int>&               sequence,
                                                            const std::vector<std::string>&                physical_colors,
                                                            const std::vector<double>&                     physical_td99_mm,
                                                            double                                         layer_height_mm,
                                                            const MixedFilamentSidewallPredictionSettings& settings,
                                                            const std::string&                             fallback)
{
    const std::vector<std::string> apparent_colors = predict_mixed_filament_sequence_apparent_colors(sequence, physical_colors,
                                                                                                     physical_td99_mm, layer_height_mm,
                                                                                                     settings);
    if (apparent_colors.empty())
        return fallback;

    std::vector<std::pair<std::string, double>> weighted_colors;
    weighted_colors.reserve(apparent_colors.size());
    for (const std::string& color : apparent_colors)
        weighted_colors.emplace_back(color, 1.0);
    return blend_weighted_colors(weighted_colors, settings);
}

} // namespace Slic3r
