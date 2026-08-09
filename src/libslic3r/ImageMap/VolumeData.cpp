#include "VolumeData.hpp"

#include "../TriangleMesh.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace Slic3r::ImageMap {

namespace {

void append_error(ValidationResult &result, std::string error)
{
    result.valid = false;
    result.errors.emplace_back(std::move(error));
}

bool finite_color(const RGBA &color)
{
    return std::all_of(color.begin(), color.end(), [](float value) { return std::isfinite(value); });
}

void hash_bytes(uint64_t &hash, const void *data, size_t size)
{
    constexpr uint64_t fnv_prime = 1099511628211ull;
    const auto        *bytes     = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= uint64_t(bytes[i]);
        hash *= fnv_prime;
    }
}

} // namespace

bool TextureAsset::valid() const
{
    if (stable_id.empty() || width == 0 || height == 0)
        return false;
    if (size_t(width) > std::numeric_limits<size_t>::max() / size_t(height) / 4)
        return false;
    return rgba.size() == size_t(width) * size_t(height) * 4;
}

bool VolumeData::content_equals(const VolumeData &other) const
{
    if (this == &other)
        return true;
    if (schema_version != other.schema_version || topology_fingerprint != other.topology_fingerprint ||
        texture_assets.size() != other.texture_assets.size() || zones.size() != other.zones.size() ||
        triangle_bindings.size() != other.triangle_bindings.size())
        return false;

    for (size_t i = 0; i < texture_assets.size(); ++i) {
        const TextureAsset &lhs = texture_assets[i];
        const TextureAsset &rhs = other.texture_assets[i];
        if (lhs.stable_id != rhs.stable_id || lhs.display_name != rhs.display_name || lhs.width != rhs.width || lhs.height != rhs.height ||
            lhs.rgba != rhs.rgba)
            return false;
    }

    for (size_t i = 0; i < zones.size(); ++i) {
        const Zone &lhs = zones[i];
        const Zone &rhs = other.zones[i];
        if (lhs.stable_id != rhs.stable_id || lhs.display_name != rhs.display_name || lhs.enabled != rhs.enabled ||
            lhs.priority != rhs.priority || lhs.render_mode != rhs.render_mode ||
            lhs.minimum_component_percent != rhs.minimum_component_percent || lhs.target_sample_size_mm != rhs.target_sample_size_mm ||
            lhs.max_facet_samples != rhs.max_facet_samples || lhs.modulation_sample_spacing_mm != rhs.modulation_sample_spacing_mm ||
            lhs.corner_smoothing_radius_mm != rhs.corner_smoothing_radius_mm || lhs.palette.size() != rhs.palette.size())
            return false;
        for (size_t palette_idx = 0; palette_idx < lhs.palette.size(); ++palette_idx) {
            const PaletteEntry &lhs_entry = lhs.palette[palette_idx];
            const PaletteEntry &rhs_entry = rhs.palette[palette_idx];
            if (lhs_entry.target_color != rhs_entry.target_color ||
                lhs_entry.mixed_filament_stable_id != rhs_entry.mixed_filament_stable_id ||
                lhs_entry.fallback_filament_id != rhs_entry.fallback_filament_id)
                return false;
        }
    }

    for (size_t i = 0; i < triangle_bindings.size(); ++i) {
        const TriangleBinding &lhs = triangle_bindings[i];
        const TriangleBinding &rhs = other.triangle_bindings[i];
        if (lhs.triangle_index != rhs.triangle_index || lhs.zone_index != rhs.zone_index || lhs.source.kind != rhs.source.kind ||
            lhs.source.texture_asset_index != rhs.source.texture_asset_index || lhs.source.wrap_u != rhs.source.wrap_u ||
            lhs.source.wrap_v != rhs.source.wrap_v || lhs.source.corner_colors != rhs.source.corner_colors)
            return false;
        for (size_t corner = 0; corner < lhs.source.uvs.size(); ++corner)
            if (lhs.source.uvs[corner].x() != rhs.source.uvs[corner].x() || lhs.source.uvs[corner].y() != rhs.source.uvs[corner].y())
                return false;
    }

    return true;
}

uint64_t topology_fingerprint(const TriangleMesh &mesh)
{
    constexpr uint64_t fnv_offset = 1469598103934665603ull;
    uint64_t           hash       = fnv_offset;
    const auto        &its        = mesh.its;
    const uint64_t     vertex_count = uint64_t(its.vertices.size());
    const uint64_t     triangle_count = uint64_t(its.indices.size());
    hash_bytes(hash, &vertex_count, sizeof(vertex_count));
    hash_bytes(hash, &triangle_count, sizeof(triangle_count));
    for (const stl_triangle_vertex_indices &indices : its.indices)
        hash_bytes(hash, indices.data(), sizeof(indices[0]) * 3);
    return hash;
}

