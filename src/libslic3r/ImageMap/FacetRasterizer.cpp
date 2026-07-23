#include "FacetRasterizer.hpp"

#include "Sampling.hpp"
#include "../TriangleMesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace Slic3r::ImageMap {
namespace {

constexpr unsigned int k_max_depth = 6;

size_t leaf_count(unsigned int depth)
{
    size_t count = 1;
    for (unsigned int i = 0; i < depth; ++i)
        count *= 4;
    return count;
}

// The old limiter repeatedly scanned every triangle and lowered one depth at a
// time. Since subdivision depths are capped at k_max_depth, processing one
// whole depth bucket at a time produces the same largest-saving-first result in
// O(k_max_depth * triangle_count), which is linear for the fixed depth cap.
void limit_depths_to_budget(std::vector<unsigned int> &depths, size_t &total_samples, size_t budget)
{
    for (unsigned int depth = k_max_depth; depth > 0 && total_samples > budget; --depth) {
        const size_t saving_per_reduction = leaf_count(depth) - leaf_count(depth - 1);
        const size_t excess               = total_samples - budget;
        size_t       reductions_remaining = 1 + (excess - 1) / saving_per_reduction;

        for (unsigned int &candidate_depth : depths) {
            if (reductions_remaining == 0 || total_samples <= budget)
                break;
            if (candidate_depth != depth)
                continue;
            --candidate_depth;
            total_samples -= saving_per_reduction;
            --reductions_remaining;
        }
    }
}

bool source_preview_continue(const SourceColorRasterizationOptions &options, int progress)
{
    if (options.cancelled && options.cancelled())
        return false;
    if (options.progress)
        options.progress(progress);
    return true;
}

unsigned int desired_depth(const TriangleMesh &mesh, const TriangleBinding &binding, const Zone &zone)
{
    const auto &indices = mesh.its.indices[binding.triangle_index];
    const Vec3f &a = mesh.its.vertices[size_t(indices[0])];
    const Vec3f &b = mesh.its.vertices[size_t(indices[1])];
    const Vec3f &c = mesh.its.vertices[size_t(indices[2])];
    const float max_edge = std::max({(b - a).norm(), (c - b).norm(), (a - c).norm()});
    const float sample_size = std::max(0.1f, zone.target_sample_size_mm);
    return std::min(k_max_depth, max_edge > sample_size ? unsigned(std::ceil(std::log2(max_edge / sample_size))) : 0u);
}

std::string encode_leaf(unsigned int filament_id, unsigned int base_filament_id)
{
    if (filament_id == 0 || filament_id > 16)
        return {};
    if (filament_id == base_filament_id)
        return "0";
    if (filament_id <= 2) {
        const int code = int(filament_id) << 2;
        return std::string(1, char(code < 10 ? '0' + code : 'A' + code - 10));
    }
    const int extension = int(filament_id) - 3;
    return std::string(1, char(extension < 10 ? '0' + extension : 'A' + extension - 10)) + "C";
}

std::string encode_tree(const std::vector<unsigned int> &ids, size_t &cursor, unsigned int depth, unsigned int base_id)
{
    if (depth == 0)
        return cursor < ids.size() ? encode_leaf(ids[cursor++], base_id) : std::string();
    std::string encoded;
    for (size_t child = 0; child < 4; ++child)
        encoded += encode_tree(ids, cursor, depth - 1, base_id);
    encoded += '3';
    return encoded;
}

void sample_leaves(const VolumeData &data,
                   const TriangleBinding &binding,
                   const Zone &zone,
                   const std::array<Vec3f, 3> &barycentric,
                   unsigned int depth,
                   const PaletteFilamentResolver &resolver,
                   unsigned int base_filament_id,
                   std::vector<unsigned int> &ids,
                   size_t &unresolved)
{
    if (depth == 0) {
        const Vec3f center = (barycentric[0] + barycentric[1] + barycentric[2]) / 3.f;
        const PaletteEntry *entry = nearest_palette_entry(zone, sample_source(data, binding, center));
        unsigned int id = entry ? resolver(*entry) : 0;
        if (id == 0 || id > 16)
            ++unresolved;
        if (id == 0 || id > 16)
            id = base_filament_id;
        ids.push_back(id);
        return;
    }
    const Vec3f m01 = (barycentric[0] + barycentric[1]) * 0.5f;
    const Vec3f m12 = (barycentric[1] + barycentric[2]) * 0.5f;
    const Vec3f m20 = (barycentric[2] + barycentric[0]) * 0.5f;
    sample_leaves(data, binding, zone, {barycentric[0], m01, m20}, depth - 1, resolver, base_filament_id, ids, unresolved);
    sample_leaves(data, binding, zone, {m01, barycentric[1], m12}, depth - 1, resolver, base_filament_id, ids, unresolved);
    sample_leaves(data, binding, zone, {m12, barycentric[2], m20}, depth - 1, resolver, base_filament_id, ids, unresolved);
    sample_leaves(data, binding, zone, {m01, m12, m20}, depth - 1, resolver, base_filament_id, ids, unresolved);
}

void emit_source_leaves(const TriangleMesh              &mesh,
                        const VolumeData                &data,
                        const TriangleBinding           *binding,
                        const std::array<Vec3f, 3>       &barycentric,
                        unsigned int                     depth,
                        const RGBA                      &fallback_color,
                        SourceColorRasterization        &result,
                        const stl_triangle_vertex_indices &triangle_indices,
                        const Vec3f                     &normal)
{
    if (depth == 0) {
        for (const Vec3f &weights : barycentric) {
            SourceColorVertex vertex;
            vertex.position = mesh.its.vertices[size_t(triangle_indices[0])] * weights.x() +
                              mesh.its.vertices[size_t(triangle_indices[1])] * weights.y() +
                              mesh.its.vertices[size_t(triangle_indices[2])] * weights.z();
            vertex.normal = normal;
            vertex.color  = binding != nullptr ? sample_source(data, *binding, weights) : fallback_color;
            result.indices.emplace_back(unsigned(result.vertices.size()));
            result.vertices.emplace_back(std::move(vertex));
        }
        ++result.sampled_leaf_count;
        return;
    }

    const Vec3f m01 = (barycentric[0] + barycentric[1]) * 0.5f;
    const Vec3f m12 = (barycentric[1] + barycentric[2]) * 0.5f;
    const Vec3f m20 = (barycentric[2] + barycentric[0]) * 0.5f;
    emit_source_leaves(mesh, data, binding, {barycentric[0], m01, m20}, depth - 1, fallback_color, result, triangle_indices, normal);
    emit_source_leaves(mesh, data, binding, {m01, barycentric[1], m12}, depth - 1, fallback_color, result, triangle_indices, normal);
    emit_source_leaves(mesh, data, binding, {m12, barycentric[2], m20}, depth - 1, fallback_color, result, triangle_indices, normal);
    emit_source_leaves(mesh, data, binding, {m01, m12, m20}, depth - 1, fallback_color, result, triangle_indices, normal);
}

struct PreviewGridKey
{
    int64_t x{0};
    int64_t y{0};
    int64_t z{0};

