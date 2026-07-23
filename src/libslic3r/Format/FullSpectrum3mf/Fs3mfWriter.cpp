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

bool has_mixed_gradient(const std::optional<MixedFilaments> &mixed_filaments)
{
    if (!mixed_filaments)
        return false;
    return std::any_of(mixed_filaments->virtual_filaments.begin(),
                       mixed_filaments->virtual_filaments.end(),
                       [](const VirtualFilament &vf) { return vf.gradient && vf.gradient->enabled; });
}

const char *source_kind_name(ImageMap::SourceKind kind)
{
    switch (kind) {
    case ImageMap::SourceKind::Texture:      return "texture";
    case ImageMap::SourceKind::VertexColors: return "vertex_colors";
    case ImageMap::SourceKind::FaceColor:    return "face_color";
    }
    return "face_color";
}

const char *wrap_mode_name(ImageMap::WrapMode mode)
{
    return mode == ImageMap::WrapMode::Clamp ? "clamp" : "repeat";
}

const char *render_mode_name(ImageMap::RenderMode mode)
{
    switch (mode) {
    case ImageMap::RenderMode::PerimeterModulationV2:     return "perimeter_modulation_v2";
    case ImageMap::RenderMode::AdaptiveLocalizedCycles:   return "adaptive_localized_cycles";
    case ImageMap::RenderMode::NormalMix:
    default:                                              return "normal_mix";
    }
}

std::vector<double> rgba_values(const RGBA &color)
{
    return {double(color[0]), double(color[1]), double(color[2]), double(color[3])};
}

