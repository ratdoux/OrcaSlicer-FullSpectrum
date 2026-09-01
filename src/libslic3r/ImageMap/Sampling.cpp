#include "Sampling.hpp"

#include "../AABBMesh.hpp"
#include "../TriangleMesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace Slic3r::ImageMap {

namespace {

float wrapped_coordinate(float value, WrapMode mode)
{
    if (!std::isfinite(value))
        return 0.f;
    if (mode == WrapMode::Clamp || mode == WrapMode::Transparent)
        return std::clamp(value, 0.f, 1.f);
    const float wrapped = value - std::floor(value);
    return wrapped < 0.f ? wrapped + 1.f : wrapped;
}

RGBA bilinear_sample(const TextureAsset& asset, const Vec2f& uv, WrapMode wrap_u, WrapMode wrap_v)
{
    if (!asset.valid())
        return RGBA{1.f, 1.f, 1.f, 1.f};
    if ((wrap_u == WrapMode::Transparent && (uv.x() < 0.f || uv.x() > 1.f)) ||
        (wrap_v == WrapMode::Transparent && (uv.y() < 0.f || uv.y() > 1.f)))
        return RGBA{0.f, 0.f, 0.f, 0.f};
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
    const Zone* zone = binding.zone_index < data.zones.size() ? &data.zones[binding.zone_index] : nullptr;
    const RGBA sampled = zone != nullptr ?
                             sample_processed_texture(data.texture_assets[size_t(binding.source.texture_asset_index)], uv,
                                                      binding.source.wrap_u, binding.source.wrap_v, *zone) :
                             bilinear_sample(data.texture_assets[size_t(binding.source.texture_asset_index)], uv,
                                             binding.source.wrap_u, binding.source.wrap_v);
    const float alpha   = sampled[3];
    return RGBA{sampled[0] * alpha + background[0] * (1.f - alpha), sampled[1] * alpha + background[1] * (1.f - alpha),
                sampled[2] * alpha + background[2] * (1.f - alpha), 1.f};
}

float sample_source_opacity(const VolumeData& data, const TriangleBinding& binding, const Vec3f& barycentric)
{
    if (binding.source.kind != SourceKind::Texture || binding.source.texture_asset_index < 0 ||
        size_t(binding.source.texture_asset_index) >= data.texture_assets.size())
        return 1.f;

    const Vec2f uv = binding.source.uvs[0] * barycentric.x() + binding.source.uvs[1] * barycentric.y() +
                     binding.source.uvs[2] * barycentric.z();
    return bilinear_sample(data.texture_assets[size_t(binding.source.texture_asset_index)], uv, binding.source.wrap_u,
                           binding.source.wrap_v)[3];
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

std::vector<std::vector<RGBA>> representative_labeled_source_colors(
    const std::vector<RGBA>& source_colors, const std::vector<int>& labels, size_t label_count, size_t max_colors, size_t max_samples)
{
    if (label_count == 0 || max_colors == 0 || max_samples == 0)
        return {};

    const size_t        item_count = std::min(source_colors.size(), labels.size());
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
    const size_t binding_probe_count = std::min(data.triangle_bindings.size(), std::max<size_t>(1024, max_samples * 4));
    for (size_t probe_index = 0; probe_index < binding_probe_count; ++probe_index) {
        const uint64_t numerator       = (uint64_t(2 * probe_index) + 1u) * uint64_t(data.triangle_bindings.size());
        const size_t   binding_index   = std::min(data.triangle_bindings.size() - 1, size_t(numerator / uint64_t(2 * binding_probe_count)));
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
                const size_t pixel_x     = pixel_index % size_t(asset.width);
                const size_t pixel_y     = pixel_index / size_t(asset.width);
                const Vec2f  uv{asset.width > 1 ? float(pixel_x) / float(asset.width - 1) : 0.f,
                               asset.height > 1 ? float(pixel_y) / float(asset.height - 1) : 0.f};
                append_sample(sample_processed_texture(asset, uv, WrapMode::Clamp, WrapMode::Clamp, zone));
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

struct LayerPlaneSampler::Impl
{
    struct Segment
    {
        Vec2d                  a{Vec2d::Zero()};
        Vec2d                  b{Vec2d::Zero()};
        Vec3f                  barycentric_a{Vec3f::Zero()};
        Vec3f                  barycentric_b{Vec3f::Zero()};
        Vec2d                  outward{Vec2d::Zero()};
        uint32_t               triangle_index{0};
        const Zone*            zone{nullptr};
        const TriangleBinding* binding{nullptr};
    };

    std::shared_ptr<const TriangleMesh>               mesh;
    std::shared_ptr<const VolumeData>                 data;
    std::vector<Segment>                              segments;
    std::unordered_map<uint64_t, std::vector<size_t>> segment_grid;

    static constexpr double grid_cell_mm = 1.0;

    static uint64_t grid_key(int x, int y) { return (uint64_t(uint32_t(x)) << 32) | uint64_t(uint32_t(y)); }
};

LayerPlaneSampler::LayerPlaneSampler(std::shared_ptr<const TriangleMesh> mesh,
                                     std::shared_ptr<const VolumeData>   data,
                                     const Transform3d&                  local_to_print,
                                     double                              print_z)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->mesh = std::move(mesh);
    m_impl->data = std::move(data);
    if (!m_impl->mesh || !m_impl->data || !std::isfinite(print_z) || std::abs(local_to_print.linear().determinant()) <= EPSILON)
        return;

    std::vector<std::vector<size_t>> bindings_by_triangle(m_impl->mesh->its.indices.size());
    for (size_t binding_index = 0; binding_index < m_impl->data->triangle_bindings.size(); ++binding_index) {
        const TriangleBinding& binding = m_impl->data->triangle_bindings[binding_index];
        if (binding.triangle_index < bindings_by_triangle.size())
            bindings_by_triangle[binding.triangle_index].push_back(binding_index);
    }
    for (std::vector<size_t>& indices : bindings_by_triangle) {
        std::stable_sort(indices.begin(), indices.end(), [this](size_t lhs, size_t rhs) {
            return m_impl->data->zones[m_impl->data->triangle_bindings[lhs].zone_index].priority >
                   m_impl->data->zones[m_impl->data->triangle_bindings[rhs].zone_index].priority;
        });
    }

    constexpr double plane_epsilon = 1e-7;
    for (size_t triangle_index = 0; triangle_index < m_impl->mesh->its.indices.size(); ++triangle_index) {
        if (bindings_by_triangle[triangle_index].empty())
            continue;
        const stl_triangle_vertex_indices& indices        = m_impl->mesh->its.indices[triangle_index];
        const std::array<Vec3d, 3>         local_vertices = {m_impl->mesh->its.vertices[size_t(indices[0])].cast<double>(),
                                                             m_impl->mesh->its.vertices[size_t(indices[1])].cast<double>(),
                                                             m_impl->mesh->its.vertices[size_t(indices[2])].cast<double>()};
        const std::array<Vec3d, 3>         print_vertices = {local_to_print * local_vertices[0], local_to_print * local_vertices[1],
                                                             local_to_print * local_vertices[2]};
        if (!print_vertices[0].allFinite() || !print_vertices[1].allFinite() || !print_vertices[2].allFinite())
            continue;

        Vec3d        world_normal = (print_vertices[1] - print_vertices[0]).cross(print_vertices[2] - print_vertices[0]);
        Vec2d        outward(world_normal.x(), world_normal.y());
        const double outward_length = outward.norm();
        if (!std::isfinite(outward_length) || outward_length <= 1e-6)
            continue;
        outward /= outward_length;

        struct PlanePoint
        {
            Vec3d point{Vec3d::Zero()};
            Vec3f barycentric{Vec3f::Zero()};
        };
        std::vector<PlanePoint> layer_points;
        layer_points.reserve(4);
        const std::array<Vec3f, 3> barycentrics    = {Vec3f(1.f, 0.f, 0.f), Vec3f(0.f, 1.f, 0.f), Vec3f(0.f, 0.f, 1.f)};
        auto                       add_layer_point = [&layer_points](const Vec3d& point, const Vec3f& barycentric) {
            if (!point.allFinite() || !barycentric.allFinite())
                return;
            if (std::any_of(layer_points.begin(), layer_points.end(),
                                                  [&point](const PlanePoint& existing) { return (existing.point - point).squaredNorm() <= 1e-16; }))
                return;
            layer_points.push_back({point, barycentric});
        };
        for (size_t edge = 0; edge < 3; ++edge) {
            const size_t next = (edge + 1) % 3;
            const double da   = print_vertices[edge].z() - print_z;
            const double db   = print_vertices[next].z() - print_z;
            if (std::abs(da) <= plane_epsilon)
                add_layer_point(print_vertices[edge], barycentrics[edge]);
            if ((da < -plane_epsilon && db > plane_epsilon) || (da > plane_epsilon && db < -plane_epsilon)) {
                const double t = std::clamp(da / (da - db), 0.0, 1.0);
                add_layer_point(print_vertices[edge] + (print_vertices[next] - print_vertices[edge]) * t,
                                barycentrics[edge] * float(1.0 - t) + barycentrics[next] * float(t));
            }
        }
        if (layer_points.size() < 2)
            continue;

        size_t best_a        = 0;
        size_t best_b        = 1;
        double best_distance = 0.0;
        for (size_t a = 0; a < layer_points.size(); ++a) {
            for (size_t b = a + 1; b < layer_points.size(); ++b) {
                const double distance = (layer_points[a].point.head<2>() - layer_points[b].point.head<2>()).squaredNorm();
                if (distance > best_distance) {
                    best_distance = distance;
                    best_a        = a;
                    best_b        = b;
                }
            }
        }
        if (!std::isfinite(best_distance) || best_distance <= 1e-12)
            continue;

        // Keep the highest-priority binding independently for each modulation
        // mode. A triangle may legitimately be covered by both workflows.
        for (RenderMode mode : {RenderMode::PerimeterModulationV2, RenderMode::AdaptiveLocalizedCycles}) {
            const TriangleBinding* selected_binding = nullptr;
            const Zone*            selected_zone    = nullptr;
            for (size_t binding_index : bindings_by_triangle[triangle_index]) {
                const TriangleBinding& binding = m_impl->data->triangle_bindings[binding_index];
                const Zone&            zone    = m_impl->data->zones[binding.zone_index];
                if (zone.enabled && zone.render_mode == mode && !zone.palette.empty()) {
                    selected_binding = &binding;
                    selected_zone    = &zone;
                    break;
                }
            }
            if (selected_binding == nullptr)
                continue;
            Impl::Segment segment;
            segment.a              = layer_points[best_a].point.head<2>();
            segment.b              = layer_points[best_b].point.head<2>();
            segment.barycentric_a  = layer_points[best_a].barycentric;
            segment.barycentric_b  = layer_points[best_b].barycentric;
            segment.outward        = outward;
            segment.triangle_index = uint32_t(triangle_index);
            segment.zone           = selected_zone;
            segment.binding        = selected_binding;
            m_impl->segments.emplace_back(std::move(segment));
        }
    }

    for (size_t segment_index = 0; segment_index < m_impl->segments.size(); ++segment_index) {
        const Impl::Segment& segment = m_impl->segments[segment_index];
        const double         length  = (segment.b - segment.a).norm();
        const size_t         steps   = std::max<size_t>(1, size_t(std::ceil(length / (0.5 * Impl::grid_cell_mm))));
        for (size_t step = 0; step <= steps; ++step) {
            const Vec2d          point = segment.a + (segment.b - segment.a) * (double(step) / double(steps));
            std::vector<size_t>& cell  = m_impl->segment_grid[Impl::grid_key(int(std::floor(point.x() / Impl::grid_cell_mm)),
                                                                             int(std::floor(point.y() / Impl::grid_cell_mm)))];
            if (cell.empty() || cell.back() != segment_index)
                cell.push_back(segment_index);
        }
    }
}

LayerPlaneSampler::~LayerPlaneSampler()                                       = default;
LayerPlaneSampler::LayerPlaneSampler(LayerPlaneSampler&&) noexcept            = default;
LayerPlaneSampler& LayerPlaneSampler::operator=(LayerPlaneSampler&&) noexcept = default;

std::optional<LayerPlaneSample> LayerPlaneSampler::sample(const Vec2d&              print_point,
                                                          const Vec2d&              outward,
                                                          double                    max_distance_mm,
                                                          std::optional<RenderMode> render_mode) const
{
    if (!m_impl || !print_point.allFinite() || !outward.allFinite() || !std::isfinite(max_distance_mm) || max_distance_mm < 0.0)
        return std::nullopt;
    const double query_length = outward.norm();
    if (!std::isfinite(query_length) || query_length <= EPSILON)
        return std::nullopt;
    const Vec2d query_outward = outward / query_length;

    const Impl::Segment* best                  = nullptr;
    double               best_squared_distance = max_distance_mm * max_distance_mm;
    double               best_alignment        = -1.0;
    double               best_t                = 0.0;
    const int            query_x               = int(std::floor(print_point.x() / Impl::grid_cell_mm));
    const int            query_y               = int(std::floor(print_point.y() / Impl::grid_cell_mm));
    const int            query_span            = std::max(1, int(std::ceil(max_distance_mm / Impl::grid_cell_mm)));
    for (int cell_x = query_x - query_span; cell_x <= query_x + query_span; ++cell_x) {
        for (int cell_y = query_y - query_span; cell_y <= query_y + query_span; ++cell_y) {
            const auto cell = m_impl->segment_grid.find(Impl::grid_key(cell_x, cell_y));
            if (cell == m_impl->segment_grid.end())
                continue;
            for (size_t segment_index : cell->second) {
                const Impl::Segment& segment = m_impl->segments[segment_index];
                if (render_mode && segment.zone->render_mode != *render_mode)
                    continue;
                // Imported meshes are not guaranteed to have consistently
                // wound triangles.  The segment normal still identifies the
                // wall axis, but its sign may be inverted independently on
                // each triangle.  Distance selects the local wall and the
                // absolute alignment prevents an inward-wound face from
                // turning into an unmapped (zero-displacement) stripe.
                const double alignment = std::abs(query_outward.dot(segment.outward));
                if (!std::isfinite(alignment) || alignment < 0.25)
                    continue;
                const Vec2d  edge         = segment.b - segment.a;
                const double edge_squared = edge.squaredNorm();
                const double t = edge_squared > EPSILON ? std::clamp((print_point - segment.a).dot(edge) / edge_squared, 0.0, 1.0) : 0.0;
                const double distance = (print_point - (segment.a + edge * t)).squaredNorm();
                if (!std::isfinite(distance) || distance > max_distance_mm * max_distance_mm)
                    continue;
                const bool better = best == nullptr || distance < best_squared_distance - 1e-10 ||
                                    (std::abs(distance - best_squared_distance) <= 1e-10 &&
                                     (segment.zone->priority > best->zone->priority ||
                                      (segment.zone->priority == best->zone->priority && alignment > best_alignment)));
                if (better) {
                    best                  = &segment;
                    best_squared_distance = distance;
                    best_alignment        = alignment;
                    best_t                = t;
                }
            }
        }
    }
    if (best == nullptr)
        return std::nullopt;

    Vec3f barycentric = best->barycentric_a * float(1.0 - best_t) + best->barycentric_b * float(best_t);
    barycentric       = barycentric.cwiseMax(0.f);
    const float sum   = barycentric.sum();
    if (!std::isfinite(sum) || sum <= EPSILON)
        return std::nullopt;
    barycentric /= sum;

    LayerPlaneSample result;
    result.color            = sample_source(*m_impl->data, *best->binding, barycentric);
    result.barycentric      = barycentric;
    result.squared_distance = best_squared_distance;
    result.triangle_index   = best->triangle_index;
    result.data             = m_impl->data.get();
    result.zone             = best->zone;
    result.palette_entry    = nearest_palette_entry(*best->zone, result.color);
    result.binding          = best->binding;
    return result.palette_entry ? std::optional<LayerPlaneSample>(result) : std::nullopt;
}

std::vector<LayerPlaneFieldSample> LayerPlaneSampler::field_samples(double                    sample_pitch_mm,
                                                                    std::optional<RenderMode> render_mode) const
{
    std::vector<LayerPlaneFieldSample> result;
    if (!m_impl || !std::isfinite(sample_pitch_mm) || sample_pitch_mm <= 0.0)
        return result;

    const double pitch_mm = std::clamp(sample_pitch_mm, 0.02, 2.0);
    size_t       estimated_count = 0;
    for (const Impl::Segment& segment : m_impl->segments) {
        if (render_mode && segment.zone->render_mode != *render_mode)
            continue;
        const double length_mm = (segment.b - segment.a).norm();
        if (std::isfinite(length_mm) && length_mm > EPSILON)
            estimated_count += std::clamp<size_t>(size_t(std::ceil(length_mm / pitch_mm)), 1, 200000);
    }
    result.reserve(estimated_count);

    for (const Impl::Segment& segment : m_impl->segments) {
        if (render_mode && segment.zone->render_mode != *render_mode)
            continue;
        const Vec2d  edge      = segment.b - segment.a;
        const double length_mm = edge.norm();
        if (!std::isfinite(length_mm) || length_mm <= EPSILON)
            continue;

        const size_t sample_count = std::clamp<size_t>(size_t(std::ceil(length_mm / pitch_mm)), 1, 200000);
        const double integration_weight_mm = std::max(0.05, length_mm / double(sample_count));
        for (size_t sample_index = 0; sample_index < sample_count; ++sample_index) {
            const double t = (double(sample_index) + 0.5) / double(sample_count);
            Vec3f barycentric = segment.barycentric_a * float(1.0 - t) + segment.barycentric_b * float(t);
            barycentric       = barycentric.cwiseMax(0.f);
            const float sum   = barycentric.sum();
            if (!std::isfinite(sum) || sum <= EPSILON)
                continue;
            barycentric /= sum;

            LayerPlaneSample sample;
            sample.color            = sample_source(*m_impl->data, *segment.binding, barycentric);
            sample.barycentric      = barycentric;
            sample.squared_distance = 0.0;
            sample.triangle_index   = segment.triangle_index;
            sample.data             = m_impl->data.get();
            sample.zone             = segment.zone;
            sample.palette_entry    = nearest_palette_entry(*segment.zone, sample.color);
            sample.binding          = segment.binding;
            if (sample.palette_entry == nullptr)
                continue;

            result.push_back({segment.a + edge * t, segment.outward, std::move(sample), integration_weight_mm});
        }
    }
    return result;
}

RGBA sample_processed_texture(const TextureAsset& asset, const Vec2f& uv, WrapMode wrap_u, WrapMode wrap_v, const Zone& zone)
{
    RGBA color = bilinear_sample(asset, uv, wrap_u, wrap_v);
    if (color[3] <= 0.f)
        return color;

    const float edge_strength = std::clamp(zone.image_edge_boost_percent, 0.f, 300.f) / 100.f;
    if (edge_strength > 1e-5f && (asset.width > 1 || asset.height > 1)) {
        const float texel_u = asset.width > 1 ? 1.f / float(asset.width - 1) : 0.f;
        const float texel_v = asset.height > 1 ? 1.f / float(asset.height - 1) : 0.f;
        std::array<double, 3> blurred{0., 0., 0.};
        double                total_weight = 0.;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                const float kernel_weight = float((x == 0 ? 2 : 1) * (y == 0 ? 2 : 1));
                const RGBA  tap = bilinear_sample(asset, uv + Vec2f(float(x) * texel_u, float(y) * texel_v), wrap_u, wrap_v);
                const double weight = double(kernel_weight * tap[3]);
                for (size_t channel = 0; channel < 3; ++channel)
                    blurred[channel] += double(tap[channel]) * weight;
                total_weight += weight;
            }
        }
        if (total_weight > std::numeric_limits<double>::epsilon()) {
            for (size_t channel = 0; channel < 3; ++channel) {
                const float local_blur = float(blurred[channel] / total_weight);
                color[channel] = std::clamp(color[channel] + (color[channel] - local_blur) * edge_strength, 0.f, 1.f);
            }
        }
    }

    const float exposure_scale = std::exp2(std::clamp(zone.image_exposure_ev, -3.f, 3.f));
    const float contrast       = std::clamp(zone.image_contrast_percent, 0.f, 300.f) / 100.f;
    for (size_t channel = 0; channel < 3; ++channel) {
        if (std::abs(exposure_scale - 1.f) > 1e-5f)
            color[channel] = std::clamp(color[channel] * exposure_scale, 0.f, 1.f);
        if (std::abs(contrast - 1.f) > 1e-5f)
            color[channel] = std::clamp(0.5f + (color[channel] - 0.5f) * contrast, 0.f, 1.f);
    }

    const float saturation = std::clamp(zone.image_saturation_percent, 0.f, 300.f) / 100.f;
    if (std::abs(saturation - 1.f) > 1e-5f) {
        const float luminance = color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f;
        for (size_t channel = 0; channel < 3; ++channel)
            color[channel] = std::clamp(luminance + (color[channel] - luminance) * saturation, 0.f, 1.f);
    }
    return color;
}

RGBA adjusted_modulation_target_color(RGBA color, const Zone& zone)
{
    const float gamma = std::clamp(zone.tone_gamma, 0.5f, 3.f);
    for (size_t channel = 0; channel < 3; ++channel) {
        color[channel] = std::clamp(color[channel], 0.f, 1.f);
        if (std::abs(gamma - 1.f) > 1e-5f)
            color[channel] = std::clamp(std::pow(color[channel], 1.f / gamma), 0.f, 1.f);
    }

    const float contrast = std::clamp(zone.overhang_contrast_percent, 25.f, 300.f) / 100.f;
    if (std::abs(contrast - 1.f) > 1e-5f) {
        const float mean = (color[0] + color[1] + color[2]) / 3.f;
        for (size_t channel = 0; channel < 3; ++channel)
            color[channel] = std::clamp(mean + (color[channel] - mean) * contrast, 0.f, 1.f);
    }
    return color;
}

void apply_modulation_component_contrast(std::vector<double>& weights, const Zone& zone)
{
    if (weights.empty())
        return;
    const double contrast = double(std::clamp(zone.overhang_contrast_percent, 25.f, 300.f)) / 100.;
    if (std::abs(contrast - 1.) <= 1e-5)
        return;
    double mean = 0.;
    for (double& weight : weights) {
        weight = std::clamp(weight, 0., 1.);
        mean += weight;
    }
    mean /= double(weights.size());
    for (double& weight : weights)
        weight = std::clamp(mean + (weight - mean) * contrast, 0., 1.);
}

size_t LayerPlaneSampler::segment_count() const { return m_impl ? m_impl->segments.size() : 0; }

} // namespace Slic3r::ImageMap