    bool operator==(const PreviewGridKey &other) const { return x == other.x && y == other.y && z == other.z; }
};

struct PreviewGridKeyHash
{
    size_t operator()(const PreviewGridKey &key) const
    {
        size_t seed = std::hash<int64_t>{}(key.x);
        seed ^= std::hash<int64_t>{}(key.y) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
        seed ^= std::hash<int64_t>{}(key.z) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
        return seed;
    }
};

struct PreviewTriangleKey
{
    std::array<unsigned int, 3> vertices;

    bool operator==(const PreviewTriangleKey &other) const { return vertices == other.vertices; }
};

struct PreviewTriangleKeyHash
{
    size_t operator()(const PreviewTriangleKey &key) const
    {
        size_t seed = std::hash<unsigned int>{}(key.vertices[0]);
        seed ^= std::hash<unsigned int>{}(key.vertices[1]) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
        seed ^= std::hash<unsigned int>{}(key.vertices[2]) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
        return seed;
    }
};

struct PreviewCluster
{
    Vec3d                 position_sum{Vec3d::Zero()};
    Vec3d                 normal_sum{Vec3d::Zero()};
    std::array<double, 4> color_sum{0., 0., 0., 0.};
    size_t                sample_count{0};
};

SourceColorRasterization rasterize_dense_source_lod(
    const TriangleMesh                         &mesh,
    const VolumeData                           &data,
    const std::vector<const TriangleBinding *> &selected,
    const RGBA                                 &fallback_color,
    const SourceColorRasterizationOptions      &options,
    size_t                                      source_triangle_count)
{
    SourceColorRasterization result;
    result.source_triangle_count = source_triangle_count;
    if (mesh.its.vertices.empty() || options.max_leaf_triangles == 0)
        return result;

    Vec3f bounds_min = mesh.its.vertices.front();
    Vec3f bounds_max = bounds_min;
    for (const Vec3f &vertex : mesh.its.vertices) {
        bounds_min = bounds_min.cwiseMin(vertex);
        bounds_max = bounds_max.cwiseMax(vertex);
    }
    const Vec3d  size = (bounds_max - bounds_min).cast<double>();
    const double bbox_surface_area = 2. * (size.x() * size.y() + size.y() * size.z() + size.z() * size.x());
    const double target_clusters   = std::max(4., double(options.max_leaf_triangles) * 0.5);
    double       cell_size         = bbox_surface_area > EPSILON ? std::sqrt(bbox_surface_area / target_clusters) :
                                                           std::max(size.norm() / std::sqrt(target_clusters), double(EPSILON));
    cell_size = std::max(cell_size, double(EPSILON));

    constexpr size_t max_attempts = 16;
    const std::array<Vec3f, 3> corner_weights{
        Vec3f(1.f, 0.f, 0.f), Vec3f(0.f, 1.f, 0.f), Vec3f(0.f, 0.f, 1.f)};
    for (size_t attempt = 0; attempt < max_attempts; ++attempt) {
        if (!source_preview_continue(options, 20 + int(std::min<size_t>(attempt, 5))))
            return {};

        std::unordered_map<PreviewGridKey, unsigned int, PreviewGridKeyHash> clusters_by_cell;
        std::vector<PreviewCluster>                                           clusters;
        std::unordered_set<PreviewTriangleKey, PreviewTriangleKeyHash>        emitted_triangles;
        std::vector<unsigned int>                                              indices;
        clusters_by_cell.reserve(std::min(mesh.its.vertices.size(), options.max_leaf_triangles));
        clusters.reserve(std::min(mesh.its.vertices.size(), options.max_leaf_triangles));
        emitted_triangles.reserve(options.max_leaf_triangles);
        indices.reserve(options.max_leaf_triangles * 3);

        bool over_budget = false;
        for (size_t triangle_idx = 0; triangle_idx < mesh.its.indices.size(); ++triangle_idx) {
            if ((triangle_idx & 0x7ffu) == 0u) {
                if (options.cancelled && options.cancelled())
                    return {};
                const int progress = 25 + int(55 * triangle_idx / mesh.its.indices.size());
                if (options.progress)
                    options.progress(progress);
            }

            const stl_triangle_vertex_indices &triangle = mesh.its.indices[triangle_idx];
            const Vec3f &a = mesh.its.vertices[size_t(triangle[0])];
            const Vec3f &b = mesh.its.vertices[size_t(triangle[1])];
            const Vec3f &c = mesh.its.vertices[size_t(triangle[2])];
            Vec3f normal = (b - a).cross(c - a);
            const float normal_length = normal.norm();
            if (std::isfinite(normal_length) && normal_length > EPSILON)
                normal /= normal_length;
            else
                normal = Vec3f::UnitZ();

            std::array<unsigned int, 3> cluster_ids;
            for (size_t corner = 0; corner < 3; ++corner) {
                const Vec3f &position = mesh.its.vertices[size_t(triangle[corner])];
                const Vec3d relative  = (position - bounds_min).cast<double>() / cell_size;
                const PreviewGridKey key{int64_t(std::floor(relative.x())), int64_t(std::floor(relative.y())),
                                         int64_t(std::floor(relative.z()))};
                auto [cell_it, inserted] = clusters_by_cell.emplace(key, unsigned(clusters.size()));
                if (inserted)
                    clusters.emplace_back();
                const unsigned int cluster_id = cell_it->second;
                cluster_ids[corner] = cluster_id;

                PreviewCluster &cluster = clusters[cluster_id];
                cluster.position_sum += position.cast<double>();
                cluster.normal_sum += normal.cast<double>();
                const RGBA color = selected[triangle_idx] != nullptr ?
                                       sample_source(data, *selected[triangle_idx], corner_weights[corner]) : fallback_color;
                for (size_t channel = 0; channel < 4; ++channel)
                    cluster.color_sum[channel] += color[channel];
                ++cluster.sample_count;
            }

            if (cluster_ids[0] == cluster_ids[1] || cluster_ids[1] == cluster_ids[2] || cluster_ids[2] == cluster_ids[0])
                continue;

            PreviewTriangleKey triangle_key{cluster_ids};
            std::sort(triangle_key.vertices.begin(), triangle_key.vertices.end());
            if (!emitted_triangles.insert(triangle_key).second)
                continue;
            indices.insert(indices.end(), cluster_ids.begin(), cluster_ids.end());
            if (indices.size() / 3 > options.max_leaf_triangles) {
                over_budget = true;
                break;
            }
        }

        if (over_budget) {
            const double measured_triangles = double(indices.size() / 3);
            cell_size *= std::max(1.15, std::sqrt(measured_triangles / double(options.max_leaf_triangles)) * 1.1);
            continue;
        }
        if (indices.empty()) {
            cell_size *= 0.75;
            continue;
        }

        std::vector<unsigned int> compact_ids(clusters.size(), std::numeric_limits<unsigned int>::max());
        size_t                    referenced_cluster_count = 0;
        for (unsigned int &index : indices) {
            unsigned int &compact_id = compact_ids[index];
            if (compact_id == std::numeric_limits<unsigned int>::max())
                compact_id = unsigned(referenced_cluster_count++);
            index = compact_id;
        }
        result.vertices.resize(referenced_cluster_count);
        for (size_t cluster_idx = 0; cluster_idx < clusters.size(); ++cluster_idx) {
            const unsigned int compact_id = compact_ids[cluster_idx];
            if (compact_id == std::numeric_limits<unsigned int>::max())
                continue;
            const PreviewCluster &cluster = clusters[cluster_idx];
            SourceColorVertex    &vertex  = result.vertices[compact_id];
            const double          divisor = double(std::max<size_t>(1, cluster.sample_count));
            vertex.position = (cluster.position_sum / divisor).cast<float>();
            vertex.normal   = cluster.normal_sum.cast<float>();
            const float normal_length = vertex.normal.norm();
            if (std::isfinite(normal_length) && normal_length > EPSILON)
                vertex.normal /= normal_length;
            else
                vertex.normal = Vec3f::UnitZ();
            for (size_t channel = 0; channel < 4; ++channel)
                vertex.color[channel] = float(cluster.color_sum[channel] / divisor);
        }
        result.indices            = std::move(indices);
        result.sampled_leaf_count = result.indices.size() / 3;
        source_preview_continue(options, 85);
        return result;
    }

    return {};
}

} // namespace