std::optional<ImageMaps> image_maps_from_geometry(const GeometryBindingInput             &geometry,
                                                   const Materials                        &materials,
                                                   const std::optional<MixedFilaments>    &mixed_filaments,
                                                   std::vector<ImageMapAssetPart>         &asset_parts)
{
    ImageMaps image_maps;
    image_maps.kind           = KIND_IMAGE_MAPS;
    image_maps.schema_version = PROFILE_VERSION;

    const std::unordered_map<int, std::string> runtime_materials = build_runtime_material_map(materials, mixed_filaments);
    std::unordered_map<uint64_t, std::string> stable_mixed_materials;
    if (mixed_filaments) {
        for (const VirtualFilament &filament : mixed_filaments->virtual_filaments) {
            if (filament.legacy_stable_id != 0)
                stable_mixed_materials.emplace(filament.legacy_stable_id, filament.id);
        }
    }

    std::unordered_map<std::string, size_t> canonical_asset_index;
    for (const VolumeBindingInput &volume_input : geometry.volumes) {
        const std::shared_ptr<const ImageMap::VolumeData> &data = volume_input.image_map_data;
        if (!data || data->empty())
            continue;

        ImageMapVolume volume;
        volume.stable_volume_id    = volume_input.stable_volume_id;
        volume.topology_fingerprint = data->topology_fingerprint;

        std::vector<std::string> asset_refs(data->texture_assets.size());
        for (size_t asset_idx = 0; asset_idx < data->texture_assets.size(); ++asset_idx) {
            const ImageMap::TextureAsset &asset = data->texture_assets[asset_idx];
            const std::string bytes(reinterpret_cast<const char *>(asset.rgba.data()), asset.rgba.size());
            const std::string content_hash = sha256_hex(std::to_string(asset.width) + "x" + std::to_string(asset.height) + "|" + bytes);
            const std::string asset_id     = "tex-" + content_hash.substr(0, 24);
            const std::string asset_path   = std::string(PATH_IMAGE_MAP_ASSETS_PREFIX) + content_hash + ".rgba8";
            asset_refs[asset_idx]          = asset_id;

            if (canonical_asset_index.emplace(asset_id, image_maps.texture_assets.size()).second) {
                image_maps.texture_assets.push_back({asset_id, asset.display_name, asset_path, asset.width, asset.height});
                asset_parts.push_back({asset_path, CONTENT_TYPE_RGBA8, bytes});
            }
        }

        volume.zones.reserve(data->zones.size());
        for (const ImageMap::Zone &source_zone : data->zones) {
            ImageMapZone zone;
            zone.id                           = source_zone.stable_id;
            zone.display_name                 = source_zone.display_name;
            zone.enabled                      = source_zone.enabled;
            zone.priority                     = source_zone.priority;
            zone.render_mode                  = render_mode_name(source_zone.render_mode);
            zone.minimum_component_percent    = source_zone.minimum_component_percent;
            zone.target_sample_size_mm        = source_zone.target_sample_size_mm;
            zone.max_facet_samples            = source_zone.max_facet_samples;
            zone.modulation_sample_spacing_mm = source_zone.modulation_sample_spacing_mm;
            zone.corner_smoothing_radius_mm   = source_zone.corner_smoothing_radius_mm;
            for (const ImageMap::PaletteEntry &source_entry : source_zone.palette) {
                ImageMapPaletteEntry entry;
                entry.target_rgba                  = rgba_values(source_entry.target_color);
                entry.fallback_runtime_filament_id = int(source_entry.fallback_filament_id);
                if (source_entry.mixed_filament_stable_id != 0) {
                    auto stable_it = stable_mixed_materials.find(source_entry.mixed_filament_stable_id);
                    if (stable_it != stable_mixed_materials.end())
                        entry.material_ref = stable_it->second;
                }
                if (entry.material_ref.empty()) {
                    auto runtime_it = runtime_materials.find(int(source_entry.fallback_filament_id));
                    if (runtime_it != runtime_materials.end())
                        entry.material_ref = runtime_it->second;
                }
                zone.palette.emplace_back(std::move(entry));
            }
            volume.zones.emplace_back(std::move(zone));
        }

        volume.triangle_bindings.reserve(data->triangle_bindings.size());
        for (const ImageMap::TriangleBinding &source_binding : data->triangle_bindings) {
            ImageMapTriangleBinding binding;
            binding.triangle_index = source_binding.triangle_index;
            binding.zone_index     = source_binding.zone_index;
            binding.source.kind    = source_kind_name(source_binding.source.kind);
            binding.source.wrap_u  = wrap_mode_name(source_binding.source.wrap_u);
            binding.source.wrap_v  = wrap_mode_name(source_binding.source.wrap_v);
            if (source_binding.source.texture_asset_index >= 0 &&
                size_t(source_binding.source.texture_asset_index) < asset_refs.size())
                binding.source.texture_asset_ref = asset_refs[size_t(source_binding.source.texture_asset_index)];
            binding.source.uvs.reserve(6);
            for (const Vec2f &uv : source_binding.source.uvs) {
                binding.source.uvs.push_back(double(uv.x()));
                binding.source.uvs.push_back(double(uv.y()));
            }
            binding.source.corner_rgba.reserve(12);
            for (const RGBA &color : source_binding.source.corner_colors)
                for (float channel : color)
                    binding.source.corner_rgba.push_back(double(channel));
            volume.triangle_bindings.emplace_back(std::move(binding));
        }
        image_maps.volumes.emplace_back(std::move(volume));
    }

    return image_maps.volumes.empty() ? std::nullopt : std::optional<ImageMaps>(std::move(image_maps));
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
    if (role == "image-maps")
        return REL_IMAGE_MAPS;
    if (role == "image-map-asset")
        return REL_IMAGE_MAP_ASSET;
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
        FEATURE_MIXED_GRADIENT,
        FEATURE_LOCAL_Z,
        FEATURE_LEGACY_PROJECTION,
        FEATURE_IMAGE_MAPS
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
    model.image_maps = image_maps_from_geometry(geometry, model.materials, model.mixed_filaments, model.image_map_assets);
    model.preserved_parts = geometry.preserved_parts;

    model.project.kind = KIND_PROJECT;
    model.project.schema_version = PROFILE_VERSION;
    model.project.display_name = geometry.project_name;
    model.project.legacy_projection_written = write_legacy_projection;

    const std::string project_seed = serialize_json(model.materials) +
                                     (model.mixed_filaments ? serialize_json(*model.mixed_filaments) : std::string()) +
                                     (model.image_maps ? serialize_json(*model.image_maps) : std::string()) +
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
    if (has_mixed_gradient(model.mixed_filaments))
        model.manifest.required_features.emplace_back(FEATURE_MIXED_GRADIENT);
    if (has_local_z(model.mixed_filaments))
        model.manifest.optional_features.emplace_back(FEATURE_LOCAL_Z);
    if (model.image_maps)
        model.manifest.required_features.emplace_back(FEATURE_IMAGE_MAPS);
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
    if (model.image_maps)
        model.manifest.authoritative_sources.emplace("image_maps", PATH_IMAGE_MAPS);

    model.manifest.legacy_projection.present = write_legacy_projection;
    if (write_legacy_projection) {
        model.manifest.legacy_projection.derived_from = {
            PATH_PROJECT,
            PATH_MATERIALS,
            PATH_ASSIGNMENTS
        };
        if (model.mixed_filaments)
            model.manifest.legacy_projection.derived_from.emplace_back(PATH_MIXED_FILAMENTS);
        if (model.image_maps)
            model.manifest.legacy_projection.derived_from.emplace_back(PATH_IMAGE_MAPS);
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
    if (model.image_maps) {
        add_part(plan, PATH_IMAGE_MAPS, CONTENT_TYPE_IMAGE_MAPS, "image-maps", serialize_json(*model.image_maps), true, &emitted_paths);
        for (const ImageMapAssetPart &asset : model.image_map_assets)
            add_part(plan, asset.path, asset.content_type, "image-map-asset", asset.bytes, true, &emitted_paths);
    }

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
