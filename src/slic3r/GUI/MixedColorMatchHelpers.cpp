#include "MixedColorMatchHelpers.hpp"
#include "MixedGradientSelector.hpp"
#include <unordered_map>
#include <unordered_set>
#include <ColorSpaceConvert.hpp>
#include "MixedFilamentColorMapPanel.hpp"
#include "libslic3r/FullSpectrumKSPairResidual.hpp"
#include "libslic3r/ImageMap/ContinuousColorSolver.hpp"
#include "libslic3r/ImageMap/Sampling.hpp"
#include "GUI_App.hpp"
#include "PresetBundle.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <numeric>
#include <cctype>
#include <set>
#include <queue>
#include <sstream>
#include <mutex>
#include <boost/log/trivial.hpp>
#include "nlohmann/json.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"

namespace Slic3r { namespace GUI {
wxColour parse_mixed_color(const std::string& value)
{
    wxColour color(value);
    if (!color.IsOk())
        color = wxColour("#26A69A");
    return color;
}

wxString normalize_color_match_hex(const wxString& value)
{
    wxString normalized = value;
    normalized.Trim(true);
    normalized.Trim(false);
    normalized.MakeUpper();
    if (!normalized.empty() && normalized[0] != '#')
        normalized.Prepend("#");
    // #RRGGBBAA → #RRGGBB (drop alpha; only RGB is used for matching)
    if (normalized.length() == 9)
        normalized = normalized.Mid(0, 7);
    return normalized;
}

bool try_parse_color_match_hex(const wxString& value, wxColour& color_out)
{
    const wxString normalized = normalize_color_match_hex(value);
    if (normalized.length() != 7)
        return false;

    for (size_t idx = 1; idx < normalized.length(); ++idx) {
        const unsigned char ch = static_cast<unsigned char>(normalized[idx]);
        if (!std::isxdigit(ch))
            return false;
    }

    wxColour parsed(normalized);
    if (!parsed.IsOk())
        return false;

    color_out = parsed;
    return true;
}

std::vector<int> normalize_color_match_weights(const std::vector<int>& weights, size_t count)
{
    // Guard against count == 0: the remainder-fill loop below would otherwise
    // run with an empty `out`/`remainders` and write out[0] out of bounds.
    if (count == 0)
        return {};
    std::vector<int> out = weights;
    if (out.size() != count)
        out.assign(count, int(100 / count));

    int sum = 0;
    for (int& value : out) {
        value = std::max(0, value);
        sum += value;
    }
    if (sum <= 0 && count > 0) {
        out.assign(count, 0);
        out[0] = 100;
        return out;
    }

    std::vector<double> remainders(count, 0.0);
    int                 assigned = 0;
    for (size_t idx = 0; idx < count; ++idx) {
        const double exact = 100.0 * double(out[idx]) / double(sum);
        out[idx]           = int(std::floor(exact));
        remainders[idx]    = exact - double(out[idx]);
        assigned += out[idx];
    }

    int missing = std::max(0, 100 - assigned);
    while (missing > 0) {
        size_t best_idx       = 0;
        double best_remainder = -1.0;
        for (size_t idx = 0; idx < remainders.size(); ++idx) {
            if (remainders[idx] > best_remainder) {
                best_remainder = remainders[idx];
                best_idx       = idx;
            }
        }
        ++out[best_idx];
        remainders[best_idx] = 0.0;
        --missing;
    }

    return out;
}

std::vector<int> expand_color_match_recipe_weights(const MixedColorMatchRecipeResult& recipe, size_t num_physical)
{
    std::vector<int> weights(num_physical, 0);
    if (!recipe.valid || num_physical == 0)
        return weights;

    if (!recipe.gradient_component_ids.empty()) {
        const std::vector<unsigned int> ids = MixedFilamentManager::decode_gradient_component_ids(recipe.gradient_component_ids);
        const std::vector<int>          raw_weights =
            normalize_color_match_weights(decode_color_match_gradient_weights(recipe.gradient_component_weights, ids.size()), ids.size());
        if (ids.size() != raw_weights.size())
            return weights;
        for (size_t idx = 0; idx < ids.size(); ++idx) {
            if (ids[idx] >= 1 && ids[idx] <= num_physical)
                weights[ids[idx] - 1] = raw_weights[idx];
        }
        return weights;
    }

    if (recipe.component_a >= 1 && recipe.component_a <= num_physical)
        weights[recipe.component_a - 1] = std::max(0, 100 - std::clamp(recipe.mix_b_percent, 0, 100));
    if (recipe.component_b >= 1 && recipe.component_b <= num_physical)
        weights[recipe.component_b - 1] = std::max(0, std::clamp(recipe.mix_b_percent, 0, 100));
    return weights;
}

std::string summarize_color_match_recipe(const MixedColorMatchRecipeResult& recipe)
{
    if (!recipe.valid)
        return {};

    std::vector<unsigned int> ids;
    std::vector<int>          weights;
    if (!recipe.gradient_component_ids.empty()) {
        ids     = MixedFilamentManager::decode_gradient_component_ids(recipe.gradient_component_ids);
        weights = normalize_color_match_weights(decode_color_match_gradient_weights(recipe.gradient_component_weights, ids.size()),
                                                ids.size());
    } else {
        ids     = {recipe.component_a, recipe.component_b};
        weights = {std::max(0, 100 - std::clamp(recipe.mix_b_percent, 0, 100)), std::max(0, std::clamp(recipe.mix_b_percent, 0, 100))};
    }
    if (ids.empty() || ids.size() != weights.size())
        return {};

    std::ostringstream out;
    for (size_t idx = 0; idx < ids.size(); ++idx) {
        if (idx > 0)
            out << '/';
        out << 'F' << ids[idx];
    }
    out << ' ';
    for (size_t idx = 0; idx < weights.size(); ++idx) {
        if (idx > 0)
            out << '/';
        out << weights[idx] << '%';
    }
    return out.str();
}

wxBitmap make_color_match_swatch_bitmap(const wxColour& color, const wxSize& size)
{
    wxBitmap   bmp(size.GetWidth(), size.GetHeight());
    wxMemoryDC dc(bmp);
    dc.SetBackground(wxBrush(wxColour(255, 255, 255)));
    dc.Clear();
    dc.SetPen(wxPen(wxColour(120, 120, 120), 1));
    dc.SetBrush(wxBrush(color.IsOk() ? color : wxColour("#26A69A")));
    dc.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
    dc.SelectObject(wxNullBitmap);
    return bmp;
}

static std::vector<std::vector<bool>> build_compatibility_matrix(size_t n)
{
    std::vector<std::vector<bool>> m(n, std::vector<bool>(n, false));
    for (size_t i = 0; i < n; ++i) {
        m[i][i] = true;
        for (size_t j = i + 1; j < n; ++j) {
            bool ok = is_filament_compatible(std::vector<unsigned int>{(unsigned int)i, (unsigned int)j});
            m[i][j] = m[j][i] = ok;
        }
    }
    return m;
}

std::vector<MixedColorMatchRecipeResult> build_color_match_presets(const std::vector<std::string>& physical_colors,
                                                                   int                             min_component_percent)
{
    std::vector<MixedColorMatchRecipeResult> presets;
    if (physical_colors.size() < 2)
        return presets;

    std::vector<wxColour> palette;
    palette.reserve(physical_colors.size());
    for (const std::string& hex : physical_colors)
        palette.emplace_back(parse_mixed_color(hex));

    constexpr size_t                k_max_presets = 9999;  // effectively unlimited
    std::unordered_set<std::string> seen_colors;
    auto                            add_candidate = [&presets, &seen_colors](MixedColorMatchRecipeResult candidate) {
        if (!candidate.valid)
            return;
        const std::string color_key = normalize_color_match_hex(candidate.preview_color.GetAsString(wxC2S_HTML_SYNTAX)).ToStdString();
        if (color_key.empty() || !seen_colors.insert(color_key).second)
            return;
        presets.emplace_back(std::move(candidate));
    };

    // Only generate 50:50 ratio for two-color combinations
    auto compat = build_compatibility_matrix(palette.size());
    for (size_t left_idx = 0; left_idx < palette.size() && presets.size() < k_max_presets; ++left_idx) {
        for (size_t right_idx = left_idx + 1; right_idx < palette.size() && presets.size() < k_max_presets; ++right_idx) {
            if (!compat[left_idx][right_idx]) continue;
            add_candidate(build_pair_color_match_candidate(palette, unsigned(left_idx + 1), unsigned(right_idx + 1), 50,
                                                           min_component_percent));
        }
    }

    const size_t           triple_limit         = std::min<size_t>(palette.size(), 6);
    const std::vector<int> equal_triple_weights = normalize_color_match_weights({1, 1, 1}, 3);
    for (size_t first_idx = 0; first_idx + 2 < triple_limit && presets.size() < k_max_presets; ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx + 1 < triple_limit && presets.size() < k_max_presets; ++second_idx) {
            for (size_t third_idx = second_idx + 1; third_idx < triple_limit && presets.size() < k_max_presets; ++third_idx) {
                if (!compat[first_idx][second_idx] || !compat[second_idx][third_idx] || !compat[first_idx][third_idx]) continue;
                const std::vector<unsigned int> ids = {unsigned(first_idx + 1), unsigned(second_idx + 1), unsigned(third_idx + 1)};
                add_candidate(build_multi_color_match_candidate(palette, ids, equal_triple_weights, min_component_percent));
                for (size_t dominant_idx = 0; dominant_idx < ids.size() && presets.size() < k_max_presets; ++dominant_idx) {
                    std::vector<int> dominant_weights(ids.size(), 25);
                    dominant_weights[dominant_idx] = 50;
                    add_candidate(build_multi_color_match_candidate(palette, ids, dominant_weights, min_component_percent));
                }
            }
        }
    }

#if 0 // four-color presets: disabled
    const size_t quad_limit = std::min<size_t>(palette.size(), 5);
    for (size_t first_idx = 0; first_idx + 3 < quad_limit && presets.size() < k_max_presets; ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx + 2 < quad_limit && presets.size() < k_max_presets; ++second_idx) {
            for (size_t third_idx = second_idx + 1; third_idx + 1 < quad_limit && presets.size() < k_max_presets; ++third_idx) {
                for (size_t fourth_idx = third_idx + 1; fourth_idx < quad_limit && presets.size() < k_max_presets; ++fourth_idx) {
                    add_candidate(build_multi_color_match_candidate(palette,
                                                                    {unsigned(first_idx + 1), unsigned(second_idx + 1),
                                                                     unsigned(third_idx + 1), unsigned(fourth_idx + 1)},
                                                                    {25, 25, 25, 25}, min_component_percent));
                }
            }
        }
    }
#endif

    return presets;
}

// ---- BlendLUT ----

BlendLUT::BlendLUT(size_t n) : m_n(n)
{
    if (n < 2) {
        m_n = 0;
        return;
    }
    // Store only b >= a entries; m_pair[a][b - a] for b in [a, n)
    m_pair.resize(n);
    for (size_t a = 0; a < n; ++a) {
        m_pair[a].resize(n - a);
        for (size_t b = a; b < n; ++b)
            m_pair[a][b - a].resize(101);
    }
}

// ---- CIELAB conversion & helpers ----

CIELab sRGB_to_CIELab(const wxColour& c)
{
    double r = c.Red()   / 255.0;
    double g = c.Green() / 255.0;
    double b = c.Blue()  / 255.0;
    float lab[3];
    RGB2Lab(float(r), float(g), float(b), &lab[0], &lab[1], &lab[2]);
    return { double(lab[0]), double(lab[1]), double(lab[2]) };
}

double delta_e_lab(const CIELab& a, const CIELab& b)
{
    return double(DeltaE00(float(a.L), float(a.a), float(a.b),
                           float(b.L), float(b.a), float(b.b)));
}

BlendLUT build_blend_lut(const std::vector<wxColour>& palette)
{
    const size_t n = palette.size();
    BlendLUT lut(n);
    if (lut.empty()) return lut;

    for (size_t a = 0; a < n; ++a) {
        for (size_t b = a; b < n; ++b) {
            for (int pct = 0; pct <= 100; ++pct) {
                wxColour blended = blend_pair_filament_mixer(palette[a], palette[b], float(pct) / 100.f);
                lut.m_pair[a][b - a][pct] = sRGB_to_CIELab(blended);
            }
        }
    }
    return lut;
}

CIELab blend_weighted_lab_accurate(const std::vector<wxColour>& palette,
                                    const std::vector<unsigned int>& ids,
                                    const std::vector<int>& weights)
{
    if (ids.size() != weights.size() || ids.empty())
        return { 50.0, 0.0, 0.0 };

    // Sort by filament ID ascending — matches blend_display_color_from_sequence
    // which iterates IDs from 1..n. Sequential lerp order matters.
    std::vector<std::pair<unsigned int, int>> sorted;
    sorted.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i)
        sorted.emplace_back(ids[i], weights[i]);
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<wxColour> colors;
    std::vector<double>   dweights;
    colors.reserve(sorted.size());
    dweights.reserve(sorted.size());
    for (const auto& [id, w] : sorted) {
        if (id == 0 || id > palette.size()) continue;
        colors.push_back(palette[id - 1]);
        dweights.push_back(double(std::max(0, w)));
    }

    wxColour blended = blend_multi_filament_mixer(colors, dweights);
    return sRGB_to_CIELab(blended);
}

// ---- ΔE2000 ----

double color_delta_e00(const wxColour& lhs, const wxColour& rhs)
{
    float lhs_l = 0.f, lhs_a = 0.f, lhs_b = 0.f;
    float rhs_l = 0.f, rhs_a = 0.f, rhs_b = 0.f;
    RGB2Lab(float(lhs.Red()) / 255.f, float(lhs.Green()) / 255.f, float(lhs.Blue()) / 255.f, &lhs_l, &lhs_a, &lhs_b);
    RGB2Lab(float(rhs.Red()) / 255.f, float(rhs.Green()) / 255.f, float(rhs.Blue()) / 255.f, &rhs_l, &rhs_a, &rhs_b);
    return double(DeltaE00(lhs_l, lhs_a, lhs_b, rhs_l, rhs_a, rhs_b));
}

