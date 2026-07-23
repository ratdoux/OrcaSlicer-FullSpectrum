#include "../libslic3r.h"
#include "../Model.hpp"
#include "../TriangleMesh.hpp"
#include "../Triangulation.hpp"

#include "OBJ.hpp"
#include "objparser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

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

namespace {

std::vector<std::string> tokenize_mtl_map_path(const std::string& value)
{
    std::vector<std::string> tokens;
    std::string              token;
    char                     quote = 0;
    for (const char ch : value) {
        if (quote != 0) {
            if (ch == quote)
                quote = 0;
            else
                token += ch;
        } else if (ch == '\'' || ch == '"') {
            quote = ch;
        } else if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!token.empty()) {
                tokens.emplace_back(std::move(token));
                token.clear();
            }
        } else {
            token += ch;
        }
    }
    if (!token.empty())
        tokens.emplace_back(std::move(token));
    return tokens;
}

bool is_mtl_number(const std::string& value)
{
    if (value.empty())
        return false;
    char* end = nullptr;
    std::strtod(value.c_str(), &end);
    return end != value.c_str() && end != nullptr && *end == '\0';
}

std::string mtl_map_texture_filename(const std::string& map_value)
{
    const std::vector<std::string> tokens = tokenize_mtl_map_path(map_value);
    size_t                         index  = 0;
    while (index < tokens.size() && !tokens[index].empty() && tokens[index][0] == '-') {
        std::string option = tokens[index++];
        std::transform(option.begin(), option.end(), option.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        if (option == "-o" || option == "-s" || option == "-t") {
            for (size_t count = 0; count < 3 && index < tokens.size() && is_mtl_number(tokens[index]); ++count)
                ++index;
        } else {
            const size_t argument_count = option == "-mm" ? 2 : 1;
            index                       = std::min(tokens.size(), index + argument_count);
        }
    }
    if (index >= tokens.size())
        return {};

    std::ostringstream filename;
    for (; index < tokens.size(); ++index) {
        if (filename.tellp() > 0)
            filename << ' ';
        filename << tokens[index];
    }
    return filename.str();
}

} // namespace