FacetRasterization rasterize_facets(const TriangleMesh            &mesh,
                                    const VolumeData              &data,
                                    unsigned int                   base_filament_id,
                                    const PaletteFilamentResolver &resolve_filament)
{
    FacetRasterization result;
    if (!resolve_filament || !data.validate(mesh).valid || base_filament_id == 0 || base_filament_id > 16)
        return result;

    std::vector<const TriangleBinding *> selected(mesh.its.indices.size(), nullptr);
    for (const TriangleBinding &binding : data.triangle_bindings) {
        if (binding.triangle_index >= selected.size() || binding.zone_index >= data.zones.size())
            continue;
        const Zone &zone = data.zones[binding.zone_index];
        if (!zone.enabled)
            continue;
        const TriangleBinding *current = selected[binding.triangle_index];
        if (!current || data.zones[current->zone_index].priority < zone.priority)
            selected[binding.triangle_index] = &binding;
    }

    std::vector<unsigned int> depths(selected.size(), 0);
    size_t total_samples = 0;
    size_t budget = 0;
    for (size_t triangle_idx = 0; triangle_idx < selected.size(); ++triangle_idx) {
        if (!selected[triangle_idx])
            continue;
        const Zone &zone = data.zones[selected[triangle_idx]->zone_index];
        depths[triangle_idx] = desired_depth(mesh, *selected[triangle_idx], zone);
        total_samples += leaf_count(depths[triangle_idx]);
        budget = std::max(budget, zone.max_facet_samples);
    }
    budget = std::max(budget, size_t(std::count_if(selected.begin(), selected.end(), [](const auto *binding) { return binding != nullptr; })));
    limit_depths_to_budget(depths, total_samples, budget);

    for (size_t triangle_idx = 0; triangle_idx < selected.size(); ++triangle_idx) {
        const TriangleBinding *binding = selected[triangle_idx];
        if (!binding)
            continue;
        const Zone &zone = data.zones[binding->zone_index];
        std::vector<unsigned int> ids;
        ids.reserve(leaf_count(depths[triangle_idx]));
        sample_leaves(data, *binding, zone,
                      {Vec3f(1.f, 0.f, 0.f), Vec3f(0.f, 1.f, 0.f), Vec3f(0.f, 0.f, 1.f)},
                      depths[triangle_idx], resolve_filament, base_filament_id, ids, result.unresolved_palette_entries);
        result.sampled_leaf_count += ids.size();
        if (std::any_of(ids.begin(), ids.end(), [base_filament_id](unsigned int id) { return id != base_filament_id; })) {
            size_t cursor = 0;
            result.facets.push_back({uint32_t(triangle_idx), encode_tree(ids, cursor, depths[triangle_idx], base_filament_id)});
        }
    }
    return result;
}