MixedColorMatchRecipeResult build_best_color_match_recipe(const std::vector<std::string>& physical_colors,
                                                          const wxColour&                 target_color,
                                                          int                             min_component_percent,
                                                          const std::vector<double>&      physical_tds,
                                                          const std::vector<std::string>& physical_material_ids,
                                                          MixedFilamentColorEngine        color_engine,
                                                          bool                            use_td_prediction,
                                                          int                             max_component_percent,
                                                          bool                            check_compatible)
{
    MixedColorMatchRecipeResult best;
    if (!target_color.IsOk() || physical_colors.size() < 2)
        return best;

    // Validate max_component_percent symmetrically with batch_match_model_colors's
    // min check. Range [50, 100]: the floor matches loop_min_weight's clamp ceiling
    // (a max < the effective min makes the search window empty and silently yields no
    // recipe); 100 is the physical cap. Out-of-range input returns an invalid result
    // rather than letting the search loops run with nonsensical bounds (e.g. a value
    // >100 would widen the upper bound past 100% and silently bypass the cap).
    if (max_component_percent < 50 || max_component_percent > 100) {
        BOOST_LOG_TRIVIAL(warning)
            << "build_best_color_match_recipe: max_component_percent=" << max_component_percent
            << " out of [50, 100]; returning invalid recipe";
        return best;
    }

    // ---- Step 1: build palette & pre-convert to Lab ----
    const size_t n = physical_colors.size();
    std::vector<wxColour> palette;
    std::vector<CIELab>   palette_lab;
    palette.reserve(n);
    palette_lab.reserve(n);
    for (const std::string& hex : physical_colors) {
        wxColour c = parse_mixed_color(hex);
        palette.emplace_back(c);
        palette_lab.emplace_back(sRGB_to_CIELab(c));
    }
    const CIELab target_lab = sRGB_to_CIELab(target_color);

    const bool use_full_spectrum = color_engine == MixedFilamentColorEngine::FullSpectrumKSPairResidual;
    auto selected_engine_preview = [&](const std::vector<unsigned int>& ids, const std::vector<int>& weights) {
        std::vector<FullSpectrumKSPairResidualColorInput> inputs;
        std::vector<wxColour>                             fallback_colors;
        std::vector<double>                               fallback_weights;
        const size_t                                      count = std::min(ids.size(), weights.size());
        inputs.reserve(count);
        fallback_colors.reserve(count);
        fallback_weights.reserve(count);

        for (size_t i = 0; i < count; ++i) {
            const unsigned int id = ids[i];
            const int          weight = std::max(0, weights[i]);
            if (id == 0 || id > physical_colors.size() || weight == 0)
                continue;

            std::optional<double> td_mm;
            if (use_td_prediction && id <= physical_tds.size()) {
                const double value = physical_tds[id - 1];
                if (std::isfinite(value) && value > 0.0)
                    td_mm = value;
            }
            std::optional<std::string> material_id;
            if (id <= physical_material_ids.size() && !physical_material_ids[id - 1].empty())
                material_id = physical_material_ids[id - 1];
            inputs.push_back({physical_colors[id - 1], weight, td_mm, material_id});
            fallback_colors.push_back(palette[id - 1]);
            fallback_weights.push_back(double(weight));
        }

        if (const std::optional<std::string> prediction = full_spectrum_ks_blend_color_multi(inputs))
            return parse_mixed_color(*prediction);
        return blend_multi_filament_mixer(fallback_colors, fallback_weights);
    };

    const int  loop_min_weight      = std::max(1, std::clamp(min_component_percent, 0, 50));

    // check_compatible=false (manual batch mode) bypasses the category filter so
    // cross-type mixes (e.g. PLA+PETG) can be computed and stored. The downstream
    // slice gate (Plater::has_incompatible_mixed_filament_in_use) still blocks
    // incompatible mixes at slice time — this only widens the candidate pool during
    // recipe search. All `if (!compat[i][j]) continue;` sites below stay unchanged;
    // an all-true matrix makes them no-ops.
    std::vector<std::vector<bool>> compat;
    if (check_compatible) {
        compat = build_compatibility_matrix(n);
    } else {
        compat.assign(n, std::vector<bool>(n, true));
    }

    // Helper: encode filament IDs as gradient_component_ids string.
    // Legacy format (all IDs ≤ 9): concatenated single chars, e.g. "123".
    // Extended format (any ID > 9): '/' separated decimals, e.g. "1/12/3".
    // Single-ID extended format uses leading '/' to disambiguate from legacy: "/12".
    auto encode_gradient_ids = [](const std::vector<unsigned int>& ids) -> std::string {
        return MixedFilamentManager::encode_gradient_component_ids(ids);
    };

    auto encode_gradient_weights = [](const std::vector<int>& weights) -> std::string {
        std::ostringstream ss;
        for (size_t i = 0; i < weights.size(); ++i) {
            if (i > 0) ss << '/';
            ss << weights[i];
        }
        return ss.str();
    };

    // ---- Step 2: build pair Blend LUT (polynomial mixing → Lab) ----
    const BlendLUT lut = build_blend_lut(palette);
    if (lut.empty()) return best;

    // ---- helper: update best from a pair candidate ----
    auto update_best_pair = [&](unsigned int a, unsigned int b, int pct, double de, const wxColour* selected_preview = nullptr) {
        if (!best.valid || de + 1e-6 < best.delta_e) {
            best.valid         = true;
            best.component_a   = a;
            best.component_b   = b;
            best.mix_b_percent = pct;
            best.preview_color = selected_preview != nullptr ? *selected_preview :
                blend_pair_filament_mixer(palette[a - 1], palette[b - 1], float(pct) / 100.f);
            best.delta_e       = de;
            best.gradient_component_ids.clear();
            best.gradient_component_weights.clear();
            best.manual_pattern.clear();
        }
    };

    // ---- Step 3: pair coarse scan (step=5%) ----
    constexpr int k_coarse_step = 5;
    constexpr int k_top_coarse  = 30;

    // max-heap of (ΔE, a, b, percent) — keeps top-k LOWEST ΔE, worst at top
    using HeapEntry = std::tuple<double, unsigned int, unsigned int, int>;
    auto cmp = [](const HeapEntry& x, const HeapEntry& y) { return std::get<0>(x) < std::get<0>(y); };
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype(cmp)> heap(cmp);

    for (size_t a = 0; a < n; ++a) {
        for (size_t b = a + 1; b < n; ++b) {
            if (!compat[a][b]) continue;
            for (int pct = std::max(loop_min_weight, 100 - max_component_percent);
                 pct <= std::min(100 - loop_min_weight, max_component_percent); pct += k_coarse_step) {
                double de = 0.0;
                if (use_full_spectrum) {
                    const wxColour preview = selected_engine_preview(
                        {unsigned(a + 1), unsigned(b + 1)}, {100 - pct, pct});
                    de = delta_e_lab(target_lab, sRGB_to_CIELab(preview));
                    update_best_pair(unsigned(a + 1), unsigned(b + 1), pct, de, &preview);
                } else {
                    de = delta_e_lab(target_lab, lut.get(a, b, pct));
                    update_best_pair(unsigned(a + 1), unsigned(b + 1), pct, de);
                }
                if (heap.size() < k_top_coarse) {
                    heap.emplace(de, unsigned(a + 1), unsigned(b + 1), pct);
                } else if (de < std::get<0>(heap.top())) {
                    heap.pop();
                    heap.emplace(de, unsigned(a + 1), unsigned(b + 1), pct);
                }
            }
        }
    }

    // ---- Step 4: pair fine search (step=1%, top-N from coarse) ----
    while (!heap.empty()) {
        auto [de, a, b, coarse_pct] = heap.top();
        heap.pop();
        int fine_min = std::max(std::max(loop_min_weight, 100 - max_component_percent), coarse_pct - k_coarse_step + 1);
        int fine_max = std::min(std::min(100 - loop_min_weight, max_component_percent), coarse_pct + k_coarse_step - 1);
        for (int pct = fine_min; pct <= fine_max; ++pct) {
            if ((pct - loop_min_weight) % k_coarse_step == 0) continue; // already evaluated in coarse
            if (use_full_spectrum) {
                const wxColour preview = selected_engine_preview({a, b}, {100 - pct, pct});
                update_best_pair(a, b, pct, delta_e_lab(target_lab, sRGB_to_CIELab(preview)), &preview);
            } else {
                update_best_pair(a, b, pct, delta_e_lab(target_lab, lut.get(a - 1, b - 1, pct)));
            }
        }
    }

    // ---- save best pair (before triple search may overwrite) ----
    MixedColorMatchRecipeResult best_pair = best;

    // ---- Step 5: early termination ----
    if (best_pair.valid && best_pair.delta_e <= 0.5)
        return best_pair;

    // ---- Step 6: adaptive candidate pool (top-N by single-color ΔE) ----
    std::vector<std::pair<double, unsigned int>> ranked_ids;
    ranked_ids.reserve(n);
    for (size_t idx = 0; idx < n; ++idx)
        ranked_ids.emplace_back(delta_e_lab(target_lab, palette_lab[idx]), unsigned(idx + 1));
    std::sort(ranked_ids.begin(), ranked_ids.end(), [](const auto& x, const auto& y) {
        if (x.first != y.first) return x.first < y.first;
        return x.second < y.second;
    });

    const size_t pool_size = std::min<size_t>(n, 8);
    std::vector<unsigned int> candidate_pool;
    candidate_pool.reserve(pool_size);
    for (size_t i = 0; i < pool_size; ++i)
        candidate_pool.emplace_back(ranked_ids[i].second);

    if (candidate_pool.size() < 3)
        return best;

    std::sort(candidate_pool.begin(), candidate_pool.end());

    // ---- Step 7: triple layered search ----
    constexpr int k_triple_coarse_step = 10;
    constexpr int k_top_triple        = 20;

    struct TripleEntry {
        double       de;
        unsigned int a, b, c;
        int          wa, wb;
        bool operator<(const TripleEntry& o) const { return de < o.de; }
    };
    std::priority_queue<TripleEntry> triple_heap;

    // Coarse (step=10%)
    for (size_t fi = 0; fi + 2 < candidate_pool.size(); ++fi) {
        for (size_t fj = fi + 1; fj + 1 < candidate_pool.size(); ++fj) {
            for (size_t fk = fj + 1; fk < candidate_pool.size(); ++fk) {
                unsigned int a = candidate_pool[fi], b = candidate_pool[fj], c = candidate_pool[fk];
                if (!compat[a - 1][b - 1] || !compat[b - 1][c - 1] || !compat[a - 1][c - 1]) continue;

                for (int wa = loop_min_weight; wa <= std::min(100 - 2 * loop_min_weight, max_component_percent); wa += k_triple_coarse_step) {
                    for (int wb = loop_min_weight; wb <= std::min(100 - wa - loop_min_weight, max_component_percent); wb += k_triple_coarse_step) {
                        int wc = 100 - wa - wb;
                        if (wc < loop_min_weight || wc > max_component_percent) continue;
                        const wxColour selected_preview = use_full_spectrum ?
                            selected_engine_preview({a, b, c}, {wa, wb, wc}) : wxColour();
                        const CIELab blended = use_full_spectrum ? sRGB_to_CIELab(selected_preview) :
                            blend_weighted_lab_accurate(palette, {a, b, c}, {wa, wb, wc});
                        double de = delta_e_lab(target_lab, blended);
                        // Update best triple
                        if (!best.valid || de + 1e-6 < best.delta_e) {
                            best.valid     = true;
                            best.component_a = a;
                            best.component_b = b;
                            best.mix_b_percent = wa + wb > 0 ? int(std::lround(100.0 * double(wb) / double(wa + wb))) : 50;
                            best.gradient_component_ids     = encode_gradient_ids({a, b, c});
                            best.gradient_component_weights = encode_gradient_weights({wa, wb, wc});
                            best.preview_color = use_full_spectrum ? selected_preview :
                                blend_multi_filament_mixer({palette[a - 1], palette[b - 1], palette[c - 1]},
                                                           {double(wa), double(wb), double(wc)});
                            best.delta_e = de;
                            best.manual_pattern.clear();
                        }
                        if (triple_heap.size() < k_top_triple) {
                            triple_heap.push({de, a, b, c, wa, wb});
                        } else if (de < triple_heap.top().de) {
                            triple_heap.pop();
                            triple_heap.push({de, a, b, c, wa, wb});
                        }
                    }
                }
            }
        }
    }

    // Fine (step=1%, refine ±5 window around coarse center)
    while (!triple_heap.empty()) {
        TripleEntry te = triple_heap.top();
        triple_heap.pop();
        int wa_min = std::max(loop_min_weight, te.wa - k_triple_coarse_step + 1);
        int wa_max = std::min(std::min(100 - 2 * loop_min_weight, max_component_percent), te.wa + k_triple_coarse_step - 1);
        for (int wa = wa_min; wa <= wa_max; ++wa) {
            if ((wa - loop_min_weight) % k_triple_coarse_step == 0) continue;
            int wb_min = std::max(loop_min_weight, te.wb - k_triple_coarse_step + 1);
            int wb_max = std::min(std::min(100 - wa - loop_min_weight, max_component_percent), te.wb + k_triple_coarse_step - 1);
            for (int wb = wb_min; wb <= wb_max; ++wb) {
                if ((wb - loop_min_weight) % k_triple_coarse_step == 0) continue;
                int wc = 100 - wa - wb;
                if (wc < loop_min_weight || wc > max_component_percent) continue;
                const wxColour selected_preview = use_full_spectrum ?
                    selected_engine_preview({te.a, te.b, te.c}, {wa, wb, wc}) : wxColour();
                const CIELab blended = use_full_spectrum ? sRGB_to_CIELab(selected_preview) :
                    blend_weighted_lab_accurate(palette, {te.a, te.b, te.c}, {wa, wb, wc});
                double de2 = delta_e_lab(target_lab, blended);
                if (!best.valid || de2 + 1e-6 < best.delta_e) {
                    best.valid     = true;
                    best.component_a = te.a;
                    best.component_b = te.b;
                    best.mix_b_percent = wa + wb > 0 ? int(std::lround(100.0 * double(wb) / double(wa + wb))) : 50;
                    best.gradient_component_ids     = encode_gradient_ids({te.a, te.b, te.c});
                    best.gradient_component_weights = encode_gradient_weights({wa, wb, wc});
                    best.preview_color = use_full_spectrum ? selected_preview :
                        blend_multi_filament_mixer({palette[te.a - 1], palette[te.b - 1], palette[te.c - 1]},
                                                   {double(wa), double(wb), double(wc)});
                    best.delta_e = de2;
                    best.manual_pattern.clear();
                }
            }
        }
    }

    // ---- final normalization: re-evaluate ΔE with consistent color_delta_e00 ----
    // Pair and triple search may use different evaluation paths (LUT vs on-the-fly
    // blend_multi_filament_mixer); re-evaluate both via the same pipeline for a fair
    // comparison, then prefer the simpler (pair) recipe when ΔE gain is negligible.
    if (best.valid)
        best.delta_e = color_delta_e00(target_color, best.preview_color);
    if (best_pair.valid) {
        best_pair.delta_e = color_delta_e00(target_color, best_pair.preview_color);
        // Pick the true winner under unified evaluation
        if (!best.valid || best_pair.delta_e + 1e-6 < best.delta_e)
            best = std::move(best_pair);
        // Prefer simpler recipe when multi-color ΔE advantage is imperceptible
        else if (!best.gradient_component_ids.empty() &&
                 best_pair.delta_e <= best.delta_e + 0.5)
            best = std::move(best_pair);
    }

    return best;
}

MixedColorMatchRecipeResult build_best_color_match_recipe(const std::vector<std::string>& physical_colors,
                                                          const wxColour&                 target_color,
                                                          int                             min_component_percent,
                                                          int                             max_component_percent,
                                                          bool                            check_compatible)
{
    return build_best_color_match_recipe(physical_colors,
                                         target_color,
                                         min_component_percent,
                                         {},
                                         {},
                                         MixedFilamentColorEngine::FilamentMixer,
                                         false,
                                         max_component_percent,
                                         check_compatible);
}

namespace {

struct DecodedColorMatchRecipe
{
    std::vector<unsigned int> component_ids;
    std::vector<int>          component_weights;
};

DecodedColorMatchRecipe decode_color_match_recipe(const MixedColorMatchRecipeResult& recipe, size_t num_physical)
{
    DecodedColorMatchRecipe decoded;
    if (!recipe.gradient_component_ids.empty()) {
        decoded.component_ids     = MixedFilamentManager::decode_gradient_component_ids(recipe.gradient_component_ids, num_physical);
        decoded.component_weights = normalize_color_match_weights(decode_color_match_gradient_weights(recipe.gradient_component_weights,
                                                                                                      decoded.component_ids.size()),
                                                                  decoded.component_ids.size());
    }
    if (decoded.component_ids.size() < 2) {
        decoded.component_ids     = {recipe.component_a, recipe.component_b};
        decoded.component_weights = {100 - std::clamp(recipe.mix_b_percent, 0, 100), std::clamp(recipe.mix_b_percent, 0, 100)};
    }
    return decoded;
}

std::vector<int> color_match_weights_at_precision(const std::vector<double>& weights, int total_units)
{
    std::vector<int> out(weights.size(), 0);
    double           total = 0.0;
    for (const double weight : weights)
        if (std::isfinite(weight) && weight > 0.0)
            total += weight;
    if (weights.empty() || total <= EPSILON || total_units <= 0)
        return out;

    std::vector<std::pair<double, size_t>> remainders;
    remainders.reserve(weights.size());
    int assigned = 0;
    for (size_t index = 0; index < weights.size(); ++index) {
        const double exact = std::max(0.0, weights[index]) * double(total_units) / total;
        out[index]         = int(std::floor(exact));
        assigned += out[index];
        remainders.emplace_back(exact - double(out[index]), index);
    }
    std::sort(remainders.begin(), remainders.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.first != rhs.first)
            return lhs.first > rhs.first;
        return lhs.second < rhs.second;
    });
    const int missing_units = total_units - assigned;
    for (int index = 0; index < missing_units && !remainders.empty(); ++index)
        ++out[remainders[size_t(index) % remainders.size()].second];
    return out;
}

wxColour predict_color_match_weights(const std::vector<unsigned int>&   component_ids,
                                     const std::vector<double>&         component_weights,
                                     const MixedFilamentDisplayContext& context)
{
    const std::vector<int>               precise_weights = color_match_weights_at_precision(component_weights, 10000);
    std::vector<MixedFilamentColorInput> inputs;
    inputs.reserve(std::min(component_ids.size(), precise_weights.size()));
    for (size_t index = 0; index < component_ids.size() && index < precise_weights.size(); ++index) {
        const unsigned int component_id = component_ids[index];
        if (component_id == 0 || component_id > context.physical_colors.size() || precise_weights[index] <= 0)
            continue;

        std::optional<double> td_mm;
        if (component_id <= context.physical_tds.size()) {
            const double value = context.physical_tds[component_id - 1];
            if (std::isfinite(value) && value > 0.0)
                td_mm = value;
        }
        std::optional<std::string> material_id;
        if (component_id <= context.physical_material_ids.size() && !context.physical_material_ids[component_id - 1].empty())
            material_id = context.physical_material_ids[component_id - 1];
        inputs.push_back({context.physical_colors[component_id - 1], precise_weights[index], td_mm, material_id});
    }
    if (inputs.empty())
        return wxColour("#26A69A");
    if (inputs.size() == 1)
        return parse_mixed_color(inputs.front().color_hex);
    return parse_mixed_color(MixedFilamentManager::blend_color_multi(inputs));
}

bool refine_color_match_recipe_with_surface_bias(MixedColorMatchRecipeResult&       recipe,
                                                 const wxColour&                    target_color,
                                                 size_t                             num_physical,
                                                 const MixedFilamentDisplayContext& context,
                                                 float                              reference_width_mm)
{
    const DecodedColorMatchRecipe decoded = decode_color_match_recipe(recipe, num_physical);
    if (decoded.component_ids.size() < 2 || decoded.component_ids.size() != decoded.component_weights.size())
        return false;

    std::vector<double> base_weights;
    base_weights.reserve(decoded.component_weights.size());
    for (const int weight : decoded.component_weights)
        base_weights.emplace_back(double(std::max(0, weight)));

    std::vector<double> best_weights = base_weights;
    wxColour            best_color   = predict_color_match_weights(decoded.component_ids, best_weights, context);
    const double        base_delta_e = color_delta_e00(target_color, best_color);
    double              best_delta_e = base_delta_e;

    // Bias is a residual correction, not a replacement for the printable layer
    // recipe. Restrict each apparent component to a five percentage-point
    // neighborhood around the integer cadence selected above.
    constexpr double                k_max_residual_shift_percent = 5.0;
    constexpr std::array<double, 4> k_steps{1.0, 0.25, 0.10, 0.05};
    for (const double step : k_steps) {
        for (size_t iteration = 0; iteration < 64; ++iteration) {
            bool                improved          = false;
            std::vector<double> iteration_weights = best_weights;
            wxColour            iteration_color   = best_color;
            double              iteration_delta_e = best_delta_e;
            for (size_t donor = 0; donor < best_weights.size(); ++donor) {
                if (best_weights[donor] + EPSILON < step)
                    continue;
                for (size_t receiver = 0; receiver < best_weights.size(); ++receiver) {
                    if (receiver == donor)
                        continue;
                    std::vector<double> candidate = best_weights;
                    candidate[donor] -= step;
                    candidate[receiver] += step;
                    bool within_residual = true;
                    for (size_t index = 0; index < candidate.size(); ++index)
                        within_residual &= std::abs(candidate[index] - base_weights[index]) <= k_max_residual_shift_percent + EPSILON;
                    if (!within_residual)
                        continue;

                    const wxColour candidate_color = predict_color_match_weights(decoded.component_ids, candidate, context);
                    const double   candidate_delta = color_delta_e00(target_color, candidate_color);
                    if (candidate_delta + 1e-6 < iteration_delta_e) {
                        iteration_weights = std::move(candidate);
                        iteration_color   = candidate_color;
                        iteration_delta_e = candidate_delta;
                        improved          = true;
                    }
                }
            }
            if (!improved)
                break;
            best_weights = std::move(iteration_weights);
            best_color   = iteration_color;
            best_delta_e = iteration_delta_e;
        }
    }

    if (base_delta_e - best_delta_e < 0.05)
        return false;
    const std::vector<float> offsets = mixed_filament_surface_offsets_for_apparent_weights(decoded.component_weights, best_weights,
                                                                                           reference_width_mm);
    if (offsets.size() != best_weights.size() ||
        std::none_of(offsets.begin(), offsets.end(), [](float offset) { return std::abs(offset) >= 0.0005f; }))
        return false;

    recipe.apparent_component_weights = std::move(best_weights);
    recipe.preview_color              = best_color;
    recipe.delta_e                    = best_delta_e;
    return true;
}

bool definition_has_surface_bias(const MixedFilamentDefinition& definition)
{
    const std::vector<float> offsets = mixed_filament_component_surface_offsets(definition);
    return std::any_of(offsets.begin(), offsets.end(), [](float offset) { return std::abs(offset) >= 0.0005f; });
}

