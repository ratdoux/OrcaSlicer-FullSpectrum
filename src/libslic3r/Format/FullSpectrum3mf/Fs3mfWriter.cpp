#include "Fs3mfWriter.hpp"

#include "Fs3mfConstants.hpp"
#include "Fs3mfIds.hpp"
#include "Fs3mfJson.hpp"
#include "Fs3mfLegacyBridge.hpp"
#include "Fs3mfValidation.hpp"

#include "../../Model.hpp"
#include "../../MixedFilament.hpp"
#include "../../PrintConfig.hpp"

#include <algorithm>
#include <iomanip>
#include <openssl/sha.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace Slic3r::FullSpectrum3mf {

namespace {

std::string sha256_hex(const std::string &bytes)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), digest);

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned char byte : digest)
        ss << std::setw(2) << unsigned(byte);
    return ss.str();
}

std::vector<std::string> filament_colours_from_materials(const Materials &materials)
{
    std::vector<std::string> colours;
    colours.reserve(materials.physical_filaments.size());
    for (const PhysicalFilament &filament : materials.physical_filaments)
        colours.emplace_back(filament.color.empty() ? "#FFFFFF" : filament.color);
    return colours;
}

std::string mixed_definitions_from_config(const DynamicPrintConfig &config)
{
    if (const auto *opt = config.option<ConfigOptionString>("mixed_filament_definitions"); opt != nullptr)
        return opt->value;
    return {};
}

std::optional<MixedFilaments> mixed_filaments_from_project_config(const DynamicPrintConfig &config,
                                                                  const Materials          &materials)
{
    const std::string definitions = mixed_definitions_from_config(config);
    if (definitions.empty() || materials.physical_filaments.size() < 2)
        return std::nullopt;

    MixedFilamentManager manager;
    const std::vector<std::string> colours = filament_colours_from_materials(materials);
    manager.auto_generate(colours);
    manager.load_custom_entries(definitions, colours);

    MixedFilaments mixed = mixed_filaments_from_manager(manager, physical_filament_refs(materials));
    return mixed.virtual_filaments.empty() ? std::nullopt : std::optional<MixedFilaments>(std::move(mixed));
}

std::unordered_map<int, std::string> build_runtime_material_map(const Materials &materials,
                                                                const std::optional<MixedFilaments> &mixed_filaments)
{
    std::unordered_map<int, std::string> map;
    for (size_t i = 0; i < materials.physical_filaments.size(); ++i)
        map.emplace(int(i + 1), materials.physical_filaments[i].id);

    if (mixed_filaments) {
        int runtime_id = int(materials.physical_filaments.size() + 1);
        for (const VirtualFilament &vf : mixed_filaments->virtual_filaments) {
            if (vf.visibility_state != "tombstoned")
                map.emplace(runtime_id++, vf.id);
        }
    }

    return map;
}

bool has_local_z(const std::optional<MixedFilaments> &mixed_filaments)
{
    if (!mixed_filaments)
        return false;
    return std::any_of(mixed_filaments->virtual_filaments.begin(),
                       mixed_filaments->virtual_filaments.end(),
                       [](const VirtualFilament &vf) { return vf.local_z.has_value(); });
}

ManifestPart make_manifest_part(const PackagePartPlan &part)
{
    ManifestPart manifest_part;
    manifest_part.role = part.role;
    manifest_part.path = part.path;
    manifest_part.content_type = part.content_type;
    manifest_part.required = part.required;
    manifest_part.checksum.value = sha256_hex(part.bytes);
    return manifest_part;
}

void add_part(PackageWritePlan &plan,
              const std::string &path,
              const std::string &content_type,
              const std::string &role,
              const std::string &bytes,
              bool required,
              std::set<std::string> *emitted_paths = nullptr)
{
    if (emitted_paths != nullptr) {
        const std::string zip_path = normalize_package_path(package_path_to_zip_path(path));
        if (!emitted_paths->insert(zip_path).second)
            return;
    }

    plan.parts.push_back(PackagePartPlan{path, content_type, role, bytes, required, true});
}

