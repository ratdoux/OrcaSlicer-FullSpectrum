#include "MixedFilament.hpp"

#include <algorithm>
#include <boost/log/trivial.hpp>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
                                 int               &mix_b_percent,
                                 bool              &pointillism_all_filaments,
                                 std::string       &gradient_component_ids,
                                 std::string       &gradient_component_weights,
                                 std::string       &manual_pattern,
                                 int               &distribution_mode)
{
    auto trim_copy = [](const std::string &s) {
        size_t lo = 0;
        size_t hi = s.size();
        while (lo < hi && std::isspace(static_cast<unsigned char>(s[lo])))
            ++lo;
        while (hi > lo && std::isspace(static_cast<unsigned char>(s[hi - 1])))
            --hi;
        return s.substr(lo, hi - lo);
    };

    auto parse_int_token = [&trim_copy](const std::string &tok, int &out) {
        const std::string t = trim_copy(tok);
        if (t.empty())
            return false;
        try {
            size_t consumed = 0;
            int v = std::stoi(t, &consumed);
            if (consumed != t.size())
                return false;
            out = v;
            return true;
        } catch (...) {
            return false;
        }
    };

    std::vector<std::string> tokens;
    std::stringstream ss(row);
    std::string token;
    while (std::getline(ss, token, ','))
        tokens.emplace_back(trim_copy(token));

    if (tokens.size() < 4 || tokens.size() > 12)
        return false;

    int values[5] = { 0, 0, 1, 1, 50 };
    if (tokens.size() == 4) {
        // Legacy: a,b,enabled,mix
        if (!parse_int_token(tokens[0], values[0]) ||
            !parse_int_token(tokens[1], values[1]) ||
            !parse_int_token(tokens[2], values[2]) ||
            !parse_int_token(tokens[3], values[4]))
            return false;
    } else {
        // Current: a,b,enabled,custom,mix[,pointillism_all[,pattern]]
        for (size_t i = 0; i < 5; ++i)
            if (!parse_int_token(tokens[i], values[i]))
                return false;
    }

    if (values[0] <= 0 || values[1] <= 0)
        return false;

    a = unsigned(values[0]);
    b = unsigned(values[1]);
    enabled = (values[2] != 0);
    custom = (tokens.size() == 4) ? true : (values[3] != 0);
    mix_b_percent = clamp_int(values[4], 0, 100);
    pointillism_all_filaments = false;
    gradient_component_ids.clear();
    gradient_component_weights.clear();
    manual_pattern.clear();
    distribution_mode = int(MixedFilament::Simple);

    size_t token_idx = 5;
    if (tokens.size() >= 6) {
        // Backward compatibility:
        // - old: token[5] is pointillism flag ("0"/"1")
        // - old: token[5] is pattern ("12", "1212", ...)
        // - new: token[5] may be metadata token ("g..." / "m...")
        const std::string &legacy = tokens[5];
        if (legacy == "0" || legacy == "1") {
            pointillism_all_filaments = (legacy == "1");
            token_idx = 6;
        } else if (legacy.empty() || legacy[0] == 'g' || legacy[0] == 'G' || legacy[0] == 'm' || legacy[0] == 'M') {
            token_idx = 5;
        } else {
            manual_pattern = legacy;
            token_idx = 6;
        }
    }

    for (size_t i = token_idx; i < tokens.size(); ++i) {
        const std::string &tok = tokens[i];
        if (tok.empty())
            continue;
        if (tok[0] == 'g' || tok[0] == 'G') {
            gradient_component_ids = tok.substr(1);
            continue;
        }
        if (tok[0] == 'w' || tok[0] == 'W') {
            gradient_component_weights = tok.substr(1);
            continue;
        }
        if (tok[0] == 'm' || tok[0] == 'M') {
            int parsed_mode = distribution_mode;
            if (parse_int_token(tok.substr(1), parsed_mode))
                distribution_mode = clamp_int(parsed_mode, int(MixedFilament::LayerCycle), int(MixedFilament::Simple));
            continue;
        }
        manual_pattern = tok;
    }

    // Compatibility for early same-layer prototype rows.
    if (distribution_mode == int(MixedFilament::LayerCycle) && pointillism_all_filaments)
        distribution_mode = int(MixedFilament::SameLayerPointillisme);
    return true;
}