std::vector<unsigned int> normalized_perimeter_component_ids(const std::vector<unsigned int>& requested, size_t num_physical)
{
    std::vector<unsigned int> component_ids;
    if (requested.empty()) {
        component_ids.resize(num_physical);
        std::iota(component_ids.begin(), component_ids.end(), 1u);
        return component_ids;
    }

    component_ids.reserve(requested.size());
    for (const unsigned int component_id : requested)
        if (component_id >= 1 && component_id <= num_physical &&
            std::find(component_ids.begin(), component_ids.end(), component_id) == component_ids.end())
            component_ids.emplace_back(component_id);
    std::sort(component_ids.begin(), component_ids.end());
    return component_ids;
}

bool definition_has_shared_image_map_cadence(const MixedFilamentDefinition&   definition,
                                             size_t                           num_physical,
                                             const std::vector<unsigned int>& expected_component_ids = {})
{
    if (num_physical < 2 || !definition.behavior.surface_bias.perimeter_modulation || definition.recipe.manual_pattern ||
        definition.behavior.gradient.enabled || definition.recipe.blend.components.size() < 2)
        return false;

    std::vector<unsigned int> actual_component_ids;
    actual_component_ids.reserve(definition.recipe.blend.components.size());
    int min_weight = 100;
    int max_weight = 0;
    for (const MixedFilamentWeightedComponent& component : definition.recipe.blend.components) {
        if (component.filament.id < 1 || component.filament.id > num_physical || component.percent <= 0 ||
            std::find(actual_component_ids.begin(), actual_component_ids.end(), component.filament.id) != actual_component_ids.end())
            return false;
        actual_component_ids.emplace_back(component.filament.id);
        min_weight = std::min(min_weight, component.percent);
        max_weight = std::max(max_weight, component.percent);
    }
    std::sort(actual_component_ids.begin(), actual_component_ids.end());
    const std::vector<unsigned int> expected = normalized_perimeter_component_ids(expected_component_ids, num_physical);
    return max_weight - min_weight <= 1 && (expected_component_ids.empty() || actual_component_ids == expected);
}

bool definition_has_adaptive_perimeter_cycle(const MixedFilamentDefinition& definition, size_t num_physical)
{
    return !definition.visibility.tombstoned && !definition.recipe.manual_pattern &&
           definition.behavior.surface_bias.perimeter_modulation && !definition_has_shared_image_map_cadence(definition, num_physical);
}

} // namespace

MixedFilamentDefinition mixed_filament_definition_from_color_match_recipe(const MixedColorMatchRecipeResult& recipe,
                                                                          size_t                             num_physical,
                                                                          MixedColorMatchEncoding            encoding,
                                                                          float                              reference_width_mm,
                                                                          const std::vector<unsigned int>&   perimeter_component_ids)
{
    MixedFilamentDefinition definition;
    definition.source.kind = MixedFilamentSourceKind::Custom;
    definition.recipe.kind = MixedFilamentRecipeKind::WeightedBlend;

    DecodedColorMatchRecipe decoded                    = decode_color_match_recipe(recipe, num_physical);
    std::vector<double>     apparent_component_weights = recipe.apparent_component_weights;
    if (encoding == MixedColorMatchEncoding::PerimeterModulatedLayerSequence) {
        decoded.component_ids                 = normalized_perimeter_component_ids(perimeter_component_ids, num_physical);
        decoded.component_weights             = normalize_color_match_weights(std::vector<int>(decoded.component_ids.size(), 1),
                                                                              decoded.component_ids.size());
        const std::vector<int> target_weights = normalize_color_match_weights(expand_color_match_recipe_weights(recipe, num_physical),
                                                                              num_physical);
        apparent_component_weights.clear();
        apparent_component_weights.reserve(decoded.component_ids.size());
        for (const unsigned int component_id : decoded.component_ids)
            apparent_component_weights.emplace_back(component_id <= target_weights.size() ? target_weights[component_id - 1] : 0);
        if (std::none_of(apparent_component_weights.begin(), apparent_component_weights.end(), [](double weight) { return weight > 0.0; }))
            apparent_component_weights.assign(decoded.component_ids.size(), 1.0);
    }
    std::vector<unsigned int>& component_ids     = decoded.component_ids;
    std::vector<int>&          component_weights = decoded.component_weights;

    for (size_t index = 0; index < component_ids.size() && index < component_weights.size(); ++index)
        definition.recipe.blend.components.push_back({{component_ids[index]}, component_weights[index]});

    if (encoding == MixedColorMatchEncoding::LayerRatio && !recipe.manual_pattern.empty()) {
        definition.recipe.manual_pattern = mixed_filament_manual_pattern_from_string(
            recipe.manual_pattern, recipe.component_a, recipe.component_b, num_physical);
    }
    const bool adaptive_perimeter_cycles = encoding == MixedColorMatchEncoding::AdaptiveLocalizedCycles;
    definition.behavior.distribution = encoding == MixedColorMatchEncoding::PerimeterModulatedLayerSequence || adaptive_perimeter_cycles ||
                                                definition.recipe.manual_pattern || definition.recipe.blend.components.size() >= 3 ?
                                            MixedFilamentDistributionMode::LayerCycle :
                                            MixedFilamentDistributionMode::Simple;
    if ((encoding == MixedColorMatchEncoding::SurfaceBias ||
         encoding == MixedColorMatchEncoding::PerimeterModulatedLayerSequence || adaptive_perimeter_cycles) &&
        apparent_component_weights.size() == component_weights.size()) {
        set_mixed_filament_component_surface_offsets(definition,
                                                     mixed_filament_surface_offsets_for_apparent_weights(component_weights,
                                                                                                         apparent_component_weights,
                                                                                                         reference_width_mm));
    }
    definition.behavior.surface_bias.perimeter_modulation =
        encoding == MixedColorMatchEncoding::PerimeterModulatedLayerSequence || adaptive_perimeter_cycles;
    definition.presentation.display_color =
        recipe.preview_color.IsOk() ? recipe.preview_color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString() : std::string("#26A69A");
    return definition;
}

static void set_color_match_config_bool(PresetBundle& preset_bundle, const std::string& key, bool value)
{
    DynamicPrintConfig &print_config = preset_bundle.prints.get_edited_preset().config;
    if (ConfigOptionBool *option = print_config.option<ConfigOptionBool>(key))
        option->value = value;
    else if (ConfigOptionInt *option = print_config.option<ConfigOptionInt>(key))
        option->value = value ? 1 : 0;
    else
        print_config.set_key_value(key, new ConfigOptionBool(value));

    if (ConfigOptionBool *option = preset_bundle.project_config.option<ConfigOptionBool>(key))
        option->value = value;
    else if (ConfigOptionInt *option = preset_bundle.project_config.option<ConfigOptionInt>(key))
        option->value = value ? 1 : 0;
    else
        preset_bundle.project_config.set_key_value(key, new ConfigOptionBool(value));
}

static void configure_surface_bias_color_match_mode(PresetBundle &preset_bundle)
{

    // Surface-bias image maps use the normal layer cadence. Local-Z is a
    // mutually exclusive geometry path and would suppress the XY offsets.
    set_color_match_config_bool(preset_bundle, "mixed_filament_component_bias_enabled", true);
    set_color_match_config_bool(preset_bundle, "dithering_local_z_mode", false);
}

void configure_adaptive_perimeter_color_match_mode()
{
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        return;

    configure_surface_bias_color_match_mode(*preset_bundle);
    // Adaptive image-map rows are selected for Local-Z by their object-zone
    // ownership, not by the global "subdivide all mixes" override. Keep the
    // direct, independent solver visible in the project settings so its
    // cadence policy is explicit and survives a 3MF round trip.
    set_color_match_config_bool(*preset_bundle, "dithering_local_z_direct_multicolor", true);
    set_color_match_config_bool(*preset_bundle, "dithering_local_z_independent_layer_height", true);
    set_color_match_config_bool(*preset_bundle, "dithering_local_z_whole_objects", false);
    set_color_match_config_bool(*preset_bundle, "dithering_local_z_infill", false);
    set_color_match_config_bool(*preset_bundle, "fs_surface_paint_only", true);
}

static std::pair<unsigned int, double> closest_physical_color_match(
    const wxColour& target_color, const std::vector<std::string>& physical_colors)
{
    std::pair<unsigned int, double> closest{1, std::numeric_limits<double>::infinity()};
    for (size_t index = 0; index < physical_colors.size(); ++index) {
        const double delta = color_delta_e00(target_color, parse_mixed_color(physical_colors[index]));
        if (delta + 1e-6 < closest.second)
            closest = {unsigned(index + 1), delta};
    }
    return closest;
}

static MixedColorMatchRecipeResult build_image_map_color_match_recipe(
    const std::vector<std::string>&    physical_colors,
    const wxColour&                    target_color,
    int                                min_component_percent,
    const MixedFilamentDisplayContext& context,
    ImageMap::ColorMixModel            color_mix_model)
{
    MixedColorMatchRecipeResult result;
    if (!target_color.IsOk() || physical_colors.size() < 2)
        return result;

    std::vector<ImageMap::ContinuousColorComponent> components;
    components.reserve(physical_colors.size());
    for (size_t index = 0; index < physical_colors.size(); ++index) {
        ImageMap::ContinuousColorComponent component;
        component.color_hex = physical_colors[index];
        if (index < context.physical_tds.size() && std::isfinite(context.physical_tds[index]) && context.physical_tds[index] > 0.0)
            component.transmission_distance_mm = context.physical_tds[index];
        if (index < context.physical_material_ids.size() && !context.physical_material_ids[index].empty())
            component.material_id = context.physical_material_ids[index];
        components.emplace_back(std::move(component));
    }

    ImageMap::ContinuousColorRecipeSolver solver(std::move(components), 4, color_mix_model);
    const RGBA target{float(target_color.Red()) / 255.f, float(target_color.Green()) / 255.f,
                      float(target_color.Blue()) / 255.f, 1.f};
    const ImageMap::ContinuousColorRecipe recipe = solver.solve(target, min_component_percent);
    if (!recipe.valid() || recipe.component_indices.size() < 2 || !recipe.predicted_color)
        return result;

    std::vector<unsigned int> component_ids;
    component_ids.reserve(recipe.component_indices.size());
    for (size_t component_index : recipe.component_indices)
        component_ids.emplace_back(unsigned(component_index + 1));

    result.valid         = true;
    result.component_a   = component_ids[0];
    result.component_b   = component_ids[1];
    result.mix_b_percent = recipe.component_percents.size() >= 2 ? recipe.component_percents[1] : 50;
    result.preview_color = wxColour(std::clamp(int(std::lround((*recipe.predicted_color)[0] * 255.f)), 0, 255),
                                    std::clamp(int(std::lround((*recipe.predicted_color)[1] * 255.f)), 0, 255),
                                    std::clamp(int(std::lround((*recipe.predicted_color)[2] * 255.f)), 0, 255));
    result.delta_e = color_delta_e00(target_color, result.preview_color);
    if (component_ids.size() > 2) {
        result.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(component_ids);
        std::ostringstream weights;
        for (size_t index = 0; index < recipe.component_percents.size(); ++index) {
            if (index > 0)
                weights << '/';
            weights << recipe.component_percents[index];
        }
        result.gradient_component_weights = weights.str();
    }
    return result;
}

static MixedColorMatchCreationResult create_adaptive_localized_color_match(
    const wxColour&                    target_color,
    const std::vector<std::string>&    physical_colors,
    int                                min_component_percent,
    size_t                             max_total_filaments,
    const MixedFilamentDisplayContext& context,
    MixedFilamentManager&              manager,
    MixedColorMatchCreationResult      result,
    ImageMap::ColorMixModel            color_mix_model)
{
    // Adaptive cycles are a structural/tool-change decision, not merely a
    // colour-search result. Keep near matches on a physical filament and only
    // introduce a local cycle when its perceptual improvement is large enough
    // to justify another material boundary on the model.
    constexpr double k_direct_filament_lock_delta_e = 3.0;
    constexpr double k_minimum_cycle_gain_delta_e    = 1.5;
    constexpr double k_relative_cycle_gain           = 0.15;
    constexpr double k_maximum_required_gain_delta_e = 4.0;

    const double direct_delta_e = result.delta_e;
    if (!std::isfinite(direct_delta_e) || direct_delta_e <= k_direct_filament_lock_delta_e)
        return result;
    const double required_gain = std::clamp(direct_delta_e * k_relative_cycle_gain,
                                            k_minimum_cycle_gain_delta_e,
                                            k_maximum_required_gain_delta_e);
    auto meaningfully_better_than_direct = [direct_delta_e, required_gain](double candidate_delta_e) {
        return std::isfinite(candidate_delta_e) && candidate_delta_e + required_gain < direct_delta_e;
    };

    size_t visible_idx = 0;
    for (const MixedFilamentDefinition& definition : manager.mixed_filament_definitions(physical_colors.size())) {
        if (definition.visibility.tombstoned)
            continue;
        const unsigned int filament_id = unsigned(physical_colors.size() + visible_idx + 1);
        ++visible_idx;
        if (!definition_has_adaptive_perimeter_cycle(definition, physical_colors.size()))
            continue;

        wxColour existing_color = parse_mixed_color(compute_mixed_filament_display_color(definition, context));
        const std::vector<unsigned int> component_ids = definition.recipe.blend.component_ids(physical_colors.size());
        if (component_ids.size() >= 2) {
            std::vector<ImageMap::ContinuousColorComponent> components;
            components.reserve(component_ids.size());
            for (unsigned int component_id : component_ids) {
                if (component_id < 1 || component_id > physical_colors.size())
                    continue;
                ImageMap::ContinuousColorComponent component;
                component.color_hex = physical_colors[component_id - 1];
                if (component_id <= context.physical_tds.size() && std::isfinite(context.physical_tds[component_id - 1]) &&
                    context.physical_tds[component_id - 1] > 0.0)
                    component.transmission_distance_mm = context.physical_tds[component_id - 1];
                if (component_id <= context.physical_material_ids.size() && !context.physical_material_ids[component_id - 1].empty())
                    component.material_id = context.physical_material_ids[component_id - 1];
                components.emplace_back(std::move(component));
            }
            ImageMap::ContinuousColorSolver solver(std::move(components), color_mix_model);
            const RGBA target{float(target_color.Red()) / 255.f, float(target_color.Green()) / 255.f,
                              float(target_color.Blue()) / 255.f, 1.f};
            if (const std::optional<RGBA> predicted = solver.predict_color(target))
                existing_color = wxColour(std::clamp(int(std::lround((*predicted)[0] * 255.f)), 0, 255),
                                          std::clamp(int(std::lround((*predicted)[1] * 255.f)), 0, 255),
                                          std::clamp(int(std::lround((*predicted)[2] * 255.f)), 0, 255));
        }
        const double   delta          = color_delta_e00(target_color, existing_color);
        if (meaningfully_better_than_direct(delta) && delta + 1e-6 < result.delta_e) {
            result.valid                = true;
            result.filament_id          = filament_id;
            result.delta_e              = delta;
            result.used_surface_bias    = true;
        }
        if (result.filament_id == filament_id && delta <= 0.5)
            return result;
    }

    MixedColorMatchRecipeResult recipe =
        build_image_map_color_match_recipe(physical_colors, target_color, min_component_percent, context, color_mix_model);
    float  reference_width_mm = 0.4f;
    double nozzle_sum         = 0.0;
    size_t nozzle_count       = 0;
    for (size_t index = 0; index < physical_colors.size() && index < context.nozzle_diameters.size(); ++index) {
        nozzle_sum += std::max(0.05, context.nozzle_diameters[index]);
        ++nozzle_count;
    }
    if (nozzle_count > 0)
        reference_width_mm = float(nozzle_sum / double(nozzle_count));
    if (recipe.valid && color_mix_model == ImageMap::ColorMixModel::FullSpectrumKmKs)
        refine_color_match_recipe_with_surface_bias(recipe, target_color, physical_colors.size(), context, reference_width_mm);
    if (!recipe.valid || !meaningfully_better_than_direct(recipe.delta_e) || recipe.delta_e >= result.delta_e - 1e-6)
        return result;

    const size_t visible_before = manager.visible_count();
    if (physical_colors.size() + visible_before >= max_total_filaments)
        return result;

    MixedFilamentDefinition definition = mixed_filament_definition_from_color_match_recipe(
        recipe, physical_colors.size(), MixedColorMatchEncoding::AdaptiveLocalizedCycles, reference_width_mm);
    if (!manager.add_custom_filament_definition(std::move(definition), physical_colors))
        return result;

    result.valid             = true;
    result.created           = true;
    result.used_surface_bias = true;
    result.filament_id       = unsigned(physical_colors.size() + visible_before + 1);
    result.delta_e           = recipe.delta_e;
    return result;
}

static std::string summarize_color_match_definition(const MixedFilamentDefinition& definition, size_t num_physical)
{
    const std::vector<unsigned int> ids     = definition.recipe.blend.component_ids(num_physical);
    const std::vector<int>          weights = definition.recipe.blend.component_percents(num_physical);
    if (ids.empty() || ids.size() != weights.size())
        return {};

    std::ostringstream out;
    for (size_t index = 0; index < ids.size(); ++index) {
        if (index > 0)
            out << '/';
        out << 'F' << ids[index];
    }
    out << ' ';
    for (size_t index = 0; index < weights.size(); ++index) {
        if (index > 0)
            out << '/';
        out << weights[index] << '%';
    }
    return out.str();
}

