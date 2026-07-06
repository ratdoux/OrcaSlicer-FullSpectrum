#include "FilamentSyncAlgorithm.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

// ---- sRGB transfer function (IEC 61966-2-1) ----
constexpr float k_srgb_threshold   = 0.04045f;
constexpr float k_srgb_linear_slope = 1.0f / 12.92f;
constexpr float k_srgb_gamma_a     = 0.055f;
constexpr float k_srgb_gamma_b     = 1.055f;
constexpr float k_srgb_gamma_exp   = 2.4f;

// ---- CIE Lab (D65) constants ----
constexpr float k_cie_delta     = 6.0f / 29.0f;       // δ
constexpr float k_cie_delta2    = k_cie_delta * k_cie_delta;
constexpr float k_cie_delta3    = k_cie_delta * k_cie_delta * k_cie_delta;
constexpr float k_cie_four_29th = 4.0f / 29.0f;       // constant term in Lab f(t) linear branch
constexpr float k_cie_lab_l_scale = 116.0f;
constexpr float k_cie_lab_l_shift = 16.0f;
constexpr float k_cie_lab_a_scale = 500.0f;
constexpr float k_cie_lab_b_scale = 200.0f;

// ---- D65 reference white ----
constexpr float k_d65_xn = 0.95047f;
constexpr float k_d65_yn = 1.00000f;
constexpr float k_d65_zn = 1.08883f;

// ---- Color channel range ----
constexpr float k_color_max = 255.0f;

struct Lab { float L, a, b; };

// ---- sRGB -> linear -> XYZ -> Lab helpers ----

float srgb_to_linear(float c)
{
    c /= k_color_max;
    return (c <= k_srgb_threshold)
        ? (c * k_srgb_linear_slope)
        : std::pow((c + k_srgb_gamma_a) / k_srgb_gamma_b, k_srgb_gamma_exp);
}

float lab_f(float t)
{
    return (t > k_cie_delta3)
        ? std::cbrt(t)
        : (t / (3.0f * k_cie_delta2) + k_cie_four_29th);
}

void rgb_to_lab(uint8_t r, uint8_t g, uint8_t b,
                float& L, float& a, float& b_val)
{
    // sRGB -> linear
    float lr = srgb_to_linear(r);
    float lg = srgb_to_linear(g);
    float lb = srgb_to_linear(b);

    // Linear RGB -> XYZ (D65, sRGB primaries)
    float x = 0.4124564f * lr + 0.3575761f * lg + 0.1804375f * lb;
    float y = 0.2126729f * lr + 0.7151522f * lg + 0.0721750f * lb;
    float z = 0.0193339f * lr + 0.1191920f * lg + 0.9503041f * lb;

    // XYZ -> Lab
    float fx = lab_f(x / k_d65_xn);
    float fy = lab_f(y / k_d65_yn);
    float fz = lab_f(z / k_d65_zn);

    L     = k_cie_lab_l_scale * fy - k_cie_lab_l_shift;
    a     = k_cie_lab_a_scale * (fx - fy);
    b_val = k_cie_lab_b_scale * (fy - fz);
}

} // anonymous namespace

namespace Slic3r {

// ---- public API ------------------------------------------------------------

float delta_e_cie76(uint8_t r1, uint8_t g1, uint8_t b1,
                    uint8_t r2, uint8_t g2, uint8_t b2)
{
    float L1, a1, b1v, L2, a2, b2v;
    rgb_to_lab(r1, g1, b1, L1, a1, b1v);
    rgb_to_lab(r2, g2, b2, L2, a2, b2v);

    float dL = L1 - L2;
    float da = a1 - a2;
    float db = b1v - b2v;
    return std::sqrt(dL * dL + da * da + db * db);
}

std::vector<int> compute_color_match(
    const std::vector<GUI::FilamentData>& design_data,
    const std::vector<GUI::FilamentData>& machine_data)
{
    const size_t designCount  = design_data.size();
    const size_t machineCount = machine_data.size();

    std::vector<int> result(designCount, -1);
    if (designCount == 0 || machineCount == 0)
        return result;

    // Precompute Lab values for all machine filaments
    std::vector<Lab> machineLab(machineCount);
    for (size_t j = 0; j < machineCount; ++j) {
        wxColour mc = GUI::getMainColor(machine_data[j].m_color);
        rgb_to_lab(mc.Red(), mc.Green(), mc.Blue(),
                   machineLab[j].L, machineLab[j].a, machineLab[j].b);
    }

    // For each design filament, find the perceptually closest machine filament
    for (size_t i = 0; i < designCount; ++i) {
        float designL, designA, designB;
        wxColour dc = GUI::getMainColor(design_data[i].m_color);
        rgb_to_lab(dc.Red(), dc.Green(), dc.Blue(),
                   designL, designA, designB);

        float bestDist = std::numeric_limits<float>::max();
        int   bestIdx  = -1;
        for (size_t j = 0; j < machineCount; ++j) {
            if (is_none_filament(machine_data[j]))
                continue;
            float dL   = designL - machineLab[j].L;
            float dA   = designA - machineLab[j].a;
            float dB   = designB - machineLab[j].b;
            float dist = dL * dL + dA * dA + dB * dB; // squared Euclidean (avoids sqrt)
            if (dist < bestDist) {
                bestDist = dist;
                bestIdx  = static_cast<int>(j);
            }
        }
        result[i] = bestIdx;
    }

    return result;
}

std::vector<int> compute_direct_override(
    size_t design_count,
    const std::vector<GUI::FilamentData>& machine_data)
{
    std::vector<int> result(design_count, -1);

    std::vector<size_t> validPos;
    for (size_t j = 0; j < machine_data.size(); ++j) {
        if (!is_none_filament(machine_data[j]))
            validPos.push_back(j);
    }

    size_t validCount = validPos.size();
    if (validCount == 0)
        return result;

    for (size_t i = 0; i < design_count; ++i)
        result[i] = static_cast<int>(validPos[i % validCount]);

    return result;
}

} // namespace Slic3r