static bool is_pattern_separator(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) || c == '/' || c == '-' || c == '_' || c == '|' || c == ':' || c == ';' || c == ',';
}

static bool decode_pattern_step(char c, char &out)
{
    if (c >= '1' && c <= '9') {
        out = c;
        return true;
    }
    switch (std::tolower(static_cast<unsigned char>(c))) {
    case 'a':
        out = '1';
        return true;
    case 'b':
        out = '2';
        return true;
    default:
        return false;
    }
}

static int mix_percent_from_normalized_pattern(const std::string &pattern)
{
    if (pattern.empty())
        return 50;
    // Legacy blend ratio for UI preview: count component-B aliases only.
    // Tokens '3'..'9' are direct physical filament IDs and are ignored here.
    const int count_b = int(std::count(pattern.begin(), pattern.end(), '2'));
    return clamp_int(int(std::lround(100.0 * double(count_b) / double(pattern.size()))), 0, 100);
}

static std::string normalize_gradient_component_ids(const std::string &components)
{
    std::string normalized;
    normalized.reserve(components.size());
    bool seen[10] = { false };
    for (const char c : components) {
        if (c < '1' || c > '9')
            continue;
        const int idx = c - '0';
        if (seen[idx])
            continue;
        seen[idx] = true;
        normalized.push_back(c);
    }
    return normalized;
}

static std::vector<unsigned int> decode_gradient_component_ids(const std::string &components, size_t num_physical)
{
    std::vector<unsigned int> ids;
    if (components.empty() || num_physical == 0)
        return ids;

    bool seen[10] = { false };
    ids.reserve(components.size());
    for (const char c : components) {
        if (c < '1' || c > '9')
            continue;
        const unsigned int id = unsigned(c - '0');
        if (id == 0 || id > num_physical || seen[id])
            continue;
        seen[id] = true;
        ids.emplace_back(id);
    }
    return ids;
}

static std::vector<int> parse_gradient_weight_tokens(const std::string &weights)
{
    std::vector<int> out;
    std::string token;
    for (const char c : weights) {
        if (c >= '0' && c <= '9') {
            token.push_back(c);
            continue;
        }
        if (!token.empty()) {
            out.emplace_back(std::max(0, std::atoi(token.c_str())));
            token.clear();
        }
    }
    if (!token.empty())
        out.emplace_back(std::max(0, std::atoi(token.c_str())));
    return out;
}

static std::vector<int> normalize_weight_vector_to_percent(const std::vector<int> &weights)
{
    std::vector<int> out(weights.size(), 0);
    if (weights.empty())
        return out;
    int sum = 0;
    for (const int w : weights)
        sum += std::max(0, w);
    if (sum <= 0)
        return out;

    std::vector<double> remainders(weights.size(), 0.);
    int assigned = 0;
    for (size_t i = 0; i < weights.size(); ++i) {
        const double exact = 100.0 * double(std::max(0, weights[i])) / double(sum);
        out[i] = int(std::floor(exact));
        remainders[i] = exact - double(out[i]);
        assigned += out[i];
    }
    int missing = std::max(0, 100 - assigned);
    while (missing > 0) {
        size_t best_idx = 0;
        double best_rem = -1.0;
        for (size_t i = 0; i < remainders.size(); ++i) {
            if (weights[i] <= 0)
                continue;
            if (remainders[i] > best_rem) {
                best_rem = remainders[i];
                best_idx = i;
            }
        }
        ++out[best_idx];
        remainders[best_idx] = 0.0;
        --missing;
    }
    return out;
}

static std::string normalize_gradient_component_weights(const std::string &weights, size_t expected_components)
{
    if (expected_components == 0)
        return std::string();
    std::vector<int> parsed = parse_gradient_weight_tokens(weights);
    if (parsed.size() != expected_components)
        return std::string();
    std::vector<int> normalized = normalize_weight_vector_to_percent(parsed);
    int sum = 0;
    for (const int v : normalized)
        sum += v;
    if (sum <= 0)
        return std::string();

    std::ostringstream ss;
    for (size_t i = 0; i < normalized.size(); ++i) {
        if (i > 0)
            ss << '/';
        ss << normalized[i];
    }
    return ss.str();
}