const char *relationship_for_role(const std::string &role)
{
    if (role == "manifest")
        return REL_MANIFEST;
    if (role == "project")
        return REL_PROJECT;
    if (role == "identity-map")
        return REL_IDENTITY_MAP;
    if (role == "materials")
        return REL_MATERIALS;
    if (role == "assignments")
        return REL_ASSIGNMENTS;
    if (role == "mixed-filaments")
        return REL_MIXED_FILAMENTS;
    return REL_MUST_PRESERVE;
}

std::vector<std::string> known_required_features()
{
    return {
        FEATURE_PROJECT_CORE,
        FEATURE_IDENTITY_MAP,
        FEATURE_MATERIALS_CORE,
        FEATURE_ASSIGNMENTS,
        FEATURE_MIXED_FILAMENTS,
        FEATURE_LOCAL_Z,
        FEATURE_LEGACY_PROJECTION
    };
}

std::string xml_escape(const std::string &text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&':  escaped += "&amp;";  break;
        case '<':  escaped += "&lt;";   break;
        case '>':  escaped += "&gt;";   break;
        case '"':  escaped += "&quot;"; break;
        case '\'': escaped += "&apos;"; break;
        default:   escaped.push_back(c); break;
        }
    }
    return escaped;
}

} // namespace

std::string stable_object_id_from_model_id(int model_object_id)
{
    return make_stable_id("obj", std::to_string(model_object_id));
}

std::string stable_volume_id_from_model_id(int model_volume_id)
{
    return make_stable_id("vol", std::to_string(model_volume_id));
}

std::string stable_object_id_from_model(const Model &model, const ModelObject &object, int model_object_id)
{
    if (model.model_info) {
        const std::string key = std::string(MODEL_METADATA_STABLE_OBJECT_PREFIX) + std::to_string(object.id().id);
        auto it = model.model_info->metadata_items.find(key);
        if (it != model.model_info->metadata_items.end() && !it->second.empty())
            return it->second;
    }
    return stable_object_id_from_model_id(model_object_id);
}

std::string stable_volume_id_from_model(const Model &model, const ModelVolume &volume, int model_volume_id)
{
    if (model.model_info) {
        const std::string key = std::string(MODEL_METADATA_STABLE_VOLUME_PREFIX) + std::to_string(volume.id().id);
        auto it = model.model_info->metadata_items.find(key);
        if (it != model.model_info->metadata_items.end() && !it->second.empty())
            return it->second;
    }
    return stable_volume_id_from_model_id(model_volume_id);
}

std::vector<PreservedPart> preserved_parts_from_model(const Model &model)
{
    if (!model.model_info)
        return {};

    auto it = model.model_info->metadata_items.find(MODEL_METADATA_PRESERVED_PARTS);
    if (it == model.model_info->metadata_items.end() || it->second.empty())
        return {};

    try {
        return parse_json<std::vector<PreservedPart>>(it->second);
    } catch (...) {
        return {};
    }
}

