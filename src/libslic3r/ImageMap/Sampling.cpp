#include "Sampling.hpp"

#include "../AABBMesh.hpp"
#include "../TriangleMesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Slic3r::ImageMap {

namespace {

float wrapped_coordinate(float value, WrapMode mode)
{
    if (!std::isfinite(value))
        return 0.f;
    if (mode == WrapMode::Clamp)
        return std::clamp(value, 0.f, 1.f);
    const float wrapped = value - std::floor(value);
    return wrapped < 0.f ? wrapped + 1.f : wrapped;
}

RGBA bilinear_sample(const TextureAsset& asset, const Vec2f& uv, WrapMode wrap_u, WrapMode wrap_v)
{
    if (!asset.valid())
        return RGBA{1.f, 1.f, 1.f, 1.f};
    const float  x       = wrapped_coordinate(uv.x(), wrap_u) * float(asset.width > 1 ? asset.width - 1 : 0);
    const float  y       = wrapped_coordinate(uv.y(), wrap_v) * float(asset.height > 1 ? asset.height - 1 : 0);
    const size_t x0      = std::min<size_t>(size_t(std::floor(x)), size_t(asset.width - 1));
    const size_t y0      = std::min<size_t>(size_t(std::floor(y)), size_t(asset.height - 1));
    const size_t x1      = std::min<size_t>(x0 + 1, size_t(asset.width - 1));
    const size_t y1      = std::min<size_t>(y0 + 1, size_t(asset.height - 1));
    const float  tx      = x - float(x0);
    const float  ty      = y - float(y0);
    auto         channel = [&asset](size_t px, size_t py, size_t component) {
        return float(asset.rgba[(py * size_t(asset.width) + px) * 4 + component]) / 255.f;
    };
    RGBA result{};
    for (size_t component = 0; component < 4; ++component) {
        const float top    = channel(x0, y0, component) + (channel(x1, y0, component) - channel(x0, y0, component)) * tx;
        const float bottom = channel(x0, y1, component) + (channel(x1, y1, component) - channel(x0, y1, component)) * tx;
        result[component]  = std::clamp(top + (bottom - top) * ty, 0.f, 1.f);
    }
    return result;
}

RGBA interpolate_colors(const std::array<RGBA, 3>& colors, const Vec3f& barycentric)
{
    RGBA result{};
    for (size_t component = 0; component < 4; ++component)
        result[component] = std::clamp(colors[0][component] * barycentric.x() + colors[1][component] * barycentric.y() +
                                           colors[2][component] * barycentric.z(),
                                       0.f, 1.f);
    return result;
}

struct RepresentativeColorBin
{
    std::array<double, 3> sum{0., 0., 0.};
    size_t                count{0};
};

float color_hue(const RGBA& color)
{
    const float maximum = std::max({color[0], color[1], color[2]});
    const float minimum = std::min({color[0], color[1], color[2]});
    const float chroma  = maximum - minimum;
    if (chroma <= 0.02f)
        return -1.f;
    float hue = 0.f;
    if (maximum == color[0])
        hue = std::fmod((color[1] - color[2]) / chroma, 6.f);
    else if (maximum == color[1])
        hue = (color[2] - color[0]) / chroma + 2.f;
    else
        hue = (color[0] - color[1]) / chroma + 4.f;
    if (hue < 0.f)
        hue += 6.f;
    return hue;
}

double color_distance_squared(const RGBA& lhs, const RGBA& rhs)
{
    double distance = 0.;
    for (size_t channel = 0; channel < 3; ++channel) {
        const double delta = double(lhs[channel]) - double(rhs[channel]);
        distance += delta * delta;
    }
    return distance;
}

} // namespace

RGBA sample_source(const VolumeData& data, const TriangleBinding& binding, const Vec3f& barycentric)
{
    const RGBA background = interpolate_colors(binding.source.corner_colors, barycentric);
    if (binding.source.kind != SourceKind::Texture || binding.source.texture_asset_index < 0 ||
        size_t(binding.source.texture_asset_index) >= data.texture_assets.size())
        return background;

    const Vec2f uv = binding.source.uvs[0] * barycentric.x() + binding.source.uvs[1] * barycentric.y() +
                     binding.source.uvs[2] * barycentric.z();
    const RGBA  sampled = bilinear_sample(data.texture_assets[size_t(binding.source.texture_asset_index)], uv, binding.source.wrap_u,
                                          binding.source.wrap_v);
    const float alpha   = sampled[3];
    return RGBA{sampled[0] * alpha + background[0] * (1.f - alpha), sampled[1] * alpha + background[1] * (1.f - alpha),
                sampled[2] * alpha + background[2] * (1.f - alpha), 1.f};
}

