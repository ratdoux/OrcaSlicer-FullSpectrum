#include "VolumeData.hpp"

#include "../TriangleMesh.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
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