static std::vector<int> decode_gradient_component_weights(const std::string &weights, size_t expected_components)
{
    if (expected_components == 0)
        return {};
    std::vector<int> parsed = parse_gradient_weight_tokens(weights);
    if (parsed.size() != expected_components)
        return {};
    std::vector<int> normalized = normalize_weight_vector_to_percent(parsed);
    int sum = 0;
    for (const int v : normalized)
        sum += v;
    return (sum > 0) ? normalized : std::vector<int>();
}

static std::vector<unsigned int> build_weighted_gradient_sequence(const std::vector<unsigned int> &ids,
                                                                  const std::vector<int>          &weights)
{
    if (ids.empty())
        return {};

    std::vector<unsigned int> filtered_ids;
    std::vector<int>          counts;
    filtered_ids.reserve(ids.size());
    counts.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        const int w = (i < weights.size()) ? std::max(0, weights[i]) : 0;
        if (w <= 0)
            continue;
        filtered_ids.emplace_back(ids[i]);
        counts.emplace_back(w);
    }
    if (filtered_ids.empty()) {
        filtered_ids = ids;
        counts.assign(ids.size(), 1);
    }

    int g = 0;
    for (const int c : counts)
        g = std::gcd(g, std::max(1, c));
    if (g > 1) {
        for (int &c : counts)
            c = std::max(1, c / g);
    }

    int cycle = std::accumulate(counts.begin(), counts.end(), 0);
    constexpr int k_max_cycle = 48;
    if (cycle > k_max_cycle) {
        const double scale = double(k_max_cycle) / double(cycle);
        for (int &c : counts)
            c = std::max(1, int(std::round(double(c) * scale)));
        cycle = std::accumulate(counts.begin(), counts.end(), 0);
        while (cycle > k_max_cycle) {
            auto it = std::max_element(counts.begin(), counts.end());
            if (it == counts.end() || *it <= 1)
                break;
            --(*it);
            --cycle;
        }
    }
    if (cycle <= 0)
        return {};

    std::vector<unsigned int> sequence;
    sequence.reserve(size_t(cycle));
    std::vector<int> emitted(counts.size(), 0);
    for (int pos = 0; pos < cycle; ++pos) {
        size_t best_idx = 0;
        double best_score = -1e9;
        for (size_t i = 0; i < counts.size(); ++i) {
            const double target = double((pos + 1) * counts[i]) / double(cycle);
            const double score = target - double(emitted[i]);
            if (score > best_score) {
                best_score = score;
                best_idx = i;
            }
        }
        ++emitted[best_idx];
        sequence.emplace_back(filtered_ids[best_idx]);
    }
    return sequence;
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
    mf.manual_pattern.clear();
    mf.gradient_component_ids.clear();
    mf.gradient_component_weights.clear();
    mf.pointillism_all_filaments = false;
    mf.distribution_mode = int(MixedFilament::Simple);
    mf.enabled = true;
    mf.custom = true;
    m_mixed.push_back(std::move(mf));
    refresh_display_colors(filament_colours);
}

void MixedFilamentManager::clear_custom_entries()
{
    m_mixed.erase(std::remove_if(m_mixed.begin(), m_mixed.end(), [](const MixedFilament &mf) { return mf.custom; }), m_mixed.end());
}

std::string MixedFilamentManager::normalize_manual_pattern(const std::string &pattern)
{
    std::string normalized;
    normalized.reserve(pattern.size());
    for (char c : pattern) {
        char step = '\0';
        if (decode_pattern_step(c, step)) {
            normalized.push_back(step);
            continue;
        }
        if (is_pattern_separator(c))
            continue;
        // Unknown token => invalid pattern.
        return std::string();
    }
    return normalized;
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
        const std::string normalized_ids = normalize_gradient_component_ids(mf.gradient_component_ids);
        const std::string normalized_weights = normalize_gradient_component_weights(mf.gradient_component_weights, normalized_ids.size());
        ss << mf.component_a << ','
           << mf.component_b << ','
           << (mf.enabled ? 1 : 0) << ','
           << (mf.custom ? 1 : 0) << ','
           << clamp_int(mf.mix_b_percent, 0, 100) << ','
           << (mf.pointillism_all_filaments ? 1 : 0) << ','
           << 'g' << normalized_ids << ','
           << 'w' << normalized_weights << ','
           << 'm' << clamp_int(mf.distribution_mode, int(MixedFilament::LayerCycle), int(MixedFilament::Simple));
        const std::string normalized_pattern = normalize_manual_pattern(mf.manual_pattern);
        if (!normalized_pattern.empty())
            ss << ',' << normalized_pattern;
    }
    return ss.str();
}