std::vector<RGBA> representative_source_colors(const std::vector<RGBA>& source_colors, size_t max_colors, size_t max_samples)
{
    if (source_colors.empty() || max_colors == 0 || max_samples == 0)
        return {};

    constexpr size_t                              levels    = 16;
    constexpr size_t                              bin_count = levels * levels * levels;
    std::array<RepresentativeColorBin, bin_count> histogram;
    const size_t                                  sample_count = std::min(source_colors.size(), max_samples);
    for (size_t sample_index = 0; sample_index < sample_count; ++sample_index) {
        const size_t source_index = sample_index * source_colors.size() / sample_count;
        const RGBA&  color        = source_colors[source_index];
        if (!std::isfinite(color[0]) || !std::isfinite(color[1]) || !std::isfinite(color[2]) || !std::isfinite(color[3]) ||
            color[3] <= 0.02f)
            continue;
        auto         quantize = [levels](float value) { return size_t(std::lround(std::clamp(value, 0.f, 1.f) * float(levels - 1))); };
        const size_t r        = quantize(color[0]);
        const size_t g        = quantize(color[1]);
        const size_t b        = quantize(color[2]);
        RepresentativeColorBin& bin = histogram[(r * levels + g) * levels + b];
        for (size_t channel = 0; channel < 3; ++channel)
            bin.sum[channel] += color[channel];
        ++bin.count;
    }

    struct Candidate
    {
        RGBA   color{0.f, 0.f, 0.f, 1.f};
        size_t count{0};
    };
    std::vector<Candidate> candidates;
    candidates.reserve(bin_count);
    for (const RepresentativeColorBin& bin : histogram) {
        if (bin.count == 0)
            continue;
        const double divisor = double(bin.count);
        candidates.push_back({RGBA{float(bin.sum[0] / divisor), float(bin.sum[1] / divisor), float(bin.sum[2] / divisor), 1.f}, bin.count});
    }
    if (candidates.empty())
        return {};

    const size_t target_count      = std::min(max_colors, candidates.size());
    const size_t maximum_bin_count = std::max_element(candidates.begin(), candidates.end(), [](const Candidate& lhs, const Candidate& rhs) {
                                         return lhs.count < rhs.count;
                                     })->count;
    std::vector<size_t> selected;
    selected.reserve(target_count);
    selected.push_back(size_t(std::distance(candidates.begin(), std::max_element(candidates.begin(), candidates.end(),
                                                                                 [](const Candidate& lhs, const Candidate& rhs) {
                                                                                     return lhs.count < rhs.count;
                                                                                 }))));

    while (selected.size() < target_count) {
        size_t best_index = candidates.size();
        double best_score = -1.;
        for (size_t candidate_index = 0; candidate_index < candidates.size(); ++candidate_index) {
            if (std::find(selected.begin(), selected.end(), candidate_index) != selected.end())
                continue;
            double nearest_distance = std::numeric_limits<double>::infinity();
            for (size_t selected_index : selected)
                nearest_distance = std::min(nearest_distance,
                                            color_distance_squared(candidates[candidate_index].color, candidates[selected_index].color));
            const double frequency = std::sqrt(double(candidates[candidate_index].count) / double(maximum_bin_count));
            const double score     = nearest_distance * (0.35 + 0.65 * frequency);
            if (score > best_score) {
                best_score = score;
                best_index = candidate_index;
            }
        }
        if (best_index >= candidates.size())
            break;
        selected.push_back(best_index);
    }

    std::vector<RGBA> result;
    result.reserve(selected.size());
    for (size_t index : selected)
        result.push_back(candidates[index].color);
    std::stable_sort(result.begin(), result.end(), [](const RGBA& lhs, const RGBA& rhs) {
        const float lhs_hue = color_hue(lhs);
        const float rhs_hue = color_hue(rhs);
        if (lhs_hue < 0.f || rhs_hue < 0.f) {
            if ((lhs_hue < 0.f) != (rhs_hue < 0.f))
                return lhs_hue < 0.f;
            const float lhs_lightness = 0.2126f * lhs[0] + 0.7152f * lhs[1] + 0.0722f * lhs[2];
            const float rhs_lightness = 0.2126f * rhs[0] + 0.7152f * rhs[1] + 0.0722f * rhs[2];
            return lhs_lightness < rhs_lightness;
        }
        return lhs_hue < rhs_hue;
    });
    return result;
}

