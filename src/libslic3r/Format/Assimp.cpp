#include "Assimp.hpp"

#include "ImportedTexture.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include <assimp/Importer.hpp>
#include <assimp/ProgressHandler.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/texture.h>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace {

constexpr std::array<const char*, 6> k_supported_extensions = {".fbx", ".gltf", ".glb", ".dae", ".ply", ".3ds"};

std::string source_format_name(const std::string& path)
{
    std::string extension = boost::filesystem::path(path).extension().string();
    if (!extension.empty() && extension.front() == '.')
        extension.erase(extension.begin());
    boost::algorithm::to_upper(extension);
    return extension.empty() ? "mesh" : extension;
}

class ImportProgressHandler final : public Assimp::ProgressHandler
{
public:
    explicit ImportProgressHandler(const ObjImageMapProgressFn& progress_fn) : m_progress_fn(progress_fn) {}

    bool Update(float percentage) override
    {
        if (!m_progress_fn)
            return true;
        const float clamped    = std::isfinite(percentage) ? std::clamp(percentage, 0.f, 1.f) : 0.f;
        const bool  keep_going = m_progress_fn(ObjImageMapProgressStage::ParseGeometry, size_t(clamped * 8000.f), 10000);
        m_cancelled |= !keep_going;
        return keep_going;
    }

    bool cancelled() const { return m_cancelled; }

private:
    ObjImageMapProgressFn m_progress_fn;
    bool                  m_cancelled{false};
};

struct MaterialInfo
{
    RGBA        color{1.f, 1.f, 1.f, 1.f};
    bool        has_color{false};
    std::string texture_file;
    unsigned    uv_channel{0};
};

RGBA to_rgba(const aiColor4D& color)
{
    return {std::clamp(color.r, 0.f, 1.f), std::clamp(color.g, 0.f, 1.f), std::clamp(color.b, 0.f, 1.f), std::clamp(color.a, 0.f, 1.f)};
}

bool decode_embedded_texture(const aiTexture& texture, const std::string& texture_ref, ObjEmbeddedTexture& out_texture)
{
    out_texture              = ObjEmbeddedTexture{};
    out_texture.display_name = texture.mFilename.length > 0 ? texture.mFilename.C_Str() : texture_ref;
    if (texture.mHeight > 0) {
        const size_t width  = texture.mWidth;
        const size_t height = texture.mHeight;
        if (width == 0 || height == 0 || width > std::numeric_limits<size_t>::max() / height ||
            width * height > std::numeric_limits<size_t>::max() / 4)
            return false;
        out_texture.width  = texture.mWidth;
        out_texture.height = texture.mHeight;
        out_texture.rgba.resize(width * height * 4);
        for (size_t pixel_idx = 0; pixel_idx < width * height; ++pixel_idx) {
            const aiTexel& pixel                = texture.pcData[pixel_idx];
            out_texture.rgba[pixel_idx * 4]     = pixel.r;
            out_texture.rgba[pixel_idx * 4 + 1] = pixel.g;
            out_texture.rgba[pixel_idx * 4 + 2] = pixel.b;
            out_texture.rgba[pixel_idx * 4 + 3] = pixel.a;
        }
        return true;
    }

    if (texture.mWidth == 0 || texture.pcData == nullptr)
        return false;
    std::string format_hint(texture.achFormatHint,
                            std::find(texture.achFormatHint, texture.achFormatHint + sizeof(texture.achFormatHint), '\0'));
    if (!format_hint.empty() && format_hint.front() != '.')
        format_hint.insert(format_hint.begin(), '.');
    return decode_imported_texture_rgba_from_memory(reinterpret_cast<const uint8_t*>(texture.pcData), texture.mWidth, format_hint,
                                                    out_texture.rgba, out_texture.width, out_texture.height);
}

