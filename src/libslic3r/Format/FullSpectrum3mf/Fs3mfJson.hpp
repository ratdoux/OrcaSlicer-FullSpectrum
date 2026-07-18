#ifndef slic3r_FullSpectrum3mf_Json_hpp_
#define slic3r_FullSpectrum3mf_Json_hpp_

#include "Fs3mfTypes.hpp"

#include <nlohmann/json.hpp>

namespace Slic3r::FullSpectrum3mf {

void to_json(nlohmann::json &j, const Checksum &v);
void from_json(const nlohmann::json &j, Checksum &v);

void to_json(nlohmann::json &j, const ManifestPart &v);
void from_json(const nlohmann::json &j, ManifestPart &v);

void to_json(nlohmann::json &j, const LegacyProjection &v);
void from_json(const nlohmann::json &j, LegacyProjection &v);

void to_json(nlohmann::json &j, const Manifest &v);
void from_json(const nlohmann::json &j, Manifest &v);

void to_json(nlohmann::json &j, const Project &v);
void from_json(const nlohmann::json &j, Project &v);

void to_json(nlohmann::json &j, const PhysicalFilament &v);
void from_json(const nlohmann::json &j, PhysicalFilament &v);

void to_json(nlohmann::json &j, const Materials &v);
void from_json(const nlohmann::json &j, Materials &v);

void to_json(nlohmann::json &j, const ModelObjectBinding &v);
void from_json(const nlohmann::json &j, ModelObjectBinding &v);

void to_json(nlohmann::json &j, const VolumeBinding &v);
void from_json(const nlohmann::json &j, VolumeBinding &v);

void to_json(nlohmann::json &j, const MaterialBinding &v);
void from_json(const nlohmann::json &j, MaterialBinding &v);

void to_json(nlohmann::json &j, const MixedFilamentBinding &v);
void from_json(const nlohmann::json &j, MixedFilamentBinding &v);

void to_json(nlohmann::json &j, const IdentityMap &v);
void from_json(const nlohmann::json &j, IdentityMap &v);

void to_json(nlohmann::json &j, const AssignmentTarget &v);
void from_json(const nlohmann::json &j, AssignmentTarget &v);

void to_json(nlohmann::json &j, const Assignment &v);
void from_json(const nlohmann::json &j, Assignment &v);

void to_json(nlohmann::json &j, const PaintStateBinding &v);
void from_json(const nlohmann::json &j, PaintStateBinding &v);

void to_json(nlohmann::json &j, const Assignments &v);
void from_json(const nlohmann::json &j, Assignments &v);

void to_json(nlohmann::json &j, const MixedFilamentOrigin &v);
void from_json(const nlohmann::json &j, MixedFilamentOrigin &v);

void to_json(nlohmann::json &j, const MixedFilamentBlend &v);
void from_json(const nlohmann::json &j, MixedFilamentBlend &v);

void to_json(nlohmann::json &j, const MixedFilamentDistribution &v);
void from_json(const nlohmann::json &j, MixedFilamentDistribution &v);

void to_json(nlohmann::json &j, const ManualPattern &v);
void from_json(const nlohmann::json &j, ManualPattern &v);

void to_json(nlohmann::json &j, const Gradient &v);
void from_json(const nlohmann::json &j, Gradient &v);

void to_json(nlohmann::json &j, const SurfaceBias &v);
void from_json(const nlohmann::json &j, SurfaceBias &v);

void to_json(nlohmann::json &j, const LocalZ &v);
void from_json(const nlohmann::json &j, LocalZ &v);

void to_json(nlohmann::json &j, const VirtualFilament &v);
void from_json(const nlohmann::json &j, VirtualFilament &v);

void to_json(nlohmann::json &j, const MixedFilaments &v);
void from_json(const nlohmann::json &j, MixedFilaments &v);

void to_json(nlohmann::json &j, const ImageMapTextureAsset &v);
void from_json(const nlohmann::json &j, ImageMapTextureAsset &v);
void to_json(nlohmann::json &j, const ImageMapSurfaceSource &v);
void from_json(const nlohmann::json &j, ImageMapSurfaceSource &v);
void to_json(nlohmann::json &j, const ImageMapTriangleBinding &v);
void from_json(const nlohmann::json &j, ImageMapTriangleBinding &v);
void to_json(nlohmann::json &j, const ImageMapPaletteEntry &v);
void from_json(const nlohmann::json &j, ImageMapPaletteEntry &v);
void to_json(nlohmann::json &j, const ImageMapZone &v);
void from_json(const nlohmann::json &j, ImageMapZone &v);
void to_json(nlohmann::json &j, const ImageMapVolume &v);
void from_json(const nlohmann::json &j, ImageMapVolume &v);
void to_json(nlohmann::json &j, const ImageMaps &v);
void from_json(const nlohmann::json &j, ImageMaps &v);

void to_json(nlohmann::json &j, const PreservedPart &v);
void from_json(const nlohmann::json &j, PreservedPart &v);

template<class T> std::string serialize_json(const T &value)
{
    nlohmann::json j = value;
    return j.dump(2);
}

template<class T> T parse_json(const std::string &bytes)
{
    nlohmann::json j = nlohmann::json::parse(bytes);
    return j.get<T>();
}

} // namespace Slic3r::FullSpectrum3mf

#endif