static MixedColorMatchCreationResult create_standard_color_match(
    const wxColour&                    target_color,
    const std::vector<std::string>&    physical_colors,
    int                                min_component_percent,
    size_t                             max_total_filaments,
    MixedColorMatchEncoding            encoding,
    const MixedFilamentDisplayContext& context,
    MixedFilamentManager&              manager)
{
    MixedColorMatchCreationResult result;
    if (!target_color.IsOk() || physical_colors.empty())
        return result;

    const auto closest_physical = closest_physical_color_match(target_color, physical_colors);
    result.valid       = true;
    result.filament_id = closest_physical.first;
    result.delta_e     = closest_physical.second;
    if (physical_colors.size() < 2 || closest_physical.second <= 0.5)
        return result;

    const std::vector<MixedFilamentDefinition> definitions = manager.mixed_filament_definitions(physical_colors.size());
    auto consider_existing_definitions = [&](bool biased_definitions) {
        size_t visible_idx = 0;
        for (const MixedFilamentDefinition& definition : definitions) {
            if (definition.visibility.tombstoned)
                continue;
            const unsigned int filament_id = unsigned(physical_colors.size() + visible_idx + 1);
            ++visible_idx;
            if (definition_has_surface_bias(definition) != biased_definitions)
                continue;

            const wxColour existing_color = parse_mixed_color(compute_mixed_filament_display_color(definition, context));
            const double   delta          = color_delta_e00(target_color, existing_color);
            if (delta + 1e-6 < result.delta_e) {
                result.filament_id       = filament_id;
                result.delta_e           = delta;
                result.used_surface_bias = biased_definitions;
            }
            if (delta <= 0.5)
                return true;
        }
        return false;
    };

    // Reuse ordinary mixed filaments before considering any geometry-modulated
    // definition. This keeps solid texture regions on the normal mix pipeline.
    if (consider_existing_definitions(false))
        return result;
    if (encoding == MixedColorMatchEncoding::SurfaceBias && consider_existing_definitions(true))
        return result;

    MixedColorMatchRecipeResult recipe = build_best_color_match_recipe(
        physical_colors,
        target_color,
        min_component_percent,
        context.physical_tds,
        context.physical_material_ids,
        MixedFilamentManager::color_engine(),
        MixedFilamentManager::use_td_for_color_prediction());
    if (!recipe.valid)
        return result;

    const DecodedColorMatchRecipe decoded_recipe     = decode_color_match_recipe(recipe, physical_colors.size());
    float                         reference_width_mm = 0.4f;
    double                        nozzle_sum         = 0.0;
    size_t                        nozzle_count       = 0;
    for (const unsigned int component_id : decoded_recipe.component_ids) {
        if (component_id == 0 || component_id > context.nozzle_diameters.size())
            continue;
        nozzle_sum += std::max(0.05, context.nozzle_diameters[component_id - 1]);
        ++nozzle_count;
    }
    if (nozzle_count > 0)
        reference_width_mm = float(nozzle_sum / double(nozzle_count));

    const bool used_surface_bias = encoding == MixedColorMatchEncoding::SurfaceBias &&
                                   refine_color_match_recipe_with_surface_bias(recipe, target_color, physical_colors.size(), context,
                                                                               reference_width_mm);
    if (recipe.delta_e >= result.delta_e - 1e-6)
        return result;

    const size_t visible_before = manager.visible_count();
    if (physical_colors.size() + visible_before >= max_total_filaments)
        return result;

    MixedFilamentDefinition definition = mixed_filament_definition_from_color_match_recipe(
        recipe, physical_colors.size(), encoding, reference_width_mm);
    if (!manager.add_custom_filament_definition(std::move(definition), physical_colors))
        return result;

    result.created           = true;
    result.used_surface_bias = used_surface_bias;
    result.filament_id       = unsigned(physical_colors.size() + visible_before + 1);
    result.delta_e           = recipe.delta_e;
    return result;
}

MixedColorMatchCreationResult create_mixed_filament_color_match(const wxColour&                  target_color,
                                                                const std::vector<std::string>&  physical_colors,
                                                                int                              min_component_percent,
                                                                size_t                           max_total_filaments,
                                                                MixedColorMatchEncoding          encoding,
                                                                const std::vector<unsigned int>& perimeter_component_ids,
                                                                ImageMap::ColorMixModel          color_mix_model)
{
    MixedColorMatchCreationResult result;
    if (!target_color.IsOk() || physical_colors.empty())
        return result;

    const auto closest_physical             = closest_physical_color_match(target_color, physical_colors);
    result.valid                            = true;
    result.filament_id                      = closest_physical.first;
    result.delta_e                          = closest_physical.second;
    const bool perimeter_modulated_sequence = encoding == MixedColorMatchEncoding::PerimeterModulatedLayerSequence;
    const bool adaptive_perimeter_cycles    = encoding == MixedColorMatchEncoding::AdaptiveLocalizedCycles;
    if (physical_colors.size() < 2 || (!perimeter_modulated_sequence && closest_physical.second <= 0.5))
        return result;

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        return result;

    MixedFilamentDisplayContext context = build_mixed_filament_display_context(physical_colors);
    if (encoding == MixedColorMatchEncoding::SurfaceBias || perimeter_modulated_sequence || adaptive_perimeter_cycles) {
        context.preview_settings.local_z_mode = false;
        context.component_bias_enabled        = true;
    }
    auto& manager = preset_bundle->mixed_filaments;
    manager.set_display_context(context);

    const std::vector<MixedFilamentDefinition> definitions = manager.mixed_filament_definitions(physical_colors.size());

    if (perimeter_modulated_sequence) {
        result                                                 = MixedColorMatchCreationResult{};
        const std::vector<unsigned int> selected_component_ids = normalized_perimeter_component_ids(perimeter_component_ids,
                                                                                                    physical_colors.size());
        if (selected_component_ids.size() < 2)
            return result;
        size_t visible_idx = 0;
        for (const MixedFilamentDefinition& definition : definitions) {
            if (definition.visibility.tombstoned)
                continue;
            const unsigned int filament_id = unsigned(physical_colors.size() + visible_idx + 1);
            ++visible_idx;
            if (!definition_has_shared_image_map_cadence(definition, physical_colors.size(), selected_component_ids))
                continue;

            const wxColour existing_color = parse_mixed_color(compute_mixed_filament_display_color(definition, context));
            const double   delta          = color_delta_e00(target_color, existing_color);
            if (!result.valid || delta + 1e-6 < result.delta_e) {
                result.valid             = true;
                result.filament_id       = filament_id;
                result.delta_e           = delta;
                result.used_surface_bias = definition_has_surface_bias(definition);
            }
            if (delta <= 0.5) {
                configure_surface_bias_color_match_mode(*preset_bundle);
                return result;
            }
        }

        std::vector<std::string> selected_physical_colors;
        selected_physical_colors.reserve(selected_component_ids.size());
        MixedFilamentDisplayContext selected_context;
        selected_context.num_physical = selected_component_ids.size();
        for (const unsigned int component_id : selected_component_ids) {
            selected_physical_colors.emplace_back(physical_colors[component_id - 1]);
            selected_context.physical_colors.emplace_back(context.physical_colors[component_id - 1]);
            selected_context.nozzle_diameters.emplace_back(
                component_id <= context.nozzle_diameters.size() ? context.nozzle_diameters[component_id - 1] : 0.4);
            selected_context.physical_tds.emplace_back(
                component_id <= context.physical_tds.size() ? context.physical_tds[component_id - 1] : 0.0);
            selected_context.physical_material_ids.emplace_back(
                component_id <= context.physical_material_ids.size() ? context.physical_material_ids[component_id - 1] : std::string());
        }
        const auto         closest_selected           = closest_physical_color_match(target_color, selected_physical_colors);
        const unsigned int closest_selected_component = selected_component_ids[closest_selected.first - 1];

        MixedColorMatchRecipeResult recipe = build_image_map_color_match_recipe(
            selected_physical_colors, target_color, min_component_percent, selected_context, color_mix_model);
        if (!recipe.valid || closest_selected.second + 1e-6 < recipe.delta_e) {
            recipe               = MixedColorMatchRecipeResult{};
            recipe.valid         = true;
            recipe.component_a   = closest_selected.first;
            recipe.component_b   = closest_selected.first == 1 ? 2 : 1;
            recipe.mix_b_percent = 0;
            recipe.preview_color = parse_mixed_color(selected_physical_colors[closest_selected.first - 1]);
            recipe.delta_e       = closest_selected.second;
        }

        const std::vector<unsigned int> local_recipe_ids =
            !recipe.gradient_component_ids.empty() ?
                MixedFilamentManager::decode_gradient_component_ids(recipe.gradient_component_ids, selected_component_ids.size()) :
                std::vector<unsigned int>{recipe.component_a, recipe.component_b};
        std::vector<unsigned int> global_recipe_ids;
        global_recipe_ids.reserve(local_recipe_ids.size());
        for (const unsigned int local_id : local_recipe_ids)
            if (local_id >= 1 && local_id <= selected_component_ids.size())
                global_recipe_ids.emplace_back(selected_component_ids[local_id - 1]);
        if (global_recipe_ids.size() >= 2) {
            recipe.component_a = global_recipe_ids[0];
            recipe.component_b = global_recipe_ids[1];
            if (!recipe.gradient_component_ids.empty())
                recipe.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(global_recipe_ids);
        } else {
            recipe.component_a = closest_selected_component;
            recipe.component_b = selected_component_ids.front() == closest_selected_component ? selected_component_ids[1] :
                                                                                                selected_component_ids.front();
        }

        float  reference_width_mm = 0.4f;
        double nozzle_sum         = 0.0;
        size_t nozzle_count       = 0;
        for (const unsigned int component_id : selected_component_ids) {
            if (component_id > context.nozzle_diameters.size())
                continue;
            nozzle_sum += std::max(0.05, context.nozzle_diameters[component_id - 1]);
            ++nozzle_count;
        }
        if (nozzle_count > 0)
            reference_width_mm = float(nozzle_sum / double(nozzle_count));

        const size_t visible_before = manager.visible_count();
        if (physical_colors.size() + visible_before >= max_total_filaments) {
            if (result.valid)
                configure_surface_bias_color_match_mode(*preset_bundle);
            return result;
        }

        MixedFilamentDefinition definition = mixed_filament_definition_from_color_match_recipe(recipe, physical_colors.size(), encoding,
                                                                                               reference_width_mm, selected_component_ids);
        const bool              uses_bias  = definition_has_surface_bias(definition);
        if (!manager.add_custom_filament_definition(std::move(definition), physical_colors)) {
            if (result.valid)
                configure_surface_bias_color_match_mode(*preset_bundle);
            return result;
        }

        configure_surface_bias_color_match_mode(*preset_bundle);
        result.valid             = true;
        result.created           = true;
        result.used_surface_bias = uses_bias;
        result.filament_id       = unsigned(physical_colors.size() + visible_before + 1);
        result.delta_e           = recipe.delta_e;
        preset_bundle->sync_mixed_filament_definitions_to_project_config();
        return result;
    }

    if (adaptive_perimeter_cycles) {
        result = create_adaptive_localized_color_match(target_color, physical_colors, min_component_percent, max_total_filaments, context,
                                                       manager, result, color_mix_model);
        configure_adaptive_perimeter_color_match_mode();
        if (result.created)
            preset_bundle->sync_mixed_filament_definitions_to_project_config();
        return result;
    }

    result = create_standard_color_match(target_color,
                                         physical_colors,
                                         min_component_percent,
                                         max_total_filaments,
                                         encoding,
                                         context,
                                         manager);
    if (result.used_surface_bias)
        configure_surface_bias_color_match_mode(*preset_bundle);
    if (result.created)
        preset_bundle->sync_mixed_filament_definitions_to_project_config();
    return result;
}

NormalColorMatchPlan preview_normal_color_matches(const std::vector<wxColour>&    target_colors,
                                                   const std::vector<std::string>& physical_colors,
                                                   int                             min_component_percent,
                                                   size_t                          max_total_filaments)
{
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (target_colors.empty() || physical_colors.empty() || preset_bundle == nullptr)
        return {};

    MixedFilamentDisplayContext context = build_mixed_filament_display_context(physical_colors);
    context.preview_settings.local_z_mode = false;
    context.component_bias_enabled        = true;
    return build_normal_color_match_plan(target_colors,
                                         physical_colors,
                                         min_component_percent,
                                         max_total_filaments,
                                         context,
                                         preset_bundle->mixed_filaments);
}

NormalColorMatchPlan build_normal_color_match_plan(const std::vector<wxColour>&       target_colors,
                                                   const std::vector<std::string>&    physical_colors,
                                                   int                                min_component_percent,
                                                   size_t                             max_total_filaments,
                                                   const MixedFilamentDisplayContext& context,
                                                   const MixedFilamentManager&        base_manager)
{
    NormalColorMatchPlan plan;
    if (target_colors.empty() || physical_colors.empty())
        return plan;

    plan.physical_colors = physical_colors;
    const std::vector<MixedFilamentDefinition> base_definitions = base_manager.mixed_filament_definitions(physical_colors.size());
    for (const MixedFilamentDefinition& definition : base_definitions)
        if (!definition.visibility.tombstoned)
            plan.base_visible_stable_ids.emplace_back(definition.identity.stable_id);

    MixedFilamentManager simulated_manager = base_manager;
    simulated_manager.set_display_context(context);
    const unsigned int first_planned_id = unsigned(physical_colors.size() + plan.base_visible_stable_ids.size() + 1);

    plan.entries.reserve(target_colors.size());
    for (const wxColour& target_color : target_colors) {
        const MixedColorMatchCreationResult result = create_standard_color_match(
            target_color,
            physical_colors,
            min_component_percent,
            max_total_filaments,
            MixedColorMatchEncoding::SurfaceBias,
            context,
            simulated_manager);
        if (!result.valid || result.filament_id == 0 || result.filament_id > max_total_filaments)
            return NormalColorMatchPlan{};

        NormalColorMatchPlanEntry entry;
        entry.filament_id       = result.filament_id;
        entry.target_color      = target_color;
        entry.delta_e           = result.delta_e;
        entry.newly_created     = result.filament_id >= first_planned_id;
        entry.used_surface_bias = result.used_surface_bias;

        if (result.filament_id <= physical_colors.size()) {
            entry.predicted_color = parse_mixed_color(physical_colors[result.filament_id - 1]);
            entry.recipe_summary  = "F" + std::to_string(result.filament_id) + " 100%";
        } else {
            const std::optional<MixedFilamentDefinition> definition =
                simulated_manager.mixed_filament_definition_from_id(result.filament_id, physical_colors.size());
            if (!definition)
                return NormalColorMatchPlan{};
            entry.predicted_color = parse_mixed_color(compute_mixed_filament_display_color(*definition, context));
            entry.recipe_summary  = summarize_color_match_definition(*definition, physical_colors.size());
            entry.used_surface_bias = definition_has_surface_bias(*definition);

            if (result.created) {
                plan.new_filaments.push_back({result.filament_id, *definition, entry.recipe_summary});
            }
        }
        entry.delta_e = color_delta_e00(entry.target_color, entry.predicted_color);
        plan.entries.emplace_back(std::move(entry));
    }

    plan.valid = plan.entries.size() == target_colors.size();
    return plan;
}

NormalColorMatchPlanCommitResult apply_normal_color_match_plan(const NormalColorMatchPlan&        plan,
                                                               MixedFilamentManager&              manager,
                                                               const MixedFilamentDisplayContext& context)
{
    NormalColorMatchPlanCommitResult result;
    if (!plan.valid) {
        result.error = "The color-match plan is no longer valid.";
        return result;
    }

    std::vector<uint64_t> current_visible_stable_ids;
    for (const MixedFilamentDefinition& definition : manager.mixed_filament_definitions(plan.physical_colors.size()))
        if (!definition.visibility.tombstoned)
            current_visible_stable_ids.emplace_back(definition.identity.stable_id);
    if (current_visible_stable_ids != plan.base_visible_stable_ids) {
        result.error = "The mixed-filament list changed while the image map was open. Generate the mapping again.";
        return result;
    }

    MixedFilamentManager candidate = manager;
    candidate.set_display_context(context);
    for (const NormalColorMatchPlannedFilament& planned : plan.new_filaments) {
        const unsigned int next_id = unsigned(plan.physical_colors.size() + candidate.visible_count() + 1);
        if (planned.filament_id != next_id) {
            result.error = "The planned mixed-filament IDs are no longer contiguous. Generate the mapping again.";
            return result;
        }
        MixedFilamentDefinition definition = planned.definition;
        definition.identity.stable_id      = 0;
        if (!candidate.add_custom_filament_definition(std::move(definition), plan.physical_colors)) {
            result.error = "Unable to commit a planned mixed filament.";
            return result;
        }
    }

    manager        = std::move(candidate);
    result.created = !plan.new_filaments.empty();
    result.valid   = true;
    return result;
}

NormalColorMatchPlanCommitResult commit_normal_color_match_plan(const NormalColorMatchPlan& plan)
{
    NormalColorMatchPlanCommitResult result;
    PresetBundle*                    preset_bundle = wxGetApp().preset_bundle;
    if (!plan.valid || preset_bundle == nullptr) {
        result.error = "The color-match plan is no longer valid.";
        return result;
    }

    MixedFilamentDisplayContext context = build_mixed_filament_display_context(plan.physical_colors);
    context.preview_settings.local_z_mode = false;
    context.component_bias_enabled        = true;
    result = apply_normal_color_match_plan(plan, preset_bundle->mixed_filaments, context);
    if (!result.valid)
        return result;

    const bool uses_surface_bias = std::any_of(plan.entries.begin(), plan.entries.end(), [](const NormalColorMatchPlanEntry& entry) {
        return entry.used_surface_bias;
    });
    if (uses_surface_bias)
        configure_surface_bias_color_match_mode(*preset_bundle);
    if (result.created)
        preset_bundle->sync_mixed_filament_definitions_to_project_config();
    return result;
}

AdaptiveColorMatchPreviewResult preview_adaptive_localized_color_matches(
    const std::vector<wxColour>& target_colors,
    const std::vector<std::string>& physical_colors,
    int min_component_percent,
    size_t max_total_filaments,
    ImageMap::ColorMixModel color_mix_model)
{
    AdaptiveColorMatchPreviewResult preview;
    if (target_colors.empty() || physical_colors.size() < 2 || wxGetApp().preset_bundle == nullptr)
        return preview;

    MixedFilamentDisplayContext context = build_mixed_filament_display_context(physical_colors);
    context.preview_settings.local_z_mode = false;
    context.component_bias_enabled        = true;

    MixedFilamentManager simulated_manager = wxGetApp().preset_bundle->mixed_filaments;
    simulated_manager.set_display_context(context);

    preview.target_filament_ids.reserve(target_colors.size());
    for (const wxColour& target_color : target_colors) {
        const auto closest_physical = closest_physical_color_match(target_color, physical_colors);
        MixedColorMatchCreationResult result;
        result.valid       = true;
        result.filament_id = closest_physical.first;
        result.delta_e     = closest_physical.second;
        result = create_adaptive_localized_color_match(target_color,
                                                       physical_colors,
                                                       min_component_percent,
                                                       max_total_filaments,
                                                       context,
                                                       simulated_manager,
                                                       result,
                                                       color_mix_model);
        if (!result.valid)
            return AdaptiveColorMatchPreviewResult{};
        preview.target_filament_ids.emplace_back(result.filament_id);
        if (result.filament_id <= physical_colors.size())
            ++preview.direct_physical_target_count;
    }

    for (size_t target_index = 0; target_index < preview.target_filament_ids.size(); ++target_index) {
        const unsigned int filament_id = preview.target_filament_ids[target_index];
        if (filament_id <= physical_colors.size())
            continue;

        auto cycle = std::find_if(preview.mixed_cycles.begin(),
                                  preview.mixed_cycles.end(),
                                  [filament_id](const AdaptiveColorMatchPreviewCycle& item) {
                                      return item.filament_id == filament_id;
                                  });
        if (cycle == preview.mixed_cycles.end()) {
            const std::optional<MixedFilamentDefinition> definition =
                simulated_manager.mixed_filament_definition_from_id(filament_id, physical_colors.size());
            if (!definition)
                return AdaptiveColorMatchPreviewResult{};
            preview.mixed_cycles.push_back({filament_id, *definition, {}});
            cycle = std::prev(preview.mixed_cycles.end());
        }
        cycle->target_indices.emplace_back(target_index);
    }

    preview.valid = preview.target_filament_ids.size() == target_colors.size();
    return preview;
}

