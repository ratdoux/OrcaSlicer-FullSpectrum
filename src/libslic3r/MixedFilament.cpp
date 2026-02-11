#include "MixedFilament.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <iomanip>
#include <numeric>

namespace Slic3r {

// ---------------------------------------------------------------------------
// Colour helpers (internal)
// ---------------------------------------------------------------------------

struct RGB {
    int r = 0, g = 0, b = 0;
};

struct RGBf {
    float r = 0.f, g = 0.f, b = 0.f;
};

static float clamp01(float v)
{
    return std::max(0.f, std::min(1.f, v));
}

static RGBf to_rgbf(const RGB &c)
{
    return {
        clamp01(static_cast<float>(c.r) / 255.f),
        clamp01(static_cast<float>(c.g) / 255.f),
        clamp01(static_cast<float>(c.b) / 255.f)
    };
}

static RGB to_rgb8(const RGBf &c)
{
    auto to_u8 = [](float v) -> int {
        return std::clamp(static_cast<int>(std::round(clamp01(v) * 255.f)), 0, 255);
    };
    return { to_u8(c.r), to_u8(c.g), to_u8(c.b) };
}

// Convert RGB to an artist-pigment style RYB space.
// This is an approximation, but it gives expected pair mixes:
// Red + Blue -> Purple, Blue + Yellow -> Green, Red + Yellow -> Orange.
static RGBf rgb_to_ryb(RGBf in)
{
    float r = clamp01(in.r);
    float g = clamp01(in.g);
    float b = clamp01(in.b);

    const float white = std::min({ r, g, b });
    r -= white;
    g -= white;
    b -= white;

    const float max_g = std::max({ r, g, b });

    float y = std::min(r, g);
    r -= y;
    g -= y;

    if (b > 0.f && g > 0.f) {
        b *= 0.5f;
        g *= 0.5f;
    }

    y += g;
    b += g;

    const float max_y = std::max({ r, y, b });
    if (max_y > 1e-6f) {
        const float n = max_g / max_y;
        r *= n;
        y *= n;
        b *= n;
    }

    r += white;
    y += white;
    b += white;
    return { clamp01(r), clamp01(y), clamp01(b) };
}

static RGBf ryb_to_rgb(RGBf in)
{
    float r = clamp01(in.r);
    float y = clamp01(in.g);
    float b = clamp01(in.b);

    const float white = std::min({ r, y, b });
    r -= white;
    y -= white;
    b -= white;

    const float max_y = std::max({ r, y, b });

    float g = std::min(y, b);
    y -= g;
    b -= g;

    if (b > 0.f && g > 0.f) {
        b *= 2.f;
        g *= 2.f;
    }

    r += y;
    g += y;

    const float max_g = std::max({ r, g, b });
    if (max_g > 1e-6f) {
        const float n = max_y / max_g;
        r *= n;
        g *= n;
        b *= n;
    }

    r += white;
    g += white;
    b += white;
    return { clamp01(r), clamp01(g), clamp01(b) };
}

// Parse "#RRGGBB" to RGB.  Returns black on failure.
static RGB parse_hex_color(const std::string &hex)
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

static std::string rgb_to_hex(const RGB &c)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
    return std::string(buf);
}

static int clamp_int(int v, int lo, int hi)
{
    return std::max(lo, std::min(hi, v));
}

static int safe_ratio_from_height(float h, float unit)
{
    if (unit <= 1e-6f)
        return 1;
    return std::max(0, int(std::lround(h / unit)));
}

static void compute_gradient_heights(const MixedFilament &mf, float lower_bound, float upper_bound, float &h_a, float &h_b)
{
    const int   mix_b = clamp_int(mf.mix_b_percent, 0, 100);
    const float pct_b = float(mix_b) / 100.f;
    const float pct_a = 1.f - pct_b;
    const float lo    = std::max(0.01f, lower_bound);
    const float hi    = std::max(lo, upper_bound);

    h_a = lo + pct_a * (hi - lo);
    h_b = lo + pct_b * (hi - lo);
}

static void normalize_ratio_pair(int &a, int &b)
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

