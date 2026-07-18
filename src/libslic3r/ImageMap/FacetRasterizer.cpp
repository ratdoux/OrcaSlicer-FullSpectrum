#include "FacetRasterizer.hpp"

#include "Sampling.hpp"
#include "../TriangleMesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

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
    while (total_samples > budget) {
        size_t best = selected.size();
        size_t saving = 0;
        for (size_t idx = 0; idx < depths.size(); ++idx) {
            if (!selected[idx] || depths[idx] == 0)
                continue;
            const size_t candidate = leaf_count(depths[idx]) - leaf_count(depths[idx] - 1);
            if (candidate > saving) {
                best = idx;
                saving = candidate;
            }
        }
        if (best == selected.size())
            break;
        --depths[best];
        total_samples -= saving;
    }

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

} // namespace Slic3r::ImageMap
