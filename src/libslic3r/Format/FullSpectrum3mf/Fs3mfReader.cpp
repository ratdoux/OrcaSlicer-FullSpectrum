#include "Fs3mfReader.hpp"

#include "Fs3mfConstants.hpp"
#include "Fs3mfJson.hpp"
#include "Fs3mfLegacyBridge.hpp"
#include "Fs3mfWriter.hpp"

#include "../../Model.hpp"
#include "../../PrintConfig.hpp"

#include <algorithm>
#include <iomanip>
#include <openssl/sha.h>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace Slic3r::FullSpectrum3mf {

namespace {

std::string canonical_key(const std::string &path)
{
    return package_path_to_zip_path(path);
}

bool find_part(const std::map<std::string, std::string> &parts, const std::string &path, std::string &out)
{
    auto it = parts.find(canonical_key(path));
    if (it == parts.end())
        it = parts.find(path);
    if (it == parts.end())
        return false;
    out = it->second;
    return true;
}

const ManifestPart *find_manifest_part(const Manifest &manifest, const std::string &path)
{
    const std::string normalized = normalize_package_path(package_path_to_zip_path(path));
    for (const ManifestPart &part : manifest.parts) {
        if (normalize_package_path(package_path_to_zip_path(part.path)) == normalized)
            return &part;
    }
    return nullptr;
}

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

bool verify_checksum(const ManifestPart &part, const std::string &bytes, std::string *warning)
{
    if (part.checksum.value.empty())
        return true;
    if (part.checksum.algorithm != "sha256") {
        if (warning)
            *warning = "Unsupported FullSpectrum checksum algorithm for " + part.path + ": " + part.checksum.algorithm;
        return !part.required;
    }

    const std::string actual = sha256_hex(bytes);
    if (actual == part.checksum.value)
        return true;

    if (warning)
        *warning = "FullSpectrum checksum mismatch for " + part.path;
    return !part.required;
}

std::string authoritative_path(const Manifest &manifest,
                               const std::string &key,
                               const std::string &alternate_key,
                               const std::string &fallback)
{
    auto it = manifest.authoritative_sources.find(key);
    if (it != manifest.authoritative_sources.end() && !it->second.empty())
        return it->second;

    it = manifest.authoritative_sources.find(alternate_key);
    if (it != manifest.authoritative_sources.end() && !it->second.empty())
        return it->second;

    return fallback;
}

bool read_manifest_part(const std::map<std::string, std::string> &parts,
                        const Manifest                           &manifest,
                        const std::string                        &path,
                        std::string                              &out,
                        std::string                              *warning)
{
    if (!find_part(parts, path, out)) {
        if (warning)
            *warning = "FullSpectrum manifest is present but required part is missing: " + path;
        return false;
    }

    if (const ManifestPart *part = find_manifest_part(manifest, path); part != nullptr)
        return verify_checksum(*part, out, warning);

    return true;
}

std::string unsupported_required_warning(const FeatureNegotiationResult &negotiation)
{
    std::ostringstream ss;
    ss << "Unsupported required FullSpectrum feature";
    if (negotiation.unsupported_required_features.size() != 1)
        ss << "s";
    ss << ": ";
    for (size_t i = 0; i < negotiation.unsupported_required_features.size(); ++i) {
        if (i != 0)
            ss << ", ";
        ss << negotiation.unsupported_required_features[i];
    }
    return ss.str();
}

bool require_kind_schema(const char        *document_name,
                         const std::string &actual_kind,
                         const char        *expected_kind,
                         const std::string &actual_schema_version,
                         std::string       *warning)
{
    if (actual_kind != expected_kind) {
        if (warning)
            *warning = std::string("FullSpectrum ") + document_name + " has unexpected kind: " + actual_kind;
        return false;
    }

    if (actual_schema_version != PROFILE_VERSION) {
        if (warning) {
            *warning = std::string("FullSpectrum ") + document_name + " has unsupported schema version: " +
                       actual_schema_version;
        }
        return false;
    }

    return true;
}

void ensure_model_info(Model &model)
{
    if (!model.model_info)
        model.model_info = std::make_shared<ModelInfo>();
}

void set_model_metadata(Model &model, const std::string &key, const std::string &value)
{
    ensure_model_info(model);
    model.model_info->metadata_items[key] = value;
}

std::string stable_object_metadata_key(const ModelObject &object)
{
    return std::string(MODEL_METADATA_STABLE_OBJECT_PREFIX) + std::to_string(object.id().id);
}

std::string stable_volume_metadata_key(const ModelVolume &volume)
{
    return std::string(MODEL_METADATA_STABLE_VOLUME_PREFIX) + std::to_string(volume.id().id);
}

void set_mixed_definitions(DynamicPrintConfig &config, const std::string &definitions)
{
    if (ConfigOptionString *opt = config.option<ConfigOptionString>("mixed_filament_definitions"))
        opt->value = definitions;
    else
        config.set_key_value("mixed_filament_definitions", new ConfigOptionString(definitions));
}

struct CanonicalParts
{
    Manifest                      manifest;
    Materials                     materials;
    IdentityMap                   identity_map;
    Assignments                   assignments;
    std::optional<MixedFilaments> mixed_filaments;
    std::optional<ImageMaps>      image_maps;
};

bool parse_canonical_parts(const std::map<std::string, std::string> &parts,
                           CanonicalParts                           &out,
                           std::string                              *warning)
{
    std::string manifest_bytes;
    if (!find_part(parts, PATH_MANIFEST, manifest_bytes))
        return false;

    out.manifest = parse_json<Manifest>(manifest_bytes);
    if (!require_kind_schema("manifest", out.manifest.kind, KIND_MANIFEST, out.manifest.schema_version, warning))
        return false;

    const FeatureNegotiationResult negotiation = negotiate_features(out.manifest);
    if (!negotiation.unsupported_required_features.empty()) {
        if (warning)
            *warning = unsupported_required_warning(negotiation);
        return false;
    }

    std::string materials_bytes;
    const std::string materials_path = authoritative_path(out.manifest, "materials", "physical_filaments", PATH_MATERIALS);
    if (!read_manifest_part(parts, out.manifest, materials_path, materials_bytes, warning))
        return false;
    out.materials = parse_json<Materials>(materials_bytes);
    if (!require_kind_schema("materials", out.materials.kind, KIND_MATERIALS, out.materials.schema_version, warning))
        return false;

    std::string identity_bytes;
    const std::string identity_path = authoritative_path(out.manifest, "identity_map", "identity-map", PATH_IDENTITY_MAP);
    if (!read_manifest_part(parts, out.manifest, identity_path, identity_bytes, warning))
        return false;
    out.identity_map = parse_json<IdentityMap>(identity_bytes);
    if (!require_kind_schema("identity map", out.identity_map.kind, KIND_IDENTITY_MAP, out.identity_map.schema_version, warning))
        return false;

    std::string assignments_bytes;
    const std::string assignments_path = authoritative_path(out.manifest, "assignments", "material_assignments", PATH_ASSIGNMENTS);
    if (!read_manifest_part(parts, out.manifest, assignments_path, assignments_bytes, warning))
        return false;
    out.assignments = parse_json<Assignments>(assignments_bytes);
    if (!require_kind_schema("assignments", out.assignments.kind, KIND_ASSIGNMENTS, out.assignments.schema_version, warning))
        return false;

    std::string mixed_bytes;
    const std::string mixed_path = authoritative_path(out.manifest, "mixed_filaments", "mixed-filaments", PATH_MIXED_FILAMENTS);
    if (find_part(parts, mixed_path, mixed_bytes)) {
        if (!read_manifest_part(parts, out.manifest, mixed_path, mixed_bytes, warning))
            return false;
        out.mixed_filaments = parse_json<MixedFilaments>(mixed_bytes);
        if (!require_kind_schema("mixed filaments", out.mixed_filaments->kind, KIND_MIXED_FILAMENTS,
                                 out.mixed_filaments->schema_version, warning))
            return false;
    }

    std::string image_maps_bytes;
    const std::string image_maps_path = authoritative_path(out.manifest, "image_maps", "image-maps", PATH_IMAGE_MAPS);
    if (find_part(parts, image_maps_path, image_maps_bytes)) {
        if (!read_manifest_part(parts, out.manifest, image_maps_path, image_maps_bytes, warning))
            return false;
        out.image_maps = parse_json<ImageMaps>(image_maps_bytes);
        if (!require_kind_schema("image maps", out.image_maps->kind, KIND_IMAGE_MAPS, out.image_maps->schema_version, warning))
            return false;
    }

    return true;
}

std::unordered_map<std::string, int> runtime_material_map(const Materials                     &materials,
                                                          const std::optional<MixedFilaments> &mixed_filaments)
{
    std::unordered_map<std::string, int> map;
    for (size_t i = 0; i < materials.physical_filaments.size(); ++i) {
        const PhysicalFilament &filament = materials.physical_filaments[i];
        const int runtime_id = int(filament.source_index == 0 ? i + 1 : filament.source_index);
        map[filament.id] = runtime_id;
    }

    if (mixed_filaments) {
        int runtime_id = int(materials.physical_filaments.size() + 1);
        for (const VirtualFilament &vf : mixed_filaments->virtual_filaments) {
            if (vf.visibility_state != "tombstoned")
                map[vf.id] = runtime_id++;
        }
    }

    return map;
}

std::unordered_map<std::string, ModelVolume *> volume_map_by_stable_id(const IdentityMap              &identity_map,
                                                                       const CanonicalBindingContext &context)
{
    std::unordered_map<std::string, ModelVolume *> map;
    for (const VolumeBinding &binding : identity_map.volume_bindings) {
        auto it = context.model_volumes_by_3mf_id.find(binding.model_volume_id);
        if (it != context.model_volumes_by_3mf_id.end() && it->second != nullptr)
            map[binding.stable_volume_id] = it->second;
    }
    return map;
}

std::vector<PreservedPart> preserved_parts_from_manifest(const std::map<std::string, std::string> &parts,
                                                         const Manifest                           &manifest)
{
    std::vector<PreservedPart> preserved;
    for (const ManifestPart &part : manifest.parts) {
        const std::string zip_path = normalize_package_path(package_path_to_zip_path(part.path));
        if (is_fullspectrum_core_package_path(zip_path))
            continue;
        if (!is_fullspectrum_json_zip_path(zip_path) && !is_preservable_extension_zip_path(zip_path))
            continue;

        std::string bytes;
        if (!find_part(parts, zip_path, bytes))
            continue;

        preserved.push_back(PreservedPart{
            part.path,
            part.content_type,
            part.role,
            bytes,
            part.required,
            true
        });
    }
    return preserved;
}

void store_preserved_parts(Model &model, const std::vector<PreservedPart> &preserved)
{
    if (preserved.empty())
        return;
    set_model_metadata(model, MODEL_METADATA_PRESERVED_PARTS, serialize_json(preserved));
}

ImageMap::SourceKind source_kind_from_string(const std::string &value)
{
    if (value == "texture")
        return ImageMap::SourceKind::Texture;
    if (value == "vertex_colors")
        return ImageMap::SourceKind::VertexColors;
    return ImageMap::SourceKind::FaceColor;
}

ImageMap::WrapMode wrap_mode_from_string(const std::string &value)
{
    return value == "clamp" ? ImageMap::WrapMode::Clamp : ImageMap::WrapMode::Repeat;
}

ImageMap::RenderMode render_mode_from_string(const std::string &value)
{
    if (value == "perimeter_modulation_v2")
        return ImageMap::RenderMode::PerimeterModulationV2;
    if (value == "adaptive_localized_cycles")
        return ImageMap::RenderMode::AdaptiveLocalizedCycles;
    return ImageMap::RenderMode::NormalMix;
}

bool image_map_volume_data_from_canonical(const Manifest                            &manifest,
                                          const ImageMaps                           &image_maps,
                                          const ImageMapVolume                     &source_volume,
                                          const std::map<std::string, std::string> &parts,
                                          const std::unordered_map<std::string, int> &material_to_runtime,
                                          const std::unordered_map<std::string, uint64_t> &material_to_stable,
                                          ImageMap::VolumeData                     &out,
                                          std::string                              *warning)
{
    out = ImageMap::VolumeData{};
    out.topology_fingerprint = source_volume.topology_fingerprint;

    std::unordered_map<std::string, int32_t> asset_indices;
    for (const ImageMapTextureAsset &source_asset : image_maps.texture_assets) {
        std::string bytes;
        if (!read_manifest_part(parts, manifest, source_asset.path, bytes, warning))
            return false;
        const size_t expected = size_t(source_asset.width) * size_t(source_asset.height) * 4;
        if (source_asset.width == 0 || source_asset.height == 0 || bytes.size() != expected) {
            if (warning)
                *warning = "FullSpectrum image-map texture asset has an invalid size: " + source_asset.path;
            return false;
        }
        ImageMap::TextureAsset asset;
        asset.stable_id   = source_asset.id;
        asset.display_name = source_asset.display_name;
        asset.width       = source_asset.width;
        asset.height      = source_asset.height;
        asset.rgba.assign(bytes.begin(), bytes.end());
        asset_indices[source_asset.id] = int32_t(out.texture_assets.size());
        out.texture_assets.emplace_back(std::move(asset));
    }

    for (const ImageMapZone &source_zone : source_volume.zones) {
        ImageMap::Zone zone;
        zone.stable_id                      = source_zone.id;
        zone.display_name                   = source_zone.display_name;
        zone.enabled                        = source_zone.enabled;
        zone.priority                       = source_zone.priority;
        zone.render_mode                    = render_mode_from_string(source_zone.render_mode);
        zone.minimum_component_percent      = source_zone.minimum_component_percent;
        zone.target_sample_size_mm          = float(source_zone.target_sample_size_mm);
        zone.max_facet_samples              = size_t(source_zone.max_facet_samples);
        zone.modulation_sample_spacing_mm   = float(source_zone.modulation_sample_spacing_mm);
        zone.corner_smoothing_radius_mm     = float(source_zone.corner_smoothing_radius_mm);
        for (const ImageMapPaletteEntry &source_entry : source_zone.palette) {
            ImageMap::PaletteEntry entry;
            if (source_entry.target_rgba.size() == 4) {
                for (size_t channel = 0; channel < 4; ++channel)
                    entry.target_color[channel] = float(source_entry.target_rgba[channel]);
            }
            auto runtime_it = material_to_runtime.find(source_entry.material_ref);
            entry.fallback_filament_id = runtime_it == material_to_runtime.end() ?
                                             unsigned(std::max(1, source_entry.fallback_runtime_filament_id)) :
                                             unsigned(runtime_it->second);
            auto stable_it = material_to_stable.find(source_entry.material_ref);
            if (stable_it != material_to_stable.end())
                entry.mixed_filament_stable_id = stable_it->second;
            zone.palette.emplace_back(std::move(entry));
        }
        out.zones.emplace_back(std::move(zone));
    }

    for (const ImageMapTriangleBinding &source_binding : source_volume.triangle_bindings) {
        ImageMap::TriangleBinding binding;
        binding.triangle_index = source_binding.triangle_index;
        binding.zone_index     = source_binding.zone_index;
        binding.source.kind    = source_kind_from_string(source_binding.source.kind);
        binding.source.wrap_u  = wrap_mode_from_string(source_binding.source.wrap_u);
        binding.source.wrap_v  = wrap_mode_from_string(source_binding.source.wrap_v);
        if (!source_binding.source.texture_asset_ref.empty()) {
            auto asset_it = asset_indices.find(source_binding.source.texture_asset_ref);
            if (asset_it == asset_indices.end()) {
                if (warning)
                    *warning = "FullSpectrum image-map triangle references a missing texture asset.";
                return false;
            }
            binding.source.texture_asset_index = asset_it->second;
        }
        if (source_binding.source.uvs.size() == 6) {
            for (size_t corner = 0; corner < 3; ++corner)
                binding.source.uvs[corner] = Vec2f(float(source_binding.source.uvs[corner * 2]),
                                                   float(source_binding.source.uvs[corner * 2 + 1]));
        }
        if (source_binding.source.corner_rgba.size() == 12) {
            for (size_t corner = 0; corner < 3; ++corner)
                for (size_t channel = 0; channel < 4; ++channel)
                    binding.source.corner_colors[corner][channel] = float(source_binding.source.corner_rgba[corner * 4 + channel]);
        }
        out.triangle_bindings.emplace_back(std::move(binding));
    }
    return true;
}

} // namespace

