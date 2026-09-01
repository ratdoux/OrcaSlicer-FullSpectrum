#include "Projection.hpp"

#include "Sampling.hpp"
#include "../TriangleMesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <queue>
#include <set>
#include <utility>
#include <vector>

namespace Slic3r::ImageMap {

namespace {

std::string unique_id(std::string preferred, const std::set<std::string> &used, const char *fallback)
{
    if (preferred.empty())
        preferred = fallback;
    if (used.find(preferred) == used.end())
        return preferred;
    for (size_t suffix = 2;; ++suffix) {
        const std::string candidate = preferred + "-" + std::to_string(suffix);
        if (used.find(candidate) == used.end())
            return candidate;
    }
}

bool triangle_intersects_unit_square(const std::array<Vec2d, 3> &triangle)
{
    if (!std::all_of(triangle.begin(), triangle.end(), [](const Vec2d &point) { return point.allFinite(); }))
        return false;

    const std::array<Vec2d, 4> square = {Vec2d(0.0, 0.0), Vec2d(1.0, 0.0), Vec2d(1.0, 1.0), Vec2d(0.0, 1.0)};
    std::array<Vec2d, 5>       axes   = {Vec2d::UnitX(), Vec2d::UnitY(), Vec2d::Zero(), Vec2d::Zero(), Vec2d::Zero()};
    for (size_t edge = 0; edge < 3; ++edge) {
        const Vec2d direction = triangle[(edge + 1) % 3] - triangle[edge];
        axes[2 + edge]        = Vec2d(-direction.y(), direction.x());
    }

    for (const Vec2d &axis : axes) {
        if (axis.squaredNorm() <= 1e-20)
            continue;
        double triangle_min = triangle[0].dot(axis);
        double triangle_max = triangle_min;
        for (size_t corner = 1; corner < triangle.size(); ++corner) {
            const double value = triangle[corner].dot(axis);
            triangle_min       = std::min(triangle_min, value);
            triangle_max       = std::max(triangle_max, value);
        }
        double square_min = square[0].dot(axis);
        double square_max = square_min;
        for (size_t corner = 1; corner < square.size(); ++corner) {
            const double value = square[corner].dot(axis);
            square_min         = std::min(square_min, value);
            square_max         = std::max(square_max, value);
        }
        if (triangle_max < square_min - 1e-9 || square_max < triangle_min - 1e-9)
            return false;
    }
    return true;
}

RGBA existing_corner_color(const VolumeData &data, uint32_t triangle_index, size_t corner, const RGBA &fallback)
{
    const TriangleBinding *selected = nullptr;
    const Zone            *zone     = nullptr;
    for (const TriangleBinding &binding : data.triangle_bindings) {
        if (binding.triangle_index != triangle_index || binding.zone_index >= data.zones.size())
            continue;
        const Zone &candidate_zone = data.zones[binding.zone_index];
        if (!candidate_zone.enabled || (zone != nullptr && candidate_zone.priority <= zone->priority))
            continue;
        selected = &binding;
        zone     = &candidate_zone;
    }
    if (selected == nullptr)
        return fallback;
    Vec3f barycentric   = Vec3f::Zero();
    barycentric[corner] = 1.f;
    return sample_source(data, *selected, barycentric);
}

} // namespace

ProjectionResult append_orthographic_projection(const TriangleMesh           &mesh,
                                                 TextureAsset                  texture,
                                                 Zone                          zone,
                                                 const OrthographicProjection &projection,
                                                 VolumeData                   &data)
{
    ProjectionResult result;
    const indexed_triangle_set &its = mesh.its;
    if (texture.stable_id.empty())
        texture.stable_id = "projected-image";
    if (!texture.valid()) {
        result.error = "The projected image texture is incomplete.";
        return result;
    }
    if (zone.palette.empty()) {
        result.error = "The projected image has no printable palette.";
        return result;
    }
    if (its.indices.empty() || its.vertices.empty()) {
        result.error = "The selected volume has no mesh triangles.";
        return result;
    }
    if (!projection.center.allFinite() || !projection.normal.allFinite() || !projection.up.allFinite() ||
        !std::isfinite(projection.width_mm) || !std::isfinite(projection.height_mm) || !std::isfinite(projection.rotation_degrees) ||
        !std::isfinite(projection.max_depth_mm) || !std::isfinite(projection.max_surface_angle_degrees) ||
        projection.width_mm <= 0.0 || projection.height_mm <= 0.0 || projection.max_depth_mm <= 0.0 ||
        projection.max_surface_angle_degrees <= 0.0 || projection.max_surface_angle_degrees >= 90.0) {
        result.error = "The image projection dimensions or orientation are invalid.";
        return result;
    }

    const uint64_t fingerprint      = topology_fingerprint(mesh);
    VolumeData     working          = data;
    const bool     has_existing_data =
        !working.zones.empty() || !working.triangle_bindings.empty() || !working.texture_assets.empty();
    if (has_existing_data) {
        const ValidationResult validation = working.validate(mesh);
        if (!validation.valid) {
            result.error = "The selected volume already contains an invalid image map.";
            return result;
        }
    } else {
        working = VolumeData{};
        working.topology_fingerprint = fingerprint;
    }

    Vec3d normal = projection.normal.normalized();
    if (!normal.allFinite() || normal.squaredNorm() <= 1e-20) {
        result.error = "The image projection normal is invalid.";
        return result;
    }
    Vec3d up = projection.up - normal * projection.up.dot(normal);
    if (!up.allFinite() || up.squaredNorm() <= 1e-12) {
        const Vec3d reference = std::abs(normal.z()) < 0.9 ? Vec3d::UnitZ() : Vec3d::UnitY();
        up = reference - normal * reference.dot(normal);
    }
    up.normalize();
    Vec3d right = up.cross(normal).normalized();
    up          = normal.cross(right).normalized();
    const double angle         = projection.rotation_degrees * PI / 180.0;
    const Vec3d  rotated_right = right * std::cos(angle) + up * std::sin(angle);
    const Vec3d  rotated_up    = up * std::cos(angle) - right * std::sin(angle);
    right                      = rotated_right;
    up                         = rotated_up;

    std::set<std::string> asset_ids;
    for (const TextureAsset &asset : working.texture_assets)
        asset_ids.insert(asset.stable_id);
    texture.stable_id = unique_id(std::move(texture.stable_id), asset_ids, "projected-image");

    std::set<std::string> zone_ids;
    int                   highest_priority = 0;
    for (const Zone &existing_zone : working.zones) {
        zone_ids.insert(existing_zone.stable_id);
        highest_priority = std::max(highest_priority, existing_zone.priority);
    }
    zone.stable_id = unique_id(std::move(zone.stable_id), zone_ids, "image-projection");
    zone.priority  = std::max(zone.priority, highest_priority + 1);

    const auto neighbors = its_face_neighbors(its);
    std::vector<std::array<Vec2d, 3>> projected_uvs(its.indices.size());
    std::vector<bool>                 eligible(its.indices.size(), false);
    const double                          minimum_alignment = std::cos(projection.max_surface_angle_degrees * PI / 180.0);
    const double                          front_tolerance   = std::max(0.05, projection.max_depth_mm * 0.02);
    for (size_t triangle_index = 0; triangle_index < its.indices.size(); ++triangle_index) {
        const stl_triangle_vertex_indices &indices = its.indices[triangle_index];
        if (indices[0] < 0 || indices[1] < 0 || indices[2] < 0 || size_t(indices[0]) >= its.vertices.size() ||
            size_t(indices[1]) >= its.vertices.size() || size_t(indices[2]) >= its.vertices.size())
            continue;
        const std::array<Vec3d, 3> vertices = {its.vertices[size_t(indices[0])].cast<double>(),
                                               its.vertices[size_t(indices[1])].cast<double>(),
                                               its.vertices[size_t(indices[2])].cast<double>()};
        Vec3d face_normal = (vertices[1] - vertices[0]).cross(vertices[2] - vertices[0]);
        const double normal_length = face_normal.norm();
        if (!std::isfinite(normal_length) || normal_length <= 1e-12)
            continue;
        face_normal /= normal_length;
        if (std::abs(face_normal.dot(normal)) < minimum_alignment)
            continue;

        double minimum_depth = std::numeric_limits<double>::infinity();
        double maximum_depth = -std::numeric_limits<double>::infinity();
        for (size_t corner = 0; corner < vertices.size(); ++corner) {
            const Vec3d relative = vertices[corner] - projection.center;
            projected_uvs[triangle_index][corner] = Vec2d(0.5 + relative.dot(right) / projection.width_mm,
                                                           0.5 + relative.dot(up) / projection.height_mm);
            if (projection.flip_horizontal)
                projected_uvs[triangle_index][corner].x() = 1.0 - projected_uvs[triangle_index][corner].x();
            if (projection.flip_vertical)
                projected_uvs[triangle_index][corner].y() = 1.0 - projected_uvs[triangle_index][corner].y();
            const double depth = -relative.dot(normal);
            minimum_depth      = std::min(minimum_depth, depth);
            maximum_depth      = std::max(maximum_depth, depth);
        }
        if (maximum_depth < -front_tolerance || minimum_depth > projection.max_depth_mm ||
            !triangle_intersects_unit_square(projected_uvs[triangle_index]))
            continue;
        eligible[triangle_index] = true;
    }

    std::vector<bool> selected(its.indices.size(), false);
    if (projection.seed_triangle < eligible.size()) {
        if (!eligible[projection.seed_triangle]) {
            result.error = "The image projection does not intersect the selected surface.";
            return result;
        }
        std::queue<uint32_t> pending;
        pending.push(projection.seed_triangle);
        selected[projection.seed_triangle] = true;
        while (!pending.empty()) {
            const uint32_t triangle_index = pending.front();
            pending.pop();
            for (int neighbor : neighbors[triangle_index]) {
                if (neighbor >= 0 && size_t(neighbor) < eligible.size() && eligible[size_t(neighbor)] &&
                    !selected[size_t(neighbor)]) {
                    selected[size_t(neighbor)] = true;
                    pending.push(uint32_t(neighbor));
                }
            }
        }
    } else {
        selected = eligible;
    }

    const VolumeData background_data = working;
    result.texture_asset_index       = int32_t(working.texture_assets.size());
    result.zone_index                = uint32_t(working.zones.size());
    working.texture_assets.emplace_back(std::move(texture));
    working.zones.emplace_back(std::move(zone));
    for (size_t triangle_index = 0; triangle_index < selected.size(); ++triangle_index) {
        if (!selected[triangle_index])
            continue;
        TriangleBinding binding;
        binding.triangle_index             = uint32_t(triangle_index);
        binding.zone_index                 = result.zone_index;
        binding.source.kind                = SourceKind::Texture;
        binding.source.texture_asset_index = result.texture_asset_index;
        binding.source.wrap_u              = WrapMode::Transparent;
        binding.source.wrap_v              = WrapMode::Transparent;
        for (size_t corner = 0; corner < 3; ++corner) {
            binding.source.uvs[corner] = projected_uvs[triangle_index][corner].cast<float>();
            binding.source.corner_colors[corner] = existing_corner_color(background_data, uint32_t(triangle_index), corner,
                                                                          projection.background_color);
        }
        working.triangle_bindings.emplace_back(std::move(binding));
        ++result.projected_triangle_count;
    }

    if (result.projected_triangle_count == 0) {
        result.error = "The image projection does not intersect the selected surface.";
        return result;
    }
    const ValidationResult validation = working.validate(mesh);
    if (!validation.valid) {
        result.error = validation.errors.empty() ? "The generated image projection is invalid." : validation.errors.front();
        return result;
    }
    data           = std::move(working);
    result.success = true;
    return result;
}

} // namespace Slic3r::ImageMap