PackageModel build_package_model(const DynamicPrintConfig &config,
                                 const GeometryBindingInput &geometry,
                                 bool write_legacy_projection)
{
    PackageModel model;
    model.materials = materials_from_project_config(config);
    model.mixed_filaments = mixed_filaments_from_project_config(config, model.materials);
    model.preserved_parts = geometry.preserved_parts;

    model.project.kind = KIND_PROJECT;
    model.project.schema_version = PROFILE_VERSION;
    model.project.display_name = geometry.project_name;
    model.project.legacy_projection_written = write_legacy_projection;

    const std::string project_seed = serialize_json(model.materials) +
                                     (model.mixed_filaments ? serialize_json(*model.mixed_filaments) : std::string()) +
                                     geometry.project_name;
    model.project.project_id = make_stable_id("proj", project_seed);

    model.identity_map.kind = KIND_IDENTITY_MAP;
    model.identity_map.schema_version = PROFILE_VERSION;
    for (const ObjectBindingInput &object : geometry.objects)
        model.identity_map.model_object_bindings.push_back({object.model_object_id, object.stable_object_id});
    for (const VolumeBindingInput &volume : geometry.volumes) {
        model.identity_map.volume_bindings.push_back({
            volume.model_object_id,
            volume.model_volume_id,
            volume.stable_object_id,
            volume.stable_volume_id
        });
    }

    for (size_t i = 0; i < model.materials.physical_filaments.size(); ++i)
        model.identity_map.material_bindings.push_back({int(i + 1), model.materials.physical_filaments[i].id});

    if (model.mixed_filaments) {
        int runtime_id = int(model.materials.physical_filaments.size() + 1);
        for (const VirtualFilament &vf : model.mixed_filaments->virtual_filaments) {
            if (vf.visibility_state != "tombstoned")
                model.identity_map.mixed_filament_bindings.push_back({runtime_id++, vf.legacy_stable_id, vf.id});
        }
    }

    model.assignments.kind = KIND_ASSIGNMENTS;
    model.assignments.schema_version = PROFILE_VERSION;
    const std::unordered_map<int, std::string> runtime_material_map =
        build_runtime_material_map(model.materials, model.mixed_filaments);

    std::set<std::pair<std::string, std::string>> emitted_volume_assignments;
    std::set<std::pair<std::string, int>> emitted_paint_bindings;
    for (const VolumeBindingInput &volume : geometry.volumes) {
        auto mat_it = runtime_material_map.find(volume.extruder_id);
        if (mat_it != runtime_material_map.end() && volume.extruder_id > 0) {
            const auto key = std::make_pair(volume.stable_volume_id, mat_it->second);
            if (emitted_volume_assignments.insert(key).second) {
                Assignment assignment;
                assignment.id = make_stable_id("assign", volume.stable_volume_id + "|" + mat_it->second);
                assignment.target.kind = "volume";
                assignment.target.stable_volume_id = volume.stable_volume_id;
                assignment.material_ref = mat_it->second;
                model.assignments.assignments.emplace_back(std::move(assignment));
            }
        }

        for (int paint_state : volume.paint_states) {
            auto paint_it = runtime_material_map.find(paint_state);
            if (paint_state <= 0 || paint_it == runtime_material_map.end())
                continue;
            const auto key = std::make_pair(volume.stable_volume_id, paint_state);
            if (emitted_paint_bindings.insert(key).second)
                model.assignments.paint_state_bindings.push_back({volume.stable_volume_id, paint_state, paint_it->second});
        }
    }

    model.manifest.kind = KIND_MANIFEST;
    model.manifest.schema_version = PROFILE_VERSION;
    model.manifest.document_class = "project";
    model.manifest.package_id = make_stable_id("pkg", project_seed);
    model.manifest.required_features = {
        FEATURE_PROJECT_CORE,
        FEATURE_IDENTITY_MAP,
        FEATURE_MATERIALS_CORE,
        FEATURE_ASSIGNMENTS
    };
    if (model.mixed_filaments)
        model.manifest.required_features.emplace_back(FEATURE_MIXED_FILAMENTS);
    if (has_local_z(model.mixed_filaments))
        model.manifest.optional_features.emplace_back(FEATURE_LOCAL_Z);
    if (write_legacy_projection)
        model.manifest.optional_features.emplace_back(FEATURE_LEGACY_PROJECTION);

    model.manifest.authoritative_sources = {
        {"project", PATH_PROJECT},
        {"identity_map", PATH_IDENTITY_MAP},
        {"materials", PATH_MATERIALS},
        {"assignments", PATH_ASSIGNMENTS}
    };
    if (model.mixed_filaments)
        model.manifest.authoritative_sources.emplace("mixed_filaments", PATH_MIXED_FILAMENTS);

    model.manifest.legacy_projection.present = write_legacy_projection;
    if (write_legacy_projection) {
        model.manifest.legacy_projection.derived_from = {
            PATH_PROJECT,
            PATH_MATERIALS,
            PATH_ASSIGNMENTS
        };
        if (model.mixed_filaments)
            model.manifest.legacy_projection.derived_from.emplace_back(PATH_MIXED_FILAMENTS);
        model.manifest.legacy_projection.paths = {
            "/Metadata/project_settings.config",
            "/Metadata/model_settings.config",
            "/Metadata/slice_info.config"
        };
    }

    return model;
}