bool ArchiveImportState::accepts_part(const std::string &zip_path) const
{
    return is_fullspectrum_json_zip_path(zip_path) || is_fullspectrum_asset_zip_path(zip_path) || is_preservable_extension_zip_path(zip_path);
}

void ArchiveImportState::add_part(std::string zip_path, std::string bytes)
{
    m_json_parts[normalize_package_path(zip_path)] = std::move(bytes);
}

bool ArchiveImportState::empty() const
{
    return m_json_parts.empty();
}

bool ArchiveImportState::apply_to_config(DynamicPrintConfig &config, std::string *warning) const
{
    return apply_canonical_mixed_filaments_to_config(m_json_parts, config, warning);
}

bool ArchiveImportState::apply_to_model_and_config(Model                         &model,
                                                   DynamicPrintConfig            &config,
                                                   const CanonicalBindingContext  &context,
                                                   std::string                   *warning) const
{
    if (!has_canonical_manifest(m_json_parts))
        return false;

    try {
        CanonicalParts canonical;
        if (!parse_canonical_parts(m_json_parts, canonical, warning)) {
            if (warning && !warning->empty())
                set_model_metadata(model, MODEL_METADATA_WARNING, *warning);
            return false;
        }

        const std::vector<std::string> refs = physical_filament_refs(canonical.materials);
        if (canonical.mixed_filaments)
            set_mixed_definitions(config, legacy_rows_from_mixed_filaments(*canonical.mixed_filaments, refs));
        else
            set_mixed_definitions(config, {});

        for (const ModelObjectBinding &binding : canonical.identity_map.model_object_bindings) {
            auto it = context.model_objects_by_3mf_id.find(binding.model_object_id);
            if (it != context.model_objects_by_3mf_id.end() && it->second != nullptr)
                set_model_metadata(model, stable_object_metadata_key(*it->second), binding.stable_object_id);
        }

        for (const VolumeBinding &binding : canonical.identity_map.volume_bindings) {
            auto it = context.model_volumes_by_3mf_id.find(binding.model_volume_id);
            if (it != context.model_volumes_by_3mf_id.end() && it->second != nullptr)
                set_model_metadata(model, stable_volume_metadata_key(*it->second), binding.stable_volume_id);
        }

        const std::unordered_map<std::string, int> material_to_runtime =
            runtime_material_map(canonical.materials, canonical.mixed_filaments);
        const std::unordered_map<std::string, ModelVolume *> volumes_by_stable_id =
            volume_map_by_stable_id(canonical.identity_map, context);

        if (canonical.image_maps) {
            std::unordered_map<std::string, uint64_t> material_to_stable;
            if (canonical.mixed_filaments) {
                for (const VirtualFilament &filament : canonical.mixed_filaments->virtual_filaments) {
                    if (filament.legacy_stable_id != 0)
                        material_to_stable[filament.id] = filament.legacy_stable_id;
                }
            }
            for (const ImageMapVolume &source_volume : canonical.image_maps->volumes) {
                auto volume_it = volumes_by_stable_id.find(source_volume.stable_volume_id);
                if (volume_it == volumes_by_stable_id.end() || volume_it->second == nullptr)
                    continue;
                ImageMap::VolumeData data;
                if (!image_map_volume_data_from_canonical(canonical.manifest,
                                                          *canonical.image_maps,
                                                          source_volume,
                                                          m_json_parts,
                                                          material_to_runtime,
                                                          material_to_stable,
                                                          data,
                                                          warning) ||
                    !volume_it->second->set_image_map_data(std::move(data))) {
                    if (warning && warning->empty())
                        *warning = "FullSpectrum image-map data does not match its model volume.";
                    set_model_metadata(model, MODEL_METADATA_WARNING, warning ? *warning : "Invalid FullSpectrum image-map data.");
                    return false;
                }
            }
        }

        for (const Assignment &assignment : canonical.assignments.assignments) {
            if (assignment.target.kind != "volume")
                continue;

            auto volume_it = volumes_by_stable_id.find(assignment.target.stable_volume_id);
            auto material_it = material_to_runtime.find(assignment.material_ref);
            if (volume_it == volumes_by_stable_id.end() || material_it == material_to_runtime.end())
                continue;

            ModelVolume *volume = volume_it->second;
            if (volume != nullptr)
                volume->config.set_key_value("extruder", new ConfigOptionInt(material_it->second));
        }

        std::unordered_map<std::string, EnforcerBlockerStateMap> state_maps_by_volume;
        std::unordered_map<std::string, int> max_state_by_volume;
        for (const PaintStateBinding &binding : canonical.assignments.paint_state_bindings) {
            auto material_it = material_to_runtime.find(binding.material_ref);
            if (material_it == material_to_runtime.end() || binding.paint_state <= 0)
                continue;

            EnforcerBlockerStateMap &state_map = state_maps_by_volume[binding.stable_volume_id];
            if (max_state_by_volume[binding.stable_volume_id] == 0) {
                for (size_t i = 0; i < state_map.size(); ++i)
                    state_map[i] = EnforcerBlockerType(i);
            }

            const size_t state_idx = size_t(binding.paint_state);
            const int runtime_id = material_it->second;
            if (state_idx < state_map.size() && runtime_id >= 0 && runtime_id < int(state_map.size())) {
                state_map[state_idx] = EnforcerBlockerType(runtime_id);
                max_state_by_volume[binding.stable_volume_id] =
                    std::max(max_state_by_volume[binding.stable_volume_id], runtime_id);
            }
        }

        for (const auto &kv : state_maps_by_volume) {
            auto volume_it = volumes_by_stable_id.find(kv.first);
            if (volume_it == volumes_by_stable_id.end() || volume_it->second == nullptr)
                continue;
            if (max_state_by_volume[kv.first] <= 0)
                continue;
            volume_it->second->mmu_segmentation_facets.remap_enforcer_block_types(
                *volume_it->second,
                EnforcerBlockerType(max_state_by_volume[kv.first]),
                kv.second);
        }

        store_preserved_parts(model, preserved_parts_from_manifest(m_json_parts, canonical.manifest));
        set_model_metadata(model, MODEL_METADATA_STATUS, "canonical-loaded");
        return true;
    } catch (const std::exception &e) {
        if (warning)
            *warning = e.what();
        set_model_metadata(model, MODEL_METADATA_WARNING, e.what());
        return false;
    }
}

bool has_canonical_manifest(const std::map<std::string, std::string> &parts)
{
    return parts.find(canonical_key(PATH_MANIFEST)) != parts.end() || parts.find(PATH_MANIFEST) != parts.end();
}

bool apply_canonical_mixed_filaments_to_config(const std::map<std::string, std::string> &parts,
                                               DynamicPrintConfig                       &config,
                                               std::string                              *warning)
{
    if (!has_canonical_manifest(parts))
        return false;

    try {
        CanonicalParts canonical;
        if (!parse_canonical_parts(parts, canonical, warning))
            return false;

        const std::vector<std::string> refs = physical_filament_refs(canonical.materials);
        if (!canonical.mixed_filaments) {
            set_mixed_definitions(config, {});
            return true;
        }

        set_mixed_definitions(config, legacy_rows_from_mixed_filaments(*canonical.mixed_filaments, refs));
        return true;
    } catch (const std::exception &e) {
        if (warning)
            *warning = e.what();
        return false;
    }
}

} // namespace Slic3r::FullSpectrum3mf
