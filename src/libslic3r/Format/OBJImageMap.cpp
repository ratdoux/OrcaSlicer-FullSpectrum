#include "OBJImageMap.hpp"

#include "ImportedTexture.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sstream>

#include <boost/filesystem.hpp>

namespace Slic3r {
namespace {

constexpr unsigned int k_max_subdivision_depth = 6;

bool report_progress(const ObjImageMapProgressFn& progress_fn,
                     ObjImageMapProgressStage     stage,
                     size_t                       current,
                     size_t                       total)
{
    if (!progress_fn)
        return true;

    total   = std::max<size_t>(total, 1);
    current = std::min(current, total);
    const size_t stride = std::max<size_t>(total / 100, 1);
    if (current != 0 && current != total && current % stride != 0)
        return true;
    return progress_fn(stage, current, total);
}

bool cancelled(std::string* warning)
{
    if (warning != nullptr)
        *warning = "OBJ image-map processing was cancelled.";
    return false;
}

struct LoadedTexture
{
    std::vector<uint8_t> rgba;
    uint32_t             width{0};
    uint32_t             height{0};
};

float wrap_uv(float value)
{
    if (!std::isfinite(value))
        return 0.f;
    constexpr float epsilon = 1e-6f;
    if (value >= -epsilon && value <= 1.f + epsilon)
        return std::clamp(value, 0.f, 1.f);
    const float wrapped = value - std::floor(value);
    return wrapped < 0.f ? wrapped + 1.f : wrapped;
}

RGBA sample_texture(const LoadedTexture& texture, const Vec2f& uv, const RGBA& background)
{
    if (texture.width == 0 || texture.height == 0 || texture.rgba.size() < size_t(texture.width) * size_t(texture.height) * 4)
        return background;

    const float  x  = wrap_uv(uv.x()) * float(texture.width > 1 ? texture.width - 1 : 0);
    const float  y  = wrap_uv(uv.y()) * float(texture.height > 1 ? texture.height - 1 : 0);
    const size_t x0 = std::min<size_t>(size_t(std::floor(x)), size_t(texture.width - 1));
    const size_t y0 = std::min<size_t>(size_t(std::floor(y)), size_t(texture.height - 1));
    const size_t x1 = std::min<size_t>(x0 + 1, size_t(texture.width - 1));
    const size_t y1 = std::min<size_t>(y0 + 1, size_t(texture.height - 1));
    const float  tx = x - float(x0);
    const float  ty = y - float(y0);

    auto channel = [&texture](size_t sx, size_t sy, size_t component) {
        return float(texture.rgba[(sy * size_t(texture.width) + sx) * 4 + component]) / 255.f;
    };

    RGBA sampled{};
    for (size_t component = 0; component < 4; ++component) {
        const float top    = channel(x0, y0, component) + (channel(x1, y0, component) - channel(x0, y0, component)) * tx;
        const float bottom = channel(x0, y1, component) + (channel(x1, y1, component) - channel(x0, y1, component)) * tx;
        sampled[component] = std::clamp(top + (bottom - top) * ty, 0.f, 1.f);
    }

    const float alpha = sampled[3];
    return RGBA{std::clamp(sampled[0] * alpha + background[0] * (1.f - alpha), 0.f, 1.f),
                std::clamp(sampled[1] * alpha + background[1] * (1.f - alpha), 0.f, 1.f),
                std::clamp(sampled[2] * alpha + background[2] * (1.f - alpha), 0.f, 1.f), 1.f};
}

boost::filesystem::path resolve_texture_path(const std::string& obj_directory, const std::string& texture_name)
{
    const boost::filesystem::path raw(texture_name);
    if (raw.is_absolute() && boost::filesystem::exists(raw))
        return raw;

    const boost::filesystem::path directory(obj_directory);
    const boost::filesystem::path relative = directory / raw;
    if (boost::filesystem::exists(relative))
        return relative;

    const boost::filesystem::path filename_only = directory / raw.filename();
    return boost::filesystem::exists(filename_only) ? filename_only : relative;
}

unsigned int desired_subdivision_depth(const indexed_triangle_set& mesh,
                                       size_t                      triangle_idx,
                                       const std::array<Vec2f, 3>& uvs,
                                       const LoadedTexture&        texture,
                                       float                       target_sample_size_mm)
{
    const stl_triangle_vertex_indices& indices     = mesh.indices[triangle_idx];
    const Vec3f&                       p0          = mesh.vertices[size_t(indices[0])];
    const Vec3f&                       p1          = mesh.vertices[size_t(indices[1])];
    const Vec3f&                       p2          = mesh.vertices[size_t(indices[2])];
    const float                        max_edge_mm = std::max({(p1 - p0).norm(), (p2 - p1).norm(), (p0 - p2).norm()});
    const float                        safe_target = std::max(0.1f, target_sample_size_mm);
    const unsigned int physical_depth = max_edge_mm > safe_target ? unsigned(std::ceil(std::log2(max_edge_mm / safe_target))) : 0u;

    auto uv_pixel_distance = [&texture](const Vec2f& a, const Vec2f& b) {
        const float du = std::abs(a.x() - b.x()) * float(texture.width);
        const float dv = std::abs(a.y() - b.y()) * float(texture.height);
        return std::max(du, dv);
    };
    const float max_texture_edge = std::max(
        {uv_pixel_distance(uvs[0], uvs[1]), uv_pixel_distance(uvs[1], uvs[2]), uv_pixel_distance(uvs[2], uvs[0])});
    const unsigned int texture_depth = max_texture_edge > 1.f ? unsigned(std::ceil(std::log2(max_texture_edge))) : 0u;
    return std::min({physical_depth, texture_depth, k_max_subdivision_depth});
}

void append_leaf_colors(
    const std::array<Vec2f, 3>& uvs, unsigned int depth, const LoadedTexture& texture, const RGBA& background, std::vector<RGBA>& colors)
{
    if (depth == 0) {
        colors.emplace_back(sample_texture(texture, (uvs[0] + uvs[1] + uvs[2]) / 3.f, background));
        return;
    }

    const Vec2f m01 = (uvs[0] + uvs[1]) * 0.5f;
    const Vec2f m12 = (uvs[1] + uvs[2]) * 0.5f;
    const Vec2f m20 = (uvs[2] + uvs[0]) * 0.5f;
    append_leaf_colors({uvs[0], m01, m20}, depth - 1, texture, background, colors);
    append_leaf_colors({m01, uvs[1], m12}, depth - 1, texture, background, colors);
    append_leaf_colors({m12, uvs[2], m20}, depth - 1, texture, background, colors);
    append_leaf_colors({m01, m12, m20}, depth - 1, texture, background, colors);
}

std::string encode_leaf(unsigned char filament_id, unsigned char base_filament_id)
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

std::string encode_tree(const std::vector<unsigned char>& filament_ids, size_t& offset, unsigned int depth, unsigned char base_filament_id)
{
    if (depth == 0)
        return offset < filament_ids.size() ? encode_leaf(filament_ids[offset++], base_filament_id) : std::string();

    std::string encoded;
    for (size_t child = 0; child < 4; ++child)
        encoded += encode_tree(filament_ids, offset, depth - 1, base_filament_id);
    encoded += '3';
    return encoded;
}

} // namespace

size_t obj_image_map_leaf_count(unsigned int subdivision_depth)
{
    size_t count = 1;
    for (unsigned int depth = 0; depth < subdivision_depth; ++depth) {
        if (count > std::numeric_limits<size_t>::max() / 4)
            return 0;
        count *= 4;
    }
    return count;
}

bool build_obj_image_map_sample_plan(const TriangleMesh&    mesh,
                                     const ObjInfo&         obj_info,
                                     float                  target_sample_size_mm,
                                     size_t                 max_samples,
                                     ObjImageMapSamplePlan& out_plan,
                                     std::string*           warning,
                                     const ObjImageMapProgressFn& progress_fn)
{
    out_plan                                   = ObjImageMapSamplePlan{};
    const indexed_triangle_set& its            = mesh.its;
    const size_t                triangle_count = its.indices.size();
    out_plan.triangle_subdivision_depths.assign(triangle_count, int8_t(-1));
    if (triangle_count == 0 || obj_info.triangle_uvs.size() != triangle_count || obj_info.triangle_uvs_valid.size() != triangle_count ||
        obj_info.triangle_texture_files.size() != triangle_count)
        return false;

    std::set<std::string> texture_names;
    for (size_t triangle_idx = 0; triangle_idx < triangle_count; ++triangle_idx) {
        const std::string& texture_name = obj_info.triangle_texture_files[triangle_idx];
        if (!texture_name.empty())
            texture_names.emplace(texture_name);
        if (!report_progress(progress_fn, ObjImageMapProgressStage::DecodeTextures, triangle_idx + 1,
                             triangle_count + texture_names.size()))
            return cancelled(warning);
    }

    std::map<std::string, LoadedTexture> loaded;
    std::set<std::string>                failed;
    size_t                               texture_idx = 0;
    for (const std::string& texture_name : texture_names) {
        if (!report_progress(progress_fn, ObjImageMapProgressStage::DecodeTextures,
                             triangle_count + texture_idx, triangle_count + texture_names.size()))
            return cancelled(warning);
        const boost::filesystem::path path = resolve_texture_path(obj_info.obj_directory, texture_name);
        LoadedTexture                 texture;
        if (!decode_imported_texture_rgba_from_file(path.string(), texture.rgba, texture.width, texture.height)) {
            failed.emplace(texture_name);
        } else {
            loaded.emplace(texture_name, std::move(texture));
        }
        ++texture_idx;
    }
    if (!report_progress(progress_fn, ObjImageMapProgressStage::DecodeTextures, 1, 1))
        return cancelled(warning);
    out_plan.loaded_texture_count = loaded.size();

    std::vector<unsigned int> depths(triangle_count, 0);
    size_t                    sample_count = 0;
    for (size_t triangle_idx = 0; triangle_idx < triangle_count; ++triangle_idx) {
        if (!report_progress(progress_fn, ObjImageMapProgressStage::AnalyzeSurface, triangle_idx, triangle_count))
            return cancelled(warning);
        const std::string& texture_name = obj_info.triangle_texture_files[triangle_idx];
        const auto         texture_it   = loaded.find(texture_name);
        if (obj_info.triangle_uvs_valid[triangle_idx] == 0 || texture_it == loaded.end())
            continue;
        const stl_triangle_vertex_indices& indices = its.indices[triangle_idx];
        if (indices[0] < 0 || indices[1] < 0 || indices[2] < 0 || size_t(indices[0]) >= its.vertices.size() ||
            size_t(indices[1]) >= its.vertices.size() || size_t(indices[2]) >= its.vertices.size())
            continue;

        depths[triangle_idx] = desired_subdivision_depth(its, triangle_idx, obj_info.triangle_uvs[triangle_idx], texture_it->second,
                                                         target_sample_size_mm);
        out_plan.triangle_subdivision_depths[triangle_idx] = int8_t(depths[triangle_idx]);
        sample_count += obj_image_map_leaf_count(depths[triangle_idx]);
        ++out_plan.textured_triangle_count;
    }

    if (!report_progress(progress_fn, ObjImageMapProgressStage::AnalyzeSurface, triangle_count, triangle_count))
        return cancelled(warning);

    const size_t effective_budget = std::max(max_samples, out_plan.textured_triangle_count);
    // A subdivision depth is capped at six, so reducing the deepest triangles
    // in six descending passes is equivalent to repeatedly searching for the
    // largest saving, without the previous O(triangles * reductions) loop.
    for (unsigned int depth = k_max_subdivision_depth; depth > 0 && sample_count > effective_budget; --depth) {
        for (size_t triangle_idx = 0; triangle_idx < triangle_count; ++triangle_idx) {
            if (depths[triangle_idx] != depth)
                continue;
            const size_t current = obj_image_map_leaf_count(depths[triangle_idx]);
            const size_t reduced = obj_image_map_leaf_count(depths[triangle_idx] - 1);
            --depths[triangle_idx];
            out_plan.triangle_subdivision_depths[triangle_idx] = int8_t(depths[triangle_idx]);
            sample_count -= current - reduced;
            if (sample_count <= effective_budget)
                break;
        }
        if (!report_progress(progress_fn, ObjImageMapProgressStage::AllocateSamples,
                             k_max_subdivision_depth - depth + 1, k_max_subdivision_depth))
            return cancelled(warning);
    }
    if (!report_progress(progress_fn, ObjImageMapProgressStage::AllocateSamples, 1, 1))
        return cancelled(warning);

    out_plan.colors.reserve(sample_count);
    for (size_t triangle_idx = 0; triangle_idx < triangle_count; ++triangle_idx) {
        if (!report_progress(progress_fn, ObjImageMapProgressStage::SampleColors, triangle_idx, triangle_count))
            return cancelled(warning);
        if (out_plan.triangle_subdivision_depths[triangle_idx] < 0)
            continue;
        const auto texture_it = loaded.find(obj_info.triangle_texture_files[triangle_idx]);
        if (texture_it == loaded.end())
            continue;
        const RGBA background = triangle_idx < obj_info.face_colors.size() ? obj_info.face_colors[triangle_idx] : RGBA{1.f, 1.f, 1.f, 1.f};
        append_leaf_colors(obj_info.triangle_uvs[triangle_idx], depths[triangle_idx], texture_it->second, background, out_plan.colors);
    }

    if (!report_progress(progress_fn, ObjImageMapProgressStage::SampleColors, triangle_count, triangle_count))
        return cancelled(warning);

    if (warning != nullptr && !failed.empty()) {
        std::ostringstream stream;
        stream << "Unable to load " << failed.size() << " OBJ image texture" << (failed.size() == 1 ? "" : "s") << '.';
        *warning = stream.str();
    }
    return !out_plan.empty();
}

bool build_obj_image_map_sample_plan_with_texture(const TriangleMesh&    mesh,
                                                  const ObjInfo&         obj_info,
                                                  const std::string&     texture_file,
                                                  float                  target_sample_size_mm,
                                                  size_t                 max_samples,
                                                  ObjImageMapSamplePlan& out_plan,
                                                  std::string*           warning,
                                                  const ObjImageMapProgressFn& progress_fn)
{
    if (texture_file.empty()) {
        out_plan = ObjImageMapSamplePlan{};
        if (warning != nullptr)
            *warning = "No image texture was selected.";
        return false;
    }

    const size_t triangle_count = mesh.its.indices.size();
    if (obj_info.triangle_uvs.size() != triangle_count || obj_info.triangle_uvs_valid.size() != triangle_count) {
        out_plan = ObjImageMapSamplePlan{};
        if (warning != nullptr)
            *warning = "The OBJ does not contain usable UV coordinates for the selected texture.";
        return false;
    }

    ObjInfo texture_info = obj_info;
    texture_info.triangle_texture_files.assign(triangle_count, std::string());
    bool has_textured_triangle = false;
    for (size_t triangle_idx = 0; triangle_idx < triangle_count; ++triangle_idx) {
        if (texture_info.triangle_uvs_valid[triangle_idx] == 0)
            continue;
        texture_info.triangle_texture_files[triangle_idx] = texture_file;
        has_textured_triangle                             = true;
    }
    texture_info.has_uv_png = has_textured_triangle;

    if (!has_textured_triangle) {
        out_plan = ObjImageMapSamplePlan{};
        if (warning != nullptr)
            *warning = "The OBJ does not contain usable UV coordinates for the selected texture.";
        return false;
    }

    const bool built = build_obj_image_map_sample_plan(mesh, texture_info, target_sample_size_mm, max_samples, out_plan, warning, progress_fn);
    if (!built && warning != nullptr && warning->empty())
        *warning = "The selected image could not be used as an OBJ texture.";
    return built;
}

bool build_obj_image_map_volume_data(const TriangleMesh&       mesh,
                                     const ObjInfo&            obj_info,
                                     ObjColorImportSource      source,
                                     const std::string&        selected_texture_file,
                                     ImageMap::Zone            zone,
                                     ImageMap::VolumeData&     out_data,
                                     std::string*              warning,
                                     const ObjImageMapProgressFn& progress_fn)
{
    out_data = ImageMap::VolumeData{};
    out_data.topology_fingerprint = ImageMap::topology_fingerprint(mesh);
    if (zone.stable_id.empty())
        zone.stable_id = "obj-image-map-zone";
    if (zone.display_name.empty())
        zone.display_name = "OBJ image map";
    out_data.zones.emplace_back(std::move(zone));

    const indexed_triangle_set& its = mesh.its;
    if (its.indices.empty())
        return false;

    std::map<std::string, int32_t> texture_indices;
    auto ensure_texture = [&](const std::string& texture_file) -> int32_t {
        if (texture_file.empty())
            return -1;
        auto existing = texture_indices.find(texture_file);
        if (existing != texture_indices.end())
            return existing->second;

        const boost::filesystem::path path = selected_texture_file.empty() ?
                                                  resolve_texture_path(obj_info.obj_directory, texture_file) :
                                                  boost::filesystem::path(texture_file);
        LoadedTexture loaded;
        if (!decode_imported_texture_rgba_from_file(path.string(), loaded.rgba, loaded.width, loaded.height))
            return -1;

        ImageMap::TextureAsset asset;
        asset.stable_id   = "obj-texture-" + std::to_string(out_data.texture_assets.size() + 1);
        asset.display_name = path.filename().string();
        asset.width       = loaded.width;
        asset.height      = loaded.height;
        asset.rgba        = std::move(loaded.rgba);
        const int32_t index = int32_t(out_data.texture_assets.size());
        out_data.texture_assets.emplace_back(std::move(asset));
        texture_indices.emplace(texture_file, index);
        return index;
    };

    size_t skipped = 0;
    if (!report_progress(progress_fn, ObjImageMapProgressStage::StoreSource, 0, its.indices.size()))
        return cancelled(warning);
    for (size_t triangle_idx = 0; triangle_idx < its.indices.size(); ++triangle_idx) {
        if (!report_progress(progress_fn, ObjImageMapProgressStage::StoreSource, triangle_idx, its.indices.size()))
            return cancelled(warning);
        const stl_triangle_vertex_indices& indices = its.indices[triangle_idx];
        if (indices[0] < 0 || indices[1] < 0 || indices[2] < 0 || size_t(indices[0]) >= its.vertices.size() ||
            size_t(indices[1]) >= its.vertices.size() || size_t(indices[2]) >= its.vertices.size()) {
            ++skipped;
            continue;
        }

        ImageMap::TriangleBinding binding;
        binding.triangle_index = uint32_t(triangle_idx);
        binding.zone_index     = 0;
        const RGBA face_color = triangle_idx < obj_info.face_colors.size() ?
                                    obj_info.face_colors[triangle_idx] : RGBA{1.f, 1.f, 1.f, 1.f};
        binding.source.corner_colors = {face_color, face_color, face_color};

        if (source == ObjColorImportSource::ImageTexture) {
            if (triangle_idx >= obj_info.triangle_uvs.size() || triangle_idx >= obj_info.triangle_uvs_valid.size() ||
                obj_info.triangle_uvs_valid[triangle_idx] == 0) {
                ++skipped;
                continue;
            }
            const std::string texture_file = !selected_texture_file.empty() ? selected_texture_file :
                                             (triangle_idx < obj_info.triangle_texture_files.size() ?
                                                  obj_info.triangle_texture_files[triangle_idx] : std::string());
            const int32_t texture_index = ensure_texture(texture_file);
            if (texture_index < 0) {
                ++skipped;
                continue;
            }
            binding.source.kind                = ImageMap::SourceKind::Texture;
            binding.source.texture_asset_index = texture_index;
            binding.source.uvs                 = obj_info.triangle_uvs[triangle_idx];
        } else if (source == ObjColorImportSource::VertexColors) {
            if (obj_info.vertex_colors.size() != its.vertices.size()) {
                if (warning)
                    *warning = "OBJ vertex colours no longer align with the imported mesh topology.";
                return false;
            }
            binding.source.kind = ImageMap::SourceKind::VertexColors;
            for (size_t corner = 0; corner < 3; ++corner)
                binding.source.corner_colors[corner] = obj_info.vertex_colors[size_t(indices[corner])];
        } else {
            if (triangle_idx >= obj_info.face_colors.size()) {
                ++skipped;
                continue;
            }
            binding.source.kind = ImageMap::SourceKind::FaceColor;
        }
        out_data.triangle_bindings.emplace_back(std::move(binding));
    }

    if (warning && skipped > 0)
        *warning = "Skipped " + std::to_string(skipped) + " OBJ triangles without a usable image-map source.";
    const bool valid = !out_data.empty() && out_data.validate(mesh).valid;
    if (!report_progress(progress_fn, ObjImageMapProgressStage::StoreSource, its.indices.size(), its.indices.size()))
        return cancelled(warning);
    return valid;
}

std::string encode_obj_image_map_triangle_filaments(const std::vector<unsigned char>& filament_ids,
                                                    size_t                            offset,
                                                    unsigned int                      subdivision_depth,
                                                    unsigned char                     base_filament_id)
{
    const size_t leaf_count = obj_image_map_leaf_count(subdivision_depth);
    if (leaf_count == 0 || offset > filament_ids.size() || leaf_count > filament_ids.size() - offset)
        return {};
    if (std::all_of(filament_ids.begin() + offset, filament_ids.begin() + offset + leaf_count,
                    [base_filament_id](unsigned char id) { return id == base_filament_id; }))
        return {};

    size_t cursor = offset;
    return encode_tree(filament_ids, cursor, subdivision_depth, base_filament_id);
}

} // namespace Slic3r
