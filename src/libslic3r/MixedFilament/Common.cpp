#include "Internal.hpp"
#include "../libslic3r.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace Slic3r {

std::pair<double, double> mixed_filament_local_z_pair_heights(double nominal_layer_height,
                                                               double min_sublayer_height,
                                                               int    mix_b_percent)
{
    const double height = std::max(0.0, nominal_layer_height);
    if (height <= EPSILON)
        return {0.0, 0.0};

    const int mix_b = std::clamp(mix_b_percent, 0, 100);
    if (mix_b <= 0)
        return {height, 0.0};
    if (mix_b >= 100)
        return {0.0, height};

    const double minimum = std::max(0.01, min_sublayer_height);
    if (height < 2.0 * minimum - EPSILON)
        return mix_b < 50 ? std::pair<double, double>{height, 0.0} : std::pair<double, double>{0.0, height};

    const double height_b = std::clamp(height * double(mix_b) / 100.0, minimum, height - minimum);
    return {height - height_b, height_b};
}

bool mixed_filament_definition_uses_local_z(const MixedFilamentDefinition &definition,
                                             bool                           global_local_z_enabled)
{
    if (definition.visibility.tombstoned || definition.recipe.manual_pattern)
        return false;
    return global_local_z_enabled || definition.behavior.gradient.enabled;
}

bool mixed_filament_local_z_uses_full_domain(bool global_full_domain_enabled,
                                              bool mixed_filament_assigned_to_region)
{
    return global_full_domain_enabled || mixed_filament_assigned_to_region;
}

bool mixed_filament_local_z_painted_override_uses_planner(bool painted_state_is_mixed,
                                                           bool painted_mixed_row_uses_local_z)
{
    return painted_state_is_mixed && painted_mixed_row_uses_local_z;
}

bool mixed_filament_local_z_should_subdivide_layer(size_t layer_id,
                                                    bool   whole_object_mode,
                                                    bool   preserve_first_layer)
{
    return !(layer_id == 0 && whole_object_mode && preserve_first_layer);
}

namespace MixedFilamentInternal {

uint64_t canonical_pair_key(unsigned int a, unsigned int b)
{
    const unsigned int lo = std::min(a, b);
    const unsigned int hi = std::max(a, b);
    return (uint64_t(lo) << 32) | uint64_t(hi);
}

// ---------------------------------------------------------------------------
// Colour helpers (internal)
// ---------------------------------------------------------------------------

// Parse "#RRGGBB" to RGB.  Returns black on failure.
RGB parse_hex_color(const std::string& hex)
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

std::string rgb_to_hex(const RGB& c)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
    return std::string(buf);
}

int clamp_int(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

float clamp_surface_offset(float v) { return std::clamp(v, -2.f, 2.f); }

float canonical_signed_bias_value(float component_a_surface_offset, float component_b_surface_offset)
{
    const float offset_a = clamp_surface_offset(component_a_surface_offset);
    const float offset_b = clamp_surface_offset(component_b_surface_offset);

    if (std::abs(offset_b) > EPSILON)
        return offset_b;
    if (std::abs(offset_a) > EPSILON)
        return (offset_a >= 0.f) ? -std::abs(offset_a) : std::abs(offset_a);
    return 0.f;
}

std::string format_surface_offset_token(float value)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4) << clamp_surface_offset(value);
    std::string out = ss.str();
    while (!out.empty() && out.back() == '0')
        out.pop_back();
    if (!out.empty() && out.back() == '.')
        out.pop_back();
    if (out == "-0")
        out = "0";
    return out.empty() ? std::string("0") : out;
}

int safe_ratio_from_height(float h, float unit)
{
    if (unit <= 1e-6f)
        return 1;
    return std::max(0, int(std::lround(h / unit)));
}

void compute_gradient_heights_from_mix(int mix_b_percent,
                                       float nominal_layer_height,
                                       float min_sublayer_height,
                                       float& h_a,
                                       float& h_b)
{
    const auto heights = mixed_filament_local_z_pair_heights(nominal_layer_height, min_sublayer_height, mix_b_percent);
    h_a                = float(heights.first);
    h_b                = float(heights.second);
}

void normalize_ratio_pair(int& a, int& b)
{
    a = std::max(0, a);
    b = std::max(0, b);
    if (a == 0 && b == 0) {
        a = 1;
        return;
    }
    if (a > 0 && b > 0) {
        const int g = std::gcd(a, b);
        if (g > 1) {
            a /= g;
            b /= g;
        }
    }
}