bool load_obj(const char *path,
              TriangleMesh *meshptr,
              ObjInfo& obj_info,
              std::string &message,
              const ObjImageMapProgressFn& progress_fn)
{
    if (meshptr == nullptr)
        return false;
    const boost::filesystem::path obj_path(path);
    boost::system::error_code     file_size_error;
    const uintmax_t               file_size = boost::filesystem::file_size(obj_path, file_size_error);
    size_t                        last_progress_percent = std::numeric_limits<size_t>::max();
    auto report_geometry_progress = [&](size_t current, size_t total = 10000) {
        if (!progress_fn)
            return true;
        total   = std::max<size_t>(total, 1);
        current = std::min(current, total);
        const size_t percent = current * 100 / total;
        if (percent == last_progress_percent && current != total)
            return true;
        last_progress_percent = percent;
        return progress_fn(ObjImageMapProgressStage::ParseGeometry, current, total);
    };
    if (!report_geometry_progress(0))
        return false;

    // Parse the OBJ file.
    ObjParser::ObjData data;
    ObjParser::MtlData mtl_data;
    std::unordered_map<const ObjParser::ObjNewMtl *, std::string> material_texture_paths;
    bool parse_cancelled = false;
    const ObjParser::ObjParseProgressFn parse_progress_fn = progress_fn ?
        ObjParser::ObjParseProgressFn([&](size_t bytes_read) {
            const size_t parse_units = !file_size_error && file_size > 0 ?
                                           size_t(std::min<uintmax_t>(bytes_read, file_size) * 6000 / file_size) : 0;
            const bool keep_going = report_geometry_progress(parse_units);
            parse_cancelled |= !keep_going;
            return keep_going;
        }) : ObjParser::ObjParseProgressFn{};
    if (!ObjParser::objparse(path, data, parse_progress_fn)) {
        if (parse_cancelled)
            return false;
        BOOST_LOG_TRIVIAL(error) << "load_obj: failed to parse " << path;
        message = _L("load_obj: failed to parse");
        return false;
    }
    if (!report_geometry_progress(6000))
        return false;
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
                    const std::string texture_filename = mtl_map_texture_filename(material->map_Kd);
                    if (texture_filename.empty())
                        continue;
                    const boost::filesystem::path raw_texture_path(texture_filename);
                    const boost::filesystem::path texture_path = raw_texture_path.is_absolute() ?
                        raw_texture_path : mtl_path.parent_path() / raw_texture_path;
                    material_texture_paths.emplace(material.get(), texture_path.lexically_normal().string());
                }
            } else {
                BOOST_LOG_TRIVIAL(error) << "load_obj: failed to load mtl_path:" << mtl_path_string;
            }
        }
    }
    // Count the generated triangles. Textured OBJ exporters commonly emit
    // n-gons, so triangulate them during import instead of rejecting the model.
    size_t num_triangles = 0;
    for (size_t i = 0; i < data.vertices.size(); ++ i) {
        if (!report_geometry_progress(6000 + (data.vertices.empty() ? 1000 : i * 1000 / data.vertices.size())))
            return false;
        // Find the end of face.
        size_t j = i;
        for (; j < data.vertices.size() && data.vertices[j].coordIdx != -1; ++ j) ;
        if (size_t num_face_vertices = j - i; num_face_vertices > 0) {
            if (num_face_vertices < 3) {
                BOOST_LOG_TRIVIAL(error) << "load_obj: failed to parse " << path
                                         << ". The file contains polygons with less than 3 vertices.";
                message = _L("The file contains polygons with less than 3 vertices.");
                return false;
            }
            num_triangles += num_face_vertices - 2;
            i = j;
        }
    }
    if (!report_geometry_progress(7000))
        return false;
    // Convert ObjData into indexed triangle set.
    indexed_triangle_set its;
    size_t               num_vertices = data.coordinates.size() / OBJ_VERTEX_LENGTH;
    its.vertices.reserve(num_vertices);
    its.indices.reserve(num_triangles);
    obj_info.triangle_uvs.reserve(num_triangles);
    obj_info.triangle_uvs_valid.reserve(num_triangles);
    obj_info.triangle_texture_files.reserve(num_triangles);
    if (exist_mtl) {
        obj_info.is_single_mtl = data.usemtls.size() == 1 && mtl_data.new_mtl_unmap.size() == 1;
        obj_info.face_colors.reserve(num_triangles);
    }
    for (size_t i = 0; i < num_vertices; ++ i) {
        if (!report_geometry_progress(7000 + (num_vertices == 0 ? 1000 : i * 1000 / num_vertices)))
            return false;
        size_t j = i * OBJ_VERTEX_LENGTH;
        its.vertices.emplace_back(data.coordinates[j], data.coordinates[j + 1], data.coordinates[j + 2]);
        if (data.has_vertex_color) {
            RGBA color{std::clamp(data.coordinates[j + 3], 0.f, 1.f), std::clamp(data.coordinates[j + 4], 0.f, 1.f), std::clamp(data.coordinates[j + 5], 0.f, 1.f),
                       std::clamp(data.coordinates[j + 6], 0.f, 1.f)};
            obj_info.vertex_colors.emplace_back(color);
        }
    }
    if (!report_geometry_progress(8000))
        return false;
    auto read_uv = [&data](int uv_idx, Vec2f &out_uv) {
        if (uv_idx < 0)
            return false;
        const size_t offset = size_t(uv_idx) * 2;
        if (offset + 1 >= data.textureCoordinates.size())
            return false;
        out_uv = Vec2f(data.textureCoordinates[offset], data.textureCoordinates[offset + 1]);
        return true;
    };
    auto material_for_face = [&data, &mtl_data](size_t face_vertex_index) -> const ObjParser::ObjNewMtl* {
        for (const ObjParser::ObjUseMtl &use_mtl : data.usemtls) {
            if (face_vertex_index < size_t(std::max(0, use_mtl.vertexIdxFirst)) ||
                (use_mtl.vertexIdxEnd >= 0 && face_vertex_index >= size_t(use_mtl.vertexIdxEnd)))
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
        const std::string texture_name    = texture_path_it != material_texture_paths.end() ?
                                                texture_path_it->second :
                                                (material != nullptr ? mtl_map_texture_filename(material->map_Kd) : std::string());
        obj_info.triangle_texture_files.emplace_back(texture_name);
        if (!texture_name.empty()) {
            obj_info.has_uv_png = true;
            obj_info.pngs.emplace(texture_name, false);
            obj_info.uv_map_pngs[generated_face_index] = texture_name;
            if (valid_uv)
                obj_info.uvs.emplace_back(triangle_uv);
        }
    };
    struct FaceCorner
    {
        int coordinate_index;
        int uv_index;
    };
    auto face_normal = [&its](const std::vector<FaceCorner>& face) {
        Vec3f normal = Vec3f::Zero();
        for (size_t index = 0; index < face.size(); ++index) {
            const Vec3f& current = its.vertices[size_t(face[index].coordinate_index)];
            const Vec3f& next    = its.vertices[size_t(face[(index + 1) % face.size()].coordinate_index)];
            normal.x() += (current.y() - next.y()) * (current.z() + next.z());
            normal.y() += (current.z() - next.z()) * (current.x() + next.x());
            normal.z() += (current.x() - next.x()) * (current.y() + next.y());
        }
        return normal;
    };
    auto projected_point = [&its](const FaceCorner& corner, const Vec3f& normal) {
        const Vec3f& point = its.vertices[size_t(corner.coordinate_index)];
        const float  ax    = std::abs(normal.x());
        const float  ay    = std::abs(normal.y());
        const float  az    = std::abs(normal.z());
        if (ax >= ay && ax >= az)
            return Point(scale_(point.y()), scale_(point.z()));
        if (ay >= ax && ay >= az)
            return Point(scale_(point.x()), scale_(point.z()));
        return Point(scale_(point.x()), scale_(point.y()));
    };
    auto append_triangle = [&its, &append_triangle_metadata, &material_color, &obj_info,
                            exist_mtl](const std::vector<FaceCorner>& face, size_t first, size_t second, size_t third, const Vec3f& normal,
                                       const ObjParser::ObjNewMtl* material) {
        if (first >= face.size() || second >= face.size() || third >= face.size())
            return false;
        const FaceCorner* corners[3] = {&face[first], &face[second], &face[third]};
        if (normal.squaredNorm() > EPSILON) {
            const Vec3f triangle_normal = (its.vertices[size_t(corners[1]->coordinate_index)] -
                                           its.vertices[size_t(corners[0]->coordinate_index)])
                                              .cross(its.vertices[size_t(corners[2]->coordinate_index)] -
                                                     its.vertices[size_t(corners[0]->coordinate_index)]);
            if (triangle_normal.squaredNorm() > EPSILON && triangle_normal.dot(normal) < 0.f)
                std::swap(corners[1], corners[2]);
        }
        its.indices.emplace_back(corners[0]->coordinate_index, corners[1]->coordinate_index, corners[2]->coordinate_index);
        const int generated_face_index = int(its.indices.size()) - 1;
        append_triangle_metadata(generated_face_index, {corners[0]->uv_index, corners[1]->uv_index, corners[2]->uv_index}, material);
        if (exist_mtl)
            obj_info.face_colors.emplace_back(material_color(material));
        return true;
    };
    auto append_face = [&append_triangle, &face_normal, &projected_point, &message, path](const std::vector<FaceCorner>& face,
                                                                                          const ObjParser::ObjNewMtl*    material) {
        const Vec3f normal = face_normal(face);
        if (face.size() == 3)
            return append_triangle(face, 0, 1, 2, normal, material);
        if (face.size() == 4)
            return append_triangle(face, 0, 1, 2, normal, material) && append_triangle(face, 0, 2, 3, normal, material);

        Polygon projected;
        projected.points.reserve(face.size());
        for (const FaceCorner& corner : face)
            projected.points.emplace_back(projected_point(corner, normal));
        std::set<Point> unique_points(projected.points.begin(), projected.points.end());
        if (unique_points.size() != face.size()) {
            BOOST_LOG_TRIVIAL(error) << "load_obj: failed to triangulate degenerate polygon in " << path;
            message = _L("The file contains a polygon that could not be triangulated.");
            return false;
        }

        const Triangulation::Indices triangles = Triangulation::triangulate(projected);
        if (triangles.empty()) {
            BOOST_LOG_TRIVIAL(error) << "load_obj: failed to triangulate polygon in " << path;
            message = _L("The file contains a polygon that could not be triangulated.");
            return false;
        }
        for (const Vec3i32& triangle : triangles)
            if (!append_triangle(face, size_t(triangle.x()), size_t(triangle.y()), size_t(triangle.z()), normal, material))
                return false;
        return true;
    };

    for (size_t i = 0; i < data.vertices.size();) {
        if (!report_geometry_progress(8000 + (data.vertices.empty() ? 2000 : i * 2000 / data.vertices.size())))
            return false;
        if (data.vertices[i].coordIdx == -1)
            ++ i;
        else {
            const size_t            face_vertex_index = i;
            std::vector<FaceCorner> face;
            while (i < data.vertices.size())
                if (const ObjParser::ObjVertex &vertex = data.vertices[i ++]; vertex.coordIdx == -1) {
                    break;
                } else {
                    if (vertex.coordIdx < 0 || vertex.coordIdx >= int(its.vertices.size())) {
                        BOOST_LOG_TRIVIAL(error) << "load_obj: failed to parse " << path << ". The file contains invalid vertex index.";
                        message = _L("The file contains invalid vertex index.");
                        return false;
                    }
                    face.push_back({vertex.coordIdx, vertex.textureCoordIdx});
                }
            while (face.size() > 3 && face.front().coordinate_index == face.back().coordinate_index)
                face.pop_back();
            if (face.size() >= 3 && !append_face(face, material_for_face(face_vertex_index)))
                return false;
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
    if (!report_geometry_progress(10000))
        return false;
    return true;
}

bool load_obj(const char *path,
              Model *model,
              ObjInfo& obj_info,
              std::string &message,
              const char *object_name_in,
              const ObjImageMapProgressFn& progress_fn)
{
    TriangleMesh mesh;

    bool ret = load_obj(path, &mesh, obj_info, message, progress_fn);

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
