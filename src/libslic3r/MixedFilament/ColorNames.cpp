#include "ColorNames.hpp"
#include "Internal.hpp"
#include "../MixedFilament.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace Slic3r {
namespace ColorNames {

namespace {

struct LabColor
{
    float L = 0.f;
    float a = 0.f;
    float b = 0.f;
};

inline LabColor srgb_to_lab(uint8_t r8, uint8_t g8, uint8_t b8)
{
    auto pivot_rgb = [](float c) -> float {
        return (c > 0.04045f) ? std::pow((c + 0.055f) / 1.055f, 2.4f) : (c / 12.92f);
    };

    const float r_lin = pivot_rgb(float(r8) / 255.0f);
    const float g_lin = pivot_rgb(float(g8) / 255.0f);
    const float b_lin = pivot_rgb(float(b8) / 255.0f);

    // Observer: 2 deg, Illuminant: D65
    const float x = (r_lin * 0.4124564f + g_lin * 0.3575761f + b_lin * 0.1804375f) / 0.95047f;
    const float y = (r_lin * 0.2126729f + g_lin * 0.7151522f + b_lin * 0.0721750f) / 1.00000f;
    const float z = (r_lin * 0.0193339f + g_lin * 0.1191920f + b_lin * 0.9503041f) / 1.08883f;

    auto pivot_xyz = [](float t) -> float {
        constexpr float epsilon = 216.0f / 24389.0f; // 0.008856
        constexpr float kappa   = 24389.0f / 27.0f;   // 903.3
        return (t > epsilon) ? std::cbrt(t) : ((kappa * t + 16.0f) / 116.0f);
    };

    const float fx = pivot_xyz(x);
    const float fy = pivot_xyz(y);
    const float fz = pivot_xyz(z);

    LabColor lab;
    lab.L = 116.0f * fy - 16.0f;
    lab.a = 500.0f * (fx - fy);
    lab.b = 200.0f * (fy - fz);
    return lab;
}

// CIEDE2000 color difference formula
float delta_e00(const LabColor& lab1, const LabColor& lab2)
{
    constexpr float k_pi = 3.14159265358979323846f;
    constexpr float deg2rad = k_pi / 180.0f;
    constexpr float rad2deg = 180.0f / k_pi;

    const float L1 = lab1.L, a1 = lab1.a, b1 = lab1.b;
    const float L2 = lab2.L, a2 = lab2.a, b2 = lab2.b;

    const float C1 = std::sqrt(a1 * a1 + b1 * b1);
    const float C2 = std::sqrt(a2 * a2 + b2 * b2);
    const float Cbar = (C1 + C2) * 0.5f;

    const float Cbar7 = std::pow(Cbar, 7.0f);
    constexpr float k25_7 = 6103515625.0f; // 25^7
    const float G = 0.5f * (1.0f - std::sqrt(Cbar7 / (Cbar7 + k25_7)));

    const float a1_prime = (1.0f + G) * a1;
    const float a2_prime = (1.0f + G) * a2;

    const float C1_prime = std::sqrt(a1_prime * a1_prime + b1 * b1);
    const float C2_prime = std::sqrt(a2_prime * a2_prime + b2 * b2);

    auto compute_h_prime = [&](float a_p, float b_p) -> float {
        if (std::abs(a_p) < 1e-6f && std::abs(b_p) < 1e-6f)
            return 0.0f;
        float h = std::atan2(b_p, a_p) * rad2deg;
        if (h < 0.0f)
            h += 360.0f;
        return h;
    };

    const float h1_prime = compute_h_prime(a1_prime, b1);
    const float h2_prime = compute_h_prime(a2_prime, b2);

    const float delta_L_prime = L2 - L1;
    const float delta_C_prime = C2_prime - C1_prime;

    float delta_h_prime = 0.0f;
    if (C1_prime * C2_prime > 1e-6f) {
        float diff = h2_prime - h1_prime;
        if (std::abs(diff) <= 180.0f)
            delta_h_prime = diff;
        else if (diff > 180.0f)
            delta_h_prime = diff - 360.0f;
        else
            delta_h_prime = diff + 360.0f;
    }
    const float delta_H_prime = 2.0f * std::sqrt(C1_prime * C2_prime) * std::sin(delta_h_prime * 0.5f * deg2rad);

    const float Lbar_prime = (L1 + L2) * 0.5f;
    const float Cbar_prime = (C1_prime + C2_prime) * 0.5f;

    float Hbar_prime = 0.0f;
    if (C1_prime * C2_prime > 1e-6f) {
        float sum = h1_prime + h2_prime;
        if (std::abs(h1_prime - h2_prime) <= 180.0f)
            Hbar_prime = sum * 0.5f;
        else if (sum < 360.0f)
            Hbar_prime = (sum + 360.0f) * 0.5f;
        else
            Hbar_prime = (sum - 360.0f) * 0.5f;
    } else {
        Hbar_prime = h1_prime + h2_prime;
    }

    const float T = 1.0f - 0.17f * std::cos((Hbar_prime - 30.0f) * deg2rad)
                         + 0.24f * std::cos((2.0f * Hbar_prime) * deg2rad)
                         + 0.32f * std::cos((3.0f * Hbar_prime + 6.0f) * deg2rad)
                         - 0.20f * std::cos((4.0f * Hbar_prime - 63.0f) * deg2rad);

    const float L_minus_50_sq = (Lbar_prime - 50.0f) * (Lbar_prime - 50.0f);
    const float S_L = 1.0f + (0.015f * L_minus_50_sq) / std::sqrt(20.0f + L_minus_50_sq);
    const float S_C = 1.0f + 0.045f * Cbar_prime;
    const float S_H = 1.0f + 0.015f * Cbar_prime * T;

    const float delta_theta = 30.0f * std::exp(-std::pow((Hbar_prime - 275.0f) / 25.0f, 2.0f));
    const float Cbar_prime7 = std::pow(Cbar_prime, 7.0f);
    const float R_C = 2.0f * std::sqrt(Cbar_prime7 / (Cbar_prime7 + k25_7));
    const float R_T = -R_C * std::sin(2.0f * delta_theta * deg2rad);

    const float term_L = delta_L_prime / S_L;
    const float term_C = delta_C_prime / S_C;
    const float term_H = delta_H_prime / S_H;

    return std::sqrt(term_L * term_L + term_C * term_C + term_H * term_H + R_T * term_C * term_H);
}

struct CSSKeywordEntry
{
    const char* name;
    uint8_t     r, g, b;
    LabColor    lab;
};

// 148 standard W3C CSS / SVG Color Keywords
// https://www.w3.org/wiki/CSS/Properties/color/keywords
static const std::vector<CSSKeywordEntry>& get_css_keyword_database()
{
    static const std::vector<CSSKeywordEntry> keywords = [] {
        struct RawEntry { const char* name; uint8_t r, g, b; };
        static constexpr RawEntry raw[] = {
            { "Alice Blue",             0xF0, 0xF8, 0xFF },
            { "Antique White",          0xFA, 0xEB, 0xD7 },
            { "Aqua",                   0x00, 0xFF, 0xFF },
            { "Aquamarine",             0x7F, 0xFF, 0xD4 },
            { "Azure",                  0xF0, 0xFF, 0xFF },
            { "Beige",                  0xF5, 0xF5, 0xDC },
            { "Bisque",                 0xFF, 0xE4, 0xC4 },
            { "Black",                  0x00, 0x00, 0x00 },
            { "Blanched Almond",        0xFF, 0xEB, 0xCD },
            { "Blue",                   0x00, 0x00, 0xFF },
            { "Blue Violet",            0x8A, 0x2B, 0xE2 },
            { "Brown",                  0xA5, 0x2A, 0x2A },
            { "Burlywood",              0xDE, 0xB8, 0x87 },
            { "Cadet Blue",             0x5F, 0x9E, 0xA0 },
            { "Chartreuse",             0x7F, 0xFF, 0x00 },
            { "Chocolate",              0xD2, 0x69, 0x1E },
            { "Coral",                  0xFF, 0x7F, 0x50 },
            { "Cornflower Blue",        0x64, 0x95, 0xED },
            { "Cornsilk",               0xFF, 0xF8, 0xDC },
            { "Crimson",                0xDC, 0x14, 0x3C },
            { "Cyan",                   0x00, 0xFF, 0xFF },
            { "Dark Blue",              0x00, 0x00, 0x8B },
            { "Dark Cyan",              0x00, 0x8B, 0x8B },
            { "Dark Goldenrod",         0xB8, 0x86, 0x0B },
            { "Dark Gray",              0xA9, 0xA9, 0xA9 },
            { "Dark Green",             0x00, 0x64, 0x00 },
            { "Dark Khaki",             0xBD, 0xB7, 0x6B },
            { "Dark Magenta",           0x8B, 0x00, 0x8B },
            { "Dark Olive Green",       0x55, 0x6B, 0x2F },
            { "Dark Orange",            0xFF, 0x8C, 0x00 },
            { "Dark Orchid",            0x99, 0x32, 0xCC },
            { "Dark Red",               0x8B, 0x00, 0x00 },
            { "Dark Salmon",            0xE9, 0x96, 0x7A },
            { "Dark Sea Green",         0x8F, 0xBC, 0x8F },
            { "Dark Slate Blue",        0x48, 0x3D, 0x8B },
            { "Dark Slate Gray",        0x2F, 0x4F, 0x4F },
            { "Dark Turquoise",         0x00, 0xCE, 0xD1 },
            { "Dark Violet",            0x94, 0x00, 0xD3 },
            { "Deep Pink",              0xFF, 0x14, 0x93 },
            { "Deep Sky Blue",          0x00, 0xBF, 0xFF },
            { "Dim Gray",               0x69, 0x69, 0x69 },
            { "Dodger Blue",            0x1E, 0x90, 0xFF },
            { "Firebrick",              0xB2, 0x22, 0x22 },
            { "Floral White",           0xFF, 0xFA, 0xF0 },
            { "Forest Green",           0x22, 0x8B, 0x22 },
            { "Fuchsia",                0xFF, 0x00, 0xFF },
            { "Gainsboro",              0xDC, 0xDC, 0xDC },
            { "Ghost White",            0xF8, 0xF8, 0xFF },
            { "Gold",                   0xFF, 0xD7, 0x00 },
            { "Goldenrod",              0xDA, 0xA5, 0x20 },
            { "Gray",                   0x80, 0x80, 0x80 },
            { "Green",                  0x00, 0x80, 0x00 },
            { "Green Yellow",           0xAD, 0xFF, 0x2F },
            { "Honeydew",               0xF0, 0xFF, 0xF0 },
            { "Hot Pink",               0xFF, 0x69, 0xB4 },
            { "Indian Red",             0xCD, 0x5C, 0x5C },
            { "Indigo",                 0x4B, 0x00, 0x82 },
            { "Ivory",                  0xFF, 0xFF, 0xF0 },
            { "Khaki",                  0xF0, 0xE6, 0x8C },
            { "Lavender",               0xE6, 0xE6, 0xFA },
            { "Lavender Blush",         0xFF, 0xF0, 0xF5 },
            { "Lawn Green",             0x7C, 0xFC, 0x00 },
            { "Lemon Chiffon",          0xFF, 0xFA, 0xCD },
            { "Light Blue",             0xAD, 0xD8, 0xE6 },
            { "Light Coral",            0xF0, 0x80, 0x80 },
            { "Light Cyan",             0xE0, 0xFF, 0xFF },
            { "Light Goldenrod Yellow", 0xFA, 0xFA, 0xD2 },
            { "Light Gray",             0xD3, 0xD3, 0xD3 },
            { "Light Green",            0x90, 0xEE, 0x90 },
            { "Light Pink",             0xFF, 0xB6, 0xC1 },
            { "Light Salmon",           0xFF, 0xA0, 0x7A },
            { "Light Sea Green",        0x20, 0xB2, 0xAA },
            { "Light Sky Blue",         0x87, 0xCE, 0xFA },
            { "Light Slate Gray",       0x77, 0x88, 0x99 },
            { "Light Steel Blue",       0xB0, 0xC4, 0xDE },
            { "Light Yellow",           0xFF, 0xFF, 0xE0 },
            { "Lime",                   0x00, 0xFF, 0x00 },
            { "Lime Green",             0x32, 0xCD, 0x32 },
            { "Linen",                  0xFA, 0xF0, 0xE6 },
            { "Magenta",                0xFF, 0x00, 0xFF },
            { "Maroon",                 0x80, 0x00, 0x00 },
            { "Medium Aquamarine",      0x66, 0xCD, 0xAA },
            { "Medium Blue",            0x00, 0x00, 0xCD },
            { "Medium Orchid",          0xBA, 0x55, 0xD3 },
            { "Medium Purple",          0x93, 0x70, 0xDB },
            { "Medium Sea Green",       0x3C, 0xB3, 0x71 },
            { "Medium Slate Blue",      0x7B, 0x68, 0xEE },
            { "Medium Spring Green",    0x00, 0xFA, 0x9A },
            { "Medium Turquoise",       0x48, 0xD1, 0xCC },
            { "Medium Violet Red",      0xC7, 0x15, 0x85 },
            { "Midnight Blue",          0x19, 0x19, 0x70 },
            { "Mint Cream",             0xF5, 0xFF, 0xFA },
            { "Misty Rose",             0xFF, 0xE4, 0xE1 },
            { "Moccasin",               0xFF, 0xE4, 0xB5 },
            { "Navajo White",           0xFF, 0xDE, 0xAD },
            { "Navy",                   0x00, 0x00, 0x80 },
            { "Old Lace",               0xFD, 0xF5, 0xE6 },
            { "Olive",                  0x80, 0x80, 0x00 },
            { "Olive Drab",             0x6B, 0x8E, 0x23 },
            { "Orange",                 0xFF, 0xA5, 0x00 },
            { "Orange Red",             0xFF, 0x45, 0x00 },
            { "Orchid",                 0xDA, 0x70, 0xD6 },
            { "Pale Goldenrod",         0xEE, 0xE8, 0xAA },
            { "Pale Green",             0x98, 0xFB, 0x98 },
            { "Pale Turquoise",         0xAF, 0xEE, 0xEE },
            { "Pale Violet Red",        0xDB, 0x70, 0x93 },
            { "Papaya Whip",            0xFF, 0xEF, 0xD5 },
            { "Peach Puff",             0xFF, 0xDA, 0xB9 },
            { "Peru",                   0xCD, 0x85, 0x3F },
            { "Pink",                   0xFF, 0xC0, 0xCB },
            { "Plum",                   0xDD, 0xA0, 0xDD },
            { "Powder Blue",            0xB0, 0xE0, 0xE6 },
            { "Purple",                 0x80, 0x00, 0x80 },
            { "Rebecca Purple",         0x66, 0x33, 0x99 },
            { "Red",                    0xFF, 0x00, 0x00 },
            { "Rosy Brown",             0xBC, 0x8F, 0x8F },
            { "Royal Blue",             0x41, 0x69, 0xE1 },
            { "Saddle Brown",           0x8B, 0x45, 0x13 },
            { "Salmon",                 0xFA, 0x80, 0x72 },
            { "Sandy Brown",            0xF4, 0xA4, 0x60 },
            { "Sea Green",              0x2E, 0x8B, 0x57 },
            { "Seashell",               0xFF, 0xF5, 0xEE },
            { "Sienna",                 0xA0, 0x52, 0x2D },
            { "Silver",                 0xC0, 0xC0, 0xC0 },
            { "Sky Blue",               0x87, 0xCE, 0xEB },
            { "Slate Blue",             0x6A, 0x5A, 0xCD },
            { "Slate Gray",             0x70, 0x80, 0x90 },
            { "Snow",                   0xFF, 0xFA, 0xFA },
            { "Spring Green",           0x00, 0xFF, 0x7F },
            { "Steel Blue",             0x46, 0x82, 0xB4 },
            { "Tan",                    0xD2, 0xB4, 0x8C },
            { "Teal",                   0x00, 0x80, 0x80 },
            { "Thistle",                0xD8, 0xBF, 0xD8 },
            { "Tomato",                 0xFF, 0x63, 0x47 },
            { "Turquoise",              0x40, 0xE0, 0xD0 },
            { "Violet",                 0xEE, 0x82, 0xEE },
            { "Wheat",                  0xF5, 0xDE, 0xB3 },
            { "White",                  0xFF, 0xFF, 0xFF },
            { "White Smoke",            0xF5, 0xF5, 0xF5 },
            { "Yellow",                 0xFF, 0xFF, 0x00 },
            { "Yellow Green",           0x9A, 0xCD, 0x32 }
        };

        std::vector<CSSKeywordEntry> list;
        list.reserve(sizeof(raw) / sizeof(raw[0]));
        for (const auto& r_entry : raw) {
            CSSKeywordEntry entry;
            entry.name = r_entry.name;
            entry.r = r_entry.r;
            entry.g = r_entry.g;
            entry.b = r_entry.b;
            entry.lab = srgb_to_lab(entry.r, entry.g, entry.b);
            list.push_back(entry);
        }
        return list;
    }();
    return keywords;
}

using MixedFilamentInternal::RGB;

RGB parse_hex(const std::string& hex)
{
    return MixedFilamentInternal::parse_hex_color(hex);
}

std::string format_hex_lower(const std::string& hex)
{
    std::string s = hex;
    if (s.empty())
        return s;
    if (s[0] != '#')
        s = "#" + s;
    for (size_t i = 1; i < s.size(); ++i)
        s[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    return s;
}

std::string format_hex_lower(const RGB& rgb)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", rgb.r, rgb.g, rgb.b);
    return std::string(buf);
}

} // anonymous namespace

std::string closest_css_color_name(uint8_t r, uint8_t g, uint8_t b)
{
    const LabColor target_lab = srgb_to_lab(r, g, b);
    const auto& keywords = get_css_keyword_database();

    float best_dist = 1e9f;
    const char* best_name = "Black";

    for (const auto& kw : keywords) {
        if (kw.r == r && kw.g == g && kw.b == b) {
             return kw.name;
        }
        const float dist = delta_e00(target_lab, kw.lab);
        if (dist < best_dist) {
            best_dist = dist;
            best_name = kw.name;
        }
    }

    return best_name;
}

std::string closest_css_color_name(const std::string& hex_color)
{
    const RGB rgb = parse_hex(hex_color);
    return closest_css_color_name(uint8_t(rgb.r), uint8_t(rgb.g), uint8_t(rgb.b));
}


std::string mixed_filament_name(const MixedFilamentDefinition&        definition,
                              const std::vector<std::string>&       physical_materials,
                              const std::vector<std::string>&       physical_colors,
                              const DescriptionOptions&             options,
                              const std::string&                    letter)
{
    std::string name;
 
    std::vector<unsigned int> component_ids;
    for (const auto& comp : definition.recipe.blend.components)
        component_ids.push_back(comp.filament.id);

    if (options.include_letter && !letter.empty())
        name += letter + ": ";

    if (options.include_color)
        name += mf_color(definition, physical_colors);

    if (options.include_material) 
        name += " " + mf_material(component_ids, physical_materials);

    if (options.include_kind) 
        name += " " + mf_kind(definition);

    if (options.include_components)
        name += " " + mf_components(definition);

    if (options.include_hex)
        name += " " + mf_hex(definition.presentation.display_color);

    return name;
}

std::string mixed_filament_name(const MixedFilamentLegacyRow&         row,
                              const std::vector<std::string>&       physical_materials,
                              const std::vector<std::string>&       physical_colors,
                              const DescriptionOptions&             options,
                              const std::string&                    letter)
{
    const MixedFilamentDefinition def = mixed_filament_definition_from_legacy_row(row, physical_colors.size());
    return mixed_filament_name(def, physical_materials, physical_colors, options, letter);
}

std::string mixed_filament_name(const std::vector<int>&               physical_indices_0based,
                              const std::vector<int>&               percentages,
                              const std::string&                    display_color_hex,
                              const std::vector<std::string>&       physical_materials,
                              const std::vector<std::string>&       physical_colors,
                              const DescriptionOptions&             options,
                              const std::string&                    letter)
{
    MixedFilamentDefinition def;
    def.presentation.display_color = display_color_hex;
    def.recipe.kind                = MixedFilamentRecipeKind::WeightedBlend;
    def.behavior.gradient.enabled  = is_gradient(def);

    for (size_t i = 0; i < physical_indices_0based.size(); ++i) {
        const int idx = physical_indices_0based[i];
        if (idx >= 0) {
            MixedFilamentWeightedComponent comp;
            comp.filament.id = unsigned(idx + 1);
            comp.percent     = (i < percentages.size()) ? percentages[i] : 0;
            def.recipe.blend.components.push_back(comp);
        }
    }

    return mixed_filament_name(def, physical_materials, physical_colors, options, letter);
}

bool is_gradient(const MixedFilamentDefinition& definition) 
{
    if (definition.behavior.gradient.enabled && 
        definition.recipe.kind == MixedFilamentRecipeKind::WeightedBlend &&
        definition.recipe.blend.components.size() >= 2) {
        return true;
    }
    return false;
}

bool is_pattern(const MixedFilamentDefinition& definition) 
{
    if (!is_gradient(definition) && 
        (definition.recipe.manual_pattern.has_value() ||
         definition.recipe.kind == MixedFilamentRecipeKind::ManualPattern)) {
        return true;
    }
    return false;
}


std::string mf_color(const MixedFilamentDefinition& definition, const std::vector<std::string>& physical_colors)
{
    if (is_gradient(definition)) {
        std::vector<unsigned int> component_ids;

        for (const auto& comp : definition.recipe.blend.components)
            component_ids.push_back(comp.filament.id);

        if (component_ids.size() >= 2) {
            const unsigned int id_first = component_ids.front();
            const unsigned int id_last  = component_ids.back();

            const std::string color_first = (id_first >= 1 && id_first <= physical_colors.size()) ? physical_colors[id_first - 1] :
                                                                                                    "#000000";
            const std::string color_last  = (id_last >= 1 && id_last <= physical_colors.size()) ? physical_colors[id_last - 1] : "#000000";

            return closest_css_color_name(color_first) + " -> " + closest_css_color_name(color_last);
        } else {
            return closest_css_color_name(definition.presentation.display_color);
        }
    } else {
        return closest_css_color_name(definition.presentation.display_color);
    }
    return "";
}

std::string mf_material(const std::vector<unsigned int>& component_ids, const std::vector<std::string>& physical_materials)
{
    std::vector<std::string> unique_mats;
    for (unsigned int id : component_ids) {
        if (id >= 1 && id <= physical_materials.size()) {
            const std::string& m = physical_materials[id - 1];
            if (!m.empty()) {
                if (std::find(unique_mats.begin(), unique_mats.end(), m) == unique_mats.end())
                    unique_mats.push_back(m);
            }
        }
    }

    if (unique_mats.empty())
        return "";

    std::string material = "";
    for (size_t i = 0; i < unique_mats.size(); ++i) {
        if (i > 0)
            material += " & ";
        material += unique_mats[i];
    }
    return material;
}

std::string mf_kind(const MixedFilamentDefinition& definition)
{
    if (is_gradient(definition)) {
        return "Gradient";
    } else if (is_pattern(definition)) {
        return "Pattern";
    } else {
        return "Mix";
    }
    return "";
}

std::string mf_components(const MixedFilamentDefinition& definition) 
{
    if (is_gradient(definition)) {
        return mf_components_gradient(definition.recipe.blend.components);

    } else if (is_pattern(definition)) {
        if (definition.recipe.manual_pattern) {
            return mf_components_pattern(*definition.recipe.manual_pattern);
        }

    } else {
        return mf_components_mix(definition.recipe.blend.components);

    }
    return "";
}

std::string mf_components_mix(const std::vector<MixedFilamentWeightedComponent>& weightedComponents)
{
    std::string components = "";
    for (size_t i = 0; i < weightedComponents.size(); ++i) {
        if (i > 0)
            components += " ";

        const auto& comp = weightedComponents[i];
        components += "[" + std::to_string(comp.filament.id) + "] " + std::to_string(comp.percent) + "%";
    }
    return components;
}

std::string mf_components_pattern(const MixedFilamentManualPattern& manualPattern)
{
    std::string components = "";
    if (!manualPattern.groups.empty()) {
        for (size_t i = 0; i < manualPattern.groups[0].size(); ++i) {
            if (i > 0)
                components += "-";

            components += "[" + std::to_string(manualPattern.groups[0][i].id) + "]";
        }
    }
    return components;
}

std::string mf_components_gradient(const std::vector<MixedFilamentWeightedComponent>& gradientComponents)
{
    std::string components = "";

    std::vector<unsigned int> component_ids;
    for (const auto& comp : gradientComponents)
        component_ids.push_back(comp.filament.id);

    if (component_ids.size() >= 2) {
        components += "[" + std::to_string(component_ids.front()) + "]->[" + std::to_string(component_ids.back()) + "]";
    }

    return components;
}

std::string mf_hex(const std::string& hex) 
{ 
    if (hex.empty())
        return "";

    return "(" + format_hex_lower(hex) + ")";
}

} // namespace ColorNames
} // namespace Slic3r
