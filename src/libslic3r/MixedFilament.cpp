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

// Parse "#RRGGBB" to RGB.  Returns black on failure.
static RGB parse_hex_color(const std::string &hex)
{
    RGB c;
    if (hex.size() >= 7 && hex[0] == '#') {
        c.r = std::stoi(hex.substr(1, 2), nullptr, 16);
        c.g = std::stoi(hex.substr(3, 2), nullptr, 16);
        c.b = std::stoi(hex.substr(5, 2), nullptr, 16);
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
    RGB a = parse_hex_color(color_a);
    RGB b = parse_hex_color(color_b);

    // Additive blend: min(a + b, 255) per channel.
    // For unequal ratios, weight accordingly.
    const float total = static_cast<float>(ratio_a + ratio_b);
    const float wa    = (total > 0.f) ? static_cast<float>(ratio_a) / total : 0.5f;
    const float wb    = 1.f - wa;

    // Use screen blending which is additive-like without oversaturation:
    // screen(A, B) = A + B - A*B/255
    // Weighted variant: blend each channel independently.
    auto screen_ch = [](int ca, int cb, float wa, float wb) -> int {
        // Weighted additive with clamping – matches user expectation:
        //   Red(255,0,0) + Green(0,255,0) = Yellow(255,255,0)
        float v = static_cast<float>(ca) * wa + static_cast<float>(cb) * wb;
        // Boost towards additive: add the minimum so pure colours combine fully.
        float additive = std::min(static_cast<float>(ca + cb), 255.f);
        // Blend between weighted-average and full-additive based on colour distance.
        float result = wa * static_cast<float>(ca) + wb * static_cast<float>(cb);
        // For the 1:1 case, use pure additive (clamped) to get R+G=Y.
        if (std::abs(wa - wb) < 0.01f)
            result = additive;
        return std::min(static_cast<int>(std::round(result)), 255);
    };

    RGB out;
    out.r = screen_ch(a.r, b.r, wa, wb);
    out.g = screen_ch(a.g, b.g, wa, wb);
    out.b = screen_ch(a.b, b.b, wa, wb);

    return rgb_to_hex(out);
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
