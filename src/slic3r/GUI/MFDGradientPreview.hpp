#pragma once

#include "libslic3r/MixedFilament.hpp"

#include <wx/colour.h>
#include <wx/gdicmn.h>
#include <wx/image.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Slic3r::GUI {

// Paint the complete gradient once. Subpixel gradient brushes repeat their
// edges on Windows and leave seams when there are more samples than pixels.
inline wxImage make_mixed_gradient_raster(const std::vector<wxColour>& samples, const wxSize& size, bool vertical)
{
    if (samples.empty() || size.x <= 0 || size.y <= 0)
        return {};
    wxImage   image(size.x, size.y);
    auto*     pixels = image.GetData();
    const int span   = vertical ? size.y : size.x;
    for (int position = 0; position < span; ++position) {
        const double t               = span > 1 ? double(position) / (span - 1) : .5;
        const double sample_position = t * (samples.size() - 1);
        const size_t first           = size_t(sample_position);
        const size_t second          = std::min(first + 1, samples.size() - 1);
        const double fraction        = sample_position - first;
        const auto   channel         = [fraction](unsigned char a, unsigned char b) {
            return static_cast<unsigned char>(std::lround((1.0 - fraction) * a + fraction * b));
        };
        const unsigned char color[]    = {channel(samples[first].Red(), samples[second].Red()),
                                          channel(samples[first].Green(), samples[second].Green()),
                                          channel(samples[first].Blue(), samples[second].Blue())};
        const int           cross_span = vertical ? size.x : size.y;
        for (int cross = 0; cross < cross_span; ++cross) {
            const int x = vertical ? cross : position;
            const int y = vertical ? size.y - 1 - position : cross;
            std::copy_n(color, 3, pixels + (size_t(y) * size.x + x) * 3);
        }
    }
    return image;
}

inline wxImage make_mixed_gradient_layer_raster(const MixedFilamentDefinition&     definition,
                                                const MixedFilamentDisplayContext& context,
                                                const wxSize&                      size,
                                                bool                               vertical)
{
    const auto ids = mixed_gradient_components(definition, context.physical_colors.size());
    if (ids.size() < 2 || size.x <= 0 || size.y <= 0)
        return {};
    const int    span     = vertical ? size.y : size.x;
    const auto&  settings = context.preview_settings;
    const double nominal  = std::max(settings.gradient_nominal_layer_height, 2.0 * settings.min_sublayer_height);
    // Match ratdouxdoux's physical-pixel spacing. Applying DPI scaling again
    // makes the layers much coarser than the adjacent blended preview.
    const double              height = std::max(1.0, double(span) / 12.0) * nominal;
    std::vector<unsigned int> layers(span, ids.front());
    unsigned int              previous = 0;
    for (double z = 0.0; z < height - EPSILON;) {
        const double                    start    = z;
        const double                    progress = std::min(1.0, (z + .5 * std::min(nominal, height - z)) / height);
        const auto                      sample   = sample_mixed_gradient_local_z(definition, context.physical_colors.size(), progress,
                                                                                 settings.gradient_middle_window, nominal, settings.min_sublayer_height,
                                                                                 context.max_layer_heights);
        std::pair<unsigned int, double> passes[] = {{sample.mix.component_b, sample.height_b}, {sample.mix.component_a, sample.height_a}};
        if (previous == sample.mix.component_b && sample.mix.component_a != sample.mix.component_b)
            std::swap(passes[0], passes[1]);
        for (const auto& pass : passes) {
            if (pass.second <= EPSILON)
                continue;
            const double end   = std::min(height, z + pass.second);
            const int    first = std::clamp(int(std::lround(z / height * span)), 0, span);
            const int    last  = std::clamp(int(std::lround(end / height * span)), first, span);
            std::fill(layers.begin() + first, layers.begin() + last, pass.first);
            z        = end;
            previous = pass.first;
        }
        if (z <= start)
            break;
    }
    std::vector<wxColour> colors;
    colors.reserve(layers.size());
    for (const auto id : layers)
        colors.emplace_back(context.physical_colors[id - 1]);
    return make_mixed_gradient_raster(colors, size, vertical);
}

} // namespace Slic3r::GUI
