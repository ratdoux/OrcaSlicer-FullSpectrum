#include "Fs3mfJson.hpp"

#include "Fs3mfConstants.hpp"

namespace Slic3r::FullSpectrum3mf {

namespace {

template<class T> void get_if_present(const nlohmann::json &j, const char *key, T &out)
{
    auto it = j.find(key);
    if (it != j.end() && !it->is_null())
        out = it->get<T>();
}

} // namespace

void to_json(nlohmann::json &j, const Checksum &v)
{
    j = nlohmann::json{{"algorithm", v.algorithm}, {"value", v.value}};
}

void from_json(const nlohmann::json &j, Checksum &v)
{
    get_if_present(j, "algorithm", v.algorithm);
    get_if_present(j, "value", v.value);
}

void to_json(nlohmann::json &j, const ManifestPart &v)
{
    j = nlohmann::json{
        {"role", v.role},
        {"path", v.path},
        {"content_type", v.content_type},
        {"required", v.required},
        {"checksum", v.checksum}
    };
}

void from_json(const nlohmann::json &j, ManifestPart &v)
{
    get_if_present(j, "role", v.role);
    get_if_present(j, "path", v.path);
    get_if_present(j, "content_type", v.content_type);
    get_if_present(j, "required", v.required);
    get_if_present(j, "checksum", v.checksum);
}

void to_json(nlohmann::json &j, const LegacyProjection &v)
{
    j = nlohmann::json{
        {"present", v.present},
        {"non_fullspectrum_policy", v.non_fullspectrum_policy},
        {"derived_from", v.derived_from},
        {"paths", v.paths}
    };
}

void from_json(const nlohmann::json &j, LegacyProjection &v)
{
    get_if_present(j, "present", v.present);
    get_if_present(j, "non_fullspectrum_policy", v.non_fullspectrum_policy);
    get_if_present(j, "derived_from", v.derived_from);
    get_if_present(j, "paths", v.paths);
}

void to_json(nlohmann::json &j, const Manifest &v)
{
    j = nlohmann::json{
        {"kind", v.kind},
        {"schema_version", v.schema_version},
        {"document_class", v.document_class},
        {"package_id", v.package_id},
        {"features", {
            {"required", v.required_features},
            {"optional", v.optional_features}
        }},
        {"parts", v.parts},
        {"authoritative_sources", v.authoritative_sources},
        {"legacy_projection", v.legacy_projection}
    };
}

void from_json(const nlohmann::json &j, Manifest &v)
{
    get_if_present(j, "kind", v.kind);
    get_if_present(j, "schema_version", v.schema_version);
    get_if_present(j, "document_class", v.document_class);
    get_if_present(j, "package_id", v.package_id);
    if (auto it = j.find("features"); it != j.end()) {
        get_if_present(*it, "required", v.required_features);
        get_if_present(*it, "optional", v.optional_features);
    }
    get_if_present(j, "parts", v.parts);
    get_if_present(j, "authoritative_sources", v.authoritative_sources);
    get_if_present(j, "legacy_projection", v.legacy_projection);
}

void to_json(nlohmann::json &j, const Project &v)
{
    j = nlohmann::json{
        {"kind", v.kind},
        {"schema_version", v.schema_version},
        {"project", {
            {"id", v.project_id},
            {"display_name", v.display_name}
        }},
        {"compatibility", {
            {"legacy_projection_written", v.legacy_projection_written},
            {"non_fullspectrum_policy", "physical-filaments-only"}
        }},
        {"feature_policy", {
            {"unknown_required_features", "fail_closed"}
        }}
    };
}

void from_json(const nlohmann::json &j, Project &v)
{
    get_if_present(j, "kind", v.kind);
    get_if_present(j, "schema_version", v.schema_version);
    if (auto it = j.find("project"); it != j.end()) {
        get_if_present(*it, "id", v.project_id);
        get_if_present(*it, "display_name", v.display_name);
    }
    if (auto it = j.find("compatibility"); it != j.end())
        get_if_present(*it, "legacy_projection_written", v.legacy_projection_written);
}

void to_json(nlohmann::json &j, const PhysicalFilament &v)
{
    j = nlohmann::json{
        {"id", v.id},
        {"display_name", v.display_name},
        {"material_family", v.material_family},
        {"diameter_mm", v.diameter_mm},
        {"color", v.color},
        {"source_index", v.source_index}
    };
}

void from_json(const nlohmann::json &j, PhysicalFilament &v)
{
    get_if_present(j, "id", v.id);
    get_if_present(j, "display_name", v.display_name);
    get_if_present(j, "material_family", v.material_family);
    get_if_present(j, "diameter_mm", v.diameter_mm);
    get_if_present(j, "color", v.color);
    get_if_present(j, "source_index", v.source_index);
}

void to_json(nlohmann::json &j, const Materials &v)
{
    j = nlohmann::json{
        {"kind", v.kind},
        {"schema_version", v.schema_version},
        {"physical_filaments", v.physical_filaments}
    };
}

void from_json(const nlohmann::json &j, Materials &v)
{
    get_if_present(j, "kind", v.kind);
    get_if_present(j, "schema_version", v.schema_version);
    get_if_present(j, "physical_filaments", v.physical_filaments);
}

void to_json(nlohmann::json &j, const ModelObjectBinding &v)
{
    j = nlohmann::json{{"model_object_id", v.model_object_id}, {"stable_object_id", v.stable_object_id}};
}

void from_json(const nlohmann::json &j, ModelObjectBinding &v)
{
    get_if_present(j, "model_object_id", v.model_object_id);
    get_if_present(j, "stable_object_id", v.stable_object_id);
}

void to_json(nlohmann::json &j, const VolumeBinding &v)
{
    j = nlohmann::json{
        {"model_object_id", v.model_object_id},
        {"model_volume_id", v.model_volume_id},
        {"stable_object_id", v.stable_object_id},
        {"stable_volume_id", v.stable_volume_id}
    };
}

void from_json(const nlohmann::json &j, VolumeBinding &v)
{
    get_if_present(j, "model_object_id", v.model_object_id);
    get_if_present(j, "model_volume_id", v.model_volume_id);
    get_if_present(j, "stable_object_id", v.stable_object_id);
    get_if_present(j, "stable_volume_id", v.stable_volume_id);
}

void to_json(nlohmann::json &j, const MaterialBinding &v)
{
    j = nlohmann::json{{"runtime_filament_id", v.runtime_filament_id}, {"material_ref", v.material_ref}};
}

void from_json(const nlohmann::json &j, MaterialBinding &v)
{
    get_if_present(j, "runtime_filament_id", v.runtime_filament_id);
    get_if_present(j, "material_ref", v.material_ref);
}

void to_json(nlohmann::json &j, const MixedFilamentBinding &v)
{
    j = nlohmann::json{
        {"runtime_filament_id", v.runtime_filament_id},
        {"legacy_stable_id", v.legacy_stable_id},
        {"material_ref", v.material_ref}
    };
}

void from_json(const nlohmann::json &j, MixedFilamentBinding &v)
{
    get_if_present(j, "runtime_filament_id", v.runtime_filament_id);
    get_if_present(j, "legacy_stable_id", v.legacy_stable_id);
    get_if_present(j, "material_ref", v.material_ref);
}

void to_json(nlohmann::json &j, const IdentityMap &v)
{
    j = nlohmann::json{
        {"kind", v.kind},
        {"schema_version", v.schema_version},
        {"model_object_bindings", v.model_object_bindings},
        {"volume_bindings", v.volume_bindings},
        {"material_bindings", v.material_bindings},
        {"mixed_filament_bindings", v.mixed_filament_bindings}
    };
}

void from_json(const nlohmann::json &j, IdentityMap &v)
{
    get_if_present(j, "kind", v.kind);
    get_if_present(j, "schema_version", v.schema_version);
    get_if_present(j, "model_object_bindings", v.model_object_bindings);
    get_if_present(j, "volume_bindings", v.volume_bindings);
    get_if_present(j, "material_bindings", v.material_bindings);
    get_if_present(j, "mixed_filament_bindings", v.mixed_filament_bindings);
}

void to_json(nlohmann::json &j, const AssignmentTarget &v)
{
    j = nlohmann::json{{"kind", v.kind}};
    if (!v.stable_object_id.empty())
        j["stable_object_id"] = v.stable_object_id;
    if (!v.stable_volume_id.empty())
        j["stable_volume_id"] = v.stable_volume_id;
    if (!v.stable_region_id.empty())
        j["stable_region_id"] = v.stable_region_id;
}

void from_json(const nlohmann::json &j, AssignmentTarget &v)
{
    get_if_present(j, "kind", v.kind);
    get_if_present(j, "stable_object_id", v.stable_object_id);
    get_if_present(j, "stable_volume_id", v.stable_volume_id);
    get_if_present(j, "stable_region_id", v.stable_region_id);
}

void to_json(nlohmann::json &j, const Assignment &v)
{
    j = nlohmann::json{{"id", v.id}, {"target", v.target}, {"material_ref", v.material_ref}};
}

void from_json(const nlohmann::json &j, Assignment &v)
{
    get_if_present(j, "id", v.id);
    get_if_present(j, "target", v.target);
    get_if_present(j, "material_ref", v.material_ref);
}

void to_json(nlohmann::json &j, const PaintStateBinding &v)
{
    j = nlohmann::json{
        {"scope", {{"stable_volume_id", v.stable_volume_id}}},
        {"paint_state", v.paint_state},
        {"material_ref", v.material_ref}
    };
}

void from_json(const nlohmann::json &j, PaintStateBinding &v)
{
    if (auto it = j.find("scope"); it != j.end())
        get_if_present(*it, "stable_volume_id", v.stable_volume_id);
    get_if_present(j, "paint_state", v.paint_state);
    get_if_present(j, "material_ref", v.material_ref);
}

void to_json(nlohmann::json &j, const Assignments &v)
{
    j = nlohmann::json{
        {"kind", v.kind},
        {"schema_version", v.schema_version},
        {"assignments", v.assignments},
        {"paint_state_bindings", v.paint_state_bindings}
    };
}

void from_json(const nlohmann::json &j, Assignments &v)
{
    get_if_present(j, "kind", v.kind);
    get_if_present(j, "schema_version", v.schema_version);
    get_if_present(j, "assignments", v.assignments);
    get_if_present(j, "paint_state_bindings", v.paint_state_bindings);
}

void to_json(nlohmann::json &j, const MixedFilamentOrigin &v)
{
    j = nlohmann::json{
        {"kind", v.kind},
        {"component_refs", v.component_refs},
        {"origin_auto_generated", v.origin_auto_generated}
    };
}

void from_json(const nlohmann::json &j, MixedFilamentOrigin &v)
{
    get_if_present(j, "kind", v.kind);
    get_if_present(j, "component_refs", v.component_refs);
    get_if_present(j, "origin_auto_generated", v.origin_auto_generated);
}

void to_json(nlohmann::json &j, const MixedFilamentBlend &v)
{
    j = nlohmann::json{{"type", v.type}, {"component_b_percent", v.component_b_percent}};
}

void from_json(const nlohmann::json &j, MixedFilamentBlend &v)
{
    get_if_present(j, "type", v.type);
    get_if_present(j, "component_b_percent", v.component_b_percent);
}

void to_json(nlohmann::json &j, const MixedFilamentDistribution &v)
{
    j = nlohmann::json{{"mode", v.mode}};
}

void from_json(const nlohmann::json &j, MixedFilamentDistribution &v)
{
    get_if_present(j, "mode", v.mode);
}

void to_json(nlohmann::json &j, const ManualPattern &v)
{
    j = nlohmann::json{{"groups", v.groups}};
}

void from_json(const nlohmann::json &j, ManualPattern &v)
{
    get_if_present(j, "groups", v.groups);
}

void to_json(nlohmann::json &j, const Gradient &v)
{
    j = nlohmann::json{{"component_refs", v.component_refs}, {"weights", v.weights}};
    if (v.enabled) {
        j["enabled"] = true;
        j["component_a_start"] = v.component_a_start;
        j["component_a_end"] = v.component_a_end;
        j["stop_positions"] = v.stop_positions;
    }
}

void from_json(const nlohmann::json &j, Gradient &v)
{
    get_if_present(j, "component_refs", v.component_refs);
    get_if_present(j, "weights", v.weights);
    get_if_present(j, "enabled", v.enabled);
    get_if_present(j, "component_a_start", v.component_a_start);
    get_if_present(j, "component_a_end", v.component_a_end);
    get_if_present(j, "stop_positions", v.stop_positions);
}

void to_json(nlohmann::json &j, const SurfaceBias &v)
{
    j = nlohmann::json{
        {"component_a_offset_mm", v.component_a_offset_mm},
        {"component_b_offset_mm", v.component_b_offset_mm}
    };
    if (!v.component_refs.empty())
        j["component_refs"] = v.component_refs;
    if (!v.component_offsets_mm.empty())
        j["component_offsets_mm"] = v.component_offsets_mm;
}

void from_json(const nlohmann::json &j, SurfaceBias &v)
{
    get_if_present(j, "component_a_offset_mm", v.component_a_offset_mm);
    get_if_present(j, "component_b_offset_mm", v.component_b_offset_mm);
    get_if_present(j, "component_refs", v.component_refs);
    get_if_present(j, "component_offsets_mm", v.component_offsets_mm);
}

void to_json(nlohmann::json &j, const LocalZ &v)
{
    j = nlohmann::json{{"max_sublayers", v.max_sublayers}, {"strategy", v.strategy}};
}

void from_json(const nlohmann::json &j, LocalZ &v)
{
    get_if_present(j, "max_sublayers", v.max_sublayers);
    get_if_present(j, "strategy", v.strategy);
}

void to_json(nlohmann::json &j, const VirtualFilament &v)
{
    j = nlohmann::json{
        {"id", v.id},
        {"legacy_stable_id", v.legacy_stable_id},
        {"visibility_state", v.visibility_state},
        {"source_kind", v.source_kind},
        {"origin", v.origin},
        {"blend", v.blend},
        {"distribution", v.distribution},
        {"manual_pattern", v.manual_pattern ? nlohmann::json(*v.manual_pattern) : nlohmann::json(nullptr)},
        {"gradient", v.gradient ? nlohmann::json(*v.gradient) : nlohmann::json(nullptr)},
        {"surface_bias", v.surface_bias}
    };
    if (v.local_z)
        j["local_z"] = *v.local_z;
}

void from_json(const nlohmann::json &j, VirtualFilament &v)
{
    get_if_present(j, "id", v.id);
    get_if_present(j, "legacy_stable_id", v.legacy_stable_id);
    get_if_present(j, "visibility_state", v.visibility_state);
    get_if_present(j, "source_kind", v.source_kind);
    get_if_present(j, "origin", v.origin);
    get_if_present(j, "blend", v.blend);
    get_if_present(j, "distribution", v.distribution);
    if (auto it = j.find("manual_pattern"); it != j.end() && !it->is_null())
        v.manual_pattern = it->get<ManualPattern>();
    if (auto it = j.find("gradient"); it != j.end() && !it->is_null())
        v.gradient = it->get<Gradient>();
    get_if_present(j, "surface_bias", v.surface_bias);
    if (auto it = j.find("local_z"); it != j.end() && !it->is_null())
        v.local_z = it->get<LocalZ>();
}

void to_json(nlohmann::json &j, const MixedFilaments &v)
{
    j = nlohmann::json{
        {"kind", v.kind},
        {"schema_version", v.schema_version},
        {"virtual_filaments", v.virtual_filaments}
    };
}

void from_json(const nlohmann::json &j, MixedFilaments &v)
{
    get_if_present(j, "kind", v.kind);
    get_if_present(j, "schema_version", v.schema_version);
    get_if_present(j, "virtual_filaments", v.virtual_filaments);
}

void to_json(nlohmann::json &j, const PreservedPart &v)
{
    j = nlohmann::json{
        {"path", v.path},
        {"content_type", v.content_type},
        {"role", v.role},
        {"bytes", v.bytes},
        {"required", v.required},
        {"must_preserve", v.must_preserve}
    };
}

void from_json(const nlohmann::json &j, PreservedPart &v)
{
    get_if_present(j, "path", v.path);
    get_if_present(j, "content_type", v.content_type);
    get_if_present(j, "role", v.role);
    get_if_present(j, "bytes", v.bytes);
    get_if_present(j, "required", v.required);
    get_if_present(j, "must_preserve", v.must_preserve);
}

} // namespace Slic3r::FullSpectrum3mf