std::vector<RGBA> representative_source_colors(const VolumeData& data, RenderMode render_mode, size_t max_colors, size_t max_samples)
{
    if (max_colors == 0 || max_samples == 0)
        return {};
    const bool has_requested_zone = std::any_of(data.zones.begin(), data.zones.end(), [render_mode](const Zone& zone) {
        return zone.enabled && zone.render_mode == render_mode;
    });
    if (!has_requested_zone)
        return {};

    std::vector<RGBA> samples;
    samples.reserve(max_samples * 2);
    const size_t valid_texture_count = size_t(
        std::count_if(data.texture_assets.begin(), data.texture_assets.end(), [](const TextureAsset& asset) { return asset.valid(); }));
    if (valid_texture_count > 0) {
        const size_t per_texture_budget = std::max<size_t>(1, max_samples / valid_texture_count);
        for (const TextureAsset& asset : data.texture_assets) {
            if (!asset.valid())
                continue;
            const size_t pixel_count  = size_t(asset.width) * size_t(asset.height);
            const size_t sample_count = std::min(pixel_count, per_texture_budget);
            for (size_t sample_index = 0; sample_index < sample_count; ++sample_index) {
                const size_t pixel_index = sample_index * pixel_count / sample_count;
                const size_t offset      = pixel_index * 4;
                samples.push_back({float(asset.rgba[offset]) / 255.f, float(asset.rgba[offset + 1]) / 255.f,
                                   float(asset.rgba[offset + 2]) / 255.f, float(asset.rgba[offset + 3]) / 255.f});
            }
        }
    }

    const size_t binding_sample_count = std::min(data.triangle_bindings.size(), max_samples / 3 + 1);
    for (size_t sample_index = 0; sample_index < binding_sample_count; ++sample_index) {
        const TriangleBinding& binding = data.triangle_bindings[sample_index * data.triangle_bindings.size() / binding_sample_count];
        if (binding.zone_index >= data.zones.size())
            continue;
        const Zone& zone = data.zones[binding.zone_index];
        if (!zone.enabled || zone.render_mode != render_mode || binding.source.kind == SourceKind::Texture)
            continue;
        samples.insert(samples.end(), binding.source.corner_colors.begin(), binding.source.corner_colors.end());
    }
    return representative_source_colors(samples, max_colors, max_samples);
}

std::vector<std::vector<RGBA>> representative_labeled_source_colors(const std::vector<RGBA>& source_colors,
                                                                     const std::vector<int>&  labels,
                                                                     size_t                   label_count,
                                                                     size_t                   max_colors,
                                                                     size_t                   max_samples)
{
    if (label_count == 0 || max_colors == 0 || max_samples == 0)
        return {};

    const size_t item_count = std::min(source_colors.size(), labels.size());
    std::vector<size_t> label_sizes(label_count, 0);
    for (size_t item_index = 0; item_index < item_count; ++item_index) {
        const int label = labels[item_index];
        if (label >= 0 && size_t(label) < label_count)
            ++label_sizes[size_t(label)];
    }

    std::vector<std::vector<RGBA>> samples(label_count);
    std::vector<size_t>            ordinals(label_count, 0);
    std::vector<size_t>            selected(label_count, 0);
    for (size_t label_index = 0; label_index < label_count; ++label_index)
        samples[label_index].reserve(std::min(label_sizes[label_index], max_samples));

    for (size_t item_index = 0; item_index < item_count; ++item_index) {
        const int label = labels[item_index];
        if (label < 0 || size_t(label) >= label_count)
            continue;

        const size_t label_index  = size_t(label);
        const size_t sample_count = std::min(label_sizes[label_index], max_samples);
        if (selected[label_index] < sample_count &&
            ordinals[label_index] == selected[label_index] * label_sizes[label_index] / sample_count) {
            samples[label_index].push_back(source_colors[item_index]);
            ++selected[label_index];
        }
        ++ordinals[label_index];
    }

    std::vector<std::vector<RGBA>> representative(label_count);
    for (size_t label_index = 0; label_index < label_count; ++label_index)
        representative[label_index] = representative_source_colors(samples[label_index], max_colors, max_samples);
    return representative;
}

