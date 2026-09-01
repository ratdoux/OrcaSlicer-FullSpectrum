#ifndef slic3r_Format_Assimp_hpp_
#define slic3r_Format_Assimp_hpp_

#include "OBJ.hpp"

#include <string>

namespace Slic3r {

class Model;

// Formats routed through Assimp because they may carry UV textures, vertex
// colours, or material colours into the shared image-map workflow.
bool is_assimp_color_mesh_file(const std::string& path);

bool load_assimp_color_mesh(const char*                  path,
                            Model*                       model,
                            ObjInfo&                     color_info,
                            std::string&                 message,
                            const char*                  object_name = nullptr,
                            const ObjImageMapProgressFn& progress_fn = {});

} // namespace Slic3r

#endif // slic3r_Format_Assimp_hpp_
