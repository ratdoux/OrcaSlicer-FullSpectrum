#include "Sampling.hpp"

#include "../AABBMesh.hpp"
#include "../TriangleMesh.hpp"

#include <algorithm>
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

RGBA bilinear_sample(const TextureAsset &asset, const Vec2f &uv, WrapMode wrap_u, WrapMode wrap_v)
{
    if (!asset.valid())
        return RGBA{1.f, 1.f, 1.f, 1.f};
    const float x = wrapped_coordinate(uv.x(), wrap_u) * float(asset.width > 1 ? asset.width - 1 : 0);
    const float y = wrapped_coordinate(uv.y(), wrap_v) * float(asset.height > 1 ? asset.height - 1 : 0);
    const size_t x0 = std::min<size_t>(size_t(std::floor(x)), size_t(asset.width - 1));
    const size_t y0 = std::min<size_t>(size_t(std::floor(y)), size_t(asset.height - 1));
    const size_t x1 = std::min<size_t>(x0 + 1, size_t(asset.width - 1));
    const size_t y1 = std::min<size_t>(y0 + 1, size_t(asset.height - 1));
    const float tx = x - float(x0);
    const float ty = y - float(y0);
    auto channel = [&asset](size_t px, size_t py, size_t component) {
        return float(asset.rgba[(py * size_t(asset.width) + px) * 4 + component]) / 255.f;
    };
    RGBA result{};
    for (size_t component = 0; component < 4; ++component) {
        const float top = channel(x0, y0, component) + (channel(x1, y0, component) - channel(x0, y0, component)) * tx;
        const float bottom = channel(x0, y1, component) + (channel(x1, y1, component) - channel(x0, y1, component)) * tx;
        result[component] = std::clamp(top + (bottom - top) * ty, 0.f, 1.f);
    }
    return result;
}

RGBA interpolate_colors(const std::array<RGBA, 3> &colors, const Vec3f &barycentric)
{
    RGBA result{};
    for (size_t component = 0; component < 4; ++component)
        result[component] = std::clamp(colors[0][component] * barycentric.x() + colors[1][component] * barycentric.y() +
                                          colors[2][component] * barycentric.z(),
                                      0.f, 1.f);
    return result;
}

} // namespace

RGBA sample_source(const VolumeData &data, const TriangleBinding &binding, const Vec3f &barycentric)
{
    const RGBA background = interpolate_colors(binding.source.corner_colors, barycentric);
    if (binding.source.kind != SourceKind::Texture || binding.source.texture_asset_index < 0 ||
        size_t(binding.source.texture_asset_index) >= data.texture_assets.size())
        return background;

    const Vec2f uv = binding.source.uvs[0] * barycentric.x() + binding.source.uvs[1] * barycentric.y() +
                     binding.source.uvs[2] * barycentric.z();
    const RGBA sampled = bilinear_sample(data.texture_assets[size_t(binding.source.texture_asset_index)], uv,
                                         binding.source.wrap_u, binding.source.wrap_v);
    const float alpha = sampled[3];
    return RGBA{sampled[0] * alpha + background[0] * (1.f - alpha),
                sampled[1] * alpha + background[1] * (1.f - alpha),
                sampled[2] * alpha + background[2] * (1.f - alpha), 1.f};
}

const PaletteEntry *nearest_palette_entry(const Zone &zone, const RGBA &color)
{
    const PaletteEntry *best = nullptr;
    double best_distance = std::numeric_limits<double>::infinity();
    for (const PaletteEntry &entry : zone.palette) {
        double distance = 0.0;
        for (size_t component = 0; component < 3; ++component) {
            const double delta = double(color[component]) - double(entry.target_color[component]);
            distance += delta * delta;
        }
        if (distance < best_distance) {
            best = &entry;
            best_distance = distance;
        }
    }
    return best;
}

Vec3f barycentric_coordinates(const Vec3d &point, const Vec3d &a, const Vec3d &b, const Vec3d &c)
{
    const Vec3d v0 = b - a;
    const Vec3d v1 = c - a;
    const Vec3d v2 = point - a;
    const double d00 = v0.dot(v0);
    const double d01 = v0.dot(v1);
    const double d11 = v1.dot(v1);
    const double d20 = v2.dot(v0);
    const double d21 = v2.dot(v1);
    const double denominator = d00 * d11 - d01 * d01;
    if (!std::isfinite(denominator) || std::abs(denominator) <= std::numeric_limits<double>::epsilon())
        return Vec3f(1.f / 3.f, 1.f / 3.f, 1.f / 3.f);
    const double v = (d11 * d20 - d01 * d21) / denominator;
    const double w = (d00 * d21 - d01 * d20) / denominator;
    const double u = 1.0 - v - w;
    Vec3f result{float(u), float(v), float(w)};
    result = result.cwiseMax(0.f);
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
        const TriangleBinding &binding = m_data->triangle_bindings[binding_idx];
        if (binding.triangle_index < m_bindings_by_triangle.size())
            m_bindings_by_triangle[binding.triangle_index].push_back(binding_idx);
    }
    for (auto &indices : m_bindings_by_triangle) {
        std::stable_sort(indices.begin(), indices.end(), [this](size_t lhs, size_t rhs) {
            return m_data->zones[m_data->triangle_bindings[lhs].zone_index].priority >
                   m_data->zones[m_data->triangle_bindings[rhs].zone_index].priority;
        });
    }
}

SurfaceSampler::~SurfaceSampler() = default;
SurfaceSampler::SurfaceSampler(SurfaceSampler &&) noexcept = default;
SurfaceSampler &SurfaceSampler::operator=(SurfaceSampler &&) noexcept = default;

std::optional<SurfaceSample> SurfaceSampler::sample(const Vec3d &local_point,
                                                    double max_distance_mm,
                                                    std::optional<RenderMode> render_mode) const
{
    if (!m_aabb || !std::isfinite(max_distance_mm) || max_distance_mm < 0.0)
        return std::nullopt;
    int face = -1;
    Vec3d closest;
    const double squared_distance = m_aabb->squared_distance(local_point, face, closest);
    if (face < 0 || size_t(face) >= m_bindings_by_triangle.size() || !std::isfinite(squared_distance) ||
        squared_distance > max_distance_mm * max_distance_mm)
        return std::nullopt;

    const TriangleBinding *selected = nullptr;
    const Zone *zone = nullptr;
    for (size_t binding_idx : m_bindings_by_triangle[size_t(face)]) {
        const TriangleBinding &candidate = m_data->triangle_bindings[binding_idx];
        const Zone &candidate_zone = m_data->zones[candidate.zone_index];
        if (candidate_zone.enabled && (!render_mode || candidate_zone.render_mode == *render_mode)) {
            selected = &candidate;
            zone = &candidate_zone;
            break;
        }
    }
    if (!selected || !zone)
        return std::nullopt;

    const stl_triangle_vertex_indices &indices = m_mesh->its.indices[size_t(face)];
    const Vec3f barycentric = barycentric_coordinates(closest,
                                                       m_mesh->its.vertices[size_t(indices[0])].cast<double>(),
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