void MixedFilamentManager::load_custom_entries(const std::string &serialized, const std::vector<std::string> &filament_colours)
{
    const size_t n = filament_colours.size();
    if (serialized.empty() || n < 2) {
        BOOST_LOG_TRIVIAL(debug) << "MixedFilamentManager::load_custom_entries skipped"
                                 << ", serialized_empty=" << (serialized.empty() ? 1 : 0)
                                 << ", physical_count=" << n;
        return;
    }

    size_t parsed_rows   = 0;
    size_t loaded_rows   = 0;
    size_t updated_auto  = 0;
    size_t skipped_rows  = 0;

    std::stringstream all(serialized);
    std::string row;
    while (std::getline(all, row, ';')) {
        if (row.empty())
            continue;
        ++parsed_rows;
        unsigned int a = 0;
        unsigned int b = 0;
        bool enabled = true;
        bool custom = true;
        int mix = 50;
        bool pointillism_all_filaments = false;
        std::string gradient_component_ids;
        std::string gradient_component_weights;
        std::string manual_pattern;
        int distribution_mode = int(MixedFilament::Simple);
        if (!parse_row_definition(row, a, b, enabled, custom, mix, pointillism_all_filaments,
                                  gradient_component_ids, gradient_component_weights, manual_pattern, distribution_mode)) {
            ++skipped_rows;
            BOOST_LOG_TRIVIAL(warning) << "MixedFilamentManager::load_custom_entries invalid row format: " << row;
            continue;
        }
        if (a == 0 || b == 0 || a > n || b > n || a == b) {
            ++skipped_rows;
            BOOST_LOG_TRIVIAL(warning) << "MixedFilamentManager::load_custom_entries row rejected"
                                       << ", row=" << row
                                       << ", a=" << a
                                       << ", b=" << b
                                       << ", physical_count=" << n;
            continue;
        }

        if (!custom) {
            auto it_auto = std::find_if(m_mixed.begin(), m_mixed.end(), [a, b](const MixedFilament &mf) {
                return !mf.custom && mf.component_a == a && mf.component_b == b;
            });
            if (it_auto != m_mixed.end()) {
                it_auto->enabled = enabled;
                it_auto->pointillism_all_filaments = pointillism_all_filaments;
                it_auto->gradient_component_ids = normalize_gradient_component_ids(gradient_component_ids);
                it_auto->gradient_component_weights =
                    normalize_gradient_component_weights(gradient_component_weights, it_auto->gradient_component_ids.size());
                it_auto->manual_pattern = normalize_manual_pattern(manual_pattern);
                it_auto->distribution_mode = clamp_int(distribution_mode, int(MixedFilament::LayerCycle), int(MixedFilament::Simple));
                it_auto->mix_b_percent = it_auto->manual_pattern.empty() ? mix : mix_percent_from_normalized_pattern(it_auto->manual_pattern);
                ++updated_auto;
                continue;
            }
        }

        MixedFilament mf;
        mf.component_a = a;
        mf.component_b = b;
        mf.mix_b_percent = mix;
        mf.ratio_a = 1;
        mf.ratio_b = 1;
        mf.pointillism_all_filaments = pointillism_all_filaments;
        mf.gradient_component_ids = normalize_gradient_component_ids(gradient_component_ids);
        mf.gradient_component_weights =
            normalize_gradient_component_weights(gradient_component_weights, mf.gradient_component_ids.size());
        mf.manual_pattern = normalize_manual_pattern(manual_pattern);
        mf.distribution_mode = clamp_int(distribution_mode, int(MixedFilament::LayerCycle), int(MixedFilament::Simple));
        if (!mf.manual_pattern.empty())
            mf.mix_b_percent = mix_percent_from_normalized_pattern(mf.manual_pattern);
        mf.enabled = enabled;
        mf.custom = custom;
        m_mixed.push_back(std::move(mf));
        ++loaded_rows;
    }
    refresh_display_colors(filament_colours);
    BOOST_LOG_TRIVIAL(info) << "MixedFilamentManager::load_custom_entries"
                            << ", physical_count=" << n
                            << ", parsed_rows=" << parsed_rows
                            << ", loaded_rows=" << loaded_rows
                            << ", updated_auto_rows=" << updated_auto
                            << ", skipped_rows=" << skipped_rows
                            << ", mixed_total=" << m_mixed.size();
}