std::vector<std::vector<RGBA>> representative_palette_source_colors(const VolumeData& data,
                                                                    size_t            zone_index,
                                                                    size_t            max_colors,
                                                                    size_t            max_samples)
{
    if (zone_index >= data.zones.size() || max_colors == 0 || max_samples == 0)
        return {};

    const Zone& zone = data.zones[zone_index];
    if (!zone.enabled || zone.palette.empty())
        return {};

    std::vector<std::vector<RGBA>> samples(zone.palette.size());
    for (size_t palette_index = 0; palette_index < zone.palette.size(); ++palette_index)
        samples[palette_index].push_back(zone.palette[palette_index].target_color);

    auto append_sample = [&zone, &samples](const RGBA& color) {
        const PaletteEntry* entry = nearest_palette_entry(zone, color);
        if (entry == nullptr)
            return;
        const size_t palette_index = size_t(entry - zone.palette.data());
        if (palette_index < samples.size())
            samples[palette_index].push_back(color);
    };

    std::vector<bool> referenced_textures(data.texture_assets.size(), false);
    // This function is called while rebuilding sidebar cards. Inspect a
    // deterministic, uniformly distributed subset instead of scanning every
    // binding twice; large image-mapped OBJs routinely contain 800k+ entries.
    const size_t binding_probe_count = std::min(
        data.triangle_bindings.size(), std::max<size_t>(1024, max_samples * 4));
    for (size_t probe_index = 0; probe_index < binding_probe_count; ++probe_index) {
        const uint64_t numerator = (uint64_t(2 * probe_index) + 1u) * uint64_t(data.triangle_bindings.size());
        const size_t binding_index = std::min(data.triangle_bindings.size() - 1,
                                              size_t(numerator / uint64_t(2 * binding_probe_count)));
        const TriangleBinding& binding = data.triangle_bindings[binding_index];
        if (binding.zone_index != zone_index)
            continue;
        if (binding.source.kind == SourceKind::Texture) {
            if (binding.source.texture_asset_index >= 0 && size_t(binding.source.texture_asset_index) < data.texture_assets.size())
                referenced_textures[size_t(binding.source.texture_asset_index)] = true;
        } else {
            for (const RGBA& color : binding.source.corner_colors)
                append_sample(color);
        }
    }

    const size_t referenced_texture_count = size_t(std::count(referenced_textures.begin(), referenced_textures.end(), true));
    if (referenced_texture_count > 0) {
        const size_t per_texture_budget = std::max<size_t>(1, max_samples / referenced_texture_count);
        for (size_t texture_index = 0; texture_index < data.texture_assets.size(); ++texture_index) {
            if (!referenced_textures[texture_index])
                continue;
            const TextureAsset& asset = data.texture_assets[texture_index];
            if (!asset.valid())
                continue;
            const size_t pixel_count  = size_t(asset.width) * size_t(asset.height);
            const size_t sample_count = std::min(pixel_count, per_texture_budget);
            for (size_t sample_index = 0; sample_index < sample_count; ++sample_index) {
                const size_t pixel_index = sample_index * pixel_count / sample_count;
                const size_t offset      = pixel_index * 4;
                append_sample({float(asset.rgba[offset]) / 255.f, float(asset.rgba[offset + 1]) / 255.f,
                               float(asset.rgba[offset + 2]) / 255.f, float(asset.rgba[offset + 3]) / 255.f});
            }
        }
    }

    std::vector<std::vector<RGBA>> representative(zone.palette.size());
    for (size_t palette_index = 0; palette_index < samples.size(); ++palette_index)
        representative[palette_index] = representative_source_colors(samples[palette_index], max_colors, max_samples);
    return representative;
}

const PaletteEntry* nearest_palette_entry(const Zone& zone, const RGBA& color)
{
    const PaletteEntry* best          = nullptr;
    double              best_distance = std::numeric_limits<double>::infinity();
    for (const PaletteEntry& entry : zone.palette) {
        double distance = 0.0;
        for (size_t component = 0; component < 3; ++component) {
            const double delta = double(color[component]) - double(entry.target_color[component]);
            distance += delta * delta;
        }
        if (distance < best_distance) {
            best          = &entry;
            best_distance = distance;
        }
    }
    return best;
}