size_t stitch_perimeter_modulation_uv_cracks(const TriangleMesh &mesh, VolumeData &data)
{
    struct EdgeUse
    {
        size_t binding_index{0};
        size_t first_corner{0};
        size_t second_corner{0};
    };

    using EdgeKey = std::pair<int, int>;
    std::map<EdgeKey, std::vector<EdgeUse>> edge_uses;
    const indexed_triangle_set &its = mesh.its;
    for (size_t binding_index = 0; binding_index < data.triangle_bindings.size(); ++binding_index) {
        const TriangleBinding &binding = data.triangle_bindings[binding_index];
        if (binding.triangle_index >= its.indices.size() || binding.zone_index >= data.zones.size() ||
            binding.source.kind != SourceKind::Texture || data.zones[binding.zone_index].render_mode == RenderMode::NormalMix)
            continue;
        const stl_triangle_vertex_indices &indices = its.indices[binding.triangle_index];
        for (size_t corner = 0; corner < 3; ++corner) {
            const size_t next = (corner + 1) % 3;
            const int vertex = indices[int(corner)];
            const int next_vertex = indices[int(next)];
            if (vertex < 0 || next_vertex < 0 || vertex == next_vertex)
                continue;
            if (vertex < next_vertex)
                edge_uses[{vertex, next_vertex}].push_back({binding_index, corner, next});
            else
                edge_uses[{next_vertex, vertex}].push_back({binding_index, next, corner});
        }
    }

    auto triangle_normal = [&its](uint32_t triangle_index) -> Vec3f {
        if (triangle_index >= its.indices.size())
            return Vec3f::Zero();
        const stl_triangle_vertex_indices &indices = its.indices[triangle_index];
        if (indices[0] < 0 || indices[1] < 0 || indices[2] < 0 || size_t(indices[0]) >= its.vertices.size() ||
            size_t(indices[1]) >= its.vertices.size() || size_t(indices[2]) >= its.vertices.size())
            return Vec3f::Zero();
        Vec3f normal = (its.vertices[size_t(indices[1])] - its.vertices[size_t(indices[0])])
                           .cross(its.vertices[size_t(indices[2])] - its.vertices[size_t(indices[0])]);
        const float length = normal.norm();
        if (!normal.allFinite() || length <= EPSILON)
            return Vec3f::Zero();
        normal /= length;
        return normal;
    };

    constexpr float max_gap_pixels = 12.f;
    constexpr float min_gap_pixels = 0.5f;
    constexpr float max_gap_variation_pixels = 2.5f;
    size_t stitched_edges = 0;
    for (const auto &edge_entry : edge_uses) {
        const std::vector<EdgeUse> &uses = edge_entry.second;
        if (uses.size() != 2)
            continue;
        const EdgeUse &first_use = uses[0];
        const EdgeUse &second_use = uses[1];
        TriangleBinding &first = data.triangle_bindings[first_use.binding_index];
        TriangleBinding &second = data.triangle_bindings[second_use.binding_index];
        if (first.triangle_index == second.triangle_index || first.zone_index != second.zone_index ||
            first.source.texture_asset_index != second.source.texture_asset_index || first.source.wrap_u != second.source.wrap_u ||
            first.source.wrap_v != second.source.wrap_v || first.source.texture_asset_index < 0 ||
            size_t(first.source.texture_asset_index) >= data.texture_assets.size())
            continue;

        const Vec3f first_normal = triangle_normal(first.triangle_index);
        const Vec3f second_normal = triangle_normal(second.triangle_index);
        if (first_normal.squaredNorm() <= EPSILON || second_normal.squaredNorm() <= EPSILON || first_normal.dot(second_normal) < 0.9999f)
            continue;

        const TextureAsset &asset = data.texture_assets[size_t(first.source.texture_asset_index)];
        if (asset.width < 2 || asset.height < 2)
            continue;
        const Vec2f pixel_scale(float(asset.width - 1), float(asset.height - 1));
        const Vec2f first_gap = (second.source.uvs[second_use.first_corner] - first.source.uvs[first_use.first_corner]).cwiseProduct(pixel_scale);
        const Vec2f second_gap =
            (second.source.uvs[second_use.second_corner] - first.source.uvs[first_use.second_corner]).cwiseProduct(pixel_scale);
        const float largest_gap = std::max(first_gap.norm(), second_gap.norm());
        if (!first_gap.allFinite() || !second_gap.allFinite() || largest_gap < min_gap_pixels || largest_gap > max_gap_pixels ||
            (first_gap - second_gap).norm() > max_gap_variation_pixels)
            continue;

        const Vec2f first_midpoint =
            0.5f * (first.source.uvs[first_use.first_corner] + second.source.uvs[second_use.first_corner]);
        const Vec2f second_midpoint =
            0.5f * (first.source.uvs[first_use.second_corner] + second.source.uvs[second_use.second_corner]);
        first.source.uvs[first_use.first_corner] = second.source.uvs[second_use.first_corner] = first_midpoint;
        first.source.uvs[first_use.second_corner] = second.source.uvs[second_use.second_corner] = second_midpoint;
        ++stitched_edges;
    }
    return stitched_edges;
}