static std::vector<double> selected_filament_transmission_distances(PresetBundle *preset_bundle, size_t count)
{
    std::vector<double> tds(count, 0.0);
    if (preset_bundle == nullptr)
        return tds;

    const Preset *edited_filament_preset = &preset_bundle->filaments.get_edited_preset();
    const std::string edited_filament_name = edited_filament_preset != nullptr ?
        Preset::remove_suffix_modified(edited_filament_preset->name) :
        std::string();

    for (size_t i = 0; i < count && i < preset_bundle->filament_presets.size(); ++i) {
        const std::string selected_filament_name = Preset::remove_suffix_modified(preset_bundle->filament_presets[i]);
        const Preset *filament_preset =
            (!edited_filament_name.empty() && selected_filament_name == edited_filament_name) ?
            edited_filament_preset :
            preset_bundle->filaments.find_preset(selected_filament_name, true);

        const ConfigOptionFloats *td_opt = filament_preset != nullptr ?
            filament_preset->config.option<ConfigOptionFloats>("filament_transmission_distance") :
            nullptr;
        if (td_opt != nullptr && !td_opt->values.empty()) {
            const double td = td_opt->get_at(0);
            if (std::isfinite(td) && td > 0.0)
                tds[i] = td;
        }
    }

    const ConfigOptionFloats *opt = preset_bundle->project_config.option<ConfigOptionFloats>("filament_transmission_distance");
    if (opt == nullptr || opt->values.empty())
        return tds;

    const size_t opt_count = opt->values.size();
    for (size_t i = 0; i < count && i < opt_count; ++i) {
        if (tds[i] > 0.0)
            continue;
        const double td = opt->get_at(unsigned(i));
        tds[i] = (std::isfinite(td) && td > 0.0) ? td : 0.0;
    }

    return tds;
}

static std::vector<std::string> selected_filament_full_spectrum_material_ids(PresetBundle* preset_bundle, size_t count)
{
    std::vector<std::string> material_ids(count);
    if (preset_bundle == nullptr)
        return material_ids;

    const Preset* edited_filament_preset = &preset_bundle->filaments.get_edited_preset();
    const std::string edited_filament_name = edited_filament_preset != nullptr ?
        Preset::remove_suffix_modified(edited_filament_preset->name) :
        std::string();

    for (size_t index = 0; index < count && index < preset_bundle->filament_presets.size(); ++index) {
        const std::string selected_filament_name = Preset::remove_suffix_modified(preset_bundle->filament_presets[index]);
        const Preset* filament_preset =
            (!edited_filament_name.empty() && selected_filament_name == edited_filament_name) ?
            edited_filament_preset :
            preset_bundle->filaments.find_preset(selected_filament_name, true);

        const ConfigOptionStrings* material_id_opt = filament_preset != nullptr ?
            filament_preset->config.option<ConfigOptionStrings>("filament_full_spectrum_material_id") :
            nullptr;
        if (material_id_opt != nullptr && !material_id_opt->values.empty()) {
            const std::string& material_id = material_id_opt->get_at(0);
            if (!material_id.empty())
                material_ids[index] = material_id;
        }
    }

    const ConfigOptionStrings* project_opt =
        preset_bundle->project_config.option<ConfigOptionStrings>("filament_full_spectrum_material_id");
    if (project_opt == nullptr || project_opt->values.empty())
        return material_ids;

    const size_t project_count = project_opt->values.size();
    for (size_t index = 0; index < count && index < project_count; ++index) {
        if (!material_ids[index].empty())
            continue;
        material_ids[index] = project_opt->get_at(unsigned(index));
    }
    return material_ids;
}

static std::vector<std::string> selected_filament_material_types(PresetBundle* preset_bundle, size_t count)
{
    std::vector<std::string> material_types(count);
    if (preset_bundle == nullptr)
        return material_types;

    const Preset* edited_filament_preset = &preset_bundle->filaments.get_edited_preset();
    const std::string edited_filament_name = edited_filament_preset != nullptr ?
        Preset::remove_suffix_modified(edited_filament_preset->name) :
        std::string();

    for (size_t index = 0; index < count && index < preset_bundle->filament_presets.size(); ++index) {
        const std::string selected_filament_name = Preset::remove_suffix_modified(preset_bundle->filament_presets[index]);
        const Preset* filament_preset =
            (!edited_filament_name.empty() && selected_filament_name == edited_filament_name) ?
            edited_filament_preset :
            preset_bundle->filaments.find_preset(selected_filament_name, true);

        if (filament_preset != nullptr) {
            std::string dummy;
            std::string type = const_cast<Preset*>(filament_preset)->get_filament_type(dummy);
            if (!type.empty())
                material_types[index] = type;
            else if (filament_preset->config.has("filament_type"))
                material_types[index] = filament_preset->config.opt_string("filament_type", 0);
        }
    }

    const ConfigOptionStrings* project_opt =
        preset_bundle->project_config.option<ConfigOptionStrings>("filament_type");
    if (project_opt != nullptr && !project_opt->values.empty()) {
        const size_t project_count = project_opt->values.size();
        for (size_t index = 0; index < count && index < project_count; ++index) {
            if (material_types[index].empty() && !project_opt->values[index].empty())
                material_types[index] = project_opt->values[index];
        }
    }

    return material_types;
}

// Shared constants + helpers for Full Spectrum preset-name resolution. Kept in
// an anonymous namespace so the table is not visible outside this TU (these are
// pure implementation details of the two functions below).
namespace {
constexpr const char*  kFullSpectrumBase  = "Snapmaker PLA Full Spectrum @U1 ";
constexpr double       kFallbackNozzle    = 0.4;   // the only SKU shipped historically
constexpr double       kMinNozzle         = 0.05;  // matches build_mixed_filament_display_context

// Format a nozzle diameter the same way the preset JSON names do ("0.4", "0.2"):
// two decimals, trailing zeros and a dangling dot stripped.
std::string format_nozzle_label(double mm)
{
    std::string s = float_to_string_decimal_point(mm, 2);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

// Read the current printer's nozzle_diameter from preset_bundle with the same
// null-check + kMinNozzle clamp as build_mixed_filament_display_context. Falls
// back to kFallbackNozzle when preset_bundle / the option is unavailable.
double current_nozzle_diameter()
{
    double nozzle = kFallbackNozzle;
    auto*  pb     = wxGetApp().preset_bundle;
    if (pb != nullptr) {
        if (const auto* opt = pb->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
            opt != nullptr && !opt->values.empty()) {
            // (C) clamp to sane physical range -- matches build_mixed_filament_display_context
            nozzle = std::max(kMinNozzle, opt->values.front());
        }
    } else {
        BOOST_LOG_TRIVIAL(warning)
            << "full_spectrum nozzle resolution: preset_bundle null; falling back to "
            << kFullSpectrumBase << format_nozzle_label(kFallbackNozzle) << " nozzle";
    }
    return nozzle;
}
} // namespace

// Full Spectrum preset name, with nozzle diameter resolved from the current
// printer's nozzle_diameter (mirrors PresetComboBoxes.cpp's @U1 <nozzle> nozzle
// convention). Falls back to the canonical 0.4 SKU when no matching preset
// exists for the current nozzle. Follows the same preset_bundle-access pattern
// as build_mixed_filament_display_context below (null-check + warning log +
// std::max clamp) -- see the header doc for the UI-thread convention.
//
// NOTE: because this function falls back to the 0.4 name on a miss, its return
// value alone cannot tell you whether the *current* nozzle actually has a preset
// -- use full_spectrum_preset_exists_for_current_nozzle() for that.
std::string full_spectrum_preset_name()
{
    const double  nozzle    = current_nozzle_diameter();
    auto*         pb        = wxGetApp().preset_bundle;
    const std::string candidate = std::string(kFullSpectrumBase) + format_nozzle_label(nozzle) + " nozzle";

    // Candidate for the current nozzle; verify the preset actually exists.
    if (pb != nullptr && pb->filaments.find_preset(candidate) != nullptr)
        return candidate;

    // Fallback: the canonical 0.4 SKU (shipped today). If even that is missing
    // (profile not loaded), return the literal name so upstream fallback chains
    // (load_full_spectrum_colors -> canonical CMYW palette) take over.
    return std::string(kFullSpectrumBase) + format_nozzle_label(kFallbackNozzle) + " nozzle";
}

// True iff a Full Spectrum preset exists for the printer's *current* nozzle
// diameter (no fallback). Used by the batch-match guard to decide whether
// Recommended mode is available, instead of hard-coding 0.4 mm: shipping a new
// nozzle variant (e.g. 0.2/0.6/0.8) just requires adding its preset JSON under
// resources/profiles/Snapmaker/filament/, and this returns true automatically.
bool full_spectrum_preset_exists_for_current_nozzle()
{
    const double     nozzle    = current_nozzle_diameter();
    auto*            pb        = wxGetApp().preset_bundle;
    const std::string candidate = std::string(kFullSpectrumBase) + format_nozzle_label(nozzle) + " nozzle";
    return pb != nullptr && pb->filaments.find_preset(candidate) != nullptr;
}

MixedFilamentDisplayContext build_mixed_filament_display_context(const std::vector<std::string>& physical_colors)
{
    MixedFilamentDisplayContext context;
    context.num_physical    = physical_colors.size();
    context.physical_colors = physical_colors;
    context.nozzle_diameters.assign(context.num_physical, 0.4);
    context.physical_tds.assign(context.num_physical, 0.0);
    context.physical_material_ids.assign(context.num_physical, std::string());
    context.physical_material_types.assign(context.num_physical, std::string());

    auto* preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        return context;

    DynamicPrintConfig* print_cfg = &preset_bundle->prints.get_edited_preset().config;
    if (const ConfigOptionFloats* opt = preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")) {
        const size_t opt_count = opt->values.size();
        if (opt_count > 0) {
            for (size_t i = 0; i < context.num_physical; ++i)
                context.nozzle_diameters[i] = std::max(0.05, opt->get_at(unsigned(std::min(i, opt_count - 1))));
        }
    }
    context.physical_tds = selected_filament_transmission_distances(preset_bundle, context.num_physical);
    context.physical_material_ids = selected_filament_full_spectrum_material_ids(preset_bundle, context.num_physical);
    context.physical_material_types = selected_filament_material_types(preset_bundle, context.num_physical);

    auto get_mixed_bool = [preset_bundle, print_cfg](const std::string& key, bool fallback) {
        if (const ConfigOptionBool* opt = preset_bundle->project_config.option<ConfigOptionBool>(key))
            return opt->value;
        if (const ConfigOptionInt* opt = preset_bundle->project_config.option<ConfigOptionInt>(key))
            return opt->value != 0;
        if (print_cfg != nullptr) {
            if (const ConfigOptionBool* opt = print_cfg->option<ConfigOptionBool>(key))
                return opt->value;
            if (const ConfigOptionInt* opt = print_cfg->option<ConfigOptionInt>(key))
                return opt->value != 0;
        }
        return fallback;
    };
    auto get_mixed_float = [preset_bundle, print_cfg](const std::string& key, float fallback) {
        if (preset_bundle->project_config.has(key))
            return float(preset_bundle->project_config.opt_float(key));
        if (print_cfg != nullptr && print_cfg->has(key))
            return float(print_cfg->opt_float(key));
        return fallback;
    };

    context.preview_settings.min_sublayer_height =
        std::max(0.01, double(get_mixed_float("mixed_filament_height_lower_bound", 0.06f)));
    context.preview_settings.preferred_a_height   = std::max(0.0, double(get_mixed_float("mixed_color_layer_height_a", 0.f)));
    context.preview_settings.preferred_b_height   = std::max(0.0, double(get_mixed_float("mixed_color_layer_height_b", 0.f)));
    context.preview_settings.nominal_layer_height = 0.2;
    if (print_cfg != nullptr && print_cfg->has("layer_height"))
        context.preview_settings.nominal_layer_height = std::max(0.01, print_cfg->opt_float("layer_height"));
    context.preview_settings.gradient_nominal_layer_height = std::max(
        2.0 * context.preview_settings.min_sublayer_height,
        double(get_mixed_float("dithering_local_z_gradient_layer_height", 0.20f)));
    if (print_cfg != nullptr && print_cfg->has("wall_loops"))
        context.preview_settings.wall_loops = std::max<size_t>(1, size_t(std::max(1, print_cfg->opt_int("wall_loops"))));
    context.preview_settings.local_z_mode              = get_mixed_bool("dithering_local_z_mode", false);
    context.preview_settings.local_z_direct_multicolor = get_mixed_bool("dithering_local_z_direct_multicolor", false) &&
                                                         context.preview_settings.preferred_a_height <= EPSILON &&
                                                         context.preview_settings.preferred_b_height <= EPSILON;
    context.component_bias_enabled = get_mixed_bool("mixed_filament_component_bias_enabled", false);

    return context;
}

namespace {

std::optional<double> gradient_preview_td(const MixedFilamentDisplayContext& context, unsigned int component_id)
{
    if (component_id == 0 || component_id > context.physical_tds.size())
        return std::nullopt;

    const double value = context.physical_tds[component_id - 1];
    return std::isfinite(value) && value > 0.0 ? std::optional<double>(value) : std::nullopt;
}

std::optional<std::string> gradient_preview_material_id(const MixedFilamentDisplayContext& context, unsigned int component_id)
{
    if (component_id == 0 || component_id > context.physical_material_ids.size())
        return std::nullopt;
    const std::string& material_id = context.physical_material_ids[component_id - 1];
    return material_id.empty() ? std::nullopt : std::optional<std::string>(material_id);
}

std::vector<double> normalized_gradient_stop_positions(const std::vector<double>& positions, size_t component_count)
{
    const size_t expected_stops = component_count >= 2 ? 2 * component_count - 1 : size_t(0);
    if (expected_stops < 3)
        return {};

    std::vector<double> normalized;
    if (positions.size() == expected_stops) {
        normalized.reserve(expected_stops);
        for (const double position : positions) {
            if (!std::isfinite(position)) {
                normalized.clear();
                break;
            }
            normalized.emplace_back(std::clamp(position, 0.0, 1.0));
        }
    }

    if (normalized.empty()) {
        normalized.resize(expected_stops);
        for (size_t index = 0; index < expected_stops; ++index)
            normalized[index] = double(index) / double(expected_stops - 1);
    }

    normalized.front() = 0.0;
    normalized.back()  = 1.0;
    for (size_t index = 1; index < normalized.size(); ++index)
        normalized[index] = std::max(normalized[index], normalized[index - 1]);
    for (size_t index = normalized.size() - 1; index > 0; --index)
        normalized[index - 1] = std::min(normalized[index - 1], normalized[index]);
    return normalized;
}

std::vector<double> gradient_positions_from_definition(const MixedFilamentDefinition& definition,
                                                       size_t                         component_count,
                                                       size_t                         num_physical,
                                                       bool                           reverse_pair)
{
    const size_t expected_stops = component_count >= 2 ? 2 * component_count - 1 : size_t(0);
    if (expected_stops < 3)
        return {};

    if (definition.behavior.gradient.stop_positions.size() == expected_stops) {
        std::vector<double> explicit_positions;
        explicit_positions.reserve(expected_stops);
        for (const float position : definition.behavior.gradient.stop_positions)
            explicit_positions.emplace_back(double(position));
        return normalized_gradient_stop_positions(explicit_positions, component_count);
    }

    std::vector<int> weights = definition.recipe.blend.component_percents(num_physical);
    if (reverse_pair && weights.size() == 2)
        std::reverse(weights.begin(), weights.end());
    if (weights.size() != component_count)
        weights.assign(component_count, 1);

    double total = 0.0;
    for (const int weight : weights)
        total += double(std::max(0, weight));
    if (total <= EPSILON) {
        weights.assign(component_count, 1);
        total = double(component_count);
    }

    std::vector<double> boundaries(component_count + 1, 0.0);
    for (size_t index = 0; index < component_count; ++index)
        boundaries[index + 1] = boundaries[index] + double(std::max(0, weights[index])) / total;
    boundaries.back() = 1.0;

    std::vector<double> positions(expected_stops, 0.0);
    positions.back() = 1.0;
    for (size_t index = 0; index + 1 < component_count; ++index)
        positions[2 * index + 1] = boundaries[index + 1];
    for (size_t index = 1; index + 1 < component_count; ++index)
        positions[2 * index] = 0.5 * (boundaries[index] + boundaries[index + 1]);
    return normalized_gradient_stop_positions(positions, component_count);
}

} // namespace

wxColour blend_mixed_filament_components(const std::vector<unsigned int>&   component_ids,
                                         const std::vector<int>&            component_percents,
                                         const MixedFilamentDisplayContext& context)
{
    const size_t count = std::min(component_ids.size(), component_percents.size());
    if (count == 0)
        return wxColour("#26A69A");

    std::vector<int> normalized = normalize_color_match_weights(component_percents, count);
    std::vector<MixedFilamentColorInput> inputs;
    inputs.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const unsigned int component_id = component_ids[index];
        if (component_id == 0 || component_id > context.physical_colors.size() || normalized[index] <= 0)
            continue;

        MixedFilamentColorInput input;
        input.color_hex = context.physical_colors[component_id - 1];
        input.percent   = normalized[index];
        input.td_mm     = gradient_preview_td(context, component_id);
        input.material_id = gradient_preview_material_id(context, component_id);
        inputs.emplace_back(std::move(input));
    }

    if (inputs.empty())
        return wxColour("#26A69A");
    if (inputs.size() == 1)
        return parse_mixed_color(inputs.front().color_hex);
    return parse_mixed_color(MixedFilamentManager::blend_color_multi(inputs));
}