Vec3f barycentric_coordinates(const Vec3d& point, const Vec3d& a, const Vec3d& b, const Vec3d& c)
{
    const Vec3d  v0          = b - a;
    const Vec3d  v1          = c - a;
    const Vec3d  v2          = point - a;
    const double d00         = v0.dot(v0);
    const double d01         = v0.dot(v1);
    const double d11         = v1.dot(v1);
    const double d20         = v2.dot(v0);
    const double d21         = v2.dot(v1);
    const double denominator = d00 * d11 - d01 * d01;
    if (!std::isfinite(denominator) || std::abs(denominator) <= std::numeric_limits<double>::epsilon())
        return Vec3f(1.f / 3.f, 1.f / 3.f, 1.f / 3.f);
    const double v = (d11 * d20 - d01 * d21) / denominator;
    const double w = (d00 * d21 - d01 * d20) / denominator;
    const double u = 1.0 - v - w;
    Vec3f        result{float(u), float(v), float(w)};
    result          = result.cwiseMax(0.f);
    const float sum = result.sum();
    return sum > EPSILON ? result / sum : Vec3f(1.f / 3.f, 1.f / 3.f, 1.f / 3.f);
}

SurfaceSampler::SurfaceSampler(std::shared_ptr<const TriangleMesh> mesh, std::shared_ptr<const VolumeData> data)
    : m_mesh(std::move(mesh)), m_data(std::move(data))
{
    if (!m_mesh || !m_data || !m_data->validate(*m_mesh).valid)
        return;
    m_aabb = std::make_unique<AABBMesh>(*m_mesh);
    m_bindings_by_triangle.resize(m_mesh->its.indices.size());
    for (size_t binding_idx = 0; binding_idx < m_data->triangle_bindings.size(); ++binding_idx) {
        const TriangleBinding& binding = m_data->triangle_bindings[binding_idx];
        if (binding.triangle_index < m_bindings_by_triangle.size())
            m_bindings_by_triangle[binding.triangle_index].push_back(binding_idx);
    }
    for (auto& indices : m_bindings_by_triangle) {
        std::stable_sort(indices.begin(), indices.end(), [this](size_t lhs, size_t rhs) {
            return m_data->zones[m_data->triangle_bindings[lhs].zone_index].priority >
                   m_data->zones[m_data->triangle_bindings[rhs].zone_index].priority;
        });
    }
}

SurfaceSampler::~SurfaceSampler()                                    = default;
SurfaceSampler::SurfaceSampler(SurfaceSampler&&) noexcept            = default;
SurfaceSampler& SurfaceSampler::operator=(SurfaceSampler&&) noexcept = default;

std::optional<SurfaceSample> SurfaceSampler::sample(const Vec3d&              local_point,
                                                    double                    max_distance_mm,
                                                    std::optional<RenderMode> render_mode) const
{
    if (!m_aabb || !std::isfinite(max_distance_mm) || max_distance_mm < 0.0)
        return std::nullopt;
    int          face = -1;
    Vec3d        closest;
    const double squared_distance = m_aabb->squared_distance(local_point, face, closest);
    if (face < 0 || size_t(face) >= m_bindings_by_triangle.size() || !std::isfinite(squared_distance) ||
        squared_distance > max_distance_mm * max_distance_mm)
        return std::nullopt;

    const TriangleBinding* selected = nullptr;
    const Zone*            zone     = nullptr;
    for (size_t binding_idx : m_bindings_by_triangle[size_t(face)]) {
        const TriangleBinding& candidate      = m_data->triangle_bindings[binding_idx];
        const Zone&            candidate_zone = m_data->zones[candidate.zone_index];
        if (candidate_zone.enabled && (!render_mode || candidate_zone.render_mode == *render_mode)) {
            selected = &candidate;
            zone     = &candidate_zone;
            break;
        }
    }
    if (!selected || !zone)
        return std::nullopt;

    const stl_triangle_vertex_indices& indices = m_mesh->its.indices[size_t(face)];
    const Vec3f   barycentric                  = barycentric_coordinates(closest, m_mesh->its.vertices[size_t(indices[0])].cast<double>(),
                                                                         m_mesh->its.vertices[size_t(indices[1])].cast<double>(),
                                                                         m_mesh->its.vertices[size_t(indices[2])].cast<double>());
    SurfaceSample result;
    result.color               = sample_source(*m_data, *selected, barycentric);
    result.closest_local_point = closest;
    result.squared_distance    = squared_distance;
    result.triangle_index      = uint32_t(face);
    result.zone                = zone;
    result.palette_entry       = nearest_palette_entry(*zone, result.color);
    result.binding             = selected;
    return result.palette_entry ? std::optional<SurfaceSample>(result) : std::nullopt;
}

} // namespace Slic3r::ImageMap