ValidationResult VolumeData::validate(const TriangleMesh &mesh) const
{
    ValidationResult result;
    const size_t     triangle_count = mesh.its.indices.size();

    if (schema_version != VOLUME_DATA_SCHEMA_VERSION)
        append_error(result, "Unsupported image-map volume schema version.");
    if (topology_fingerprint == 0 || topology_fingerprint != ImageMap::topology_fingerprint(mesh))
        append_error(result, "Image-map topology does not match the model volume.");
    if (zones.empty())
        append_error(result, "Image-map volume has no zones.");

    std::set<std::string> asset_ids;
    for (const TextureAsset &asset : texture_assets) {
        if (!asset.valid())
            append_error(result, "Image-map texture asset is incomplete.");
        if (!asset_ids.insert(asset.stable_id).second)
            append_error(result, "Image-map texture asset IDs must be unique.");
    }

    std::set<std::string> zone_ids;
    for (const Zone &zone : zones) {
        if (zone.stable_id.empty() || !zone_ids.insert(zone.stable_id).second)
            append_error(result, "Image-map zone IDs must be non-empty and unique.");
        if (zone.palette.empty())
            append_error(result, "Image-map zone has no printable palette.");
        if (!std::isfinite(zone.target_sample_size_mm) || zone.target_sample_size_mm <= 0.f || zone.max_facet_samples == 0)
            append_error(result, "Image-map sampling limits are invalid.");
        if (!std::isfinite(zone.modulation_sample_spacing_mm) || zone.modulation_sample_spacing_mm <= 0.f ||
            !std::isfinite(zone.corner_smoothing_radius_mm) || zone.corner_smoothing_radius_mm < 0.f)
            append_error(result, "Image-map modulation limits are invalid.");
        for (const PaletteEntry &entry : zone.palette) {
            if (!finite_color(entry.target_color) || entry.fallback_filament_id == 0)
                append_error(result, "Image-map palette entry is invalid.");
        }
    }

    std::set<std::pair<uint32_t, uint32_t>> binding_keys;
    for (const TriangleBinding &binding : triangle_bindings) {
        if (binding.triangle_index >= triangle_count || binding.zone_index >= zones.size()) {
            append_error(result, "Image-map triangle binding is out of range.");
            continue;
        }
        if (!binding_keys.emplace(binding.triangle_index, binding.zone_index).second)
            append_error(result, "Image-map triangle binding is duplicated.");
        if (binding.source.kind == SourceKind::Texture &&
            (binding.source.texture_asset_index < 0 || size_t(binding.source.texture_asset_index) >= texture_assets.size()))
            append_error(result, "Image-map triangle references a missing texture asset.");
        for (const Vec2f &uv : binding.source.uvs) {
            if (!uv.allFinite())
                append_error(result, "Image-map UV coordinate is not finite.");
        }
        for (const RGBA &color : binding.source.corner_colors) {
            if (!finite_color(color))
                append_error(result, "Image-map corner colour is not finite.");
        }
    }

    return result;
}

size_t VolumeData::memsize() const
{
    size_t size = sizeof(*this);

    size += texture_assets.capacity() * sizeof(TextureAsset);
    for (const TextureAsset &asset : texture_assets) {
        size += asset.stable_id.capacity();
        size += asset.display_name.capacity();
        size += asset.rgba.capacity() * sizeof(uint8_t);
    }

    size += zones.capacity() * sizeof(Zone);
    for (const Zone &zone : zones) {
        size += zone.stable_id.capacity();
        size += zone.display_name.capacity();
        size += zone.palette.capacity() * sizeof(PaletteEntry);
    }

    size += triangle_bindings.capacity() * sizeof(TriangleBinding);
    return size;
}

} // namespace Slic3r::ImageMap
