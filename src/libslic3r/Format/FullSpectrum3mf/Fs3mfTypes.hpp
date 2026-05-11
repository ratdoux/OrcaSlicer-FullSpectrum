#ifndef slic3r_FullSpectrum3mf_Types_hpp_
#define slic3r_FullSpectrum3mf_Types_hpp_

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::FullSpectrum3mf {

struct Checksum
{
    std::string algorithm = "sha256";
    std::string value;
};

struct ManifestPart
{
    std::string role;
    std::string path;
    std::string content_type;
    bool        required = false;
    Checksum    checksum;
};

struct LegacyProjection
{
    bool                     present = false;
    std::string              non_fullspectrum_policy = "physical-filaments-only";
    std::vector<std::string> derived_from;
    std::vector<std::string> paths;
};

struct Manifest
{
    std::string                         kind;
    std::string                         schema_version;
    std::string                         document_class = "project";
    std::string                         package_id;
    std::vector<std::string>            required_features;
    std::vector<std::string>            optional_features;
    std::vector<ManifestPart>           parts;
    std::map<std::string, std::string>  authoritative_sources;
    LegacyProjection                    legacy_projection;
};

struct Project
{
    std::string kind;
    std::string schema_version;
    std::string project_id;
    std::string display_name;
    bool        legacy_projection_written = true;
};

struct PhysicalFilament
{
    std::string id;
    std::string display_name;
    std::string material_family;
    double      diameter_mm = 1.75;
    std::string color = "#FFFFFF";
    size_t      source_index = 0;
};

struct Materials
{
    std::string                   kind;
    std::string                   schema_version;
    std::vector<PhysicalFilament> physical_filaments;
};

struct ModelObjectBinding
{
    int         model_object_id = 0;
    std::string stable_object_id;
};

struct VolumeBinding
{
    int         model_object_id = 0;
    int         model_volume_id = 0;
    std::string stable_object_id;
    std::string stable_volume_id;
};

struct MaterialBinding
{
    int         runtime_filament_id = 0;
    std::string material_ref;
};

struct MixedFilamentBinding
{
    int         runtime_filament_id = 0;
    uint64_t    legacy_stable_id = 0;
    std::string material_ref;
};

struct IdentityMap
{
    std::string                       kind;
    std::string                       schema_version;
    std::vector<ModelObjectBinding>   model_object_bindings;
    std::vector<VolumeBinding>        volume_bindings;
    std::vector<MaterialBinding>      material_bindings;
    std::vector<MixedFilamentBinding> mixed_filament_bindings;
};

struct AssignmentTarget
{
    std::string kind;
    std::string stable_object_id;
    std::string stable_volume_id;
    std::string stable_region_id;
};

struct Assignment
{
    std::string      id;
    AssignmentTarget target;
    std::string      material_ref;
};

struct PaintStateBinding
{
    std::string stable_volume_id;
    int         paint_state = 0;
    std::string material_ref;
};

struct Assignments
{
    std::string                   kind;
    std::string                   schema_version;
    std::vector<Assignment>       assignments;
    std::vector<PaintStateBinding> paint_state_bindings;
};

struct MixedFilamentOrigin
{
    std::string              kind = "pair";
    std::vector<std::string> component_refs;
    bool                     origin_auto_generated = false;
};

struct MixedFilamentBlend
{
    std::string type = "pair_ratio";
    int         component_b_percent = 50;
};

struct MixedFilamentDistribution
{
    std::string mode = "simple";
};

struct ManualPattern
{
    std::vector<std::vector<std::string>> groups;
};

struct Gradient
{
    std::vector<std::string> component_refs;
    std::vector<int>         weights;
};

struct SurfaceBias
{
    double component_a_offset_mm = 0.0;
    double component_b_offset_mm = 0.0;
};

struct LocalZ
{
    int         max_sublayers = 0;
    std::string strategy = "standard-pair-split";
};

struct VirtualFilament
{
    std::string                         id;
    uint64_t                            legacy_stable_id = 0;
    std::string                         visibility_state = "active";
    std::string                         source_kind = "auto";
    MixedFilamentOrigin                 origin;
    MixedFilamentBlend                  blend;
    MixedFilamentDistribution           distribution;
    std::optional<ManualPattern>        manual_pattern;
    std::optional<Gradient>             gradient;
    SurfaceBias                         surface_bias;
    std::optional<LocalZ>               local_z;
};

struct MixedFilaments
{
    std::string                  kind;
    std::string                  schema_version;
    std::vector<VirtualFilament> virtual_filaments;
};

struct PreservedPart
{
    std::string path;
    std::string content_type;
    std::string role;
    std::string bytes;
    bool        required = false;
    bool        must_preserve = true;
};

struct PackageModel
{
    Manifest                      manifest;
    Project                       project;
    IdentityMap                   identity_map;
    Materials                     materials;
    Assignments                   assignments;
    std::optional<MixedFilaments> mixed_filaments;
    std::vector<PreservedPart>    preserved_parts;
};

struct PackagePartPlan
{
    std::string path;
    std::string content_type;
    std::string role;
    std::string bytes;
    bool        required = false;
    bool        must_preserve = false;
};

struct PackageRelationshipPlan
{
    std::string target;
    std::string type;
    std::string source;
};

struct PackageWritePlan
{
    std::vector<PackagePartPlan>         parts;
    std::vector<PreservedPart>           preserved_parts;
    std::vector<PackageRelationshipPlan> relationships;
    std::vector<std::string>             required_features;
    std::vector<std::string>             optional_features;
};

struct FeatureNegotiationResult
{
    bool                     can_edit = true;
    bool                     can_slice = true;
    bool                     can_print = true;
    bool                     should_preserve_only = false;
    std::vector<std::string> unsupported_required_features;
    std::vector<std::string> unsupported_optional_features;
    std::vector<std::string> warnings;
};

} // namespace Slic3r::FullSpectrum3mf

#endif