bool ensure_embedded_texture(const aiScene& scene, const std::string& texture_ref, ObjInfo& color_info)
{
    if (color_info.embedded_textures.count(texture_ref) != 0)
        return true;
    const aiTexture* texture = scene.GetEmbeddedTexture(texture_ref.c_str());
    if (texture == nullptr)
        return false;

    ObjEmbeddedTexture decoded;
    if (!decode_embedded_texture(*texture, texture_ref, decoded)) {
        BOOST_LOG_TRIVIAL(warning) << "Unable to decode embedded " << color_info.source_format << " texture " << texture_ref;
        return false;
    }
    color_info.embedded_textures.emplace(texture_ref, std::move(decoded));
    return true;
}

MaterialInfo read_material(const aiScene& scene, unsigned material_index, ObjInfo& color_info)
{
    MaterialInfo result;
    if (material_index >= scene.mNumMaterials || scene.mMaterials[material_index] == nullptr)
        return result;

    const aiMaterial& material = *scene.mMaterials[material_index];
    aiString          material_name;
    const bool        has_material_name   = material.Get(AI_MATKEY_NAME, material_name) == AI_SUCCESS && material_name.length > 0;
    const bool        is_default_material = has_material_name && std::string(material_name.C_Str()) == AI_DEFAULT_MATERIAL_NAME;
    aiColor4D         color;
    if (material.Get(AI_MATKEY_BASE_COLOR, color) == AI_SUCCESS || material.Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS) {
        result.color                 = to_rgba(color);
        const bool is_implicit_white = !has_material_name && result.color[0] >= 1.f - EPSILON && result.color[1] >= 1.f - EPSILON &&
                                       result.color[2] >= 1.f - EPSILON && result.color[3] >= 1.f - EPSILON;
        result.has_color = !is_default_material && !is_implicit_white;
    }
    float opacity = 1.f;
    if (!is_default_material && material.Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) {
        result.color[3] = std::clamp(result.color[3] * opacity, 0.f, 1.f);
        result.has_color |= has_material_name || opacity < 1.f - EPSILON;
    }

    aiString texture_path;
    unsigned uv_channel     = 0;
    aiReturn texture_result = material.GetTexture(aiTextureType_BASE_COLOR, 0, &texture_path, nullptr, &uv_channel);
    if (texture_result != AI_SUCCESS)
        texture_result = material.GetTexture(aiTextureType_DIFFUSE, 0, &texture_path, nullptr, &uv_channel);
    if (texture_result == AI_SUCCESS && texture_path.length > 0) {
        const std::string texture_ref = texture_path.C_Str();
        if (scene.GetEmbeddedTexture(texture_ref.c_str()) == nullptr || ensure_embedded_texture(scene, texture_ref, color_info)) {
            result.texture_file = texture_ref;
            result.uv_channel   = std::min<unsigned>(uv_channel, AI_MAX_NUMBER_OF_TEXTURECOORDS - 1);
        }
    }
    return result;
}

bool uses_metric_scene_units(const std::string& path)
{
    return boost::algorithm::iends_with(path, ".gltf") || boost::algorithm::iends_with(path, ".glb") ||
           boost::algorithm::iends_with(path, ".dae");
}

float scene_to_millimeters(const std::string& path)
{
    // Assimp converts FBX coordinates to centimetres and glTF/Collada to metres.
    if (boost::algorithm::iends_with(path, ".fbx"))
        return 10.f;
    return uses_metric_scene_units(path) ? 1000.f : 1.f;
}

bool converts_to_y_up(const std::string& path)
{
    // PLY has no mandated axis convention and is treated like STL/OBJ (Z-up).
    return !boost::algorithm::iends_with(path, ".ply");
}

Vec3f printable_position(const aiVector3D& vertex, float scale, bool y_up)
{
    const Vec3f scaled(vertex.x * scale, vertex.y * scale, vertex.z * scale);
    return y_up ? Vec3f(scaled.x(), -scaled.z(), scaled.y()) : scaled;
}