static void compute_gradient_ratios(MixedFilament &mf, int gradient_mode, float lower_bound, float upper_bound, int cycle_layers)
{
    if (gradient_mode == 1) {
        // Height-weighted mode:
        // map blend to [lower, upper], then convert relative heights to an integer cadence.
        float h_a = 0.f;
        float h_b = 0.f;
        compute_gradient_heights(mf, lower_bound, upper_bound, h_a, h_b);
        // Use lower-bound as quantization unit so this mode differs clearly from layer-cycle mode.
        const float unit = std::max(0.01f, std::min(h_a, h_b));
        mf.ratio_a = std::max(1, safe_ratio_from_height(h_a, unit));
        mf.ratio_b = std::max(1, safe_ratio_from_height(h_b, unit));
    } else {
        // Layer-cycle mode:
        // distribute an integer cycle directly by blend percentages.
        const int mix_b = clamp_int(mf.mix_b_percent, 0, 100);
        const float pct_b = float(mix_b) / 100.f;
        const int cycle = std::max(2, cycle_layers);
        mf.ratio_b = clamp_int(int(std::lround(pct_b * cycle)), 0, cycle);
        mf.ratio_a = cycle - mf.ratio_b;
    }

    normalize_ratio_pair(mf.ratio_a, mf.ratio_b);
}

static int safe_mod(int x, int m)
{
    if (m <= 0)
        return 0;
    int r = x % m;
    return (r < 0) ? (r + m) : r;
}

static int dithering_phase_step(int cycle)
{
    if (cycle <= 1)
        return 0;
    int step = cycle / 2 + 1;
    while (std::gcd(step, cycle) != 1)
        ++step;
    return step % cycle;
}

static bool use_component_b_advanced_dither(int layer_index, int ratio_a, int ratio_b)
{
    ratio_a = std::max(0, ratio_a);
    ratio_b = std::max(0, ratio_b);

    const int cycle = ratio_a + ratio_b;
    if (cycle <= 0 || ratio_b <= 0)
        return false;
    if (ratio_a <= 0)
        return true;

    // Base ordered pattern: as evenly distributed as possible for ratio_b/cycle.
    const int pos = safe_mod(layer_index, cycle);
    const int cycle_idx = (layer_index - pos) / cycle;

    // Rotate each cycle to avoid visible long-period vertical striping.
    const int phase = safe_mod(cycle_idx * dithering_phase_step(cycle), cycle);
    const int p = safe_mod(pos + phase, cycle);

    const int b_before = (p * ratio_b) / cycle;
    const int b_after  = ((p + 1) * ratio_b) / cycle;
    return b_after > b_before;
}

static bool parse_row_definition(const std::string &row,
                                 unsigned int      &a,
                                 unsigned int      &b,
                                 bool              &enabled,
                                 bool              &custom,
                                 int               &mix_b_percent)
{
    std::vector<int> values;
    std::stringstream ss(row);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty())
            return false;
        try {
            values.push_back(std::stoi(token));
        } catch (...) {
            return false;
        }
    }

    if (values.size() != 4 && values.size() != 5)
        return false;

    if (values[0] <= 0 || values[1] <= 0)
        return false;

    a = unsigned(values[0]);
    b = unsigned(values[1]);
    enabled = (values[2] != 0);
    custom = (values.size() == 5) ? (values[3] != 0) : true;
    mix_b_percent = clamp_int(values.size() == 5 ? values[4] : values[3], 0, 100);
    return true;
}

// ---------------------------------------------------------------------------
// MixedFilamentManager
// ---------------------------------------------------------------------------

void MixedFilamentManager::auto_generate(const std::vector<std::string> &filament_colours)
{
    // Keep a copy of the old list so we can preserve user-modified ratios and
    // enabled flags and custom rows.
    std::vector<MixedFilament> old = std::move(m_mixed);
    m_mixed.clear();

    const size_t n = filament_colours.size();
    if (n < 2)
        return;

    std::vector<MixedFilament> custom_rows;
    custom_rows.reserve(old.size());
    for (const MixedFilament &prev : old) {
        if (!prev.custom)
            continue;
        if (prev.component_a == 0 || prev.component_b == 0 || prev.component_a > n || prev.component_b > n || prev.component_a == prev.component_b)
            continue;
        custom_rows.push_back(prev);
    }

    // Generate all C(N,2) pairwise combinations.
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            MixedFilament mf;
            mf.component_a = static_cast<unsigned int>(i + 1); // 1-based
            mf.component_b = static_cast<unsigned int>(j + 1);
            mf.ratio_a     = 1;
            mf.ratio_b     = 1;
            mf.mix_b_percent = 50;
            mf.enabled     = true;
            mf.custom      = false;

            // Try to preserve previous settings.
            for (const auto &prev : old) {
                if (!prev.custom &&
                    prev.component_a == mf.component_a &&
                    prev.component_b == mf.component_b) {
                    mf.enabled = prev.enabled;
                    break;
                }
            }
            m_mixed.push_back(mf);
        }
    }

    for (MixedFilament &mf : custom_rows)
        m_mixed.push_back(std::move(mf));

    refresh_display_colors(filament_colours);
}

