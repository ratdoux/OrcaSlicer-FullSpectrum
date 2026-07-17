#ifndef slic3r_Format_ImportedTexture_hpp_
#define slic3r_Format_ImportedTexture_hpp_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {

// PNG/JPEG decoding used by the experimental OBJ image-map importer.
// Adapted from OrcaSlicer-ImageMap (AGPL-3.0) by sentientstardust.
bool decode_imported_texture_rgba_from_file(const std::string&    texture_path,
                                            std::vector<uint8_t>& out_rgba,
                                            uint32_t&             out_width,
                                            uint32_t&             out_height);

bool decode_imported_texture_rgba_from_memory(const uint8_t*        data,
                                              size_t                data_size,
                                              const std::string&    mime_type_or_name,
                                              std::vector<uint8_t>& out_rgba,
                                              uint32_t&             out_width,
                                              uint32_t&             out_height);

bool is_supported_imported_texture_path(const std::string& texture_path);

} // namespace Slic3r

#endif
