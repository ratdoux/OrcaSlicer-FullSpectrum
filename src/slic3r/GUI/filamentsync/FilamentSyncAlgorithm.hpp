#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "FilamentData.hpp"

namespace Slic3r {

// Closest-color matching using CIE76 Delta E in CIE Lab (D65).
// Returns design_index -> machine_index mapping; -1 means unmatched.
std::vector<int> compute_color_match(
    const std::vector<GUI::FilamentData>& design_data,
    const std::vector<GUI::FilamentData>& machine_data);

// Sequential 1:1 override: design[i] -> valid machine[i % validCount], skipping NONE slots.
std::vector<int> compute_direct_override(
    size_t design_count,
    const std::vector<GUI::FilamentData>& machine_data);

// CIE76 perceptual color distance between two sRGB colors.
// Lower value = perceptually closer.
float delta_e_cie76(uint8_t r1, uint8_t g1, uint8_t b1,
                    uint8_t r2, uint8_t g2, uint8_t b2);

} // namespace Slic3r