void MixedFilamentManager::remove_physical_filament(unsigned int deleted_filament_id)
{
    if (deleted_filament_id == 0 || m_mixed.empty())
        return;

    std::vector<MixedFilament> filtered;
    filtered.reserve(m_mixed.size());
    for (MixedFilament mf : m_mixed) {
        if (mf.component_a == deleted_filament_id || mf.component_b == deleted_filament_id)
            continue;

        if (mf.component_a > deleted_filament_id)
            --mf.component_a;
        if (mf.component_b > deleted_filament_id)
            --mf.component_b;

        filtered.emplace_back(std::move(mf));
    }
    m_mixed = std::move(filtered);
}

void MixedFilamentManager::add_custom_filament(unsigned int component_a,
                                               unsigned int component_b,
                                               int          mix_b_percent,
                                               const std::vector<std::string> &filament_colours)
{
    const size_t n = filament_colours.size();
    if (n < 2)
        return;

    component_a = std::max<unsigned int>(1, std::min<unsigned int>(component_a, unsigned(n)));
    component_b = std::max<unsigned int>(1, std::min<unsigned int>(component_b, unsigned(n)));
    if (component_a == component_b) {
        component_b = (component_a == 1) ? 2 : 1;
    }

    MixedFilament mf;
    mf.component_a = component_a;
    mf.component_b = component_b;
    mf.mix_b_percent = clamp_int(mix_b_percent, 0, 100);
    mf.ratio_a = 1;
    mf.ratio_b = 1;
    mf.enabled = true;
    mf.custom = true;
    m_mixed.push_back(std::move(mf));
    refresh_display_colors(filament_colours);
}

void MixedFilamentManager::clear_custom_entries()
{
    m_mixed.erase(std::remove_if(m_mixed.begin(), m_mixed.end(), [](const MixedFilament &mf) { return mf.custom; }), m_mixed.end());
}

void MixedFilamentManager::apply_gradient_settings(int   gradient_mode,
                                                   float lower_bound,
                                                   float upper_bound,
                                                   int   cycle_layers,
                                                   bool  advanced_dithering)
{
    m_gradient_mode      = (gradient_mode != 0) ? 1 : 0;
    m_height_lower_bound = std::max(0.01f, lower_bound);
    m_height_upper_bound = std::max(m_height_lower_bound, upper_bound);
    m_cycle_layers       = std::max(2, cycle_layers);
    m_advanced_dithering = advanced_dithering;

    for (MixedFilament &mf : m_mixed) {
        if (!mf.custom) {
            mf.ratio_a = 1;
            mf.ratio_b = 1;
            continue;
        }
        compute_gradient_ratios(mf, m_gradient_mode, m_height_lower_bound, m_height_upper_bound, m_cycle_layers);
    }
}

std::string MixedFilamentManager::serialize_custom_entries() const
{
    std::ostringstream ss;
    bool first = true;
    for (const MixedFilament &mf : m_mixed) {
        if (!first)
            ss << ';';
        first = false;
        ss << mf.component_a << ','
           << mf.component_b << ','
           << (mf.enabled ? 1 : 0) << ','
           << (mf.custom ? 1 : 0) << ','
           << clamp_int(mf.mix_b_percent, 0, 100);
    }
    return ss.str();
}

void MixedFilamentManager::load_custom_entries(const std::string &serialized, const std::vector<std::string> &filament_colours)
{
    const size_t n = filament_colours.size();
    if (serialized.empty() || n < 2)
        return;

    std::stringstream all(serialized);
    std::string row;
    while (std::getline(all, row, ';')) {
        if (row.empty())
            continue;
        unsigned int a = 0;
        unsigned int b = 0;
        bool enabled = true;
        bool custom = true;
        int mix = 50;
        if (!parse_row_definition(row, a, b, enabled, custom, mix))
            continue;
        if (a == 0 || b == 0 || a > n || b > n || a == b)
            continue;

        if (!custom) {
            auto it_auto = std::find_if(m_mixed.begin(), m_mixed.end(), [a, b](const MixedFilament &mf) {
                return !mf.custom && mf.component_a == a && mf.component_b == b;
            });
            if (it_auto != m_mixed.end()) {
                it_auto->enabled = enabled;
                it_auto->mix_b_percent = mix;
                continue;
            }
        }

        MixedFilament mf;
        mf.component_a = a;
        mf.component_b = b;
        mf.mix_b_percent = mix;
        mf.ratio_a = 1;
        mf.ratio_b = 1;
        mf.enabled = enabled;
        mf.custom = custom;
        m_mixed.push_back(std::move(mf));
    }
    refresh_display_colors(filament_colours);
}

