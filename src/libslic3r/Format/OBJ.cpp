#include "../libslic3r.h"
#include "../Model.hpp"
#include "../TriangleMesh.hpp"

#include "OBJ.hpp"
#include "objparser.hpp"

#include <algorithm>
#include <string>

#include <boost/log/trivial.hpp>

#ifdef _WIN32
#define DIR_SEPARATOR '\\'
#else
#define DIR_SEPARATOR '/'
#endif

//Translation
#include "I18N.hpp"
#define _L(s) Slic3r::I18N::translate(s)

namespace Slic3r {

bool load_obj(const char *path, TriangleMesh *meshptr, ObjInfo& obj_info, std::string &message)
{
    if (meshptr == nullptr)
        return false;
    // Parse the OBJ file.
    ObjParser::ObjData data;
    ObjParser::MtlData mtl_data;
    std::unordered_map<const ObjParser::ObjNewMtl *, std::string> material_texture_paths;
    if (! ObjParser::objparse(path, data)) {
        BOOST_LOG_TRIVIAL(error) << "load_obj: failed to parse " << path;
        message = _L("load_obj: failed to parse");
        return false;
    }
    const boost::filesystem::path obj_path(path);
    obj_info.obj_directory = obj_path.parent_path().string();
    bool exist_mtl = false;
    if (data.mtllibs.size() > 0) { // read mtl
        for (const std::string &mtl_name : data.mtllibs) {
            if (mtl_name.empty()) {
                continue;
            }
            exist_mtl = true;
            const boost::filesystem::path raw_mtl_path(mtl_name);
            const boost::filesystem::path mtl_path = raw_mtl_path.is_absolute() && boost::filesystem::exists(raw_mtl_path) ?
                raw_mtl_path : obj_path.parent_path() / raw_mtl_path;
            const std::string mtl_path_string = mtl_path.string();
            if (boost::filesystem::exists(mtl_path)) {
                if (!ObjParser::mtlparse(mtl_path_string.c_str(), mtl_data)) {
                    BOOST_LOG_TRIVIAL(error) << "load_obj:load_mtl: failed to parse " << mtl_path_string;
                    message = _L("load mtl in obj: failed to parse");
                    return false;
                }
                for (const auto &material_entry : mtl_data.new_mtl_unmap) {
                    const std::shared_ptr<ObjParser::ObjNewMtl> &material = material_entry.second;
                    if (!material || material->map_Kd.empty() || material_texture_paths.count(material.get()) != 0)
                        continue;
                    const boost::filesystem::path raw_texture_path(material->map_Kd);
                    const boost::filesystem::path texture_path = raw_texture_path.is_absolute() ?
                        raw_texture_path : mtl_path.parent_path() / raw_texture_path;
                    material_texture_paths.emplace(material.get(), texture_path.lexically_normal().string());
                }
            } else {
                BOOST_LOG_TRIVIAL(error) << "load_obj: failed to load mtl_path:" << mtl_path_string;
            }
        }
    }
    // Count the faces and verify, that all faces are triangular.
    size_t num_faces = 0;
    size_t num_quads = 0;
    for (size_t i = 0; i < data.vertices.size(); ++ i) {
        // Find the end of face.
        size_t j = i;
        for (; j < data.vertices.size() && data.vertices[j].coordIdx != -1; ++ j) ;
        if (size_t num_face_vertices = j - i; num_face_vertices > 0) {
            if (num_face_vertices > 4) {
                // Non-triangular and non-quad faces are not supported as of now.
                BOOST_LOG_TRIVIAL(error) << "load_obj: failed to parse " << path << ". The file contains polygons with more than 4 vertices.";
                message = _L("The file contains polygons with more than 4 vertices.");
                return false;
            } else if (num_face_vertices < 3) {
                // Non-triangular and non-quad faces are not supported as of now.
                BOOST_LOG_TRIVIAL(error) << "load_obj: failed to parse " << path << ". The file contains polygons with less than 2 vertices.";
                message = _L("The file contains polygons with less than 2 vertices.");
                return false;
            }
            if (num_face_vertices == 4)
                ++ num_quads;
            ++ num_faces;
            i = j;
        }
    }
    // Convert ObjData into indexed triangle set.
    indexed_triangle_set its;
    size_t               num_vertices = data.coordinates.size() / OBJ_VERTEX_LENGTH;
    its.vertices.reserve(num_vertices);
    its.indices.reserve(num_faces + num_quads);
    obj_info.triangle_uvs.reserve(num_faces + num_quads);
    obj_info.triangle_uvs_valid.reserve(num_faces + num_quads);
    obj_info.triangle_texture_files.reserve(num_faces + num_quads);
    if (exist_mtl) {
        obj_info.is_single_mtl = data.usemtls.size() == 1 && mtl_data.new_mtl_unmap.size() == 1;
        obj_info.face_colors.reserve(num_faces + num_quads);
    }
    bool has_color = data.has_vertex_color;
    for (size_t i = 0; i < num_vertices; ++ i) {
        size_t j = i * OBJ_VERTEX_LENGTH;
        its.vertices.emplace_back(data.coordinates[j], data.coordinates[j + 1], data.coordinates[j + 2]);
        if (data.has_vertex_color) {
            RGBA color{std::clamp(data.coordinates[j + 3], 0.f, 1.f), std::clamp(data.coordinates[j + 4], 0.f, 1.f), std::clamp(data.coordinates[j + 5], 0.f, 1.f),
                       std::clamp(data.coordinates[j + 6], 0.f, 1.f)};
            obj_info.vertex_colors.emplace_back(color);
        }
    }
    auto read_uv = [&data](int uv_idx, Vec2f &out_uv) {
        if (uv_idx < 0)
            return false;
        const size_t offset = size_t(uv_idx) * 2;
        if (offset + 1 >= data.textureCoordinates.size())
            return false;
        out_uv = Vec2f(data.textureCoordinates[offset], data.textureCoordinates[offset + 1]);
        return true;
    };
    auto material_for_face = [&data, &mtl_data](int generated_face_index) -> const ObjParser::ObjNewMtl * {
        for (const ObjParser::ObjUseMtl &use_mtl : data.usemtls) {
            if (generated_face_index < use_mtl.face_start ||
                (use_mtl.face_end >= 0 && generated_face_index > use_mtl.face_end))
                continue;
            const auto material_it = mtl_data.new_mtl_unmap.find(use_mtl.name);
            if (material_it != mtl_data.new_mtl_unmap.end() && material_it->second)
                return material_it->second.get();
        }
        return nullptr;
    };
    auto material_color = [](const ObjParser::ObjNewMtl *material) {
        if (material == nullptr)
            return UNDEFINE_COLOR;
        RGBA color{};
        bool merge_ambient = true;
        for (size_t component = 0; component < 3; ++component)
            merge_ambient &= material->Ka[component] + material->Kd[component] <= 1.f;
        for (size_t component = 0; component < 3; ++component)
            color[component] = std::clamp(merge_ambient ? material->Ka[component] + material->Kd[component] : material->Kd[component],
                                          0.f,
                                          1.f);
        color[3] = std::clamp(material->Tr, 0.f, 1.f);
        return color;
    };
    auto append_triangle_metadata = [&obj_info, &read_uv, &material_texture_paths](int generated_face_index,
                                                                                   const std::array<int, 3> &uv_indices,
                                                                                   const ObjParser::ObjNewMtl *material) {
        std::array<Vec2f, 3> triangle_uv{Vec2f::Zero(), Vec2f::Zero(), Vec2f::Zero()};
        const bool valid_uv = read_uv(uv_indices[0], triangle_uv[0]) &&
                              read_uv(uv_indices[1], triangle_uv[1]) &&
                              read_uv(uv_indices[2], triangle_uv[2]);
        obj_info.triangle_uvs.emplace_back(triangle_uv);
        obj_info.triangle_uvs_valid.emplace_back(valid_uv ? uint8_t(1) : uint8_t(0));

        const auto texture_path_it = material_texture_paths.find(material);
        const std::string texture_name = texture_path_it != material_texture_paths.end() ?
            texture_path_it->second : (material != nullptr ? material->map_Kd : std::string());
        obj_info.triangle_texture_files.emplace_back(texture_name);
        if (!texture_name.empty()) {
            obj_info.has_uv_png = true;
            obj_info.pngs.emplace(texture_name, false);
            obj_info.uv_map_pngs[generated_face_index] = texture_name;
            if (valid_uv)
                obj_info.uvs.emplace_back(triangle_uv);
        }
    };

    int indices[ONE_FACE_SIZE];
    int uv_indices[ONE_FACE_SIZE];
    for (size_t i = 0; i < data.vertices.size();)
        if (data.vertices[i].coordIdx == -1)
            ++ i;
        else {
            int cnt = 0;
            while (i < data.vertices.size())
                if (const ObjParser::ObjVertex &vertex = data.vertices[i ++]; vertex.coordIdx == -1) {
                    break;
                } else {
                    assert(cnt < OBJ_VERTEX_LENGTH);
                    if (vertex.coordIdx < 0 || vertex.coordIdx >= int(its.vertices.size())) {
                        BOOST_LOG_TRIVIAL(error) << "load_obj: failed to parse " << path << ". The file contains invalid vertex index.";
                        message = _L("The file contains invalid vertex index.");
                        return false;
                    }
                    indices[cnt] = vertex.coordIdx;
                    uv_indices[cnt] = vertex.textureCoordIdx;
                    cnt++;
                }
            if (cnt) {
                assert(cnt == 3 || cnt == 4);
                const int generated_face_index = int(its.indices.size());
                const ObjParser::ObjNewMtl *material = material_for_face(generated_face_index);
                const RGBA face_color = material_color(material);
                // Insert one or two faces (triangulate a quad).
                its.indices.emplace_back(indices[0], indices[1], indices[2]);
                int face_index = int(its.indices.size()) - 1;
                append_triangle_metadata(face_index, {uv_indices[0], uv_indices[1], uv_indices[2]}, material);
                if (exist_mtl)
                    obj_info.face_colors.emplace_back(face_color);
                if (cnt == 4) {
                    its.indices.emplace_back(indices[0], indices[2], indices[3]);
                    face_index = int(its.indices.size()) - 1;
                    append_triangle_metadata(face_index, {uv_indices[0], uv_indices[2], uv_indices[3]}, material);
                    if (exist_mtl)
                        obj_info.face_colors.emplace_back(face_color);
                }
            }
        }

    *meshptr = TriangleMesh(std::move(its));
    if (meshptr->empty()) {
        BOOST_LOG_TRIVIAL(error) << "load_obj: This OBJ file couldn't be read because it's empty. " << path;
        message = _L("This OBJ file couldn't be read because it's empty.");
        return false;
    }
    if (meshptr->volume() < 0) {
        meshptr->flip_triangles();
        for (std::array<Vec2f, 3> &triangle_uv : obj_info.triangle_uvs)
            std::swap(triangle_uv[1], triangle_uv[2]);
    }
    return true;
}

bool load_obj(const char *path, Model *model, ObjInfo& obj_info, std::string &message, const char *object_name_in)
{
    TriangleMesh mesh;

    bool ret = load_obj(path, &mesh, obj_info, message);

    if (ret) {
        std::string  object_name;
        if (object_name_in == nullptr) {
            const char *last_slash = strrchr(path, DIR_SEPARATOR);
            object_name.assign((last_slash == nullptr) ? path : last_slash + 1);
        } else
           object_name.assign(object_name_in);
        model->add_object(object_name.c_str(), path, std::move(mesh));
    }

    return ret;
}

bool store_obj(const char *path, TriangleMesh *mesh)
{
    //FIXME returning false even if write failed.
    mesh->WriteOBJFile(path);
    return true;
}

bool store_obj(const char *path, ModelObject *model_object)
{
    TriangleMesh mesh = model_object->mesh();
    return store_obj(path, &mesh);
}

bool store_obj(const char *path, Model *model)
{
    TriangleMesh mesh = model->mesh();
    return store_obj(path, &mesh);
}

}; // namespace Slic3r