bool build_mesh(const aiScene&               scene,
                const std::string&           input_path,
                ObjInfo&                     color_info,
                TriangleMesh&                out_mesh,
                std::string&                 message,
                const ObjImageMapProgressFn& progress_fn)
{
    indexed_triangle_set its;
    size_t               vertex_count   = 0;
    size_t               triangle_count = 0;
    for (unsigned mesh_idx = 0; mesh_idx < scene.mNumMeshes; ++mesh_idx) {
        const aiMesh* mesh = scene.mMeshes[mesh_idx];
        if (mesh == nullptr)
            continue;
        const size_t mesh_triangles = std::count_if(mesh->mFaces, mesh->mFaces + mesh->mNumFaces,
                                                    [](const aiFace& face) { return face.mNumIndices == 3; });
        if (mesh_triangles == 0)
            continue;
        vertex_count += mesh->mNumVertices;
        triangle_count += mesh_triangles;
    }
    if (vertex_count == 0 || triangle_count == 0 || vertex_count > size_t(std::numeric_limits<int>::max())) {
        message = "The file does not contain a printable triangle mesh.";
        return false;
    }

    its.vertices.reserve(vertex_count);
    its.indices.reserve(triangle_count);
    color_info.vertex_colors.reserve(vertex_count);
    color_info.face_colors.reserve(triangle_count);
    color_info.triangle_uvs.reserve(triangle_count);
    color_info.triangle_uvs_valid.reserve(triangle_count);
    color_info.triangle_texture_files.reserve(triangle_count);

    std::vector<MaterialInfo> materials(scene.mNumMaterials);
    for (unsigned material_idx = 0; material_idx < scene.mNumMaterials; ++material_idx)
        materials[material_idx] = read_material(scene, material_idx, color_info);

    const float        scale                = scene_to_millimeters(input_path);
    const bool         y_up                 = converts_to_y_up(input_path);
    bool               all_vertices_colored = true;
    bool               any_material_color   = false;
    std::set<unsigned> used_materials;
    size_t             converted_meshes = 0;
    for (unsigned mesh_idx = 0; mesh_idx < scene.mNumMeshes; ++mesh_idx) {
        const aiMesh* mesh = scene.mMeshes[mesh_idx];
        if (mesh == nullptr)
            continue;
        const size_t mesh_triangles = std::count_if(mesh->mFaces, mesh->mFaces + mesh->mNumFaces,
                                                    [](const aiFace& face) { return face.mNumIndices == 3; });
        if (mesh_triangles == 0)
            continue;
        if (its.vertices.size() > size_t(std::numeric_limits<int>::max()) - mesh->mNumVertices) {
            message = "The imported scene contains too many vertices.";
            return false;
        }

        const int  vertex_offset     = int(its.vertices.size());
        const bool has_vertex_colors = mesh->HasVertexColors(0);
        all_vertices_colored &= has_vertex_colors;
        for (unsigned vertex_idx = 0; vertex_idx < mesh->mNumVertices; ++vertex_idx) {
            its.vertices.emplace_back(printable_position(mesh->mVertices[vertex_idx], scale, y_up));
            color_info.vertex_colors.emplace_back(has_vertex_colors ? to_rgba(mesh->mColors[0][vertex_idx]) : RGBA{1.f, 1.f, 1.f, 1.f});
        }

        const MaterialInfo material = mesh->mMaterialIndex < materials.size() ? materials[mesh->mMaterialIndex] : MaterialInfo{};
        used_materials.emplace(mesh->mMaterialIndex);
        any_material_color |= material.has_color;
        const bool has_uvs     = mesh->HasTextureCoords(material.uv_channel);
        const bool has_texture = has_uvs && !material.texture_file.empty();
        for (unsigned face_idx = 0; face_idx < mesh->mNumFaces; ++face_idx) {
            const aiFace& face = mesh->mFaces[face_idx];
            if (face.mNumIndices != 3)
                continue;
            if (face.mIndices[0] >= mesh->mNumVertices || face.mIndices[1] >= mesh->mNumVertices || face.mIndices[2] >= mesh->mNumVertices) {
                message = "The imported scene contains an invalid triangle index.";
                return false;
            }
            its.indices.emplace_back(vertex_offset + int(face.mIndices[0]), vertex_offset + int(face.mIndices[1]),
                                     vertex_offset + int(face.mIndices[2]));
            color_info.face_colors.emplace_back(material.color);

            std::array<Vec2f, 3> triangle_uv{Vec2f::Zero(), Vec2f::Zero(), Vec2f::Zero()};
            if (has_uvs) {
                for (size_t corner = 0; corner < 3; ++corner) {
                    const aiVector3D& uv = mesh->mTextureCoords[material.uv_channel][face.mIndices[corner]];
                    triangle_uv[corner]  = Vec2f(uv.x, uv.y);
                }
            }
            color_info.triangle_uvs.emplace_back(triangle_uv);
            color_info.triangle_uvs_valid.emplace_back(has_uvs ? uint8_t(1) : uint8_t(0));
            color_info.triangle_texture_files.emplace_back(has_texture ? material.texture_file : std::string());
            color_info.has_uv_png |= has_texture;
        }

        ++converted_meshes;
        if (progress_fn && !progress_fn(ObjImageMapProgressStage::ParseGeometry, 8000 + converted_meshes * 2000 / scene.mNumMeshes, 10000))
            return false;
    }

    if (!all_vertices_colored)
        color_info.vertex_colors.clear();
    if (!any_material_color)
        color_info.face_colors.clear();
    color_info.is_single_mtl = any_material_color && used_materials.size() == 1;

    out_mesh = TriangleMesh(std::move(its));
    if (out_mesh.empty()) {
        message = "The file does not contain a printable triangle mesh.";
        return false;
    }
    if (out_mesh.volume() < 0) {
        out_mesh.flip_triangles();
        for (std::array<Vec2f, 3>& triangle_uv : color_info.triangle_uvs)
            std::swap(triangle_uv[1], triangle_uv[2]);
    }
    return !progress_fn || progress_fn(ObjImageMapProgressStage::ParseGeometry, 10000, 10000);
}

} // namespace