unsigned int MixedFilamentManager::resolve(unsigned int filament_id,
                                           size_t       num_physical,
                                           int          layer_index,
                                           float        layer_print_z,
                                           float        layer_height,
                                           bool         force_height_weighted) const
{
    if (!is_mixed(filament_id, num_physical))
        return filament_id;

    const size_t idx = index_of(filament_id, num_physical);
    if (idx >= m_mixed.size())
        return 1; // fallback to first extruder

    const MixedFilament &mf = m_mixed[idx];

    // Height-weighted cadence can be forced by the local-Z planner. The
    // regular gradient height mode keeps historical behavior (custom rows).
    const bool use_height_weighted = force_height_weighted || (m_gradient_mode == 1 && mf.custom);
    if (use_height_weighted) {
        float h_a = 0.f;
        float h_b = 0.f;
        compute_gradient_heights(mf, m_height_lower_bound, m_height_upper_bound, h_a, h_b);
        const float cycle_h = std::max(0.01f, h_a + h_b);
        const float z_anchor = (layer_height > 1e-6f)
            ? std::max(0.f, layer_print_z - 0.5f * layer_height)
            : std::max(0.f, layer_print_z);
        float phase = std::fmod(z_anchor, cycle_h);
        if (phase < 0.f)
            phase += cycle_h;
        return (phase < h_a) ? mf.component_a : mf.component_b;
    }

    const int cycle = mf.ratio_a + mf.ratio_b;
    if (cycle <= 0)
        return mf.component_a;

    if (m_gradient_mode == 0 && m_advanced_dithering && mf.custom)
        return use_component_b_advanced_dither(layer_index, mf.ratio_a, mf.ratio_b) ? mf.component_b : mf.component_a;

    const int pos = ((layer_index % cycle) + cycle) % cycle; // safe modulo for negatives
    return (pos < mf.ratio_a) ? mf.component_a : mf.component_b;
}

std::string MixedFilamentManager::blend_color(const std::string &color_a,
                                              const std::string &color_b,
                                              int ratio_a, int ratio_b)
{
    const int safe_a = std::max(0, ratio_a);
    const int safe_b = std::max(0, ratio_b);
    const float total = static_cast<float>(safe_a + safe_b);
    const float wa    = (total > 0.f) ? static_cast<float>(safe_a) / total : 0.5f;
    const float wb    = 1.f - wa;

    const RGBf rgb_a = to_rgbf(parse_hex_color(color_a));
    const RGBf rgb_b = to_rgbf(parse_hex_color(color_b));
    const RGBf ryb_a = rgb_to_ryb(rgb_a);
    const RGBf ryb_b = rgb_to_ryb(rgb_b);

    RGBf ryb_out;
    ryb_out.r = wa * ryb_a.r + wb * ryb_b.r;
    ryb_out.g = wa * ryb_a.g + wb * ryb_b.g;
    ryb_out.b = wa * ryb_a.b + wb * ryb_b.b;

    RGBf rgb_out = ryb_to_rgb(ryb_out);
    const float v_out = std::max({ rgb_out.r, rgb_out.g, rgb_out.b });
    const float v_tgt = wa * std::max({ rgb_a.r, rgb_a.g, rgb_a.b }) +
                        wb * std::max({ rgb_b.r, rgb_b.g, rgb_b.b });
    if (v_out > 1e-6f && v_tgt > 0.f) {
        const float scale = v_tgt / v_out;
        rgb_out.r = clamp01(rgb_out.r * scale);
        rgb_out.g = clamp01(rgb_out.g * scale);
        rgb_out.b = clamp01(rgb_out.b * scale);
    }

    return rgb_to_hex(to_rgb8(rgb_out));
}

void MixedFilamentManager::refresh_display_colors(const std::vector<std::string> &filament_colours)
{
    for (MixedFilament &mf : m_mixed) {
        if (mf.component_a == 0 || mf.component_b == 0 ||
            mf.component_a > filament_colours.size() || mf.component_b > filament_colours.size()) {
            mf.display_color = "#26A69A";
            continue;
        }
        const int ratio_a = std::max(0, 100 - clamp_int(mf.mix_b_percent, 0, 100));
        const int ratio_b = clamp_int(mf.mix_b_percent, 0, 100);
        mf.display_color = blend_color(
            filament_colours[mf.component_a - 1],
            filament_colours[mf.component_b - 1],
            ratio_a, ratio_b);
    }
}

size_t MixedFilamentManager::enabled_count() const
{
    size_t count = 0;
    for (const auto &mf : m_mixed)
        if (mf.enabled)
            ++count;
    return count;
}

std::vector<std::string> MixedFilamentManager::display_colors() const
{
    std::vector<std::string> colors;
    for (const auto &mf : m_mixed)
        if (mf.enabled)
            colors.push_back(mf.display_color);
    return colors;
}

} // namespace Slic3r