PackageWritePlan build_write_plan(const PackageModel &model)
{
    ValidationResult validation = validate_package_model(model);
    if (!validation.valid) {
        std::ostringstream ss;
        ss << "Invalid FullSpectrum 3MF package model:";
        for (const std::string &error : validation.errors)
            ss << "\n" << error;
        throw std::runtime_error(ss.str());
    }

    PackageWritePlan plan;
    plan.required_features = model.manifest.required_features;
    plan.optional_features = model.manifest.optional_features;
    std::set<std::string> emitted_paths;

    add_part(plan, PATH_PROJECT, CONTENT_TYPE_PROJECT, "project", serialize_json(model.project), true, &emitted_paths);
    add_part(plan, PATH_IDENTITY_MAP, CONTENT_TYPE_IDENTITY_MAP, "identity-map", serialize_json(model.identity_map), true, &emitted_paths);
    add_part(plan, PATH_MATERIALS, CONTENT_TYPE_MATERIALS, "materials", serialize_json(model.materials), true, &emitted_paths);
    add_part(plan, PATH_ASSIGNMENTS, CONTENT_TYPE_ASSIGNMENTS, "assignments", serialize_json(model.assignments), true, &emitted_paths);
    if (model.mixed_filaments)
        add_part(plan, PATH_MIXED_FILAMENTS, CONTENT_TYPE_MIXED_FILAMENTS, "mixed-filaments", serialize_json(*model.mixed_filaments), true, &emitted_paths);

    for (const PreservedPart &preserved : model.preserved_parts) {
        if (preserved.path.empty() || is_fullspectrum_core_package_path(preserved.path))
            continue;
        if (!emitted_paths.insert(normalize_package_path(package_path_to_zip_path(preserved.path))).second)
            continue;
        plan.parts.push_back(PackagePartPlan{
            preserved.path,
            preserved.content_type.empty() ? "application/json" : preserved.content_type,
            preserved.role.empty() ? "extension" : preserved.role,
            preserved.bytes,
            preserved.required,
            preserved.must_preserve
        });
        plan.preserved_parts.push_back(preserved);
    }

    Manifest manifest = model.manifest;
    manifest.parts.clear();
    for (const PackagePartPlan &part : plan.parts)
        manifest.parts.emplace_back(make_manifest_part(part));

    const std::string manifest_bytes = serialize_json(manifest);
    plan.parts.insert(plan.parts.begin(), PackagePartPlan{PATH_MANIFEST, CONTENT_TYPE_MANIFEST, "manifest", manifest_bytes, true, true});

    for (const PackagePartPlan &part : plan.parts) {
        const char *relationship = relationship_for_role(part.role);
        plan.relationships.push_back(PackageRelationshipPlan{part.path, relationship, {}});
        if (part.must_preserve && std::string(relationship) != REL_MUST_PRESERVE)
            plan.relationships.push_back(PackageRelationshipPlan{part.path, REL_MUST_PRESERVE, {}});
    }

    return plan;
}

PackageWritePlan build_write_plan(const DynamicPrintConfig &config,
                                  const GeometryBindingInput &geometry,
                                  bool write_legacy_projection)
{
    return build_write_plan(build_package_model(config, geometry, write_legacy_projection));
}

FeatureNegotiationResult negotiate_features(const Manifest &manifest)
{
    FeatureNegotiationResult result;
    const std::vector<std::string> known = known_required_features();

    for (const std::string &feature : manifest.required_features) {
        if (std::find(known.begin(), known.end(), feature) == known.end()) {
            result.unsupported_required_features.emplace_back(feature);
            result.can_edit = false;
            result.can_slice = false;
            result.can_print = false;
            result.should_preserve_only = true;
        }
    }

    for (const std::string &feature : manifest.optional_features) {
        if (std::find(known.begin(), known.end(), feature) == known.end())
            result.unsupported_optional_features.emplace_back(feature);
    }

    return result;
}

std::string relationships_xml(const std::vector<PackageRelationshipPlan> &relationships,
                              const std::string &id_prefix)
{
    std::ostringstream stream;
    int rel_idx = 1;
    for (const PackageRelationshipPlan &relationship : relationships) {
        std::string target = relationship.target;
        if (target.empty())
            continue;
        if (target.front() != '/')
            target.insert(target.begin(), '/');
        stream << " <Relationship Target=\"" << xml_escape(target)
               << "\" Id=\"" << xml_escape(id_prefix) << rel_idx++
               << "\" Type=\"" << xml_escape(relationship.type) << "\"/>\n";
    }
    return stream.str();
}

} // namespace Slic3r::FullSpectrum3mf
