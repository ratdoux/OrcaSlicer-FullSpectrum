#ifndef slic3r_Format_OBJ_hpp_
#define slic3r_Format_OBJ_hpp_
#include "libslic3r/Color.hpp"
#include "libslic3r/Point.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
namespace Slic3r {

class TriangleMesh;
class Model;
class ModelObject;
struct ObjImageMapSamplePlan;

enum class ObjColorImportSource : uint8_t {
    VertexColors,
    FaceColors,
    ImageTexture
};

typedef std::function<void(std::vector<RGBA> &input_colors,
                           bool is_single_color,
                           std::vector<unsigned char> &filament_ids,
                           unsigned char &first_extruder_id,
                           ObjColorImportSource source)> ObjImportColorFn;
// Load an OBJ file into a provided model.
struct ObjInfo {
    std::vector<RGBA> vertex_colors;
    std::vector<RGBA> face_colors;
    bool              is_single_mtl{false};
    std::vector<std::array<Vec2f,3>> uvs;
    std::vector<std::array<Vec2f, 3>> triangle_uvs;
    std::vector<uint8_t>              triangle_uvs_valid;
    std::vector<std::string>          triangle_texture_files;
    std::string        obj_directory;
    std::map<std::string,bool>  pngs;
    std::unordered_map<int, std::string> uv_map_pngs;
    bool              has_uv_png{false};

};
extern bool load_obj(const char *path, TriangleMesh *mesh, ObjInfo &vertex_colors, std::string &message);
extern bool load_obj(const char *path, Model *model, ObjInfo &vertex_colors, std::string &message, const char *object_name = nullptr);

extern bool store_obj(const char *path, TriangleMesh *mesh);
extern bool store_obj(const char *path, ModelObject *model);
extern bool store_obj(const char *path, Model *model);

}; // namespace Slic3r

#endif /* slic3r_Format_OBJ_hpp_ */
