#ifndef slic3r_FullSpectrum3mf_Constants_hpp_
#define slic3r_FullSpectrum3mf_Constants_hpp_

#include <utility>
#include <string>
#include <vector>

namespace Slic3r::FullSpectrum3mf {

inline constexpr const char *PROFILE_VERSION = "1.0.0";

inline constexpr const char *KIND_MANIFEST       = "org.fullspectrum.package-manifest";
inline constexpr const char *KIND_PROJECT        = "org.fullspectrum.project";
inline constexpr const char *KIND_IDENTITY_MAP   = "org.fullspectrum.identity-map";
inline constexpr const char *KIND_MATERIALS      = "org.fullspectrum.materials";
inline constexpr const char *KIND_ASSIGNMENTS    = "org.fullspectrum.assignments";
inline constexpr const char *KIND_MIXED_FILAMENTS = "org.fullspectrum.mixed-filaments";
inline constexpr const char *KIND_IMAGE_MAPS      = "org.fullspectrum.image-maps";

inline constexpr const char *PATH_MANIFEST        = "/Metadata/fullspectrum/manifest.json";
inline constexpr const char *PATH_PROJECT         = "/Metadata/fullspectrum/project.json";
inline constexpr const char *PATH_IDENTITY_MAP    = "/Metadata/fullspectrum/identity-map.json";
inline constexpr const char *PATH_MATERIALS       = "/Metadata/fullspectrum/materials.json";
inline constexpr const char *PATH_ASSIGNMENTS     = "/Metadata/fullspectrum/assignments.json";
inline constexpr const char *PATH_MIXED_FILAMENTS = "/Metadata/fullspectrum/mixed-filaments.json";
inline constexpr const char *PATH_IMAGE_MAPS      = "/Metadata/fullspectrum/image-maps.json";
inline constexpr const char *PATH_IMAGE_MAP_ASSETS_PREFIX = "/Metadata/fullspectrum/assets/";

inline constexpr const char *CONTENT_TYPE_MANIFEST        = "application/vnd.fullspectrum.manifest+json";
inline constexpr const char *CONTENT_TYPE_PROJECT         = "application/vnd.fullspectrum.project+json";
inline constexpr const char *CONTENT_TYPE_IDENTITY_MAP    = "application/vnd.fullspectrum.identity-map+json";
inline constexpr const char *CONTENT_TYPE_MATERIALS       = "application/vnd.fullspectrum.materials+json";
inline constexpr const char *CONTENT_TYPE_ASSIGNMENTS     = "application/vnd.fullspectrum.assignments+json";
inline constexpr const char *CONTENT_TYPE_MIXED_FILAMENTS = "application/vnd.fullspectrum.mixed-filaments+json";
inline constexpr const char *CONTENT_TYPE_IMAGE_MAPS      = "application/vnd.fullspectrum.image-maps+json";
inline constexpr const char *CONTENT_TYPE_RGBA8           = "application/vnd.fullspectrum.rgba8";

inline constexpr const char *REL_MANIFEST        = "https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-manifest";
inline constexpr const char *REL_PROJECT         = "https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-project";
inline constexpr const char *REL_IDENTITY_MAP    = "https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-identity-map";
inline constexpr const char *REL_MATERIALS       = "https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-materials";
inline constexpr const char *REL_ASSIGNMENTS     = "https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-assignments";
inline constexpr const char *REL_MIXED_FILAMENTS = "https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-mixed-filaments";
inline constexpr const char *REL_IMAGE_MAPS      = "https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-image-maps";
inline constexpr const char *REL_IMAGE_MAP_ASSET = "https://schemas.fullspectrum.dev/3mf/2026/relationships/fs-image-map-asset";
inline constexpr const char *REL_MUST_PRESERVE   = "http://schemas.openxmlformats.org/package/2006/relationships/mustpreserve";

inline constexpr const char *FEATURE_PROJECT_CORE    = "fs.project.core.v1";
inline constexpr const char *FEATURE_IDENTITY_MAP    = "fs.identity-map.v1";
inline constexpr const char *FEATURE_MATERIALS_CORE  = "fs.materials.core.v1";
inline constexpr const char *FEATURE_ASSIGNMENTS     = "fs.assignments.v1";
inline constexpr const char *FEATURE_MIXED_FILAMENTS = "fs.mixed-filaments.v1";
inline constexpr const char *FEATURE_MIXED_GRADIENT  = "fs.mixed-gradient.v1";
inline constexpr const char *FEATURE_LOCAL_Z         = "fs.local-z.v1";
inline constexpr const char *FEATURE_LEGACY_PROJECTION = "fs.legacy-projection.v1";
inline constexpr const char *FEATURE_IMAGE_MAPS       = "fs.image-maps.v1";

inline constexpr const char *MODEL_METADATA_PRESERVED_PARTS = "FullSpectrum3mf:PreservedParts";
inline constexpr const char *MODEL_METADATA_STATUS          = "FullSpectrum3mf:Status";
inline constexpr const char *MODEL_METADATA_WARNING         = "FullSpectrum3mf:Warning";
inline constexpr const char *MODEL_METADATA_STABLE_OBJECT_PREFIX = "FullSpectrum3mf:StableObject:";
inline constexpr const char *MODEL_METADATA_STABLE_VOLUME_PREFIX = "FullSpectrum3mf:StableVolume:";

std::string package_path_to_zip_path(const std::string &path);
std::string normalize_package_path(const std::string &path);
bool is_fullspectrum_json_zip_path(const std::string &path);
bool is_fullspectrum_core_package_path(const std::string &path);
bool is_preservable_extension_zip_path(const std::string &path);
bool is_fullspectrum_asset_zip_path(const std::string &path);
std::vector<std::pair<std::string, std::string>> content_type_overrides();

} // namespace Slic3r::FullSpectrum3mf

#endif