MixedFilamentGradientPreview build_mixed_filament_gradient_preview(
    const std::vector<unsigned int>&   ordered_component_ids,
    const std::vector<double>&         component_stop_positions,
    const MixedFilamentDisplayContext& context,
    size_t                             sample_count)
{
    MixedFilamentGradientPreview preview;
    if (ordered_component_ids.size() < 2 || context.physical_colors.empty())
        return preview;
    for (const unsigned int component_id : ordered_component_ids) {
        if (component_id == 0 || component_id > context.physical_colors.size())
            return preview;
    }

    preview.component_ids       = ordered_component_ids;
    preview.component_positions = normalized_gradient_stop_positions(component_stop_positions, ordered_component_ids.size());
    if (preview.component_positions.empty())
        return {};

    sample_count = std::max<size_t>(2, sample_count);
    preview.sampled_colors.reserve(sample_count);
    for (size_t sample_index = 0; sample_index < sample_count; ++sample_index) {
        const double t = double(sample_index) / double(sample_count - 1);
        size_t segment = ordered_component_ids.size() - 2;
        for (size_t index = 0; index + 1 < ordered_component_ids.size(); ++index) {
            if (t <= preview.component_positions[2 * index + 2] || index + 2 == ordered_component_ids.size()) {
                segment = index;
                break;
            }
        }

        const double start = preview.component_positions[2 * segment];
        const double mid   = preview.component_positions[2 * segment + 1];
        const double end   = preview.component_positions[2 * segment + 2];
        double       weight_b;
        if (t <= start)
            weight_b = 0.0;
        else if (t >= end)
            weight_b = 1.0;
        else if (t <= mid)
            weight_b = 0.5 * (t - start) / std::max(EPSILON, mid - start);
        else
            weight_b = 0.5 + 0.5 * (t - mid) / std::max(EPSILON, end - mid);

        const int percent_b = std::clamp(int(std::lround(100.0 * weight_b)), 0, 100);
        preview.sampled_colors.emplace_back(blend_mixed_filament_components(
            {ordered_component_ids[segment], ordered_component_ids[segment + 1]},
            {100 - percent_b, percent_b},
            context));
    }
    return preview;
}

MixedFilamentGradientPreview build_mixed_filament_gradient_preview(const MixedFilamentDefinition&     definition,
                                                                   const MixedFilamentDisplayContext& context,
                                                                   size_t                             sample_count)
{
    if (definition.visibility.tombstoned || !definition.behavior.gradient.enabled)
        return {};

    const size_t num_physical = context.num_physical == 0 ? context.physical_colors.size() : context.num_physical;
    std::vector<unsigned int> component_ids = definition.recipe.blend.component_ids(num_physical);
    if (component_ids.size() < 2)
        return {};

    const bool reverse_pair = component_ids.size() == 2 &&
                              definition.behavior.gradient.component_a_start < definition.behavior.gradient.component_a_end;
    if (reverse_pair)
        std::reverse(component_ids.begin(), component_ids.end());

    return build_mixed_filament_gradient_preview(
        component_ids,
        gradient_positions_from_definition(definition, component_ids.size(), num_physical, reverse_pair),
        context,
        sample_count);
}

std::vector<wxColour> build_adaptive_cycle_attainable_colors(const std::vector<unsigned int>&   component_ids,
                                                             const std::vector<RGBA>&           requested_colors,
                                                             const MixedFilamentDisplayContext& context,
                                                             size_t                             sample_count,
                                                             ImageMap::ColorMixModel             color_mix_model)
{
    std::vector<unsigned int> valid_component_ids;
    valid_component_ids.reserve(component_ids.size());
    for (const unsigned int component_id : component_ids) {
        if (component_id < 1 || component_id > context.physical_colors.size() ||
            std::find(valid_component_ids.begin(), valid_component_ids.end(), component_id) != valid_component_ids.end())
            continue;
        valid_component_ids.emplace_back(component_id);
    }
    if (valid_component_ids.empty())
        return {};

    std::vector<ImageMap::ContinuousColorComponent> solver_components;
    solver_components.reserve(valid_component_ids.size());
    std::vector<RGBA> physical_colors;
    physical_colors.reserve(valid_component_ids.size());
    for (const unsigned int component_id : valid_component_ids) {
        ImageMap::ContinuousColorComponent component;
        component.color_hex                = context.physical_colors[component_id - 1];
        component.transmission_distance_mm = gradient_preview_td(context, component_id);
        component.material_id              = gradient_preview_material_id(context, component_id);
        solver_components.emplace_back(std::move(component));

        const wxColour color = parse_mixed_color(context.physical_colors[component_id - 1]);
        physical_colors.push_back({float(color.Red()) / 255.f, float(color.Green()) / 255.f, float(color.Blue()) / 255.f, 1.f});
    }

    sample_count = std::max<size_t>(2, sample_count);
    ImageMap::ContinuousColorSolver solver(std::move(solver_components), color_mix_model);
    if (!solver.valid()) {
        std::vector<wxColour> fallback;
        fallback.reserve(physical_colors.size());
        for (const RGBA& color : physical_colors)
            fallback.emplace_back(std::clamp(int(std::lround(color[0] * 255.f)), 0, 255),
                                  std::clamp(int(std::lround(color[1] * 255.f)), 0, 255),
                                  std::clamp(int(std::lround(color[2] * 255.f)), 0, 255));
        return fallback;
    }

    std::vector<RGBA> projection_targets = requested_colors;
    projection_targets.insert(projection_targets.end(), physical_colors.begin(), physical_colors.end());

    // A one-dimensional card cannot display the complete multi-component
    // simplex. Sample each pair boundary, then add the source-color projections
    // below so the card includes both the attainable envelope and the colors
    // this cycle will actually be asked to reproduce.
    const size_t pair_samples = std::max<size_t>(3, sample_count);
    for (size_t first = 0; first < physical_colors.size(); ++first) {
        for (size_t second = first + 1; second < physical_colors.size(); ++second) {
            for (size_t sample_index = 0; sample_index < pair_samples; ++sample_index) {
                const float t = float(sample_index) / float(pair_samples - 1);
                RGBA        target;
                for (size_t channel = 0; channel < 3; ++channel)
                    target[channel] = physical_colors[first][channel] * (1.f - t) + physical_colors[second][channel] * t;
                target[3] = 1.f;
                projection_targets.emplace_back(target);
            }
        }
    }

    std::vector<RGBA> attainable;
    attainable.reserve(projection_targets.size());
    for (const RGBA& target : projection_targets) {
        if (const std::optional<RGBA> predicted = solver.predict_color(target))
            attainable.emplace_back(*predicted);
    }
    attainable = ImageMap::representative_source_colors(attainable, sample_count, attainable.size());

    std::vector<wxColour> colors;
    colors.reserve(attainable.size());
    for (const RGBA& color : attainable)
        colors.emplace_back(std::clamp(int(std::lround(color[0] * 255.f)), 0, 255),
                            std::clamp(int(std::lround(color[1] * 255.f)), 0, 255),
                            std::clamp(int(std::lround(color[2] * 255.f)), 0, 255));
    return colors;
}

wxColour compute_color_match_recipe_display_color(const MixedColorMatchRecipeResult& recipe, const MixedFilamentDisplayContext& context)
{
    if (!recipe.valid)
        return recipe.preview_color.IsOk() ? recipe.preview_color : wxColour("#26A69A");

    MixedFilament entry;
    entry.component_a                = recipe.component_a;
    entry.component_b                = recipe.component_b;
    entry.mix_b_percent              = recipe.mix_b_percent;
    entry.manual_pattern             = recipe.manual_pattern;
    entry.gradient_component_ids     = recipe.gradient_component_ids;
    entry.gradient_component_weights = recipe.gradient_component_weights;
    entry.distribution_mode          = recipe.gradient_component_ids.empty() ? int(MixedFilament::Simple) : int(MixedFilament::LayerCycle);

    return parse_mixed_color(compute_mixed_filament_display_color(entry, context));
}

std::vector<int> decode_color_match_gradient_weights(const std::string& value, size_t expected_components)
{
    std::vector<int> weights;
    if (value.empty() || expected_components == 0)
        return weights;

    std::string token;
    for (const char ch : value) {
        if (ch >= '0' && ch <= '9') {
            token.push_back(ch);
            continue;
        }
        if (!token.empty()) {
            weights.emplace_back(std::max(0, std::atoi(token.c_str())));
            token.clear();
        }
    }
    if (!token.empty())
        weights.emplace_back(std::max(0, std::atoi(token.c_str())));
    if (weights.size() != expected_components)
        weights.clear();
    return weights;
}

MixedColorMatchRecipeResult build_pair_color_match_candidate(
    const std::vector<wxColour>& palette, unsigned int component_a, unsigned int component_b, int mix_b_percent, int min_component_percent)
{
    MixedColorMatchRecipeResult candidate;
    if (component_a == 0 || component_b == 0 || component_a == component_b)
        return candidate;
    if (component_a > palette.size() || component_b > palette.size())
        return candidate;
    if (!color_match_weights_within_range({100 - std::clamp(mix_b_percent, 0, 100), std::clamp(mix_b_percent, 0, 100)},
                                          min_component_percent))
        return candidate;

    candidate.valid         = true;
    candidate.component_a   = component_a;
    candidate.component_b   = component_b;
    candidate.mix_b_percent = std::clamp(mix_b_percent, 0, 100);
    candidate.preview_color = blend_pair_filament_mixer(palette[component_a - 1], palette[component_b - 1],
                                                        float(candidate.mix_b_percent) / 100.f);
    return candidate;
}

MixedColorMatchRecipeResult build_multi_color_match_candidate(const std::vector<wxColour>&     palette,
                                                              const std::vector<unsigned int>& ids,
                                                              const std::vector<int>&          weights,
                                                              int                              min_component_percent)
{
    MixedColorMatchRecipeResult candidate;
    if (ids.size() < 3 || ids.size() != weights.size())
        return candidate;
    if (!color_match_weights_within_range(weights, min_component_percent))
        return candidate;

    std::vector<std::pair<int, unsigned int>> weighted_ids;
    weighted_ids.reserve(ids.size());
    for (size_t idx = 0; idx < ids.size(); ++idx) {
        if (ids[idx] == 0 || ids[idx] > palette.size())
            return candidate;
        if (weights[idx] <= 0)
            continue;
        weighted_ids.emplace_back(weights[idx], ids[idx]);
    }
    if (weighted_ids.size() < 3)
        return candidate;

    std::sort(weighted_ids.begin(), weighted_ids.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.first != rhs.first)
            return lhs.first > rhs.first;
        return lhs.second < rhs.second;
    });

    std::vector<unsigned int> ordered_ids;
    std::vector<int>          ordered_weights;
    ordered_ids.reserve(weighted_ids.size());
    ordered_weights.reserve(weighted_ids.size());
    for (const auto& [weight, filament_id] : weighted_ids) {
        ordered_ids.emplace_back(filament_id);
        ordered_weights.emplace_back(weight);
    }

    const std::vector<unsigned int> sequence = build_color_match_sequence(ordered_ids, ordered_weights);
    if (sequence.empty())
        return candidate;

    candidate.valid             = true;
    candidate.component_a       = ordered_ids[0];
    candidate.component_b       = ordered_ids[1];
    const int pair_weight_total = ordered_weights[0] + ordered_weights[1];
    candidate.mix_b_percent     = pair_weight_total > 0 ?
                                      std::clamp(int(std::lround(100.0 * double(ordered_weights[1]) / double(pair_weight_total))), 0, 100) :
                                      50;
    candidate.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(ordered_ids);
    {
        std::ostringstream weights_ss;
        for (size_t weight_idx = 0; weight_idx < ordered_weights.size(); ++weight_idx) {
            if (weight_idx > 0)
                weights_ss << '/';
            weights_ss << ordered_weights[weight_idx];
        }
        candidate.gradient_component_weights = weights_ss.str();
    }
    candidate.preview_color = blend_sequence_filament_mixer(palette, sequence);
    return candidate;
}

bool color_match_weights_within_range(const std::vector<int>& weights, int min_component_percent)
{
    if (min_component_percent <= 0)
        return true;

    const int min_allowed       = std::clamp(min_component_percent, 0, 50);
    int       active_components = 0;
    for (const int weight : weights) {
        if (weight <= 0)
            continue;
        ++active_components;
        if (weight < min_allowed)
            return false;
    }
    return active_components >= 2;
}

std::vector<unsigned int> build_color_match_sequence(const std::vector<unsigned int>& ids, const std::vector<int>& weights)
{
    if (ids.empty() || ids.size() != weights.size())
        return {};

    constexpr int k_max_cycle = 48;

    std::vector<unsigned int> filtered_ids;
    std::vector<int>          counts;
    filtered_ids.reserve(ids.size());
    counts.reserve(weights.size());
    for (size_t idx = 0; idx < ids.size(); ++idx) {
        const int weight = std::max(0, weights[idx]);
        if (weight <= 0)
            continue;
        filtered_ids.emplace_back(ids[idx]);
        counts.emplace_back(std::max(1, int(std::round((double(weight) / 100.0) * k_max_cycle))));
    }

    if (filtered_ids.empty())
        return {};

    int cycle = std::accumulate(counts.begin(), counts.end(), 0);
    while (cycle > k_max_cycle) {
        auto it = std::max_element(counts.begin(), counts.end());
        if (it == counts.end() || *it <= 1)
            break;
        --(*it);
        --cycle;
    }

    if (cycle <= 0)
        return {};

    std::vector<unsigned int> sequence;
    sequence.reserve(size_t(cycle));
    std::vector<int> emitted(counts.size(), 0);
    for (int pos = 0; pos < cycle; ++pos) {
        size_t best_idx   = 0;
        double best_score = -1e9;
        for (size_t idx = 0; idx < counts.size(); ++idx) {
            const double target = double((pos + 1) * counts[idx]) / double(std::max(1, cycle));
            const double score  = target - double(emitted[idx]);
            if (score > best_score) {
                best_score = score;
                best_idx   = idx;
            }
        }
        ++emitted[best_idx];
        sequence.emplace_back(filtered_ids[best_idx]);
    }

    return sequence;
}

wxColour blend_sequence_filament_mixer(const std::vector<wxColour>& palette, const std::vector<unsigned int>& sequence)
{
    if (palette.empty() || sequence.empty())
        return wxColour("#26A69A");

    std::vector<int> counts(palette.size() + 1, 0);
    for (const unsigned int filament_id : sequence) {
        if (filament_id == 0 || filament_id > palette.size())
            continue;
        ++counts[filament_id];
    }

    std::vector<wxColour> colors;
    std::vector<double>   weights;
    colors.reserve(palette.size());
    weights.reserve(palette.size());
    for (size_t filament_id = 1; filament_id <= palette.size(); ++filament_id) {
        if (counts[filament_id] <= 0)
            continue;
        colors.emplace_back(palette[filament_id - 1]);
        weights.emplace_back(double(counts[filament_id]));
    }

    return blend_multi_filament_mixer(colors, weights);
}

// ---------------------------------------------------------------------------
// Material compatibility
// ---------------------------------------------------------------------------

enum class FilamentCategory : uint8_t {
    PLA, PETG, TPU, PET, ABS, ASA, PC, PA, SUPPORT,
    UNKNOWN
};

static constexpr const char* k_category_names[] = {
    "PLA", "PETG", "TPU", "PET", "ABS", "ASA", "PC", "PA", "SUPPORT"
};
static constexpr size_t k_category_count = sizeof(k_category_names) / sizeof(k_category_names[0]);
// Matrix dimension covers all valid categories + UNKNOWN sentinel
static constexpr size_t k_compat_dim = static_cast<size_t>(FilamentCategory::UNKNOWN) + 1;

static FilamentCategory filament_category_from_name(const std::string& name)
{
    for (size_t i = 0; i < k_category_count; ++i) {
        if (name == k_category_names[i])
            return static_cast<FilamentCategory>(i);
    }
    return FilamentCategory::UNKNOWN;
}

// 2D compatibility matrix. Dimension is tied to the enum so adding a category
// to FilamentCategory automatically grows the table.
static std::vector<std::vector<bool>> s_compat;
// atomic, not plain bool: the unlocked first read below and the locked write in
// load_filament_compatibility() would otherwise be a data race (classic non-atomic
// double-checked locking). acquire/release is sufficient — the mutex inside provides
// the heavy lifting; this flag only needs to publish "already loaded" without a race.
static std::atomic<bool>              s_compat_loaded{false};
static std::mutex                     s_compat_mutex;

static void load_filament_compatibility()
{
    if (s_compat_loaded.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(s_compat_mutex);
    if (s_compat_loaded.load(std::memory_order_acquire)) return;

    // Default: each category compatible only with itself
    s_compat.assign(k_compat_dim, std::vector<bool>(k_compat_dim, false));
    for (size_t i = 0; i < k_category_count; ++i)
        s_compat[i][i] = true;

    try {
        // Prefer user data dir (where PresetUpdater deploys updates), fall back to bundled resources
        const boost::filesystem::path user_path = (boost::filesystem::path(Slic3r::data_dir()) / PRESET_SYSTEM_DIR
                                                    / PresetBundle::SM_BUNDLE / "filament"
                                                    / "filament_compatibility.json").make_preferred();
        const boost::filesystem::path rsrc_path = (boost::filesystem::path(Slic3r::resources_dir()) / "profiles"
                                                    / PresetBundle::SM_BUNDLE / "filament"
                                                    / "filament_compatibility.json").make_preferred();
        const std::string path = (boost::filesystem::exists(user_path) ? user_path : rsrc_path).string();

        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            BOOST_LOG_TRIVIAL(error) << "Failed to open filament compatibility config: " << path;
            return;
        }
        nlohmann::json j;
        ifs >> j;

        if (!j.contains("compatibility")) {
            BOOST_LOG_TRIVIAL(error) << "Missing 'compatibility' key in " << path;
            return;
        }

        for (auto& [cat_a_str, partner_list] : j["compatibility"].items()) {
            FilamentCategory cat_a = filament_category_from_name(cat_a_str);
            if (cat_a == FilamentCategory::UNKNOWN) {
                BOOST_LOG_TRIVIAL(warning) << "Unknown category '" << cat_a_str << "' in compatibility config";
                continue;
            }

            if (!partner_list.is_array()) {
                BOOST_LOG_TRIVIAL(warning) << "Expected array for category '" << cat_a_str << "'";
                continue;
            }

            for (auto& cat_b_val : partner_list) {
                const std::string cat_b_str = cat_b_val.get<std::string>();
                FilamentCategory   cat_b     = filament_category_from_name(cat_b_str);
                if (cat_b == FilamentCategory::UNKNOWN) {
                    BOOST_LOG_TRIVIAL(warning) << "Unknown category '" << cat_b_str << "' in compatibility config";
                    continue;
                }
                s_compat[static_cast<size_t>(cat_a)][static_cast<size_t>(cat_b)] = true;
                s_compat[static_cast<size_t>(cat_b)][static_cast<size_t>(cat_a)] = true;
            }
        }
        s_compat_loaded.store(true, std::memory_order_release);
        BOOST_LOG_TRIVIAL(info) << "Loaded filament compatibility matrix from " << path;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to parse filament compatibility config: " << e.what();
    }
}

static bool is_category_compatible(FilamentCategory a, FilamentCategory b)
{
    return s_compat[static_cast<size_t>(a)][static_cast<size_t>(b)];
}

struct ResolvedFilamentCategory {
    unsigned int     filament_id;
    FilamentCategory category;
};