std::pair<int, int> gradient_ratios_from_mix(int mix_b_percent,
                                             int gradient_mode,
                                             float nominal_layer_height,
                                             float min_sublayer_height)
{
    int ratio_a = 1;
    int ratio_b = 1;
    if (gradient_mode == 1) {
        // Height-weighted mode:
        // Convert the direct nominal-layer thickness split into an integer cadence.
        float h_a = 0.f;
        float h_b = 0.f;
        compute_gradient_heights_from_mix(mix_b_percent, nominal_layer_height, min_sublayer_height, h_a, h_b);
        if (h_a <= EPSILON) {
            ratio_a = 0;
            ratio_b = 1;
        } else if (h_b <= EPSILON) {
            ratio_a = 1;
            ratio_b = 0;
        } else {
            const float unit = std::max(0.01f, std::min(h_a, h_b));
            ratio_a          = std::max(1, safe_ratio_from_height(h_a, unit));
            ratio_b          = std::max(1, safe_ratio_from_height(h_b, unit));
        }
    } else {
        // Layer-cycle mode:
        // derive a gradual integer cadence directly from the blend ratio
        // by fixing the minority side to one layer and scaling the majority.
        const int mix_b = clamp_int(mix_b_percent, 0, 100);
        if (mix_b <= 0) {
            ratio_a = 1;
            ratio_b = 0;
        } else if (mix_b >= 100) {
            ratio_a = 0;
            ratio_b = 1;
        } else {
            const int  pct_b        = mix_b;
            const int  pct_a        = 100 - pct_b;
            const bool b_is_major   = pct_b >= pct_a;
            const int  major_pct    = b_is_major ? pct_b : pct_a;
            const int  minor_pct    = b_is_major ? pct_a : pct_b;
            const int  major_layers = std::max(1, int(std::lround(double(major_pct) / double(std::max(1, minor_pct)))));
            ratio_a                 = b_is_major ? 1 : major_layers;
            ratio_b                 = b_is_major ? major_layers : 1;
        }
    }

    normalize_ratio_pair(ratio_a, ratio_b);
    return { ratio_a, ratio_b };
}

int safe_mod(int x, int m)
{
    if (m <= 0)
        return 0;
    int r = x % m;
    return (r < 0) ? (r + m) : r;
}

int dithering_phase_step(int cycle)
{
    if (cycle <= 1)
        return 0;
    int step = cycle / 2 + 1;
    while (std::gcd(step, cycle) != 1)
        ++step;
    return step % cycle;
}

bool use_component_b_advanced_dither(int layer_index, int ratio_a, int ratio_b)
{
    ratio_a = std::max(0, ratio_a);
    ratio_b = std::max(0, ratio_b);

    const int cycle = ratio_a + ratio_b;
    if (cycle <= 0 || ratio_b <= 0)
        return false;
    if (ratio_a <= 0)
        return true;

    // Base ordered pattern: as evenly distributed as possible for ratio_b/cycle.
    const int pos       = safe_mod(layer_index, cycle);
    const int cycle_idx = (layer_index - pos) / cycle;

    // Rotate each cycle to avoid visible long-period vertical striping.
    const int phase = safe_mod(cycle_idx * dithering_phase_step(cycle), cycle);
    const int p     = safe_mod(pos + phase, cycle);

    const int b_before = (p * ratio_b) / cycle;
    const int b_after  = ((p + 1) * ratio_b) / cycle;
    return b_after > b_before;
}

double mixed_filament_reference_nozzle_mm(unsigned int component_a, unsigned int component_b, const std::vector<double>& nozzle_diameters)
{
    std::vector<double> samples;
    samples.reserve(2);

    auto append_if_valid = [&samples, &nozzle_diameters](unsigned int component_id) {
        if (component_id >= 1 && component_id <= nozzle_diameters.size())
            samples.emplace_back(std::max(0.05, nozzle_diameters[size_t(component_id - 1)]));
    };

    append_if_valid(component_a);
    append_if_valid(component_b);

    if (samples.empty())
        return 0.4;
    return std::accumulate(samples.begin(), samples.end(), 0.0) / double(samples.size());
}

}} // namespace Slic3r::MixedFilamentInternal
