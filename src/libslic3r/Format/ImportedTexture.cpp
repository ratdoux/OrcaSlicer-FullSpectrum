#include "ImportedTexture.hpp"

#include "libslic3r/PNGReadWrite.hpp"

#include <algorithm>
#include <csetjmp>
#include <iterator>
#include <limits>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/nowide/fstream.hpp>

#include <jpeglib.h>

namespace Slic3r {
namespace {

bool checked_rgba_buffer_size(size_t width, size_t height, size_t& buffer_size)
{
    buffer_size = 0;
    if (width == 0 || height == 0 || width > std::numeric_limits<size_t>::max() / height)
        return false;
    const size_t pixel_count = width * height;
    if (pixel_count > std::numeric_limits<size_t>::max() / 4)
        return false;
    buffer_size = pixel_count * 4;
    return true;
}

bool decode_png_rgba(const uint8_t* data, size_t data_size, std::vector<uint8_t>& out_rgba, uint32_t& out_width, uint32_t& out_height)
{
    if (data == nullptr || data_size == 0)
        return false;

    png::ReadBuf         read_buffer{reinterpret_cast<const char*>(data), data_size};
    png::ImageColorscale image;
    if (!png::decode_colored_png(read_buffer, image) || image.cols == 0 || image.rows == 0 ||
        (image.bytes_per_pixel != 3 && image.bytes_per_pixel != 4))
        return false;

    size_t rgba_size = 0;
    if (!checked_rgba_buffer_size(image.cols, image.rows, rgba_size))
        return false;

    const size_t row_stride = image.cols * size_t(image.bytes_per_pixel);
    if (image.buf.size() < image.rows * row_stride)
        return false;

    // decode_colored_png already stores rows bottom-up, matching OBJ UVs.
    out_rgba.assign(rgba_size, uint8_t(255));
    for (size_t y = 0; y < image.rows; ++y) {
        for (size_t x = 0; x < image.cols; ++x) {
            const size_t source       = y * row_stride + x * size_t(image.bytes_per_pixel);
            const size_t destination  = (y * image.cols + x) * 4;
            out_rgba[destination + 0] = image.buf[source + 0];
            out_rgba[destination + 1] = image.buf[source + 1];
            out_rgba[destination + 2] = image.buf[source + 2];
            out_rgba[destination + 3] = image.bytes_per_pixel == 4 ? image.buf[source + 3] : uint8_t(255);
        }
    }

    out_width  = uint32_t(image.cols);
    out_height = uint32_t(image.rows);
    return true;
}

struct JpegErrorManager
{
    jpeg_error_mgr pub;
    jmp_buf        jump_buffer;
};

void jpeg_error_exit(j_common_ptr info)
{
    auto* error = reinterpret_cast<JpegErrorManager*>(info->err);
    longjmp(error->jump_buffer, 1);
}

bool decode_jpeg_rgba(const uint8_t* data, size_t data_size, std::vector<uint8_t>& out_rgba, uint32_t& out_width, uint32_t& out_height)
{
    if (data == nullptr || data_size == 0 || data_size > size_t(std::numeric_limits<unsigned long>::max()))
        return false;

    jpeg_decompress_struct info{};
    JpegErrorManager       error{};
    info.err             = jpeg_std_error(&error.pub);
    error.pub.error_exit = jpeg_error_exit;
    bool jpeg_created    = false;
    auto destroy_jpeg    = [&]() {
        if (jpeg_created) {
            jpeg_destroy_decompress(&info);
            jpeg_created = false;
        }
    };

    if (setjmp(error.jump_buffer)) {
        destroy_jpeg();
        return false;
    }

    jpeg_create_decompress(&info);
    jpeg_created = true;
    jpeg_mem_src(&info, data, static_cast<unsigned long>(data_size));
    if (jpeg_read_header(&info, TRUE) != JPEG_HEADER_OK || !jpeg_start_decompress(&info)) {
        destroy_jpeg();
        return false;
    }

    const uint32_t width           = info.output_width;
    const uint32_t height          = info.output_height;
    const int      components      = info.output_components;
    const size_t   scanline_stride = size_t(width) * size_t(std::max(components, 0));
    size_t         rgba_size       = 0;
    if (!checked_rgba_buffer_size(width, height, rgba_size) || components <= 0 || scanline_stride > std::numeric_limits<JDIMENSION>::max()) {
        destroy_jpeg();
        return false;
    }

    out_rgba.assign(rgba_size, uint8_t(255));
    JSAMPARRAY scanline = (*info.mem->alloc_sarray)(reinterpret_cast<j_common_ptr>(&info), JPOOL_IMAGE, JDIMENSION(scanline_stride), 1);
    uint32_t   source_y = 0;
    while (info.output_scanline < info.output_height) {
        jpeg_read_scanlines(&info, scanline, 1);
        const unsigned char* source        = scanline[0];
        const size_t         destination_y = size_t(height - source_y - 1);
        for (uint32_t x = 0; x < width; ++x) {
            const size_t destination = (destination_y * size_t(width) + size_t(x)) * 4;
            if (components >= 3) {
                const size_t source_offset = size_t(x) * size_t(components);
                out_rgba[destination + 0]  = source[source_offset + 0];
                out_rgba[destination + 1]  = source[source_offset + 1];
                out_rgba[destination + 2]  = source[source_offset + 2];
            } else {
                out_rgba[destination + 0] = source[x];
                out_rgba[destination + 1] = source[x];
                out_rgba[destination + 2] = source[x];
            }
        }
        ++source_y;
    }

    const bool finished = jpeg_finish_decompress(&info) == TRUE;
    destroy_jpeg();
    if (!finished)
        return false;

    out_width  = width;
    out_height = height;
    return true;
}

bool has_png_signature(const uint8_t* data, size_t data_size)
{
    static constexpr uint8_t signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    return data_size >= sizeof(signature) && std::equal(std::begin(signature), std::end(signature), data);
}

bool has_jpeg_signature(const uint8_t* data, size_t data_size)
{
    return data_size >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff;
}

} // namespace

bool decode_imported_texture_rgba_from_file(const std::string&    texture_path,
                                            std::vector<uint8_t>& out_rgba,
                                            uint32_t&             out_width,
                                            uint32_t&             out_height)
{
    out_rgba.clear();
    out_width  = 0;
    out_height = 0;
    if (!is_supported_imported_texture_path(texture_path))
        return false;

    boost::nowide::ifstream input(texture_path, std::ios::binary);
    if (!input.is_open())
        return false;
    const std::string encoded((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return !encoded.empty() && decode_imported_texture_rgba_from_memory(reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size(),
                                                                        texture_path, out_rgba, out_width, out_height);
}

bool decode_imported_texture_rgba_from_memory(const uint8_t*        data,
                                              size_t                data_size,
                                              const std::string&    mime_type_or_name,
                                              std::vector<uint8_t>& out_rgba,
                                              uint32_t&             out_width,
                                              uint32_t&             out_height)
{
    out_rgba.clear();
    out_width  = 0;
    out_height = 0;
    if (data == nullptr || data_size == 0)
        return false;

    if (boost::algorithm::iequals(mime_type_or_name, "image/png") || boost::algorithm::iends_with(mime_type_or_name, ".png") ||
        has_png_signature(data, data_size))
        return decode_png_rgba(data, data_size, out_rgba, out_width, out_height);

    if (boost::algorithm::iequals(mime_type_or_name, "image/jpeg") || boost::algorithm::iequals(mime_type_or_name, "image/jpg") ||
        boost::algorithm::iends_with(mime_type_or_name, ".jpg") || boost::algorithm::iends_with(mime_type_or_name, ".jpeg") ||
        has_jpeg_signature(data, data_size))
        return decode_jpeg_rgba(data, data_size, out_rgba, out_width, out_height);

    return false;
}

bool is_supported_imported_texture_path(const std::string& texture_path)
{
    return boost::algorithm::iends_with(texture_path, ".png") || boost::algorithm::iends_with(texture_path, ".jpg") ||
           boost::algorithm::iends_with(texture_path, ".jpeg");
}

} // namespace Slic3r