SourceColorRasterization rasterize_source_colors(const TriangleMesh &mesh,
                                                 const VolumeData   &data,
                                                 RenderMode          render_mode,
                                                 const RGBA         &fallback_color)
{
    return rasterize_source_colors(mesh, data, render_mode, fallback_color, SourceColorRasterizationOptions{});
}

SourceColorRasterization rasterize_source_colors(const TriangleMesh                    &mesh,
                                                 const VolumeData                      &data,
                                                 RenderMode                             render_mode,
                                                 const RGBA                            &fallback_color,
                                                 const SourceColorRasterizationOptions &options)
{
    SourceColorRasterization result;
    if (!data.validate(mesh).valid || mesh.its.indices.empty())
        return result;
    if (!source_preview_continue(options, 5))
        return result;

    std::vector<const TriangleBinding *> selected(mesh.its.indices.size(), nullptr);
    for (const TriangleBinding &binding : data.triangle_bindings) {
        if (binding.triangle_index >= selected.size() || binding.zone_index >= data.zones.size())
            continue;
        const Zone &zone = data.zones[binding.zone_index];
        if (!zone.enabled || zone.render_mode != render_mode)
            continue;
        const TriangleBinding *current = selected[binding.triangle_index];
        if (!current || data.zones[current->zone_index].priority < zone.priority)
            selected[binding.triangle_index] = &binding;
    }

    result.source_triangle_count = size_t(std::count_if(selected.begin(), selected.end(), [](const auto *binding) { return binding != nullptr; }));
    if (result.source_triangle_count == 0)
        return result;
    if (!source_preview_continue(options, 15))
        return {};

    if (options.max_leaf_triangles != 0 && mesh.its.indices.size() > options.max_leaf_triangles)
        return rasterize_dense_source_lod(mesh, data, selected, fallback_color, options, result.source_triangle_count);

    std::vector<unsigned int> depths(selected.size(), 0);
    size_t total_samples = mesh.its.indices.size() - result.source_triangle_count;
    size_t budget        = 0;
    for (size_t triangle_idx = 0; triangle_idx < selected.size(); ++triangle_idx) {
        if (!selected[triangle_idx])
            continue;
        const Zone &zone     = data.zones[selected[triangle_idx]->zone_index];
        depths[triangle_idx] = desired_depth(mesh, *selected[triangle_idx], zone);
        total_samples += leaf_count(depths[triangle_idx]);
        budget = std::max(budget, zone.max_facet_samples);
    }
    if (options.max_leaf_triangles != 0)
        budget = std::min(budget, options.max_leaf_triangles);
    // A complete preview needs at least one leaf per source triangle. Dense
    // meshes therefore use their existing geometry as the LOD and receive no
    // extra subdivision once they are above the preview cap.
    budget = std::max(budget, mesh.its.indices.size());
    limit_depths_to_budget(depths, total_samples, budget);
    if (!source_preview_continue(options, 25))
        return {};

    result.vertices.reserve(total_samples * 3);
    result.indices.reserve(total_samples * 3);
    const std::array<Vec3f, 3> root_barycentric{
        Vec3f(1.f, 0.f, 0.f), Vec3f(0.f, 1.f, 0.f), Vec3f(0.f, 0.f, 1.f)};
    int last_progress = 25;
    for (size_t triangle_idx = 0; triangle_idx < mesh.its.indices.size(); ++triangle_idx) {
        if ((triangle_idx & 0x7ffu) == 0u) {
            const int progress = 25 + int(60 * triangle_idx / mesh.its.indices.size());
            if (progress != last_progress) {
                if (!source_preview_continue(options, progress))
                    return {};
                last_progress = progress;
            } else if (options.cancelled && options.cancelled()) {
                return {};
            }
        }
        const stl_triangle_vertex_indices &indices = mesh.its.indices[triangle_idx];
        const Vec3f &a = mesh.its.vertices[size_t(indices[0])];
        const Vec3f &b = mesh.its.vertices[size_t(indices[1])];
        const Vec3f &c = mesh.its.vertices[size_t(indices[2])];
        Vec3f normal = (b - a).cross(c - a);
        const float normal_length = normal.norm();
        if (std::isfinite(normal_length) && normal_length > EPSILON)
            normal /= normal_length;
        else
            normal = Vec3f::UnitZ();
        emit_source_leaves(mesh, data, selected[triangle_idx], root_barycentric,
                           selected[triangle_idx] ? depths[triangle_idx] : 0u,
                           fallback_color, result, indices, normal);
    }
    source_preview_continue(options, 85);
    return result;
}

} // namespace Slic3r::ImageMap