unsigned int MixedFilamentManager::resolve(unsigned int filament_id,
                                           size_t       num_physical,
                                           int          layer_index,
                                           float        layer_print_z,
                                           float        layer_height,
                                           bool         force_height_weighted) const
{
    const int mixed_idx = mixed_index_from_filament_id(filament_id, num_physical);
    if (mixed_idx < 0)
        return filament_id;

    const MixedFilament &mf = m_mixed[size_t(mixed_idx)];

    // Manual pattern takes precedence when provided. Pattern uses repeating
    // steps: '1' => component_a, '2' => component_b, '3'..'9' => direct
    // physical filament IDs.
    if (!mf.manual_pattern.empty()) {
        const int pos = safe_mod(layer_index, int(mf.manual_pattern.size()));
        const char token = mf.manual_pattern[size_t(pos)];
        if (token == '2')
            return mf.component_b;
        if (token == '1')
            return mf.component_a;
        if (token >= '3' && token <= '9') {
            const unsigned int direct = unsigned(token - '0');
            if (direct >= 1 && direct <= num_physical)
                return direct;
        }
        return mf.component_a;
    }

    const bool use_simple_mode = mf.distribution_mode == int(MixedFilament::Simple);
    const std::vector<unsigned int> gradient_ids = decode_gradient_component_ids(mf.gradient_component_ids, num_physical);
    if (!use_simple_mode && gradient_ids.size() >= 3) {
        const std::vector<int> gradient_weights =
            decode_gradient_component_weights(mf.gradient_component_weights, gradient_ids.size());
        const std::vector<unsigned int> gradient_sequence = build_weighted_gradient_sequence(
            gradient_ids, gradient_weights.empty() ? std::vector<int>(gradient_ids.size(), 1) : gradient_weights);
        if (!gradient_sequence.empty()) {
            const size_t pos = size_t(safe_mod(layer_index, int(gradient_sequence.size())));
            return gradient_sequence[pos];
        }
    }

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

int MixedFilamentManager::mixed_index_from_filament_id(unsigned int filament_id, size_t num_physical) const
{
    if (filament_id <= num_physical)
        return -1;

    const size_t enabled_virtual_idx = size_t(filament_id - num_physical - 1);
    size_t enabled_seen = 0;
    for (size_t i = 0; i < m_mixed.size(); ++i) {
        if (!m_mixed[i].enabled)
            continue;
        if (enabled_seen == enabled_virtual_idx)
            return int(i);
        ++enabled_seen;
    }
    return -1;
}

const MixedFilament *MixedFilamentManager::mixed_filament_from_id(unsigned int filament_id, size_t num_physical) const
{
    const int idx = mixed_index_from_filament_id(filament_id, num_physical);
    return idx >= 0 ? &m_mixed[size_t(idx)] : nullptr;
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
        const std::vector<unsigned int> gradient_ids = decode_gradient_component_ids(mf.gradient_component_ids, filament_colours.size());
        if (mf.distribution_mode != int(MixedFilament::Simple) && gradient_ids.size() >= 3) {
            const std::vector<int> gradient_weights =
                decode_gradient_component_weights(mf.gradient_component_weights, gradient_ids.size());
            const std::vector<unsigned int> gradient_sequence =
                build_weighted_gradient_sequence(gradient_ids,
                    gradient_weights.empty() ? std::vector<int>(gradient_ids.size(), 1) : gradient_weights);
            if (gradient_sequence.empty()) {
                mf.display_color = "#26A69A";
                continue;
            }

            std::vector<int> counts(gradient_ids.size(), 0);
            for (const unsigned int id : gradient_sequence) {
                auto it = std::find(gradient_ids.begin(), gradient_ids.end(), id);
                if (it != gradient_ids.end())
                    ++counts[size_t(it - gradient_ids.begin())];
            }

            std::string blended = filament_colours[gradient_ids.front() - 1];
            int accum = std::max(1, counts.front());
            for (size_t i = 1; i < gradient_ids.size(); ++i) {
                const int wi = std::max(0, counts[i]);
                if (wi == 0)
                    continue;
                blended = blend_color(blended, filament_colours[gradient_ids[i] - 1], accum, wi);
                accum += wi;
            }
            mf.display_color = blended;
            continue;
        }
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

