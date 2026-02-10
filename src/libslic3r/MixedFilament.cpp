#include "MixedFilament.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <iomanip>

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

// ---------------------------------------------------------------------------
// MixedFilamentManager
// ---------------------------------------------------------------------------

void MixedFilamentManager::auto_generate(const std::vector<std::string> &filament_colours)
{
    // Keep a copy of the old list so we can preserve user-modified ratios and
    // enabled flags.
    std::vector<MixedFilament> old = std::move(m_mixed);
    m_mixed.clear();

    const size_t n = filament_colours.size();
    if (n < 2)
        return;

    // Generate all C(N,2) pairwise combinations.
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            MixedFilament mf;
            mf.component_a = static_cast<unsigned int>(i + 1); // 1-based
            mf.component_b = static_cast<unsigned int>(j + 1);
            mf.ratio_a     = 1;
            mf.ratio_b     = 1;
            mf.enabled     = true;

            // Try to preserve previous settings.
            for (const auto &prev : old) {
                if (prev.component_a == mf.component_a &&
                    prev.component_b == mf.component_b) {
                    mf.ratio_a = prev.ratio_a;
                    mf.ratio_b = prev.ratio_b;
                    mf.enabled = prev.enabled;
                    break;
                }
            }

            mf.display_color = blend_color(filament_colours[i],
                                           filament_colours[j],
                                           mf.ratio_a, mf.ratio_b);
            m_mixed.push_back(mf);
        }
    }
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

unsigned int MixedFilamentManager::resolve(unsigned int filament_id,
                                           size_t       num_physical,
                                           int          layer_index) const
{
    if (!is_mixed(filament_id, num_physical))
        return filament_id;

    const size_t idx = index_of(filament_id, num_physical);
    if (idx >= m_mixed.size())
        return 1; // fallback to first extruder

    const MixedFilament &mf = m_mixed[idx];
    const int cycle = mf.ratio_a + mf.ratio_b;
    if (cycle <= 0)
        return mf.component_a;

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