static FilamentCategory get_filament_category(const std::string& filament_type);

// Resolves a set of 0-based filament IDs into their (id, category) pairs.
// Skips IDs that cannot be resolved (out of range, missing preset, missing type).
// Caller must have called load_filament_compatibility() first.
static std::vector<ResolvedFilamentCategory> resolve_filament_categories(
    const std::vector<unsigned int>& filament_ids,
    PresetBundle*                    preset_bundle)
{
    std::vector<ResolvedFilamentCategory> result;
    if (!preset_bundle) return result;

    const auto& filament_presets = preset_bundle->filament_presets;
    result.reserve(filament_ids.size());

    for (unsigned int id : filament_ids) {
        if (id >= filament_presets.size()) continue;
        const Preset* preset = preset_bundle->filaments.find_preset(filament_presets[id]);
        if (!preset) continue;
        auto* type_opt = dynamic_cast<const ConfigOptionStrings*>(preset->config.option("filament_type"));
        if (!type_opt || type_opt->values.empty()) continue;

        FilamentCategory cat = get_filament_category(type_opt->values[0]);
        result.push_back({id, cat});
    }
    return result;
}

// Hardcoded filament_type → category. New filament types should be added here.
static const std::unordered_map<std::string, FilamentCategory>& filament_type_category_map()
{
    static const std::unordered_map<std::string, FilamentCategory> m = {
        // PLA family — from classification table
        {"PLA", FilamentCategory::PLA}, {"PLA-CF", FilamentCategory::PLA},
        // ABS
        {"ABS", FilamentCategory::ABS},
        // ASA
        {"ASA", FilamentCategory::ASA},
        // PETG family — from classification table
        {"PETG", FilamentCategory::PETG}, {"PETG-CF", FilamentCategory::PETG}, {"PCTG", FilamentCategory::PETG},
        // TPU
        {"TPU", FilamentCategory::TPU},
        // PET — from compatibility matrix
        {"PET", FilamentCategory::PET},
        // PA
        {"PA", FilamentCategory::PA}, {"PA-CF", FilamentCategory::PA},
        // PC
        {"PC", FilamentCategory::PC},
        // Support materials — from classification table
        {"BVOH", FilamentCategory::SUPPORT}, {"PVA", FilamentCategory::SUPPORT},
    };
    return m;
}

static std::string normalize_filament_type(const std::string& type)
{
    std::string normalized = type;
    size_t start = normalized.find_first_not_of(" \t\r\n");
    size_t end   = normalized.find_last_not_of(" \t\r\n");
    if (start != std::string::npos && end != std::string::npos)
        normalized = normalized.substr(start, end - start + 1);
    // toupper from <cctype> requires an argument in [0, UCHAR_MAX] or EOF; passing a
    // negative char (signed byte >= 0x80, e.g. a non-ASCII filament_type) is UB. The lambda
    // forces the unsigned char conversion the C API expects.
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return normalized;
}

static FilamentCategory get_filament_category(const std::string& filament_type)
{
    const std::string normalized = normalize_filament_type(filament_type);
    const auto&       m          = filament_type_category_map();
    auto              it         = m.find(normalized);
    if (it != m.end()) return it->second;
    return FilamentCategory::UNKNOWN;
}

bool is_filament_compatible(const std::vector<unsigned int>& filament_ids)
{
    if (filament_ids.size() <= 1) return true;

    load_filament_compatibility();

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle) {
        BOOST_LOG_TRIVIAL(error) << "PresetBundle is null in filament compatibility check";
        return false;
    }

    auto resolved = resolve_filament_categories(filament_ids, preset_bundle);

    for (const auto& r : resolved) {
        if (r.category == FilamentCategory::UNKNOWN) {
            BOOST_LOG_TRIVIAL(info) << "Filament type at index " << r.filament_id
                                    << " not in compatibility table, treating as incompatible";
            return false;
        }
    }

    if (resolved.size() <= 1) return true;

    if (std::all_of(resolved.begin() + 1, resolved.end(),
                    [&](const ResolvedFilamentCategory& r) { return r.category == resolved[0].category; }))
        return true;

    for (size_t i = 0; i < resolved.size(); ++i) {
        for (size_t j = i + 1; j < resolved.size(); ++j) {
            if (!is_category_compatible(resolved[i].category, resolved[j].category)) {
                BOOST_LOG_TRIVIAL(info) << "Incompatible filament categories: '"
                                        << k_category_names[static_cast<size_t>(resolved[i].category)]
                                        << "' vs '"
                                        << k_category_names[static_cast<size_t>(resolved[j].category)] << "'";
                return false;
            }
        }
    }

    return true;
}

bool is_filament_compatible(const MixedFilament& mf)
{
    std::vector<unsigned int> fids;

    if (!mf.manual_pattern.empty()) {
        // Cycle / pattern mode: the pattern tokens encode all participating
        // filaments. component_a / component_b are not added separately —
        // in cycle mode they are hardcoded to 1 and 2 and would introduce
        // phantom filaments into the compatibility check.
        const std::string norm = MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern);
        if (norm.empty()) {
            BOOST_LOG_TRIVIAL(warning)
                << "Mixed filament compatibility: manual_pattern '"
                << mf.manual_pattern << "' normalized to empty, malformed pattern";
            return false;
        }

        PresetBundle* preset_bundle = wxGetApp().preset_bundle;
        if (!preset_bundle || preset_bundle->filament_presets.empty()) {
            BOOST_LOG_TRIVIAL(error)
                << "Mixed filament compatibility: PresetBundle is "
                << (preset_bundle ? "empty (no filament presets)" : "null")
                << ", cannot resolve pattern '" << mf.manual_pattern << "'";
            return false;
        }

        const size_t num_physical = preset_bundle->filament_presets.size();
        const auto groups = MixedFilamentManager::split_pattern_groups(norm);
        for (const auto& group : groups) {
            const auto tokens = MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical);
            for (const auto& token : tokens) {
                const unsigned int eid = MixedFilamentManager::physical_filament_from_token(token, mf, num_physical);
                if (eid >= 1 && eid <= num_physical)
                    fids.push_back(eid - 1);
            }
        }
    } else {
        if (mf.component_a >= 1) fids.push_back(mf.component_a - 1);
        if (mf.component_b >= 1) fids.push_back(mf.component_b - 1);
        if (!mf.gradient_component_ids.empty()) {
            for (unsigned int fid : MixedFilamentManager::decode_gradient_component_ids(mf.gradient_component_ids))
                if (fid >= 1) fids.push_back(fid - 1);
        }
    }

    if (fids.empty()) {
        BOOST_LOG_TRIVIAL(warning)
            << "Mixed filament compatibility: no valid filament IDs extracted"
            << " (component_a=" << mf.component_a
            << ", component_b=" << mf.component_b
            << ", manual_pattern='" << mf.manual_pattern
            << "', gradient_component_ids='" << mf.gradient_component_ids << "')"
            << " — treating as incompatible";
        return false;
    }

    return is_filament_compatible(fids);
}

std::optional<std::pair<unsigned int, unsigned int>> find_incompatible_filament_pair(const std::vector<unsigned int>& filament_ids)
{
    if (filament_ids.size() <= 1) return std::nullopt;

    load_filament_compatibility();

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle) {
        BOOST_LOG_TRIVIAL(error) << "PresetBundle is null in filament incompatibility search";
        return std::nullopt;
    }

    auto resolved = resolve_filament_categories(filament_ids, preset_bundle);

    // Treat UNKNOWN filament types as incompatible, consistent with is_filament_compatible().
    for (const auto& r : resolved) {
        if (r.category == FilamentCategory::UNKNOWN) {
            BOOST_LOG_TRIVIAL(info) << "Filament type at index " << r.filament_id
                                    << " not in compatibility table, treating as incompatible";
            // Return this UNKNOWN paired with the first other resolved filament.
            for (const auto& other : resolved) {
                if (other.filament_id != r.filament_id)
                    return std::make_pair(r.filament_id + 1, other.filament_id + 1);
            }
        }
    }

    if (resolved.size() <= 1) return std::nullopt;

    if (std::all_of(resolved.begin() + 1, resolved.end(),
                    [&](const ResolvedFilamentCategory& r) { return r.category == resolved[0].category; }))
        return std::nullopt;

    for (size_t i = 0; i < resolved.size(); ++i) {
        for (size_t j = i + 1; j < resolved.size(); ++j) {
            if (!is_category_compatible(resolved[i].category, resolved[j].category))
                return std::make_pair(
                    resolved[i].filament_id + 1,
                    resolved[j].filament_id + 1);
        }
    }

    return std::nullopt;
}

CyclePatternParseResult parse_cycle_pattern(const std::string& normalized_pattern, int num_physical)
{
    CyclePatternParseResult result;
    if (normalized_pattern.empty() || num_physical <= 0) return result;

    const auto groups = MixedFilamentManager::split_pattern_groups(normalized_pattern);
    for (const auto& group : groups) {
        const auto tokens = MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical);
        for (const auto& token : tokens) {
            ++result.total_tokens;
            char* end = nullptr;
            unsigned long id = std::strtoul(token.c_str(), &end, 10);
            if (!end || *end != '\0') {
                if (result.invalid_token.empty()) result.invalid_token = token;
                continue;
            }
            if (id < 1 || id > (unsigned long)num_physical) {
                if (result.invalid_id == 0) result.invalid_id = (unsigned int)id;
                continue;
            }
            result.ids.push_back((unsigned int)id);
        }
    }
    return result;
}

std::string summarize_cycle_pattern_text(const std::string& normalized_pattern,
                                         const MixedFilament& entry,
                                         int num_physical)
{
    if (normalized_pattern.empty() || num_physical <= 0)
        return {};

    const auto groups = MixedFilamentManager::split_pattern_groups(normalized_pattern);
    if (groups.empty())
        return {};

    std::map<unsigned int, int> counts;
    int                         total = 0;
    for (const auto& group : groups) {
        const auto tokens = MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical);
        for (const auto& token : tokens) {
            unsigned int eid = MixedFilamentManager::physical_filament_from_token(token, entry, num_physical);
            if (eid >= 1 && eid <= static_cast<unsigned>(num_physical)) {
                counts[eid]++;
                total++;
            }
        }
    }

    if (total <= 0 || counts.empty())
        return {};

    std::vector<std::pair<unsigned int, int>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Compute floor percentages; distribute remainder via largest remainders.
    std::vector<int> pcts(sorted.size());
    int              sum_pct = 0;
    for (size_t i = 0; i < sorted.size(); ++i) {
        pcts[i]  = int((static_cast<long long>(sorted[i].second) * 100) / total);
        sum_pct += pcts[i];
    }

    if (sum_pct < 100) {
        // Remainder indexed by original position, value = count * 100 % total
        std::vector<std::pair<size_t, int>> rem;
        rem.reserve(sorted.size());
        for (size_t i = 0; i < sorted.size(); ++i)
            rem.emplace_back(i, int((static_cast<long long>(sorted[i].second) * 100) % total));
        // Sort descending by remainder, then by original index for stability
        std::sort(rem.begin(), rem.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second > b.second;
            return a.first < b.first;
        });
        for (int extra = 100 - sum_pct; extra > 0; --extra) {
            pcts[rem.front().first]++;
            rem.erase(rem.begin());
        }
    }

    std::ostringstream out;
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (i > 0)
            out << '+';
        out << 'F' << sorted[i].first << ' ' << pcts[i] << '%';
    }
    return out.str();
}

// ---- Batch Match Mapping ----

std::vector<ModelColorEntry> extract_model_colors(const Print& print)
{
    std::vector<ModelColorEntry> colors;
    static constexpr int MAX_EXTRUDER_ID = int(MAXIMUM_EXTRUDER_NUMBER);

    auto& filament_colours = print.config().filament_colour.values;

    // Also read mixed-filament display colors for virtual ID lookup.
    // NOTE: Do NOT bind a reference via a ternary with a temporary on one branch —
    // the temporary would be destroyed at the end of the full expression, leaving
    // a dangling reference (use-after-scope UB). Use the raw pointer and null-check
    // it only at the virtual-lookup call site; the physical-colour path does not
    // depend on preset_bundle at all.
    auto* pb = wxGetApp().preset_bundle;
    // Log the null-preset_bundle condition ONCE here (not per virtual-ID iteration
    // inside the loop below) — otherwise a model with N virtual-painted faces would
    // emit N identical warnings. pb is loop-invariant, so the inner check can stay
    // a silent `continue`.
    if (pb == nullptr) {
        BOOST_LOG_TRIVIAL(warning)
            << "extract_model_colors: preset_bundle null; virtual filament colors will be skipped";
    }

    for (const PrintObject* obj : print.objects()) {
        if (!obj) continue;
        const ModelObject* model_obj = obj->model_object();
        if (!model_obj) continue;

        for (const ModelVolume* vol : model_obj->volumes) {
            if (!vol || vol->type() != ModelVolumeType::MODEL_PART) continue;

            for (int eid : vol->get_extruders()) {
                if (eid < 1 || eid > MAX_EXTRUDER_ID) continue;
                size_t idx = size_t(eid - 1);

                std::string color_hex;
                if (idx < filament_colours.size()) {
                    // Physical filament color
                    color_hex = filament_colours[idx];
                } else {
                    // Virtual mixed filament — look up its display_color.
                    // pb null is already logged once above the loop; skip silently here.
                    if (pb == nullptr) {
                        continue;
                    }
                    const MixedFilament* mf = pb->mixed_filaments.mixed_filament_from_id(
                        static_cast<unsigned int>(eid), filament_colours.size());
                    if (mf && !mf->display_color.empty())
                        color_hex = mf->display_color;
                }
                if (color_hex.empty()) continue;

                wxColour c;
                if (!try_parse_color_match_hex(color_hex, c)) {
                    BOOST_LOG_TRIVIAL(warning)
                        << "extract_model_colors: invalid color for extruder "
                        << eid << " = '" << color_hex << "', skipping";
                    continue;
                }

                // Deduplicate by NORMALIZED hex value — same RGB written differently
                // (e.g. "#ff0000" vs "#FF0000", "FF0000" vs "#FF0000", or alpha variants
                // "#RRGGBBAA" vs "#RRGGBB") must collapse to one entry. Comparing raw
                // strings would double-count such colors and waste virtual slots. Mirrors
                // the normalized dedup in build_color_match_presets (line ~213). The
                // stored hex_value keeps its original form for display.
                const std::string color_key = normalize_color_match_hex(color_hex).ToStdString();
                auto it = std::find_if(colors.begin(), colors.end(),
                    [&](const ModelColorEntry& e) {
                        return normalize_color_match_hex(e.hex_value).ToStdString() == color_key;
                    });
                if (it != colors.end()) {
                    it->extruder_ids.push_back(static_cast<unsigned int>(eid));
                    continue;
                }

                colors.push_back({
                    static_cast<unsigned int>(colors.size() + 1),
                    c,
                    color_hex,
                    {static_cast<unsigned int>(eid)}
                });
            }
        }
    }

    if (colors.size() > 64) {
        BOOST_LOG_TRIVIAL(warning)
            << "extract_model_colors: truncating " << colors.size() << " colors to 64";
        colors.resize(64);
    }

    BOOST_LOG_TRIVIAL(info)
        << "extract_model_colors: extracted " << colors.size() << " unique model colors";
    return colors;
}

// ---- Batch Match Algorithm ----

#if 0 // Dead code — no deduplication is performed (explicit policy since phase2)
std::vector<ColorMappingEntry> deduplicate_batch_mappings(
    const std::vector<ColorMappingEntry>& raw_mappings,
    double                                 merge_threshold)
{
    if (merge_threshold <= 0.0 || merge_threshold > 100.0) {
        BOOST_LOG_TRIVIAL(warning)
            << "deduplicate_batch_mappings: invalid threshold " << merge_threshold
            << ", returning unmodified";
        return raw_mappings;
    }
    std::vector<ColorMappingEntry> result;

    for (const auto& mapping : raw_mappings) {
        bool merged = false;
        for (auto& existing : result) {
            if (color_delta_e00(existing.matched_color, mapping.matched_color) < merge_threshold) {
                existing.merged_model_indices.push_back(mapping.model_color_index);
                existing.source_extruder_ids.insert(
                    existing.source_extruder_ids.end(),
                    mapping.source_extruder_ids.begin(),
                    mapping.source_extruder_ids.end());
                merged = true;
                break;
            }
        }
        if (!merged) {
            result.push_back(mapping);
        }
    }
    BOOST_LOG_TRIVIAL(debug)
        << "deduplicate_batch_mappings: " << raw_mappings.size()
        << " -> " << result.size() << " (threshold DeltaE<" << merge_threshold << ")";
    return result;
}
#endif // 0

void assign_batch_virtual_filament_ids(
    BatchMatchResult& result,
    size_t             num_physical,
    size_t             existing_mixed_count)
{
    if (num_physical < 1) {
        BOOST_LOG_TRIVIAL(error)
            << "assign_batch_virtual_filament_ids: invalid num_physical=" << num_physical;
        return;
    }
    unsigned int next_virtual_id = static_cast<unsigned int>(num_physical + existing_mixed_count + 1);

    for (auto& mapping : result.mappings) {
        if (mapping.is_pure_recipe) {
            if (mapping.recipe.component_a < 1 || mapping.recipe.component_a > num_physical) {
                BOOST_LOG_TRIVIAL(warning)
                    << "assign_batch_virtual_filament_ids: component_a="
                    << mapping.recipe.component_a << " out of range [1," << num_physical
                    << "], treating as mixed";
                mapping.target_filament_id = next_virtual_id++;
                continue;
            }
            mapping.target_filament_id = mapping.recipe.component_a;
        } else {
            mapping.target_filament_id = next_virtual_id++;
        }
    }
}

