#include "Fs3mfConstants.hpp"

#include <algorithm>

namespace Slic3r::FullSpectrum3mf {

std::string package_path_to_zip_path(const std::string &path)
{
    if (!path.empty() && path.front() == '/')
        return path.substr(1);
    return path;
}

std::string normalize_package_path(const std::string &path)
{
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    return normalized;
}

bool is_fullspectrum_json_zip_path(const std::string &path)
{
    const std::string normalized = normalize_package_path(path);
    constexpr const char *prefix = "Metadata/fullspectrum/";
    constexpr const char *suffix = ".json";

    const size_t prefix_len = std::char_traits<char>::length(prefix);
    const size_t suffix_len = std::char_traits<char>::length(suffix);

    return normalized.size() >= prefix_len + suffix_len &&
           normalized.rfind(prefix, 0) == 0 &&
           normalized.compare(normalized.size() - suffix_len, suffix_len, suffix) == 0;
}

bool is_fullspectrum_core_package_path(const std::string &path)
{
    const std::string normalized = normalize_package_path(path);
    for (const auto &content_type : content_type_overrides()) {
        if (normalized == package_path_to_zip_path(content_type.first) || normalized == content_type.first)
            return true;
    }
    return false;
}

bool is_preservable_extension_zip_path(const std::string &path)
{
    const std::string normalized = normalize_package_path(path);
    constexpr const char *prefix = "Metadata/extensions/";
    return normalized.rfind(prefix, 0) == 0;
}

bool is_fullspectrum_asset_zip_path(const std::string &path)
{
    const std::string normalized = normalize_package_path(path);
    return normalized.rfind(package_path_to_zip_path(PATH_IMAGE_MAP_ASSETS_PREFIX), 0) == 0;
}

std::vector<std::pair<std::string, std::string>> content_type_overrides()
{
    return {
        {PATH_MANIFEST, CONTENT_TYPE_MANIFEST},
        {PATH_PROJECT, CONTENT_TYPE_PROJECT},
        {PATH_IDENTITY_MAP, CONTENT_TYPE_IDENTITY_MAP},
        {PATH_MATERIALS, CONTENT_TYPE_MATERIALS},
        {PATH_ASSIGNMENTS, CONTENT_TYPE_ASSIGNMENTS},
        {PATH_MIXED_FILAMENTS, CONTENT_TYPE_MIXED_FILAMENTS},
        {PATH_IMAGE_MAPS, CONTENT_TYPE_IMAGE_MAPS}
    };
}

} // namespace Slic3r::FullSpectrum3mf
