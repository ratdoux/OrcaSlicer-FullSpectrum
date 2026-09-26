#include "filament_mixer.h"

#include <algorithm>
#include <cmath>

#include "filament_mixer_model.h"

namespace Slic3r {
namespace {

inline float clamp01(float x)
{
    return std::max(0.0f, std::min(1.0f, x));
}

inline float srgb_to_linear(float x)
{
    return (x >= 0.04045f) ? std::pow((x + 0.055f) / 1.055f, 2.4f) : x / 12.92f;
}

inline float linear_to_srgb(float x)
{
    return (x >= 0.0031308f) ? (1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f) : (12.92f * x);
}

inline unsigned char to_u8(float x)
{
    const float clamped = clamp01(x);
    return static_cast<unsigned char>(clamped * 255.0f + 0.5f);
}

inline float to_f01(unsigned char x)
{
    return static_cast<float>(x) / 255.0f;
}

} // namespace

void filament_mixer_lerp(unsigned char  r1,
                         unsigned char  g1,
                         unsigned char  b1,
                         unsigned char  r2,
                         unsigned char  g2,
                         unsigned char  b2,
                         float          t,
                         unsigned char* out_r,
                         unsigned char* out_g,
                         unsigned char* out_b)
{
    if (!(t > 0.f) || (r1 == r2 && g1 == g2 && b1 == b2)) {
        *out_r = r1;
        *out_g = g1;
        *out_b = b1;
        return;
    }
    if (t >= 1.f) {
        *out_r = r2;
        *out_g = g2;
        *out_b = b2;
        return;
    }

    // Anchor the fitted polynomial P to the input colors A and B:
    // C(t) = P(t) - (1-t)*(P(0)-A) - t*(P(1)-B).
    // Evaluate before gamut clipping. The constant and linear terms cancel,
    // leaving the RGB interpolation plus only the nonlinear terms in t.
    // This preserves pigment-style curvature without a jump at either end.
    const double colors[6]       = {double(r1), double(g1), double(b1), double(r2), double(g2), double(b2)};
    const double ratio           = t;
    const double ratio_powers[5] = {1.0, ratio, ratio * ratio, ratio * ratio * ratio, ratio * ratio * ratio * ratio};
    double       channels[3]     = {(1.0 - ratio) * r1 + ratio * r2, (1.0 - ratio) * g1 + ratio * g2, (1.0 - ratio) * b1 + ratio * b2};
    for (int i = 0; i < ::filament_mixer::detail::N_FEATURES; ++i) {
        const int degree = ::filament_mixer::detail::POWERS[i][6];
        if (degree < 2)
            continue;
        double feature = ratio_powers[degree] - ratio;
        for (int j = 0; j < 6; ++j)
            for (int power = 0; power < ::filament_mixer::detail::POWERS[i][j]; ++power)
                feature *= colors[j];
        for (int channel = 0; channel < 3; ++channel)
            channels[channel] += feature * ::filament_mixer::detail::COEF[i][channel];
    }
    auto to_channel = [](double value) { return static_cast<unsigned char>(std::lround(std::clamp(value, 0.0, 255.0))); };
    *out_r          = to_channel(channels[0]);
    *out_g          = to_channel(channels[1]);
    *out_b          = to_channel(channels[2]);
}

void filament_mixer_lerp_float(float r1, float g1, float b1,
                               float r2, float g2, float b2,
                               float t,
                               float* out_r, float* out_g, float* out_b)
{
    unsigned char ur = 0, ug = 0, ub = 0;
    filament_mixer_lerp(to_u8(r1), to_u8(g1), to_u8(b1),
                        to_u8(r2), to_u8(g2), to_u8(b2),
                        t, &ur, &ug, &ub);
    *out_r = to_f01(ur);
    *out_g = to_f01(ug);
    *out_b = to_f01(ub);
}

void filament_mixer_lerp_linear_float(float r1, float g1, float b1,
                                      float r2, float g2, float b2,
                                      float t,
                                      float* out_r, float* out_g, float* out_b)
{
    const float sr1 = linear_to_srgb(clamp01(r1));
    const float sg1 = linear_to_srgb(clamp01(g1));
    const float sb1 = linear_to_srgb(clamp01(b1));
    const float sr2 = linear_to_srgb(clamp01(r2));
    const float sg2 = linear_to_srgb(clamp01(g2));
    const float sb2 = linear_to_srgb(clamp01(b2));

    float out_sr = 0.0f, out_sg = 0.0f, out_sb = 0.0f;
    filament_mixer_lerp_float(sr1, sg1, sb1, sr2, sg2, sb2, t, &out_sr, &out_sg, &out_sb);

    *out_r = srgb_to_linear(clamp01(out_sr));
    *out_g = srgb_to_linear(clamp01(out_sg));
    *out_b = srgb_to_linear(clamp01(out_sb));
}

} // namespace Slic3r