std::vector<ColorMappingEntry> merge_duplicate_recipe_mappings(
    const std::vector<ColorMappingEntry>& mappings)
{
    if (mappings.size() < 2) return mappings;

    // Fingerprint: the recipe fields that define which physical filaments are mixed and in
    // what ratio. Two recipes with the same fingerprint produce the same blend, so they must
    // share one virtual slot. Derived fields (preview_color, delta_e, display_color) are
    // excluded — they are deterministic functions of the identity fields.
    auto fingerprint = [](const MixedColorMatchRecipeResult& r) {
        std::ostringstream oss;
        // Use '|' as a field separator so (a=1,b=12) != (a=11,b=2); the ints alone would
        // collide if concatenated without a delimiter.
        oss << r.component_a << '|' << r.component_b << '|' << r.mix_b_percent << '|'
            << r.manual_pattern << '|' << r.gradient_component_ids << '|'
            << r.gradient_component_weights;
        return oss.str();
    };

    std::vector<ColorMappingEntry> result;
    result.reserve(mappings.size());
    // recipe fingerprint → index into result. Only non-pure mappings are tracked: pure
    // recipes target existing physical IDs and never allocate a new slot, so deduping them
    // is both unnecessary and semantically wrong (two pure recipes hitting the same physical
    // is the normal, expected case — they share the ID already).
    std::unordered_map<std::string, size_t> seen;

    for (const ColorMappingEntry& m : mappings) {
        if (m.is_pure_recipe) {
            result.push_back(m);
            continue;
        }
        const std::string key = fingerprint(m.recipe);
        auto it = seen.find(key);
        if (it != seen.end()) {
            // Merge into the first occurrence: union source_extruder_ids (so apply still
            // remaps every source extruder to this target) and accumulate merged_model_indices
            // for traceability. The survivor keeps its own target_filament_id — the merged-in
            // mapping's target is discarded (it was a duplicate slot allocation).
            ColorMappingEntry& survivor = result[it->second];
            survivor.source_extruder_ids.insert(survivor.source_extruder_ids.end(),
                                                m.source_extruder_ids.begin(),
                                                m.source_extruder_ids.end());
            survivor.merged_model_indices.insert(survivor.merged_model_indices.end(),
                                                 m.merged_model_indices.begin(),
                                                 m.merged_model_indices.end());
        } else {
            seen.emplace(key, result.size());
            result.push_back(m);
        }
    }

    // The merge above unions source_extruder_ids / merged_model_indices by appending.
    // The two are normally disjoint across merged mappings (extract_model_colors
    // aggregates extruders by unique color), but sort+unique defensively so
    // survivor.source_extruder_ids stays idempotent for apply_batch_match_to_model
    // (unordered_map) and merged_model_indices doesn't double-count in traceability.
    // Only runs when a merge actually happened; the common no-merge path is untouched.
    if (result.size() != mappings.size()) {
        for (ColorMappingEntry& e : result) {
            if (e.is_pure_recipe) continue;
            std::sort(e.source_extruder_ids.begin(), e.source_extruder_ids.end());
            e.source_extruder_ids.erase(
                std::unique(e.source_extruder_ids.begin(), e.source_extruder_ids.end()),
                e.source_extruder_ids.end());
            std::sort(e.merged_model_indices.begin(), e.merged_model_indices.end());
            e.merged_model_indices.erase(
                std::unique(e.merged_model_indices.begin(), e.merged_model_indices.end()),
                e.merged_model_indices.end());
        }
    }

    if (result.size() != mappings.size()) {
        BOOST_LOG_TRIVIAL(info)
            << "merge_duplicate_recipe_mappings: " << mappings.size()
            << " -> " << result.size() << " (merged " << (mappings.size() - result.size())
            << " duplicate recipe mappings)";
    }
    return result;
}

BatchMatchResult batch_match_model_colors(
    const std::vector<ModelColorEntry>&          model_colors,
    const std::vector<std::string>&             physical_colors,
    int                                          min_component_percent,
    int                                          max_component_percent,
    std::shared_ptr<std::atomic<bool>>           cancel_token,
    std::function<void(int,int)>                 progress_callback,
    bool                                         check_compatible)
{
    BatchMatchResult result;
    result.success = true;

    if (min_component_percent < 0 || min_component_percent > 50) {
        result.success = false;
        result.error_message = "min_component_percent must be in [0, 50]";
        return result;
    }
    // Symmetric with build_best_color_match_recipe's max check (see line ~375). Without this,
    // an out-of-range max would be forwarded per-recipe and silently rejected by the inner
    // function, surfacing to the user as the misleading "No valid recipes found" instead of
    // a parameter error.
    if (max_component_percent < 50 || max_component_percent > 100) {
        result.success = false;
        result.error_message = "max_component_percent must be in [50, 100]";
        return result;
    }
    if (model_colors.empty()) {
        result.success = false;
        result.error_message = "No model colors to match";
        return result;
    }
    if (physical_colors.size() < 2) {
        result.success = false;
        result.error_message = "Need at least 2 physical filaments";
        return result;
    }

    const int total_count = static_cast<int>(model_colors.size());
    for (size_t i = 0; i < model_colors.size(); ++i) {
        if (cancel_token && cancel_token->load()) {
            // User cancellation (Stop Matching) — not an error. error_message is intentionally
            // empty: handle_batch_match_result treats error_code==2 as a silent rollback and
            // never displays it. See BatchMatchResult.error_code docs.
            result.success    = false;
            result.error_code = 2;
            return result;
        }

        const auto& entry = model_colors[i];
        MixedColorMatchRecipeResult recipe =
            build_best_color_match_recipe(physical_colors, entry.color, min_component_percent, max_component_percent,
                                          check_compatible);

        if (!recipe.valid) {
            BOOST_LOG_TRIVIAL(warning)
                << "batch_match: no valid recipe for color " << entry.color_index
                << " (" << entry.hex_value << ")";
            continue;
        }

        ColorMappingEntry mapping;
        mapping.model_color_index   = entry.color_index;
        mapping.source_color         = entry.color;
        mapping.recipe               = recipe;
        mapping.delta_e              = recipe.delta_e;
        mapping.matched_color        = recipe.preview_color;
        mapping.is_pure_recipe       = (recipe.mix_b_percent == 0);
        mapping.pure_delta_e         = recipe.delta_e;
        mapping.merged_model_indices = {entry.color_index};
        mapping.source_extruder_ids  = entry.extruder_ids;
        result.mappings.push_back(mapping);

        if (progress_callback)
            progress_callback(static_cast<int>(i) + 1, total_count);
    }

    if (result.mappings.empty()) {
        result.success = false;
        result.error_message = "No valid recipes found for any model color";
        result.error_code = 1;
        return result;
    }

    // Each model color always gets its own mapping entry — no deduplication.
    // Merging by matched_color would lose distinct model-source colors whose
    // recipes happen to produce similar output shades.

    double sum_de = 0.0;
    for (const auto& m : result.mappings)
        sum_de += m.delta_e;
    result.avg_delta_e = sum_de / double(result.mappings.size());

    result.selected_physical_ids.clear();
    for (size_t i = 1; i <= physical_colors.size(); ++i)
        result.selected_physical_ids.push_back(static_cast<unsigned int>(i));

    BOOST_LOG_TRIVIAL(info)
        << "batch_match: " << total_count << " model colors -> "
        << result.mappings.size() << " unique recipes, avg DeltaE=" << result.avg_delta_e;
    return result;
}

void populate_mixed_filaments_from_mappings(
    BatchMatchResult&                           result,
    const MixedFilamentDisplayContext&          context)
{
    result.mixed_filaments.clear();

    for (const auto& mapping : result.mappings) {
        if (mapping.is_pure_recipe) continue;

        MixedFilament mf;
        mf.component_a    = mapping.recipe.component_a;
        mf.component_b    = mapping.recipe.component_b;
        mf.mix_b_percent  = mapping.recipe.mix_b_percent;
        mf.manual_pattern = mapping.recipe.manual_pattern;
        mf.stable_id      = 0; // filled by add_batch_custom_filaments
        mf.ratio_a        = 1;
        mf.ratio_b        = 1;
        mf.distribution_mode = mapping.recipe.gradient_component_ids.empty()
            ? int(MixedFilament::Simple) : int(MixedFilament::LayerCycle);
        mf.gradient_component_ids     = mapping.recipe.gradient_component_ids;
        mf.gradient_component_weights = mapping.recipe.gradient_component_weights;
        mf.enabled        = true;
        mf.deleted        = false;
        mf.custom         = true;
        mf.origin_auto    = false;
        mf.ui_mode        = 2; // MATCH
        mf.display_color  = mapping.matched_color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();

        result.mixed_filaments.push_back(std::move(mf));
    }
}

std::vector<std::string> recommend_best_filament_combo(
    const std::vector<ModelColorEntry>&  model_colors,
    const std::vector<std::string>&      all_preset_colors,
    int                                  min_component_percent,
    int                                  max_component_percent,
    std::shared_ptr<std::atomic<bool>>   cancel_token)
{
    if (model_colors.empty() || all_preset_colors.size() < 4)
        return {};

    // Loose bounds only: this function scores colour COMBOS, it does not impose the
    // final per-component cap (that is the Pass-2 batch_match_model_colors job, which
    // independently validates min∈[0,50]/max∈[50,100]). So the legal range here is the
    // full [0,100] for both — narrower limits (e.g. mirroring batch_match_model_colors's
    // [0,50]/[50,100]) would wrongly reject valid callers like the worker's min=15.
    // min>max is a genuine logic error: the search window is empty and every combo
    // silently scores as "no valid recipe". Reject with a log instead of returning {}.
    if (min_component_percent < 0 || min_component_percent > 100 ||
        max_component_percent < 0 || max_component_percent > 100 ||
        min_component_percent > max_component_percent) {
        BOOST_LOG_TRIVIAL(warning)
            << "recommend_best_filament_combo: invalid percent range min="
            << min_component_percent << " max=" << max_component_percent
            << " (need 0..100 and min<=max); returning empty";
        return {};
    }

    // Step 1: Top-15 pre-filter by single-color ΔE
    const size_t top_n = std::min<size_t>(15, all_preset_colors.size());
    std::vector<std::pair<double, size_t>> ranked;
    ranked.reserve(all_preset_colors.size());

    // Average ΔE of each preset color against all model colors → quick filter
    for (size_t fidx = 0; fidx < all_preset_colors.size(); ++fidx) {
        wxColour fc;
        if (!try_parse_color_match_hex(all_preset_colors[fidx], fc)) continue;
        double sum_de = 0.0;
        for (const auto& mc : model_colors)
            sum_de += color_delta_e00(fc, mc.color);
        ranked.emplace_back(sum_de / std::max(1.0, double(model_colors.size())), fidx);
    }
    std::sort(ranked.begin(), ranked.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    ranked.resize(top_n);

    // Step 2: Enumerate C(N,4) combos from top-15 candidates
    struct ComboScore { size_t i0, i1, i2, i3; double score; };
    std::vector<ComboScore> combos;
    const size_t n = ranked.size();
    for (size_t i0 = 0; i0 + 3 < n; ++i0) {
        for (size_t i1 = i0 + 1; i1 + 2 < n; ++i1) {
            for (size_t i2 = i1 + 1; i2 + 1 < n; ++i2) {
                for (size_t i3 = i2 + 1; i3 < n; ++i3) {
                    if (cancel_token && cancel_token->load()) return {};

                    // Build 4-color palette from original indices
                    std::vector<std::string> combo_colors = {
                        all_preset_colors[ranked[i0].second],
                        all_preset_colors[ranked[i1].second],
                        all_preset_colors[ranked[i2].second],
                        all_preset_colors[ranked[i3].second]
                    };

                    // Score: average batch match ΔE over model colors (single-threaded, small loop).
                    // check_compatible=false: this function is called only from the RECOMMENDED-mode
                    // worker, whose palette is a single Full Spectrum PLA preset (same material by
                    // construction) — no cross-type pair can exist, so the filter is a no-op. It
                    // also keeps the worker off preset_bundle: build_compatibility_matrix reads
                    // preset_bundle->filament_presets unsynchronized, which would race the UI thread
                    // (data-race UB) from this worker call site. The slice gate still enforces real
                    // incompatibility at slice time.
                    double sum_de = 0.0;
                    int matched = 0;
                    for (const auto& mc : model_colors) {
                        auto recipe = build_best_color_match_recipe(combo_colors, mc.color, min_component_percent, max_component_percent,
                                                                     /*check_compatible=*/ false);
                        if (recipe.valid) {
                            sum_de += recipe.delta_e;
                            ++matched;
                        }
                    }
                    if (matched == 0) continue;

                    double avg_de = sum_de / double(matched);
                    double score = 1.0 / (1.0 + avg_de); // lower ΔE → higher score
                    combos.push_back({ranked[i0].second, ranked[i1].second,
                                      ranked[i2].second, ranked[i3].second, score});
                }
            }
        }
    }

    if (combos.empty()) return {};

    // Step 3: Return best combo
    std::sort(combos.begin(), combos.end(),
        [](const auto& a, const auto& b) { return a.score > b.score; });

    return {
        all_preset_colors[combos[0].i0],
        all_preset_colors[combos[0].i1],
        all_preset_colors[combos[0].i2],
        all_preset_colors[combos[0].i3]
    };
}

void apply_batch_match_to_model(const BatchMatchResult& result)
{
    if (!result.success || result.mappings.empty()) return;

    // Direct mapping: each mapping entry carries the model extruder IDs that
    // need remapping to the target_filament_id.  No color hex comparison needed.
    std::unordered_map<int, unsigned int> extruder_remap;
    for (const auto& mapping : result.mappings) {
        for (unsigned int src_eid : mapping.source_extruder_ids) {
            if (mapping.target_filament_id != src_eid)
                extruder_remap[static_cast<int>(src_eid)] = mapping.target_filament_id;
        }
    }
    if (extruder_remap.empty()) return;

    // Compute total filaments: physical + all mixed (including newly created).
    // Use project_config filament_colour — same source as Plater callback `colors`.
    PresetBundle* pb = wxGetApp().preset_bundle;
    if (!pb) return;
    ConfigOptionStrings* co = pb->project_config.option<ConfigOptionStrings>("filament_colour");
    if (!co || co->values.empty()) return;
    const size_t num_physical    = co->values.size();
    const size_t total_filaments = pb->mixed_filaments.total_filaments(num_physical);

    // Build EnforcerBlockerStateMap: identity by default, remap where needed
    constexpr size_t MAX_EBT = static_cast<size_t>(EnforcerBlockerType::ExtruderMax);
    EnforcerBlockerStateMap state_map;
    for (size_t i = 0; i <= MAX_EBT; ++i)
        state_map[i] = static_cast<EnforcerBlockerType>(i);
    for (const auto& [eid, target] : extruder_remap) {
        if (eid <= static_cast<int>(MAX_EBT) && target <= static_cast<unsigned int>(MAX_EBT)) {
            state_map[static_cast<size_t>(eid)] = static_cast<EnforcerBlockerType>(target);
        } else if (target > static_cast<unsigned int>(MAX_EBT)) {
            BOOST_LOG_TRIVIAL(warning)
                << "apply_batch_match: cannot remap extruder " << eid
                << " -> " << target << " (target exceeds EnforcerBlockerType::ExtruderMax="
                << MAX_EBT << "), triangle-level remap skipped for this entry";
        }
    }

    // Apply remap at two levels independently:
    //   1) MMU-painted: triangle-level extruder data via remap_extruder_ids
    //      (only MODEL_PART volumes carry paint data)
    //   2) Config-level: volume/object config extruder assignment
    //      Applies to MODEL_PART AND PARAMETER_MODIFIER (modifier) volumes.
    //      A modifier keeps the colour the user picked in the object list; if
    //      that colour's extruder is remapped by the match (e.g. physical slot
    //      3 -> virtual mixed filament 7), the modifier must follow so its
    //      displayed/printed colour stays the same hue.  Previously the loop
    //      early-continued on non-MODEL_PART, dropping the modifier's colour
    //      (it then fell back to the object's extruder, which itself may have
    //      been remapped — surfacing as "colour reset to extruder 1").
    for (ModelObject* mo : wxGetApp().model().objects) {
        // Pre-read the object's effective extruder ONCE, before iterating volumes.
        // ModelVolume::extruder_id() falls back to the OBJECT config when a volume
        // has no own "extruder", and this loop rewrites that object config — so
        // calling extruder_id() again mid-loop would return the just-written value
        // for later inheriting volumes, making two volumes that share the same
        // source resolve different targets (order-dependent). Snapshotting the
        // original object extruder eliminates that hazard: every inheriting volume
        // (part or modifier) follows the same target, so their colours stay
        // consistent — which is the whole point of including modifiers here.
        const ConfigOption* obj_opt     = mo->config.option("extruder");
        const int           orig_obj_eid = (obj_opt ? obj_opt->getInt() : 0);
        auto                obj_it       = extruder_remap.find(orig_obj_eid);
        const bool          obj_remap    = (orig_obj_eid > 0 && obj_it != extruder_remap.end());
        // Remap the object-level filament independently of the volume loop.
        // Multi-part objects where EVERY volume owns its own extruder (e.g.
        // Creality 3mf exports) used to keep the parent row on its stale
        // filament: the object was only rewritten inside the "volume
        // inherits" branch below, which never runs when no volume inherits.
        // Write the object whenever its extruder is in the remap table so
        // the parent follows the match; inheriting volumes resolve through
        // the (already rewritten) object config.
        if (obj_remap)
            mo->config.set_key_value("extruder", new ConfigOptionInt(static_cast<int>(obj_it->second)));
        for (ModelVolume* mv : mo->volumes) {
            const ModelVolumeType vt = mv->type();
            const bool is_part      = (vt == ModelVolumeType::MODEL_PART);
            const bool is_modifier  = (vt == ModelVolumeType::PARAMETER_MODIFIER);

            // Level 1: triangle-level MMU data (MODEL_PART only — modifiers
            // carry no paint data).
            if (is_part && !mv->mmu_segmentation_facets.empty())
                mv->remap_extruder_ids(total_filaments, state_map);

            // Level 2: config-level extruder for parts and modifiers.
            if (!is_part && !is_modifier) continue;

            const ConfigOption* vol_opt = mv->config.option("extruder");
            if (vol_opt && vol_opt->getInt() > 0) {
                // Volume owns its extruder — remap it on the volume alone.
                auto it = extruder_remap.find(vol_opt->getInt());
                if (it != extruder_remap.end())
                    mv->config.set_key_value("extruder", new ConfigOptionInt(static_cast<int>(it->second)));
            }
            // Volumes without their own extruder inherit the object's extruder,
            // which was already remapped above — no per-volume action needed.
        }

        // Level 3: layer-object extruder (layer_config_ranges). A layer object
        // holds the user-assigned filament for a height range; remap it with
        // the same table used for volumes so it follows the match instead of
        // being stranded on a physical slot that cleanup deletes
        // (→ "layer filament >N reset to default").
        for (auto& lr : mo->layer_config_ranges) {
            ModelConfig&         lcfg = lr.second;
            const ConfigOption*  lopt = lcfg.option("extruder");
            if (!lopt) continue;
            const int old_eid = lopt->getInt();
            if (old_eid <= 0) continue;
            auto lit = extruder_remap.find(old_eid);
            if (lit == extruder_remap.end()) continue;
            lcfg.set_key_value("extruder", new ConfigOptionInt(static_cast<int>(lit->second)));
        }
    }

    BOOST_LOG_TRIVIAL(info)
        << "apply_batch_match: remapped " << extruder_remap.size()
        << " extruder IDs across all volumes (total_filaments=" << total_filaments << ")";
}

}} // namespace Slic3r::GUI