bool is_assimp_color_mesh_file(const std::string& path)
{
    return std::any_of(k_supported_extensions.begin(), k_supported_extensions.end(),
                       [&path](const char* extension) { return boost::algorithm::iends_with(path, extension); });
}

bool load_assimp_color_mesh(const char*                  path,
                            Model*                       model,
                            ObjInfo&                     color_info,
                            std::string&                 message,
                            const char*                  object_name_in,
                            const ObjImageMapProgressFn& progress_fn)
{
    if (path == nullptr || model == nullptr)
        return false;

    color_info               = ObjInfo{};
    color_info.obj_directory = boost::filesystem::path(path).parent_path().string();
    color_info.source_format = source_format_name(path);

    Assimp::Importer importer;
    auto*            progress_handler = new ImportProgressHandler(progress_fn);
    importer.SetProgressHandler(progress_handler);
    constexpr unsigned flags = aiProcess_Triangulate | aiProcess_SortByPType | aiProcess_ValidateDataStructure | aiProcess_FindInvalidData |
                               aiProcess_GenUVCoords | aiProcess_TransformUVCoords | aiProcess_PreTransformVertices;
    const aiScene* scene = importer.ReadFile(path, flags);
    if (scene == nullptr) {
        if (!progress_handler->cancelled())
            message = std::string("Unable to import ") + color_info.source_format + " file: " + importer.GetErrorString();
        return false;
    }

    TriangleMesh mesh;
    if (!build_mesh(*scene, path, color_info, mesh, message, progress_fn))
        return false;

    const std::string object_name = object_name_in != nullptr ? object_name_in : boost::filesystem::path(path).filename().string();
    model->add_object(object_name.c_str(), path, std::move(mesh));
    return true;
}

} // namespace Slic3r
