#include <GL/glew.h>

#include "3DScene.hpp"
#include "GLShader.hpp"
#include "GUI_App.hpp"
#include "GUI_Colors.hpp"
#include "Plater.hpp"
#include "BitmapCache.hpp"
#include "Camera.hpp"
#include "MixedColorMatchHelpers.hpp"
#include "Frustum.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/ImageMap/ContinuousColorSolver.hpp"
#include "libslic3r/ImageMap/SimplePmCalibration.hpp"
#include "libslic3r/ImageMap/FacetRasterizer.hpp"
#include "libslic3r/ImageMap/Sampling.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/QuadricEdgeCollapse.hpp"
#include <thread>
#include <algorithm>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <unordered_map>

#include <boost/log/trivial.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <Eigen/Dense>

#ifdef HAS_GLSAFE
void glAssertRecentCallImpl(const char* file_name, unsigned int line, const char* function_name)
{
#if defined(NDEBUG)
    // In release mode, only show OpenGL errors if sufficiently high loglevel.
    if (Slic3r::get_logging_level() < 5)
        return;
#endif // NDEBUG

    GLenum err = glGetError();
    if (err == GL_NO_ERROR)
        return;
    const char* sErr = 0;
    switch (err) {
    case GL_INVALID_ENUM: sErr = "Invalid Enum"; break;
    case GL_INVALID_VALUE: sErr = "Invalid Value"; break;
    // be aware that GL_INVALID_OPERATION is generated if glGetError is executed between the execution of glBegin and the corresponding
    // execution of glEnd
    case GL_INVALID_OPERATION: sErr = "Invalid Operation"; break;
    case GL_STACK_OVERFLOW: sErr = "Stack Overflow"; break;
    case GL_STACK_UNDERFLOW: sErr = "Stack Underflow"; break;
    case GL_OUT_OF_MEMORY: sErr = "Out Of Memory"; break;
    default: sErr = "Unknown"; break;
    }
    BOOST_LOG_TRIVIAL(error) << "OpenGL error in " << file_name << ":" << line << ", function " << function_name << "() : " << (int) err
                             << " - " << sErr;
    assert(false);
}
#endif // HAS_GLSAFE

// BBS
std::vector<Slic3r::ColorRGBA> get_extruders_colors()
{
    unsigned char                  rgba_color[4] = {};
    std::vector<std::string>       colors        = Slic3r::GUI::wxGetApp().plater()->get_extruder_colors_from_plater_config();
    std::vector<Slic3r::ColorRGBA> colors_out(colors.size());
    for (const std::string& color : colors) {
        Slic3r::GUI::BitmapCache::parse_color4(color, rgba_color);
        size_t color_idx      = &color - &colors.front();
        colors_out[color_idx] = {
            float(rgba_color[0]) / 255.f,
            float(rgba_color[1]) / 255.f,
            float(rgba_color[2]) / 255.f,
            float(rgba_color[3]) / 255.f,
        };
    }
    return colors_out;
}

float FullyTransparentMaterialThreshold  = 0.1f;
float FullTransparentModdifiedToFixAlpha = 0.3f;
// Be careful changing this value because it could break thumbnail color due to rounding error!
// The color rendering on BambuLab's "send to printer" screen relies on the assumption that this color can be accurately rendered by OpenGL,
// value like 0.18f could not because in C++ (int)(0.18f * 255) == 45 however in OpenGL it renders this as 46
// which breaks the `SelectMachineDialog::record_edge_pixels_data()` function!
float FULL_BLACK_THRESHOLD = 0.2f;

Slic3r::ColorRGBA adjust_color_for_rendering(const Slic3r::ColorRGBA& colors)
{
    if (colors.a() < FullyTransparentMaterialThreshold) { // completely transparent
        return {1, 1, 1, FullTransparentModdifiedToFixAlpha};
    } else if (colors.r() < FULL_BLACK_THRESHOLD && colors.g() < FULL_BLACK_THRESHOLD && colors.b() < FULL_BLACK_THRESHOLD) { // black
        return {FULL_BLACK_THRESHOLD, FULL_BLACK_THRESHOLD, FULL_BLACK_THRESHOLD, colors.a()};
    } else
        return colors;
}

namespace Slic3r {

namespace GUI {

namespace {
constexpr const char* IMAGE_MAP_PREVIEW_PREDICTED_COLORS_KEY = "image_map_preview_predicted_colors";
}

bool image_map_preview_predicted_colors()
{
    return wxGetApp().app_config != nullptr && wxGetApp().app_config->get_bool(IMAGE_MAP_PREVIEW_PREDICTED_COLORS_KEY);
}

void set_image_map_preview_predicted_colors(bool enabled)
{
    if (wxGetApp().app_config != nullptr)
        wxGetApp().app_config->set_bool(IMAGE_MAP_PREVIEW_PREDICTED_COLORS_KEY, enabled);
}

} // namespace GUI

enum class SourceColorPreviewStatus : unsigned char { Running, Ready, Uploading, Uploaded, Cancelled, Failed };

struct SourceTexturePreviewPart
{
    std::unique_ptr<GUI::GLModel::Geometry> geometry;
    std::vector<unsigned char>              rgba;
    unsigned int                            width{0};
    unsigned int                            height{0};
    ImageMap::WrapMode                      wrap_u{ImageMap::WrapMode::Repeat};
    ImageMap::WrapMode                      wrap_v{ImageMap::WrapMode::Repeat};
};

struct SourceColorPreviewJob
{
    explicit SourceColorPreviewJob(std::shared_ptr<const ImageMap::VolumeData> source_data) : data(std::move(source_data)) {}

    std::shared_ptr<const ImageMap::VolumeData> data;
    std::atomic<SourceColorPreviewStatus>       status{SourceColorPreviewStatus::Running};
    std::atomic<int>                            progress{1};
    std::atomic<bool>                           cancel{false};
    std::atomic<bool>                           ready_presented{false};
    std::atomic<bool>                           uploaded_presented{false};
    std::mutex                                  result_mutex;
    std::unique_ptr<GUI::GLModel::Geometry>     geometry;
    std::vector<SourceTexturePreviewPart>       texture_parts;
};

namespace {

// Preserve enough facial/texture detail for large scanned OBJs. Rasterization
// is bounded, cancellable and runs off the UI thread, so this can be materially
// higher than the old emergency cap without bringing back transform stalls.
constexpr size_t       k_source_color_preview_triangle_cap = 200'000;
constexpr size_t       k_source_color_preview_cache_cap    = 65'536;
constexpr unsigned int k_source_texture_preview_max_edge   = 2'048;
constexpr size_t       k_source_texture_preview_max_pixels = size_t(2'048) * size_t(2'048);
constexpr unsigned int k_interactive_texture_preview_max_edge   = 2'048;
constexpr size_t       k_interactive_texture_preview_max_pixels = size_t(2) * size_t(1'024) * size_t(1'024);

struct SourceColorPreviewAssignment
{
    uint32_t                                        zone_index{0};
    RGBA                                            target_color{1.f, 1.f, 1.f, 1.f};
    unsigned int                                    filament_id{0};
    RGBA                                            display_color{1.f, 1.f, 1.f, 1.f};
    std::vector<ImageMap::ContinuousColorComponent> components;
};

const SourceColorPreviewAssignment* nearest_source_color_assignment(const std::vector<SourceColorPreviewAssignment>& assignments,
                                                                    const RGBA&                                      color,
                                                                    std::optional<uint32_t> zone_index = std::nullopt)
{
    const SourceColorPreviewAssignment* best_assignment = nullptr;
    double                              best_distance   = std::numeric_limits<double>::infinity();
    for (const SourceColorPreviewAssignment& assignment : assignments) {
        if (zone_index && assignment.zone_index != *zone_index)
            continue;
        double distance = 0.0;
        for (size_t component = 0; component < 3; ++component) {
            const double delta = double(color[component]) - double(assignment.target_color[component]);
            distance += delta * delta;
        }
        if (distance < best_distance) {
            best_distance   = distance;
            best_assignment = &assignment;
        }
    }
    return best_assignment;
}

unsigned int nearest_source_color_filament(const std::vector<SourceColorPreviewAssignment>& assignments,
                                           const RGBA&                                      color,
                                           std::optional<uint32_t>                          zone_index = std::nullopt)
{
    const SourceColorPreviewAssignment* assignment = nearest_source_color_assignment(assignments, color, zone_index);
    return assignment != nullptr ? assignment->filament_id : 0;
}

size_t source_color_preview_signature(const std::vector<ImageMap::ContinuousColorComponent>& components)
{
    size_t seed         = components.size();
    auto   hash_combine = [&seed](size_t value) { seed ^= value + size_t(0x9e3779b9) + (seed << 6) + (seed >> 2); };
    hash_combine(size_t(ImageMap::simple_pm_calibration_revision()));
    for (const ImageMap::ContinuousColorComponent& component : components) {
        hash_combine(std::hash<std::string>{}(component.color_hex));
        hash_combine(component.transmission_distance_mm ? std::hash<double>{}(*component.transmission_distance_mm) : 0u);
        hash_combine(component.material_id ? std::hash<std::string>{}(*component.material_id) : 0u);
    }
    return seed;
}

size_t source_color_preview_zone_signature(const ImageMap::VolumeData& data)
{
    size_t seed = data.zones.size();
    auto hash_combine = [&seed](size_t value) { seed ^= value + size_t(0x9e3779b9) + (seed << 6) + (seed >> 2); };
    auto hash_scalar = [&hash_combine](double value) { hash_combine(std::hash<double>{}(value)); };
    for (const ImageMap::Zone& zone : data.zones) {
        hash_combine(size_t(zone.enabled));
        hash_combine(static_cast<size_t>(zone.render_mode));
        hash_combine(static_cast<size_t>(zone.color_mix_model));
        hash_scalar(zone.modulation_sample_spacing_mm);
        hash_combine(size_t(zone.disable_broad_path_smoothing));
        hash_scalar(zone.gaussian_smoothing_strength);
        hash_scalar(zone.first_path_smoothing_strength);
        hash_scalar(zone.second_path_smoothing_strength);
        hash_scalar(zone.corner_smoothing_radius_mm);
        hash_scalar(zone.tone_gamma);
        hash_scalar(zone.overhang_contrast_percent);
        hash_scalar(zone.image_exposure_ev);
        hash_scalar(zone.image_contrast_percent);
        hash_scalar(zone.image_saturation_percent);
        hash_scalar(zone.image_edge_boost_percent);
    }
    return seed;
}

bool source_color_preview_components_equal(const std::vector<ImageMap::ContinuousColorComponent>& lhs,
                                           const std::vector<ImageMap::ContinuousColorComponent>& rhs)
{
    if (lhs.size() != rhs.size())
        return false;
    for (size_t index = 0; index < lhs.size(); ++index) {
        if (lhs[index].color_hex != rhs[index].color_hex ||
            lhs[index].transmission_distance_mm != rhs[index].transmission_distance_mm ||
            lhs[index].material_id != rhs[index].material_id)
            return false;
    }
    return true;
}

std::shared_ptr<const ImageMap::ContinuousColorSolver> source_color_preview_solver(
    const std::vector<ImageMap::ContinuousColorComponent>& components,
    ImageMap::ColorMixModel                                color_mix_model)
{
    struct CacheEntry
    {
        ImageMap::ColorMixModel                         color_mix_model{ImageMap::ColorMixModel::FullSpectrumKmKs};
        uint64_t                                        calibration_revision{0};
        std::vector<ImageMap::ContinuousColorComponent> components;
        std::shared_ptr<const ImageMap::ContinuousColorSolver> solver;
    };
    struct CacheState
    {
        std::mutex              mutex;
        std::vector<CacheEntry> entries;
    };

    // Solver construction includes generating and predicting the complete
    // printable candidate set. Live controls used to launch that same work in
    // every replacement job; cancellation cannot interrupt the constructor,
    // so those obsolete jobs accumulated at 2%. Serialize cache misses and
    // retain a small set covering the active filament/model configurations.
    // Preview workers are detached and may still be winding down during GUI
    // shutdown, so keep this process-lifetime state out of static destruction.
    static CacheState*          cache = new CacheState;
    std::lock_guard<std::mutex> lock(cache->mutex);
    const uint64_t calibration_revision = ImageMap::simple_pm_calibration_revision();
    const auto cached = std::find_if(cache->entries.begin(), cache->entries.end(), [&components, color_mix_model, calibration_revision](const CacheEntry& entry) {
        return entry.color_mix_model == color_mix_model && entry.calibration_revision == calibration_revision &&
               source_color_preview_components_equal(entry.components, components);
    });
    if (cached != cache->entries.end())
        return cached->solver;

    auto solver = std::make_shared<ImageMap::ContinuousColorSolver>(components, color_mix_model);
    if (!solver->valid())
        return {};
    constexpr size_t maximum_cached_solvers = 12;
    if (cache->entries.size() >= maximum_cached_solvers)
        cache->entries.erase(cache->entries.begin());
    cache->entries.push_back(CacheEntry{color_mix_model, calibration_revision, components, solver});
    return solver;
}

std::vector<ImageMap::ContinuousColorComponent> source_color_preview_components()
{
    std::vector<ImageMap::ContinuousColorComponent> components;
    PresetBundle*                                   preset_bundle = GUI::wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        return components;

    const ConfigOptionStrings* colors = preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    if (colors == nullptr)
        return components;
    const ConfigOptionFloats* transmission_distances = preset_bundle->project_config.option<ConfigOptionFloats>(
        "filament_transmission_distance");
    const ConfigOptionStrings* material_ids = preset_bundle->project_config.option<ConfigOptionStrings>(
        "filament_full_spectrum_material_id");

    components.reserve(colors->values.size());
    for (size_t index = 0; index < colors->values.size(); ++index) {
        ImageMap::ContinuousColorComponent component;
        component.color_hex = colors->values[index];
        if (MixedFilamentManager::use_td_for_color_prediction() && transmission_distances != nullptr &&
            index < transmission_distances->values.size()) {
            const double td_mm = transmission_distances->values[index];
            if (std::isfinite(td_mm) && td_mm > EPSILON)
                component.transmission_distance_mm = td_mm;
        }
        if (material_ids != nullptr && index < material_ids->values.size() && !material_ids->values[index].empty())
            component.material_id = material_ids->values[index];
        components.emplace_back(std::move(component));
    }
    return components;
}

uint32_t preview_color_key(const RGBA& color)
{
    auto channel = [](float value) { return uint32_t(std::lround(std::clamp(value, 0.f, 1.f) * 255.f)); };
    return (channel(color[0]) << 16) | (channel(color[1]) << 8) | channel(color[2]);
}

RGBA source_color_preview_rgba(const std::string& color_hex)
{
    const wxColour color = GUI::parse_mixed_color(color_hex);
    return {float(color.Red()) / 255.f, float(color.Green()) / 255.f, float(color.Blue()) / 255.f, 1.f};
}

size_t source_color_preview_assignment_signature(const std::vector<SourceColorPreviewAssignment>& assignments)
{
    size_t seed         = assignments.size();
    auto   hash_combine = [&seed](size_t value) { seed ^= value + size_t(0x9e3779b9) + (seed << 6) + (seed >> 2); };
    for (const SourceColorPreviewAssignment& assignment : assignments) {
        hash_combine(assignment.zone_index);
        hash_combine(preview_color_key(assignment.target_color));
        hash_combine(assignment.filament_id);
        hash_combine(preview_color_key(assignment.display_color));
        for (const ImageMap::ContinuousColorComponent& component : assignment.components) {
            hash_combine(std::hash<std::string>{}(component.color_hex));
            hash_combine(component.transmission_distance_mm ? std::hash<double>{}(*component.transmission_distance_mm) : 0u);
            hash_combine(component.material_id ? std::hash<std::string>{}(*component.material_id) : 0u);
        }
    }
    return seed;
}

std::array<Vec2f, 3> unwrap_source_texture_uvs(const ImageMap::SurfaceSource& source)
{
    std::array<Vec2f, 3> out         = source.uvs;
    auto                 unwrap_axis = [&out](bool use_u_axis, ImageMap::WrapMode wrap_mode) {
        if (wrap_mode != ImageMap::WrapMode::Repeat)
            return;
        std::array<float, 3> values{use_u_axis ? out[0].x() : out[0].y(), use_u_axis ? out[1].x() : out[1].y(),
                                    use_u_axis ? out[2].x() : out[2].y()};
        if (!std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); }))
            return;
        const bool has_repeat_evidence = std::any_of(values.begin(), values.end(), [](float value) {
            constexpr float epsilon = 1e-6f;
            return value < -epsilon || value > 1.f + epsilon;
        });
        auto       span                = [](const std::array<float, 3>& coordinates) {
            return std::max({coordinates[0], coordinates[1], coordinates[2]}) - std::min({coordinates[0], coordinates[1], coordinates[2]});
        };
        const float original_span = span(values);
        if (!has_repeat_evidence || original_span <= 0.5f)
            return;

        std::array<float, 3> best      = values;
        float                best_span = original_span;
        for (size_t anchor = 0; anchor < values.size(); ++anchor) {
            std::array<float, 3> candidate = values;
            for (size_t index = 0; index < candidate.size(); ++index) {
                const float delta = values[index] - values[anchor];
                candidate[index]  = values[anchor] + delta - std::round(delta);
            }
            const float candidate_span = span(candidate);
            if (candidate_span + 1e-6f < best_span) {
                best      = candidate;
                best_span = candidate_span;
            }
        }
        if (best_span >= original_span - 1e-6f)
            return;
        for (size_t index = 0; index < out.size(); ++index) {
            if (use_u_axis)
                out[index].x() = best[index];
            else
                out[index].y() = best[index];
        }
    };
    unwrap_axis(true, source.wrap_u);
    unwrap_axis(false, source.wrap_v);
    return out;
}

std::pair<unsigned int, unsigned int> source_texture_preview_size(const ImageMap::TextureAsset& asset, bool interactive_preview)
{
    if (!asset.valid())
        return {0, 0};
    const unsigned int maximum_edge = interactive_preview ? k_interactive_texture_preview_max_edge : k_source_texture_preview_max_edge;
    const size_t maximum_pixels = interactive_preview ? k_interactive_texture_preview_max_pixels : k_source_texture_preview_max_pixels;
    double scale = std::min({1.0, double(maximum_edge) / double(asset.width), double(maximum_edge) / double(asset.height),
                             std::sqrt(double(maximum_pixels) / (double(asset.width) * double(asset.height)))});
    return {std::max(1u, unsigned(std::floor(double(asset.width) * scale))),
            std::max(1u, unsigned(std::floor(double(asset.height) * scale)))};
}

double source_texture_preview_mm_per_pixel(const indexed_triangle_set&                         its,
                                           const std::vector<const ImageMap::TriangleBinding*>& bindings,
                                           unsigned int                                        preview_width,
                                           unsigned int                                        preview_height)
{
    double model_length_sum = 0.;
    double pixel_length_sum = 0.;
    for (const ImageMap::TriangleBinding* binding : bindings) {
        if (binding == nullptr || binding->triangle_index >= its.indices.size())
            continue;
        const stl_triangle_vertex_indices& indices = its.indices[binding->triangle_index];
        if (indices[0] < 0 || indices[1] < 0 || indices[2] < 0 || size_t(indices[0]) >= its.vertices.size() ||
            size_t(indices[1]) >= its.vertices.size() || size_t(indices[2]) >= its.vertices.size())
            continue;
        const std::array<Vec2f, 3> uvs = unwrap_source_texture_uvs(binding->source);
        for (size_t edge = 0; edge < 3; ++edge) {
            const size_t next = (edge + 1) % 3;
            const Vec2f  pixel_delta((uvs[next].x() - uvs[edge].x()) * float(preview_width),
                                    (uvs[next].y() - uvs[edge].y()) * float(preview_height));
            const double pixel_length = double(pixel_delta.norm());
            const double model_length = double((its.vertices[size_t(indices[next])] - its.vertices[size_t(indices[edge])]).norm());
            if (!std::isfinite(pixel_length) || !std::isfinite(model_length) || pixel_length <= EPSILON || model_length <= EPSILON)
                continue;
            pixel_length_sum += pixel_length;
            model_length_sum += model_length;
        }
    }
    return pixel_length_sum > EPSILON ? std::clamp(model_length_sum / pixel_length_sum, 0.002, 2.0) : 0.05;
}

struct PreviewFilterLayout
{
    // An almost-horizontal mapped surface lies in one slice plane, so its
    // reconstruction neighbourhood is genuinely two-dimensional. Side walls
    // instead use the equal-Z scan lines below; filtering across those lines
    // would mix different print layers in the prepare preview.
    bool                             gaussian_is_two_dimensional{false};
    double                           mm_per_step{0.05};
    std::vector<std::vector<size_t>> equal_z_lines;
};

PreviewFilterLayout source_texture_preview_filter_layout(
    const indexed_triangle_set&                         its,
    const std::vector<const ImageMap::TriangleBinding*>& bindings,
    unsigned int                                        preview_width,
    unsigned int                                        preview_height,
    const Transform3d&                                  mesh_to_world,
    const std::atomic<bool>&                            cancel)
{
    PreviewFilterLayout result;
    if (preview_width == 0 || preview_height == 0)
        return result;

    // Fit the affine texture-pixel -> world-position map represented by the
    // bound triangles. Projection UVs are affine even when triangle edges
    // split the image, and the Z rows remain affine on common curved targets
    // such as cylinders. Using all corners makes the estimate insensitive to
    // the mesh tessellation and gives us the direction tangent to a slice.
    Eigen::Matrix3d normal_matrix = Eigen::Matrix3d::Zero();
    Eigen::Matrix3d right_hand    = Eigen::Matrix3d::Zero();
    size_t          sample_count  = 0;
    for (size_t binding_index = 0; binding_index < bindings.size(); ++binding_index) {
        if ((binding_index & 0x3ffu) == 0u && cancel.load(std::memory_order_relaxed))
            return result;
        const ImageMap::TriangleBinding* binding = bindings[binding_index];
        if (binding == nullptr || binding->triangle_index >= its.indices.size())
            continue;
        const stl_triangle_vertex_indices& indices = its.indices[binding->triangle_index];
        if (indices[0] < 0 || indices[1] < 0 || indices[2] < 0 || size_t(indices[0]) >= its.vertices.size() ||
            size_t(indices[1]) >= its.vertices.size() || size_t(indices[2]) >= its.vertices.size())
            continue;
        const std::array<Vec2f, 3> uvs = unwrap_source_texture_uvs(binding->source);
        for (size_t corner = 0; corner < 3; ++corner) {
            const Eigen::Vector3d predictor(double(uvs[corner].x()) * double(preview_width),
                                            double(uvs[corner].y()) * double(preview_height), 1.0);
            const Vec3d world = mesh_to_world * its.vertices[size_t(indices[corner])].cast<double>();
            if (!predictor.allFinite() || !world.allFinite())
                continue;
            normal_matrix.noalias() += predictor * predictor.transpose();
            right_hand.noalias() += predictor * world.transpose();
            ++sample_count;
        }
    }

    Vec3d world_per_x_pixel = Vec3d::Zero();
    Vec3d world_per_y_pixel = Vec3d::Zero();
    bool  fitted            = false;
    if (sample_count >= 3) {
        Eigen::FullPivLU<Eigen::Matrix3d> decomposition(normal_matrix);
        if (decomposition.rank() == 3) {
            const Eigen::Matrix3d coefficients = decomposition.solve(right_hand);
            if (coefficients.allFinite()) {
                world_per_x_pixel = coefficients.row(0).transpose();
                world_per_y_pixel = coefficients.row(1).transpose();
                fitted            = true;
            }
        }
    }

    const double fallback_mm_per_pixel = source_texture_preview_mm_per_pixel(its, bindings, preview_width, preview_height);
    Vec2d        equal_z_tangent(1.0, 0.0);
    if (fitted) {
        const Vec2d  z_gradient(world_per_x_pixel.z(), world_per_y_pixel.z());
        const double xy_scale = std::max(world_per_x_pixel.head<2>().norm(), world_per_y_pixel.head<2>().norm());
        // When Z changes by less than five percent of an in-plane pixel, the
        // mapped patch is effectively coplanar with a layer and the slicer's
        // world-XY Gaussian really can gather samples in both texture axes.
        result.gaussian_is_two_dimensional = z_gradient.norm() <= std::max(1e-7, 0.05 * xy_scale);
        if (!result.gaussian_is_two_dimensional && z_gradient.squaredNorm() > 1e-14)
            equal_z_tangent = Vec2d(z_gradient.y(), -z_gradient.x()).normalized();
    }

    const bool advance_x = std::abs(equal_z_tangent.x()) >= std::abs(equal_z_tangent.y());
    if (advance_x) {
        const double slope       = std::abs(equal_z_tangent.x()) > 1e-12 ? equal_z_tangent.y() / equal_z_tangent.x() : 0.0;
        const double end_offset  = -slope * double(preview_width - 1);
        const int    minimum_bin = int(std::floor(std::min(0.0, end_offset))) - 1;
        const int    maximum_bin = int(std::ceil(double(preview_height - 1) + std::max(0.0, end_offset))) + 1;
        result.equal_z_lines.resize(size_t(maximum_bin - minimum_bin + 1));
        // X-major insertion leaves every digital scan line in path order.
        for (unsigned int x = 0; x < preview_width; ++x) {
            if ((x & 0x1fu) == 0u && cancel.load(std::memory_order_relaxed))
                return result;
            for (unsigned int y = 0; y < preview_height; ++y) {
                const int bin = int(std::lround(double(y) - slope * double(x)));
                result.equal_z_lines[size_t(bin - minimum_bin)].emplace_back(size_t(y) * size_t(preview_width) + x);
            }
        }
        if (fitted)
            result.mm_per_step = (world_per_x_pixel + slope * world_per_y_pixel).head<2>().norm();
    } else {
        const double slope       = equal_z_tangent.x() / equal_z_tangent.y();
        const double end_offset  = -slope * double(preview_height - 1);
        const int    minimum_bin = int(std::floor(std::min(0.0, end_offset))) - 1;
        const int    maximum_bin = int(std::ceil(double(preview_width - 1) + std::max(0.0, end_offset))) + 1;
        result.equal_z_lines.resize(size_t(maximum_bin - minimum_bin + 1));
        // Y-major insertion leaves every digital scan line in path order.
        for (unsigned int y = 0; y < preview_height; ++y) {
            if ((y & 0x1fu) == 0u && cancel.load(std::memory_order_relaxed))
                return result;
            for (unsigned int x = 0; x < preview_width; ++x) {
                const int bin = int(std::lround(double(x) - slope * double(y)));
                result.equal_z_lines[size_t(bin - minimum_bin)].emplace_back(size_t(y) * size_t(preview_width) + x);
            }
        }
        if (fitted)
            result.mm_per_step = (slope * world_per_x_pixel + world_per_y_pixel).head<2>().norm();
    }
    if (!std::isfinite(result.mm_per_step) || result.mm_per_step <= 0.002)
        result.mm_per_step = fallback_mm_per_pixel;
    result.mm_per_step = std::clamp(result.mm_per_step, 0.002, 2.0);
    return result;
}

bool box_blur_weight_field(const std::vector<float>& source,
                           std::vector<float>&       destination,
                           unsigned int              width,
                           unsigned int              height,
                           size_t                    components,
                           int                       radius,
                           bool                      horizontal,
                           const std::atomic<bool>&  cancel)
{
    if (radius <= 0 || source.empty()) {
        destination = source;
        return true;
    }
    destination.resize(source.size());
    const int extent = horizontal ? int(width) : int(height);
    const int lines  = horizontal ? int(height) : int(width);
    auto index = [width, components, horizontal](int line, int position, size_t component) {
        const size_t x = size_t(horizontal ? position : line);
        const size_t y = size_t(horizontal ? line : position);
        return (y * size_t(width) + x) * components + component;
    };
    const double scale = 1. / double(2 * radius + 1);
    for (int line = 0; line < lines; ++line) {
        if ((line & 0x1f) == 0 && cancel.load(std::memory_order_relaxed))
            return false;
        for (size_t component = 0; component < components; ++component) {
            double sum = double(radius + 1) * source[index(line, 0, component)];
            for (int tap = 1; tap <= radius; ++tap)
                sum += source[index(line, std::min(tap, extent - 1), component)];
            for (int position = 0; position < extent; ++position) {
                destination[index(line, position, component)] = float(sum * scale);
                const int remove_position = std::clamp(position - radius, 0, extent - 1);
                const int add_position    = std::clamp(position + radius + 1, 0, extent - 1);
                sum += source[index(line, add_position, component)] - source[index(line, remove_position, component)];
            }
        }
    }
    return true;
}

bool box_blur_weight_lines(const std::vector<float>&              source,
                           std::vector<float>&                    destination,
                           const std::vector<std::vector<size_t>>& lines,
                           size_t                                 components,
                           int                                    radius,
                           const std::atomic<bool>&                cancel)
{
    if (radius <= 0 || source.empty()) {
        destination = source;
        return true;
    }
    destination.resize(source.size());
    const double scale = 1. / double(2 * radius + 1);
    for (size_t line_index = 0; line_index < lines.size(); ++line_index) {
        if ((line_index & 0x1fu) == 0u && cancel.load(std::memory_order_relaxed))
            return false;
        const std::vector<size_t>& line = lines[line_index];
        if (line.empty())
            continue;
        const int extent = int(line.size());
        for (size_t component = 0; component < components; ++component) {
            auto value = [&source, &line, components, component](int position) {
                return source[line[size_t(position)] * components + component];
            };
            double sum = double(radius + 1) * value(0);
            for (int tap = 1; tap <= radius; ++tap)
                sum += value(std::min(tap, extent - 1));
            for (int position = 0; position < extent; ++position) {
                destination[line[size_t(position)] * components + component] = float(sum * scale);
                const int remove_position = std::clamp(position - radius, 0, extent - 1);
                const int add_position    = std::clamp(position + radius + 1, 0, extent - 1);
                sum += value(add_position) - value(remove_position);
            }
        }
    }
    return true;
}

bool gaussian_blur_weight_field(std::vector<float>&       field,
                                unsigned int              width,
                                unsigned int              height,
                                size_t                    components,
                                double                    sigma_pixels,
                                const PreviewFilterLayout& layout,
                                const std::atomic<bool>&  cancel)
{
    if (sigma_pixels <= 0.15 || field.empty())
        return true;
    constexpr int box_count = 3;
    const double ideal_width = std::sqrt(12. * sigma_pixels * sigma_pixels / double(box_count) + 1.);
    int          lower_width = std::max(1, int(std::floor(ideal_width)));
    if ((lower_width & 1) == 0)
        --lower_width;
    const int upper_width = lower_width + 2;
    const double numerator = 12. * sigma_pixels * sigma_pixels - double(box_count * lower_width * lower_width) -
                             double(4 * box_count * lower_width + 3 * box_count);
    const int lower_count = std::clamp(int(std::lround(numerator / double(-4 * lower_width - 4))), 0, box_count);

    std::vector<float> temporary;
    for (int pass = 0; pass < box_count; ++pass) {
        const int radius = ((pass < lower_count ? lower_width : upper_width) - 1) / 2;
        if (layout.gaussian_is_two_dimensional) {
            if (!box_blur_weight_field(field, temporary, width, height, components, radius, true, cancel) ||
                !box_blur_weight_field(temporary, field, width, height, components, radius, false, cancel))
                return false;
        } else if (!box_blur_weight_lines(field, temporary, layout.equal_z_lines, components, radius, cancel)) {
            return false;
        } else {
            field.swap(temporary);
        }
    }
    return true;
}

void compact_preview_weights(std::vector<float>& field, size_t components)
{
    for (size_t offset = 0; offset < field.size(); offset += components) {
        float maximum = 0.f;
        for (size_t component = 0; component < components; ++component)
            maximum = std::max(maximum, field[offset + component]);
        if (maximum <= EPSILON)
            continue;
        for (size_t component = 0; component < components; ++component)
            field[offset + component] = std::clamp(field[offset + component] / maximum, 0.f, 1.f);
    }
}

bool triangular_preview_smoothing(std::vector<float>&       field,
                                  const PreviewFilterLayout& layout,
                                  size_t                    components,
                                  double                    radius_steps,
                                  const std::atomic<bool>&  cancel)
{
    if (radius_steps <= 0.5 || field.empty())
        return true;
    std::vector<float>  smoothed(field);
    auto range_sum = [](const std::vector<double>& values, int begin, int end) {
        return end >= begin ? values[size_t(end + 1)] - values[size_t(begin)] : 0.;
    };
    for (size_t line_index = 0; line_index < layout.equal_z_lines.size(); ++line_index) {
        if ((line_index & 0x1fu) == 0u && cancel.load(std::memory_order_relaxed))
            return false;
        const std::vector<size_t>& line = layout.equal_z_lines[line_index];
        if (line.size() < 2)
            continue;
        std::vector<double> prefix(line.size() + 1);
        std::vector<double> indexed_prefix(line.size() + 1);
        for (size_t component = 0; component < components; ++component) {
            prefix[0] = indexed_prefix[0] = 0.;
            for (size_t position = 0; position < line.size(); ++position) {
                const double inset = 1. - double(field[line[position] * components + component]);
                prefix[position + 1]         = prefix[position] + inset;
                indexed_prefix[position + 1] = indexed_prefix[position] + inset * double(position);
            }
            for (size_t position = 0; position < line.size(); ++position) {
                const int center = int(position);
                const int left   = std::max(0, int(std::ceil(double(center) - radius_steps)));
                const int right  = std::min(int(line.size()) - 1, int(std::floor(double(center) + radius_steps)));
                const double left_sum     = range_sum(prefix, left, center);
                const double left_indexed = range_sum(indexed_prefix, left, center);
                const double right_sum     = range_sum(prefix, center + 1, right);
                const double right_indexed = range_sum(indexed_prefix, center + 1, right);
                const double weighted_sum = left_sum * (1. - double(center) / radius_steps) + left_indexed / radius_steps +
                                            right_sum * (1. + double(center) / radius_steps) - right_indexed / radius_steps;
                const double left_count   = double(center - left + 1);
                const double right_count  = double(std::max(0, right - center));
                const double left_indices = 0.5 * double(left + center) * left_count;
                const double right_indices = right_count > 0. ? 0.5 * double(center + 1 + right) * right_count : 0.;
                const double weight_sum = left_count * (1. - double(center) / radius_steps) + left_indices / radius_steps +
                                          right_count * (1. + double(center) / radius_steps) - right_indices / radius_steps;
                const size_t offset = line[position] * components + component;
                const double inset  = 1. - double(field[offset]);
                const double filtered_inset = weight_sum > EPSILON ? std::min(inset, weighted_sum / weight_sum) : inset;
                smoothed[offset] = float(std::clamp(1. - filtered_inset, 0., 1.));
            }
        }
    }
    field.swap(smoothed);
    return true;
}

bool slope_limit_preview_weights(std::vector<float>&       field,
                                 const PreviewFilterLayout& layout,
                                 size_t                    components,
                                 double                    sample_spacing_mm,
                                 const std::atomic<bool>&  cancel)
{
    if (field.empty())
        return true;
    constexpr double max_displacement_mm = 0.63;
    // Slice-time limiting runs on the resampled perimeter, where every
    // sample-to-sample edge receives the 0.015 mm transition allowance. A
    // preview texel may cover several of those edges (especially in 0.02 mm
    // Ultra mode), so charging the allowance only once per texel stretches a
    // transition and makes the prepare result visibly softer than the sliced
    // path. Integrate the same allowance over the represented distance.
    const double bounded_sample_spacing = std::clamp(sample_spacing_mm, 0.02, 2.0);
    const double represented_steps      = layout.mm_per_step / bounded_sample_spacing;
    const float  limit                  = float((layout.mm_per_step * 0.35 + 0.015 * represented_steps) /
                               max_displacement_mm);
    for (int pass = 0; pass < 4; ++pass) {
        for (size_t line_index = 0; line_index < layout.equal_z_lines.size(); ++line_index) {
            if ((line_index & 0x1fu) == 0u && cancel.load(std::memory_order_relaxed))
                return false;
            const std::vector<size_t>& line = layout.equal_z_lines[line_index];
            if (line.size() < 2)
                continue;
            for (size_t component = 0; component < components; ++component) {
                for (size_t position = 1; position < line.size(); ++position) {
                    const size_t offset          = line[position] * components + component;
                    const size_t previous_offset = line[position - 1] * components + component;
                    field[offset] = std::max(field[offset], field[previous_offset] - limit);
                }
                for (size_t position = line.size() - 1; position-- > 0;) {
                    const size_t offset      = line[position] * components + component;
                    const size_t next_offset = line[position + 1] * components + component;
                    field[offset] = std::max(field[offset], field[next_offset] - limit);
                }
            }
        }
    }
    return true;
}

bool build_perimeter_modulation_texture_preview(const ImageMap::TextureAsset&                      asset,
                                                const ImageMap::Zone&                              zone,
                                                const ImageMap::ContinuousColorSolver&             solver,
                                                const indexed_triangle_set&                        its,
                                                const std::vector<const ImageMap::TriangleBinding*>& bindings,
                                                unsigned int                                       width,
                                                unsigned int                                       height,
                                                ImageMap::WrapMode                                 wrap_u,
                                                ImageMap::WrapMode                                 wrap_v,
                                                const Transform3d&                                 mesh_to_world,
                                                std::atomic<bool>&                                 cancel,
                                                const std::function<void(float)>&                   progress,
                                                std::vector<unsigned char>&                        output)
{
    const size_t components = solver.component_count();
    if (!solver.valid() || components == 0 || width == 0 || height == 0)
        return false;
    const size_t pixel_count = size_t(width) * size_t(height);
    std::vector<float> weights(pixel_count * components, 0.f);
    std::vector<float> alpha(pixel_count, 1.f);
    constexpr size_t quantized_color_count = 32u * 32u * 32u;
    std::vector<uint16_t> pixel_color_keys(pixel_count, 0);
    std::vector<uint8_t>  used_color_keys(quantized_color_count, 0);

    // Sampling is deliberately separate from physical-mixture lookup. A
    // photographic row may contain thousands of new colours; doing both in
    // one loop left progress at exactly 10% until all those KD searches had
    // completed, which looked like a hung worker.
    for (unsigned int y = 0; y < height; ++y) {
        if ((y & 0x1fu) == 0u && cancel.load(std::memory_order_relaxed))
            return false;
        if ((y & 0x07u) == 0u && progress)
            progress(0.18f * float(y) / float(height));
        for (unsigned int x = 0; x < width; ++x) {
            const Vec2f uv((float(x) + 0.5f) / float(width), (float(y) + 0.5f) / float(height));
            RGBA sampled = ImageMap::sample_processed_texture(asset, uv, wrap_u, wrap_v, zone);
            const size_t pixel = size_t(y) * size_t(width) + x;
            alpha[pixel] = sampled[3];
            for (size_t channel = 0; channel < 3; ++channel)
                sampled[channel] = sampled[channel] * sampled[3] + (1.f - sampled[3]);
            sampled[3] = 1.f;
            const RGBA target = ImageMap::adjusted_modulation_target_color(sampled, zone);
            auto quantized_channel = [&target](size_t channel) {
                return uint32_t(std::lround(std::clamp(target[channel], 0.f, 1.f) * 31.f));
            };
            const uint32_t key = (quantized_channel(0) << 10) | (quantized_channel(1) << 5) | quantized_channel(2);
            pixel_color_keys[pixel] = uint16_t(key);
            used_color_keys[key]    = 1;
        }
    }
    if (progress)
        progress(0.18f);

    std::vector<uint16_t> unique_color_keys;
    unique_color_keys.reserve(quantized_color_count);
    for (size_t key = 0; key < used_color_keys.size(); ++key)
        if (used_color_keys[key] != 0)
            unique_color_keys.emplace_back(uint16_t(key));

    std::vector<float> solved_weights(quantized_color_count * components, 0.f);
    std::atomic<size_t> next_color{0};
    std::atomic<size_t> completed_colors{0};
    const size_t worker_count = std::min<size_t>(4, unique_color_keys.size());
    std::vector<std::thread> solve_workers;
    solve_workers.reserve(worker_count);
    for (size_t worker = 0; worker < worker_count; ++worker) {
        solve_workers.emplace_back([&]() {
            for (;;) {
                const size_t color_index = next_color.fetch_add(1, std::memory_order_relaxed);
                if (color_index >= unique_color_keys.size() || cancel.load(std::memory_order_relaxed))
                    return;
                const uint32_t key   = unique_color_keys[color_index];
                const RGBA target{float((key >> 10) & 31u) / 31.f, float((key >> 5) & 31u) / 31.f,
                                  float(key & 31u) / 31.f, 1.f};
                // The shared solver retains this candidate index, making
                // later slider edits a constant-time lookup.
                std::vector<double> solved = solver.solve_quantized_5bit(target);
                ImageMap::apply_modulation_component_contrast(solved, zone);
                if (solved.size() == components) {
                    const size_t offset = size_t(key) * components;
                    for (size_t component = 0; component < components; ++component)
                        solved_weights[offset + component] = float(solved[component]);
                }
                const size_t completed = completed_colors.fetch_add(1, std::memory_order_relaxed) + 1;
                if ((completed & 0x1fu) == 0u && progress)
                    progress(0.18f + 0.30f * float(completed) / float(std::max<size_t>(unique_color_keys.size(), 1)));
            }
        });
    }
    for (std::thread& worker : solve_workers)
        worker.join();
    if (cancel.load(std::memory_order_relaxed))
        return false;
    if (progress)
        progress(0.48f);

    for (unsigned int y = 0; y < height; ++y) {
        if ((y & 0x1fu) == 0u && cancel.load(std::memory_order_relaxed))
            return false;
        if ((y & 0x0fu) == 0u && progress)
            progress(0.48f + 0.07f * float(y) / float(height));
        for (unsigned int x = 0; x < width; ++x) {
            const size_t pixel         = size_t(y) * size_t(width) + x;
            const size_t source_offset = size_t(pixel_color_keys[pixel]) * components;
            const size_t output_offset = pixel * components;
            std::copy_n(solved_weights.begin() + source_offset, components, weights.begin() + output_offset);
        }
    }
    if (progress)
        progress(0.55f);

    const PreviewFilterLayout filter_layout =
        source_texture_preview_filter_layout(its, bindings, width, height, mesh_to_world, cancel);
    if (cancel.load(std::memory_order_relaxed))
        return false;
    if (progress)
        progress(0.60f);
    const double gaussian_strength = std::clamp(double(zone.gaussian_smoothing_strength), 0., 4.);
    const double base_sigma_mm      = std::max(0.04, 0.45 * double(zone.modulation_sample_spacing_mm));
    if (!gaussian_blur_weight_field(weights, width, height, components,
                                    base_sigma_mm * gaussian_strength / filter_layout.mm_per_step, filter_layout, cancel))
        return false;
    compact_preview_weights(weights, components);
    if (progress)
        progress(0.69f);

    if (!zone.disable_broad_path_smoothing) {
        const double smoothing_base_mm = std::max(0.42, double(zone.corner_smoothing_radius_mm));
        const double first_radius_mm   = std::max(0.30, 1.15 * smoothing_base_mm) *
                                       std::clamp(double(zone.first_path_smoothing_strength), 0., 4.);
        const double second_radius_mm = std::max(0.20, 0.45 * smoothing_base_mm) *
                                        std::clamp(double(zone.second_path_smoothing_strength), 0., 4.);
        if (!triangular_preview_smoothing(weights, filter_layout, components, first_radius_mm / filter_layout.mm_per_step, cancel))
            return false;
        if (progress)
            progress(0.75f);
        if (!slope_limit_preview_weights(weights, filter_layout, components, zone.modulation_sample_spacing_mm, cancel))
            return false;
        if (progress)
            progress(0.80f);
        if (!triangular_preview_smoothing(weights, filter_layout, components, second_radius_mm / filter_layout.mm_per_step, cancel))
            return false;
    } else {
        if (!slope_limit_preview_weights(weights, filter_layout, components, zone.modulation_sample_spacing_mm, cancel))
            return false;
    }
    if (progress)
        progress(0.84f);

    output.resize(pixel_count * 4);
    std::unordered_map<uint64_t, RGBA> prediction_cache;
    prediction_cache.reserve(8192);
    std::vector<double> quantized_weights(components);
    for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
        if ((pixel & 0xffffu) == 0u && cancel.load(std::memory_order_relaxed))
            return false;
        if ((pixel & 0xffffu) == 0u && progress)
            progress(0.84f + 0.16f * float(double(pixel) / double(pixel_count)));
        uint64_t key = 0;
        for (size_t component = 0; component < components; ++component) {
            const uint64_t quantized = uint64_t(std::lround(std::clamp(weights[pixel * components + component], 0.f, 1.f) * 31.f));
            quantized_weights[component] = double(quantized) / 31.;
            key = key * 33u + quantized;
        }
        auto predicted = prediction_cache.find(key);
        if (predicted == prediction_cache.end()) {
            const RGBA color = solver.predict_weights(quantized_weights).value_or(RGBA{1.f, 1.f, 1.f, 1.f});
            predicted = prediction_cache.emplace(key, color).first;
        }
        for (size_t channel = 0; channel < 3; ++channel)
            output[pixel * 4 + channel] = uint8_t(std::lround(std::clamp(predicted->second[channel], 0.f, 1.f) * 255.f));
        output[pixel * 4 + 3] = uint8_t(std::lround(std::clamp(alpha[pixel], 0.f, 1.f) * 255.f));
    }
    if (progress)
        progress(1.f);
    return true;
}

std::shared_ptr<SourceColorPreviewJob> start_source_color_preview(std::shared_ptr<const TriangleMesh>             mesh,
                                                                  std::shared_ptr<const ImageMap::VolumeData>     data,
                                                                  std::vector<ImageMap::ContinuousColorComponent> components,
                                                                  std::vector<SourceColorPreviewAssignment>       assignments,
                                                                  std::string                                     volume_name,
                                                                  ImageMap::RenderMode                            render_mode,
                                                                  bool                                            quantize_to_palette,
                                                                  bool                                            predicted_colors,
                                                                  Transform3d                                     mesh_to_world,
                                                                  bool                                            interactive_preview)
{
    auto job = std::make_shared<SourceColorPreviewJob>(data);
    std::thread([job, mesh = std::move(mesh), data = std::move(data), components = std::move(components),
                 assignments = std::move(assignments), volume_name = std::move(volume_name), render_mode, quantize_to_palette,
                 predicted_colors, mesh_to_world = std::move(mesh_to_world), interactive_preview]() mutable {
        const auto started_at = std::chrono::steady_clock::now();
        try {
            job->progress.store(2, std::memory_order_relaxed);
            const bool adaptive_cycle_preview = predicted_colors && render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles;
            quantize_to_palette                = predicted_colors && quantize_to_palette;
            auto color_model_index = [](ImageMap::ColorMixModel model) {
                return std::min<size_t>(size_t(model), size_t(ImageMap::ColorMixModel::FilamentMixer));
            };
            std::array<std::shared_ptr<const ImageMap::ContinuousColorSolver>, 2> shared_solvers;
            constexpr size_t                                                     no_solver = std::numeric_limits<size_t>::max();
            std::vector<std::shared_ptr<const ImageMap::ContinuousColorSolver>>   cycle_solvers;
            std::vector<size_t> assignment_solver_indices(assignments.size(), no_solver);
            if (adaptive_cycle_preview) {
                std::unordered_map<uint64_t, size_t> solver_by_filament;
                solver_by_filament.reserve(assignments.size());
                for (size_t assignment_index = 0; assignment_index < assignments.size(); ++assignment_index) {
                    if (job->cancel.load(std::memory_order_relaxed)) {
                        job->status.store(SourceColorPreviewStatus::Cancelled, std::memory_order_release);
                        return;
                    }
                    const unsigned int filament_id = assignments[assignment_index].filament_id;
                    const ImageMap::ColorMixModel color_mix_model = assignments[assignment_index].zone_index < data->zones.size() ?
                                                                        data->zones[assignments[assignment_index].zone_index].color_mix_model :
                                                                        ImageMap::ColorMixModel::FullSpectrumKmKs;
                    const uint64_t solver_key = uint64_t(filament_id) | (uint64_t(color_model_index(color_mix_model)) << 32);
                    const auto existing = solver_by_filament.find(solver_key);
                    if (existing != solver_by_filament.end()) {
                        assignment_solver_indices[assignment_index] = existing->second;
                    } else if (assignments[assignment_index].components.size() >= 2) {
                        const size_t solver_index = cycle_solvers.size();
                        std::shared_ptr<const ImageMap::ContinuousColorSolver> solver =
                            source_color_preview_solver(assignments[assignment_index].components, color_mix_model);
                        if (solver && solver->valid()) {
                            cycle_solvers.emplace_back(std::move(solver));
                            solver_by_filament.emplace(solver_key, solver_index);
                            assignment_solver_indices[assignment_index] = solver_index;
                        }
                    }
                    job->progress.store(2 + int(8 * (assignment_index + 1) / std::max<size_t>(assignments.size(), 1)),
                                        std::memory_order_relaxed);
                }
                // Every palette target sharing a cycle reuses the same solver.
                // Store a guaranteed-attainable fallback as well, so a failed
                // lookup can never leak the original texture RGB into the
                // printable-result preview.
                for (size_t assignment_index = 0; assignment_index < assignments.size(); ++assignment_index) {
                    const size_t solver_index = assignment_solver_indices[assignment_index];
                    if (solver_index == no_solver || solver_index >= cycle_solvers.size())
                        continue;
                    if (const std::optional<RGBA> predicted = cycle_solvers[solver_index]->predict_modulation_color(
                            assignments[assignment_index].target_color))
                        assignments[assignment_index].display_color = *predicted;
                }
            } else if (!quantize_to_palette) {
                for (const ImageMap::Zone& zone : data->zones) {
                    if (job->cancel.load(std::memory_order_relaxed)) {
                        job->status.store(SourceColorPreviewStatus::Cancelled, std::memory_order_release);
                        return;
                    }
                    if (!zone.enabled || zone.render_mode != render_mode)
                        continue;
                    const size_t index = color_model_index(zone.color_mix_model);
                    if (!shared_solvers[index])
                        shared_solvers[index] = source_color_preview_solver(components, zone.color_mix_model);
                }
            }
            if (job->cancel.load(std::memory_order_relaxed)) {
                job->status.store(SourceColorPreviewStatus::Cancelled, std::memory_order_release);
                return;
            }
            job->progress.store(10, std::memory_order_relaxed);

            // Keep texture mappings on their original triangles and let the
            // GPU interpolate the image. Projected decals are intentionally
            // partial and use transparent UV wrapping, so they need a simple
            // background surface plus a clipped texture overlay rather than
            // the subdivided vertex-colour fallback below.
            const indexed_triangle_set&                   its = mesh->its;
            std::vector<const ImageMap::TriangleBinding*> selected(its.indices.size(), nullptr);
            for (const ImageMap::TriangleBinding& binding : data->triangle_bindings) {
                if (binding.triangle_index >= selected.size() || binding.zone_index >= data->zones.size())
                    continue;
                const ImageMap::Zone& zone = data->zones[binding.zone_index];
                if (!zone.enabled || zone.render_mode != render_mode)
                    continue;
                const ImageMap::TriangleBinding* current = selected[binding.triangle_index];
                if (current == nullptr || data->zones[current->zone_index].priority < zone.priority)
                    selected[binding.triangle_index] = &binding;
            }

            bool texture_preview_eligible       = false;
            bool texture_preview_needs_background = false;
            for (const ImageMap::TriangleBinding* binding : selected) {
                if (binding == nullptr) {
                    texture_preview_needs_background = true;
                    continue;
                }
                if (binding->source.kind != ImageMap::SourceKind::Texture || binding->source.texture_asset_index < 0 ||
                    size_t(binding->source.texture_asset_index) >= data->texture_assets.size() ||
                    !data->texture_assets[size_t(binding->source.texture_asset_index)].valid() ||
                    !std::all_of(binding->source.uvs.begin(), binding->source.uvs.end(),
                                 [](const Vec2f& uv) { return std::isfinite(uv.x()) && std::isfinite(uv.y()); })) {
                    texture_preview_eligible = false;
                    break;
                }
                texture_preview_eligible = true;
                texture_preview_needs_background |= binding->source.wrap_u == ImageMap::WrapMode::Transparent ||
                                                    binding->source.wrap_v == ImageMap::WrapMode::Transparent;
            }

            if (texture_preview_eligible) {
                using TextureGroupKey = std::tuple<int32_t, uint32_t, uint8_t, uint8_t>;
                std::map<TextureGroupKey, std::vector<const ImageMap::TriangleBinding*>> groups;
                for (const ImageMap::TriangleBinding* binding : selected) {
                    if (binding == nullptr)
                        continue;
                    groups[{binding->source.texture_asset_index, binding->zone_index, uint8_t(binding->source.wrap_u),
                            uint8_t(binding->source.wrap_v)}]
                        .push_back(binding);
                }

                std::unique_ptr<GUI::GLModel::Geometry> background_geometry;
                if (texture_preview_needs_background) {
                    background_geometry = std::make_unique<GUI::GLModel::Geometry>();
                    background_geometry->format = {GUI::GLModel::Geometry::EPrimitiveType::Triangles,
                                                   GUI::GLModel::Geometry::EVertexLayout::P3N3C4};
                    background_geometry->reserve_vertices(its.indices.size() * 3);
                    background_geometry->reserve_indices(its.indices.size() * 3);
                    background_geometry->color = ColorRGBA::WHITE();
                    unsigned int next_vertex = 0;
                    for (size_t triangle_index = 0; triangle_index < its.indices.size(); ++triangle_index) {
                        const stl_triangle_vertex_indices& indices = its.indices[triangle_index];
                        if (indices[0] < 0 || indices[1] < 0 || indices[2] < 0 || size_t(indices[0]) >= its.vertices.size() ||
                            size_t(indices[1]) >= its.vertices.size() || size_t(indices[2]) >= its.vertices.size())
                            continue;
                        const Vec3f& p0            = its.vertices[size_t(indices[0])];
                        const Vec3f& p1            = its.vertices[size_t(indices[1])];
                        const Vec3f& p2            = its.vertices[size_t(indices[2])];
                        Vec3f        normal        = (p1 - p0).cross(p2 - p0);
                        const float  normal_length = normal.norm();
                        if (normal_length <= EPSILON)
                            continue;
                        normal /= normal_length;
                        const ImageMap::TriangleBinding* binding = selected[triangle_index];
                        const std::array<RGBA, 3> colors = binding != nullptr ? binding->source.corner_colors :
                                                                                 std::array<RGBA, 3>{RGBA{1.f, 1.f, 1.f, 1.f},
                                                                                                     RGBA{1.f, 1.f, 1.f, 1.f},
                                                                                                     RGBA{1.f, 1.f, 1.f, 1.f}};
                        background_geometry->add_vertex(p0, normal, colors[0]);
                        background_geometry->add_vertex(p1, normal, colors[1]);
                        background_geometry->add_vertex(p2, normal, colors[2]);
                        background_geometry->add_triangle(next_vertex, next_vertex + 1, next_vertex + 2);
                        next_vertex += 3;
                    }
                    if (background_geometry->is_empty())
                        background_geometry.reset();
                }

                size_t total_pixels = 0;
                for (const auto& group : groups) {
                    const auto [width, height] =
                        source_texture_preview_size(data->texture_assets[size_t(std::get<0>(group.first))], interactive_preview);
                    total_pixels += size_t(width) * size_t(height);
                }
                size_t                                processed_pixels = 0;
                std::vector<SourceTexturePreviewPart> texture_parts;
                texture_parts.reserve(groups.size());

                for (const auto& [group_key, bindings] : groups) {
                    if (job->cancel.load(std::memory_order_relaxed)) {
                        job->status.store(SourceColorPreviewStatus::Cancelled, std::memory_order_release);
                        return;
                    }
                    const int32_t                 asset_index  = std::get<0>(group_key);
                    const uint32_t                zone_index   = std::get<1>(group_key);
                    const ImageMap::TextureAsset& asset        = data->texture_assets[size_t(asset_index)];
                    const ImageMap::ContinuousColorSolver* shared_solver = nullptr;
                    if (zone_index < data->zones.size()) {
                        const size_t solver_index = color_model_index(data->zones[zone_index].color_mix_model);
                        if (shared_solvers[solver_index] && shared_solvers[solver_index]->valid())
                            shared_solver = shared_solvers[solver_index].get();
                    }
                    const auto [preview_width, preview_height] = source_texture_preview_size(asset, interactive_preview);
                    if (preview_width == 0 || preview_height == 0)
                        continue;

                    SourceTexturePreviewPart part;
                    part.width            = preview_width;
                    part.height           = preview_height;
                    part.wrap_u           = ImageMap::WrapMode(std::get<2>(group_key));
                    part.wrap_v           = ImageMap::WrapMode(std::get<3>(group_key));
                    part.geometry         = std::make_unique<GUI::GLModel::Geometry>();
                    part.geometry->format = {GUI::GLModel::Geometry::EPrimitiveType::Triangles,
                                             GUI::GLModel::Geometry::EVertexLayout::P3N3T2};
                    part.geometry->reserve_vertices(bindings.size() * 3);
                    part.geometry->reserve_indices(bindings.size() * 3);
                    part.geometry->color = ColorRGBA::WHITE();

                    unsigned int next_vertex = 0;
                    for (const ImageMap::TriangleBinding* binding : bindings) {
                        if (binding->triangle_index >= its.indices.size())
                            continue;
                        const stl_triangle_vertex_indices& indices = its.indices[binding->triangle_index];
                        if (indices[0] < 0 || indices[1] < 0 || indices[2] < 0 || size_t(indices[0]) >= its.vertices.size() ||
                            size_t(indices[1]) >= its.vertices.size() || size_t(indices[2]) >= its.vertices.size())
                            continue;
                        const Vec3f& p0            = its.vertices[size_t(indices[0])];
                        const Vec3f& p1            = its.vertices[size_t(indices[1])];
                        const Vec3f& p2            = its.vertices[size_t(indices[2])];
                        Vec3f        normal        = (p1 - p0).cross(p2 - p0);
                        const float  normal_length = normal.norm();
                        if (normal_length <= EPSILON)
                            continue;
                        normal /= normal_length;
                        const std::array<Vec2f, 3> uvs = unwrap_source_texture_uvs(binding->source);
                        part.geometry->add_vertex(p0, normal, uvs[0]);
                        part.geometry->add_vertex(p1, normal, uvs[1]);
                        part.geometry->add_vertex(p2, normal, uvs[2]);
                        part.geometry->add_triangle(next_vertex, next_vertex + 1, next_vertex + 2);
                        next_vertex += 3;
                    }
                    if (part.geometry->is_empty())
                        continue;

                    struct CachedTextureColor
                    {
                        RGBA         color{1.f, 1.f, 1.f, 1.f};
                        unsigned int filament_id{0};
                    };
                    std::unordered_map<uint32_t, CachedTextureColor> color_cache;
                    color_cache.reserve(32u * 32u * 32u);
                    part.rgba.resize(size_t(preview_width) * size_t(preview_height) * 4);

                    auto source_channel = [&asset, preview_width, preview_height](unsigned int x, unsigned int y, size_t channel) {
                        if (preview_width == asset.width && preview_height == asset.height)
                            return float(asset.rgba[(size_t(y) * size_t(asset.width) + x) * 4 + channel]);
                        const float  source_x = std::clamp((float(x) + 0.5f) * float(asset.width) / float(preview_width) - 0.5f, 0.f,
                                                           float(asset.width - 1));
                        const float  source_y = std::clamp((float(y) + 0.5f) * float(asset.height) / float(preview_height) - 0.5f, 0.f,
                                                           float(asset.height - 1));
                        const size_t x0       = size_t(std::floor(source_x));
                        const size_t y0       = size_t(std::floor(source_y));
                        const size_t x1       = std::min(x0 + 1, size_t(asset.width - 1));
                        const size_t y1       = std::min(y0 + 1, size_t(asset.height - 1));
                        const float  tx       = source_x - float(x0);
                        const float  ty       = source_y - float(y0);
                        auto         value    = [&asset, channel](size_t sx, size_t sy) {
                            return float(asset.rgba[(sy * size_t(asset.width) + sx) * 4 + channel]);
                        };
                        const float top    = value(x0, y0) + (value(x1, y0) - value(x0, y0)) * tx;
                        const float bottom = value(x0, y1) + (value(x1, y1) - value(x0, y1)) * tx;
                        return top + (bottom - top) * ty;
                    };
                    const bool raw_source_texture = assignments.empty() && (shared_solver == nullptr || !shared_solver->valid());
                    const ImageMap::Zone* preview_zone = zone_index < data->zones.size() ? &data->zones[zone_index] : nullptr;
                    if (predicted_colors && preview_zone != nullptr &&
                        preview_zone->render_mode == ImageMap::RenderMode::PerimeterModulationV2 && shared_solver != nullptr &&
                        shared_solver->valid()) {
                        const size_t part_pixels = size_t(preview_width) * size_t(preview_height);
                        auto update_part_progress = [job, processed_pixels, part_pixels, total_pixels](float part_progress) {
                            const double completed = double(processed_pixels) +
                                                     double(std::clamp(part_progress, 0.f, 1.f)) * double(part_pixels);
                            const int requested = 10 + int(88. * completed / double(std::max<size_t>(total_pixels, 1)));
                            int       current   = job->progress.load(std::memory_order_relaxed);
                            while (current < requested &&
                                   !job->progress.compare_exchange_weak(current, requested, std::memory_order_relaxed))
                                ;
                        };
                        if (build_perimeter_modulation_texture_preview(asset, *preview_zone, *shared_solver, its, bindings,
                                                                       preview_width, preview_height, part.wrap_u, part.wrap_v,
                                                                       mesh_to_world, job->cancel, update_part_progress, part.rgba)) {
                            processed_pixels += part_pixels;
                            texture_parts.emplace_back(std::move(part));
                            continue;
                        }
                        if (job->cancel.load(std::memory_order_relaxed)) {
                            job->status.store(SourceColorPreviewStatus::Cancelled, std::memory_order_release);
                            return;
                        }
                    }

                    for (unsigned int y = 0; y < preview_height; ++y) {
                        if ((y & 0x1fu) == 0u) {
                            if (job->cancel.load(std::memory_order_relaxed)) {
                                job->status.store(SourceColorPreviewStatus::Cancelled, std::memory_order_release);
                                return;
                            }
                            job->progress.store(10 + int(88 * processed_pixels / std::max<size_t>(total_pixels, 1)),
                                                std::memory_order_relaxed);
                        }
                        for (unsigned int x = 0; x < preview_width; ++x) {
                            RGBA source_color;
                            if (preview_zone != nullptr) {
                                const Vec2f uv((float(x) + 0.5f) / float(preview_width),
                                               (float(y) + 0.5f) / float(preview_height));
                                source_color = ImageMap::sample_processed_texture(asset, uv, part.wrap_u, part.wrap_v, *preview_zone);
                            } else {
                                source_color = {source_channel(x, y, 0) / 255.f, source_channel(x, y, 1) / 255.f,
                                                source_channel(x, y, 2) / 255.f, source_channel(x, y, 3) / 255.f};
                            }
                            const uint8_t source_r     = uint8_t(std::lround(std::clamp(source_color[0], 0.f, 1.f) * 255.f));
                            const uint8_t source_g     = uint8_t(std::lround(std::clamp(source_color[1], 0.f, 1.f) * 255.f));
                            const uint8_t source_b     = uint8_t(std::lround(std::clamp(source_color[2], 0.f, 1.f) * 255.f));
                            const float   source_alpha = source_color[3];
                            RGBA          target{(float(source_r) / 255.f) * source_alpha + (1.f - source_alpha),
                                                 (float(source_g) / 255.f) * source_alpha + (1.f - source_alpha),
                                                 (float(source_b) / 255.f) * source_alpha + (1.f - source_alpha), 1.f};
                            if (preview_zone != nullptr && preview_zone->render_mode != ImageMap::RenderMode::NormalMix)
                                target = ImageMap::adjusted_modulation_target_color(target, *preview_zone);
                            const uint32_t cache_key = (uint32_t(std::lround(target[0] * 31.f)) << 10) |
                                                       (uint32_t(std::lround(target[1] * 31.f)) << 5) |
                                                       uint32_t(std::lround(target[2] * 31.f));
                            auto cached = color_cache.find(cache_key);
                            if (cached == color_cache.end()) {
                                CachedTextureColor                  result{target, 0};
                                const SourceColorPreviewAssignment* assignment = nearest_source_color_assignment(assignments, target,
                                                                                                                 zone_index);
                                if (quantize_to_palette) {
                                    if (assignment != nullptr) {
                                        result.color       = assignment->display_color;
                                        result.filament_id = assignment->filament_id;
                                    }
                                } else if (adaptive_cycle_preview) {
                                    if (assignment != nullptr) {
                                        result.color                  = assignment->display_color;
                                        result.filament_id            = assignment->filament_id;
                                        const size_t assignment_index = size_t(assignment - assignments.data());
                                        const size_t solver_index     = assignment_index < assignment_solver_indices.size() ?
                                                                            assignment_solver_indices[assignment_index] :
                                                                            no_solver;
                                        if (solver_index != no_solver && solver_index < cycle_solvers.size() &&
                                            cycle_solvers[solver_index] != nullptr && cycle_solvers[solver_index]->valid()) {
                                            if (const std::optional<RGBA> predicted = cycle_solvers[solver_index]->predict_modulation_color(
                                                    target))
                                                result.color = *predicted;
                                        }
                                    }
                                } else if (shared_solver != nullptr && shared_solver->valid()) {
                                    if (const std::optional<RGBA> predicted = shared_solver->predict_modulation_color(target))
                                        result.color = *predicted;
                                    result.filament_id = nearest_source_color_filament(assignments, target, zone_index);
                                }
                                cached = color_cache.emplace(cache_key, result).first;
                            }
                            const size_t output_offset = (size_t(y) * size_t(preview_width) + x) * 4;
                            if (raw_source_texture) {
                                part.rgba[output_offset + 0] = source_r;
                                part.rgba[output_offset + 1] = source_g;
                                part.rgba[output_offset + 2] = source_b;
                            } else {
                                for (size_t channel = 0; channel < 3; ++channel)
                                    part.rgba[output_offset + channel] = uint8_t(
                                        std::lround(std::clamp(cached->second.color[channel], 0.f, 1.f) * 255.f));
                            }
                            part.rgba[output_offset + 3] = assignments.empty() ?
                                                                 uint8_t(std::lround(std::clamp(source_alpha, 0.f, 1.f) * 255.f)) :
                                                                 uint8_t(std::min(cached->second.filament_id, 255u));
                            ++processed_pixels;
                        }
                    }
                    texture_parts.emplace_back(std::move(part));
                }

                if (!texture_parts.empty()) {
                    {
                        std::lock_guard<std::mutex> lock(job->result_mutex);
                        job->geometry      = std::move(background_geometry);
                        job->texture_parts = std::move(texture_parts);
                    }
                    job->progress.store(99, std::memory_order_relaxed);
                    job->status.store(SourceColorPreviewStatus::Ready, std::memory_order_release);
                    const auto elapsed_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count();
                    BOOST_LOG_TRIVIAL(info) << "Image-map UV texture preview prepared"
                                            << " volume=\"" << volume_name << "\""
                                            << " groups=" << groups.size() << " source_triangles=" << its.indices.size()
                                            << " preview_pixels=" << processed_pixels << " elapsed_ms=" << elapsed_ms;
                    return;
                }
            }

            ImageMap::SourceColorRasterizationOptions options;
            options.max_leaf_triangles    = k_source_color_preview_triangle_cap;
            options.source_data_validated = true;
            options.cancelled             = [job]() { return job->cancel.load(std::memory_order_relaxed); };
            options.progress              = [job](int progress) {
                int current = job->progress.load(std::memory_order_relaxed);
                while (current < progress && !job->progress.compare_exchange_weak(current, progress, std::memory_order_relaxed))
                    ;
            };

            ImageMap::SourceColorRasterization rasterized = ImageMap::rasterize_source_colors(*mesh, *data, render_mode,
                                                                                              RGBA{1.f, 1.f, 1.f, 1.f}, options);
            if (job->cancel.load(std::memory_order_relaxed)) {
                job->status.store(SourceColorPreviewStatus::Cancelled, std::memory_order_release);
                return;
            }

            auto geometry                  = std::make_unique<GUI::GLModel::Geometry>();
            geometry->format.type          = GUI::GLModel::Geometry::EPrimitiveType::Triangles;
            geometry->format.vertex_layout = GUI::GLModel::Geometry::EVertexLayout::P3N3C4;
            geometry->reserve_vertices(rasterized.vertices.size());
            geometry->reserve_indices(rasterized.indices.size());
            geometry->indices = std::move(rasterized.indices);
            geometry->color   = ColorRGBA::WHITE();

            const size_t vertex_count  = rasterized.vertices.size();
            int          last_progress = 85;
            struct CachedPreviewColor
            {
                RGBA         color{1.f, 1.f, 1.f, 1.f};
                unsigned int filament_id{0};
            };
            std::unordered_map<uint32_t, CachedPreviewColor> color_cache;
            color_cache.reserve(std::min(vertex_count, k_source_color_preview_cache_cap));
            const ImageMap::ContinuousColorSolver* shared_solver = nullptr;
            for (const ImageMap::Zone& zone : data->zones) {
                if (!zone.enabled || zone.render_mode != render_mode)
                    continue;
                const size_t solver_index = color_model_index(zone.color_mix_model);
                if (shared_solvers[solver_index] && shared_solvers[solver_index]->valid())
                    shared_solver = shared_solvers[solver_index].get();
                break;
            }
            for (size_t vertex_idx = 0; vertex_idx < vertex_count; ++vertex_idx) {
                if ((vertex_idx & 0x3fffu) == 0u) {
                    if (job->cancel.load(std::memory_order_relaxed)) {
                        job->status.store(SourceColorPreviewStatus::Cancelled, std::memory_order_release);
                        return;
                    }
                    const int progress = vertex_count == 0 ? 99 : 85 + int(14 * vertex_idx / vertex_count);
                    if (progress != last_progress) {
                        job->progress.store(progress, std::memory_order_relaxed);
                        last_progress = progress;
                    }
                }
                const ImageMap::SourceColorVertex& vertex = rasterized.vertices[vertex_idx];
                RGBA         result_color = adaptive_cycle_preview ? RGBA{0.55f, 0.55f, 0.55f, vertex.color[3]} : vertex.color;
                unsigned int filament_id  = 0;
                if (quantize_to_palette) {
                    const uint32_t key    = preview_color_key(vertex.color);
                    const auto     cached = color_cache.find(key);
                    if (cached != color_cache.end()) {
                        result_color = cached->second.color;
                        filament_id  = cached->second.filament_id;
                    } else {
                        const RGBA quantized_target{float((key >> 16) & 0xffu) / 255.f, float((key >> 8) & 0xffu) / 255.f,
                                                    float(key & 0xffu) / 255.f, vertex.color[3]};
                        if (const SourceColorPreviewAssignment* assignment = nearest_source_color_assignment(assignments, quantized_target);
                            assignment != nullptr) {
                            result_color = assignment->display_color;
                            filament_id  = assignment->filament_id;
                        }
                        if (color_cache.size() < k_source_color_preview_cache_cap) {
                            RGBA cached_color = result_color;
                            cached_color[3]   = 1.f;
                            color_cache.emplace(key, CachedPreviewColor{cached_color, filament_id});
                        }
                    }
                } else if (adaptive_cycle_preview) {
                    const uint32_t key    = preview_color_key(vertex.color);
                    const auto     cached = color_cache.find(key);
                    if (cached != color_cache.end()) {
                        result_color = cached->second.color;
                        filament_id  = cached->second.filament_id;
                    } else {
                        const RGBA quantized_target{float((key >> 16) & 0xffu) / 255.f, float((key >> 8) & 0xffu) / 255.f,
                                                    float(key & 0xffu) / 255.f, vertex.color[3]};
                        const SourceColorPreviewAssignment* assignment = nearest_source_color_assignment(assignments, quantized_target);
                        if (assignment != nullptr) {
                            filament_id                   = assignment->filament_id;
                            result_color                  = assignment->display_color;
                            const size_t assignment_index = size_t(assignment - assignments.data());
                            const size_t solver_index     = assignment_index < assignment_solver_indices.size() ?
                                                                assignment_solver_indices[assignment_index] :
                                                                no_solver;
                            if (solver_index != no_solver && solver_index < cycle_solvers.size() &&
                                cycle_solvers[solver_index] != nullptr && cycle_solvers[solver_index]->valid()) {
                                if (const std::optional<RGBA> predicted = cycle_solvers[solver_index]->predict_modulation_color(
                                        quantized_target))
                                    result_color = *predicted;
                            }
                        }
                        if (color_cache.size() < k_source_color_preview_cache_cap) {
                            RGBA cached_color = result_color;
                            cached_color[3]   = 1.f;
                            color_cache.emplace(key, CachedPreviewColor{cached_color, filament_id});
                        }
                    }
                } else if (shared_solver != nullptr && shared_solver->valid()) {
                    const uint32_t key    = preview_color_key(vertex.color);
                    const auto     cached = color_cache.find(key);
                    if (cached != color_cache.end()) {
                        result_color = cached->second.color;
                        filament_id  = cached->second.filament_id;
                    } else {
                        RGBA quantized_target{float((key >> 16) & 0xffu) / 255.f, float((key >> 8) & 0xffu) / 255.f,
                                              float(key & 0xffu) / 255.f, vertex.color[3]};
                        if (const std::optional<RGBA> predicted = shared_solver->predict_modulation_color(quantized_target))
                            result_color = *predicted;
                        filament_id = nearest_source_color_filament(assignments, quantized_target);
                        if (color_cache.size() < k_source_color_preview_cache_cap) {
                            RGBA cached_color = result_color;
                            cached_color[3]   = 1.f;
                            color_cache.emplace(key, CachedPreviewColor{cached_color, filament_id});
                        }
                    }
                } else {
                    filament_id = nearest_source_color_filament(assignments, vertex.color);
                }
                // The source-preview shader treats alpha as a compact, transient
                // palette assignment. It restores the volume alpha before
                // lighting, so normal image-map rendering remains opaque.
                result_color[3] = assignments.empty() ? vertex.color[3] : float(std::min(filament_id, 255u)) / 255.f;
                geometry->add_vertex(vertex.position, vertex.normal, result_color);
            }

            {
                std::lock_guard<std::mutex> lock(job->result_mutex);
                job->geometry = std::move(geometry);
            }
            // Keep the 99% overlay visible for one frame before the UI thread
            // initializes and uploads the GL model.
            job->progress.store(99, std::memory_order_relaxed);
            job->status.store(SourceColorPreviewStatus::Ready, std::memory_order_release);

            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at)
                                        .count();
            BOOST_LOG_TRIVIAL(info) << "Image-map result preview prepared"
                                    << " volume=\"" << volume_name << "\""
                                    << " mode=\"" << (quantize_to_palette ? "quantized" : "optical") << "\""
                                    << " source_triangles=" << rasterized.source_triangle_count
                                    << " preview_triangles=" << rasterized.sampled_leaf_count << " lod_passes=" << rasterized.lod_pass_count
                                    << " elapsed_ms=" << elapsed_ms;
        } catch (const std::exception& error) {
            BOOST_LOG_TRIVIAL(error) << "KM/K-S image-map result preview failed"
                                     << " volume=\"" << volume_name << "\""
                                     << " error=\"" << error.what() << "\"";
            job->status.store(SourceColorPreviewStatus::Failed, std::memory_order_release);
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "KM/K-S image-map result preview failed"
                                     << " volume=\"" << volume_name << "\""
                                     << " error=\"unknown exception\"";
            job->status.store(SourceColorPreviewStatus::Failed, std::memory_order_release);
        }
    }).detach();
    return job;
}

} // namespace

// LOD mesh sharing map: maps TriangleMesh* -> LOD entry for that mesh.
// When multiple volumes reference the same TriangleMesh, LOD simplified models are shared.
// The entry holds an owning shared_ptr to the mesh, so the raw pointer used as
// lookup key cannot dangle or be reused by another mesh while the entry
// exists. Entries are maintained by load_object_volume()/release_volume():
// a volume registers itself on creation and is removed on deletion; the entry
// dies (releasing the mesh) with its last volume.
struct MeshLodEntry {
    std::shared_ptr<const TriangleMesh> mesh; // keeps the key mesh alive
    std::set<GLVolume*>                  volumes;
};
static std::map<const TriangleMesh*, MeshLodEntry> g_meshVolumesMap;

// LOD run-time constants
const unsigned char LOD_UPDATE_FREQUENCY = 20;
const float         ZOOM_THRESHOLD       = 0.3f;
// pixel thresholds for LOD screen-size evaluation
const Vec2i32       LOD_SCREEN_MIN        = Vec2i32(150, 110);
const Vec2i32       LOD_SCREEN_MAX       = Vec2i32(300, 200);
const int           SUPER_LARGE_FACES    = 500000;
const int           LARGE_FACES          = 100000;

//QEM face threshold
const int           INIT_FACE_LOW_COUNT  = 200;
const int           FINAL_FACE_LOW_COUNT = 1000;
const float         QEM_FACE_RATIO       = 0.5f;
const float         AABB_RANGE_EPSILON   = 1.0f;

//QEM Middle Small max error threshold
const float MIDDLE_LOD_NORMAL_FACE_MAX_ERROR      = 0.1f;
const float MIDDLE_LOD_SUPER_LARGE_FACE_MAX_ERROR = 0.08f;
const float MIDDLE_LOD_LARGE_FACE_MAX_ERROR       = 0.05f;

const float SMALL_LOD_NORMAL_FACE_MAX_ERROR      = 0.5f;
const float SMALL_LOD_SUPER_LARGE_FACE_MAX_ERROR = 0.4f;
const float SMALL_LOD_LARGE_FACE_MAX_ERROR       = 0.3f;

// Cached camera state for LOD evaluation
float                 GLVolume::s_lastCameraZoomValue = 0.0f;
float                 GLVolume::s_curZoom             = 1.0f;
Matrix4d              GLVolume::s_curViewProjMatrix   = Matrix4d::Identity();
std::array<int, 4>    GLVolume::s_curViewport         = {0, 0, 0, 0};

// Project a 3D point to 2D screen coordinates using the view-projection matrix
static Vec2f CalcPtInScreen(const Vec3d& pt, const Matrix4d& viewProjMat, int windowWidth, int windowHeight)
{
    Vec4d point(pt.x(), pt.y(), pt.z(), 1.0);
    Vec4d pointNDCSpace = viewProjMat * point;
    Vec3d pointScreenSpace = Vec3d(pointNDCSpace.x(), pointNDCSpace.y(), pointNDCSpace.z()) / pointNDCSpace.w();
    float x = 0.5f * (1 + pointScreenSpace(0)) * windowWidth;
    float y = 0.5f * (1 - pointScreenSpace(1)) * windowHeight;
    return Vec2f(x, y);
}

// Determine which LOD level to use based on the volume's bounding box screen-space size
static LODLevel CalcVolumeBoxInScreenBiggerThanThreshold(const BoundingBoxf3& worldAABB, const Matrix4d& viewProjMat, int windowWidth, int windowHeight)
{
    const Vec3d& min3d = worldAABB.min;
    const Vec3d& max3d = worldAABB.max;
    std::array<Vec3d, 8> srcVertices;
    srcVertices[0] = min3d;
    srcVertices[1] = Vec3d(max3d.x(), min3d.y(), min3d.z());
    srcVertices[2] = Vec3d(max3d.x(), max3d.y(), min3d.z());
    srcVertices[3] = Vec3d(min3d.x(), max3d.y(), min3d.z());
    srcVertices[4] = Vec3d(min3d.x(), min3d.y(), max3d.z());
    srcVertices[5] = Vec3d(max3d.x(), min3d.y(), max3d.z());
    srcVertices[6] = max3d;
    srcVertices[7] = Vec3d(min3d.x(), max3d.y(), max3d.z());

    BoundingBoxf box2d;
    for (int i = 0; i < srcVertices.size(); i++)
    {
        Vec2f screenPt = CalcPtInScreen(srcVertices[i], viewProjMat, windowWidth, windowHeight);
        box2d.merge(screenPt.cast<double>());
    }
    double sizeX = box2d.size().x();
    double sizeY = box2d.size().y();
    if (sizeX >= LOD_SCREEN_MAX.x() || sizeY >= LOD_SCREEN_MAX.y())
    {
        return LODLevel::High;
    }
    if (sizeX <= LOD_SCREEN_MIN.x() && sizeY <= LOD_SCREEN_MIN.y())
    {
        return LODLevel::Small;
    }
    else
    {
        return LODLevel::Middle;
    }
}

const float GLVolume::SinkingContours::HalfWidth = 0.25f;

void GLVolume::SinkingContours::render()
{
    update();

    GLShaderProgram* shader = GUI::wxGetApp().get_current_shader();
    if (shader == nullptr)
        return;

    const GUI::Camera& camera = GUI::wxGetApp().plater()->get_camera();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * Geometry::assemble_transform(m_shift));
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    m_model.render();
}

void GLVolume::SinkingContours::update()
{
    const int    object_idx = m_parent.object_idx();
    const Model& model      = GUI::wxGetApp().plater()->model();

    if (0 <= object_idx && object_idx < int(model.objects.size()) && m_parent.is_sinking() && !m_parent.is_below_printbed()) {
        const BoundingBoxf3& box = m_parent.transformed_convex_hull_bounding_box();
        if (!m_old_box.size().isApprox(box.size()) || m_old_box.min.z() != box.min.z()) {
            m_old_box = box;
            m_shift   = Vec3d::Zero();

            const TriangleMesh& mesh = model.objects[object_idx]->volumes[m_parent.volume_idx()]->mesh();

            m_model.reset();
            GUI::GLModel::Geometry init_data;
            init_data.format = {GUI::GLModel::Geometry::EPrimitiveType::Triangles, GUI::GLModel::Geometry::EVertexLayout::P3};
            init_data.color  = ColorRGBA::WHITE();
            unsigned int      vertices_counter = 0;
            MeshSlicingParams slicing_params;
            slicing_params.trafo    = m_parent.world_matrix();
            const Polygons polygons = union_(slice_mesh(mesh.its, 0.0f, slicing_params));
            for (const ExPolygon& expoly : diff_ex(expand(polygons, float(scale_(HalfWidth))), shrink(polygons, float(scale_(HalfWidth))))) {
                const std::vector<Vec3d> triangulation = triangulate_expolygon_3d(expoly);
                init_data.reserve_vertices(init_data.vertices_count() + triangulation.size());
                init_data.reserve_indices(init_data.indices_count() + triangulation.size());
                for (const Vec3d& v : triangulation) {
                    init_data.add_vertex((Vec3f) (v.cast<float>() + 0.015f * Vec3f::UnitZ())); // add a small positive z to avoid z-fighting
                    ++vertices_counter;
                    if (vertices_counter % 3 == 0)
                        init_data.add_triangle(vertices_counter - 3, vertices_counter - 2, vertices_counter - 1);
                }
            }
            m_model.init_from(std::move(init_data));
        } else
            m_shift = box.center() - m_old_box.center();
    } else
        m_model.reset();
}

ColorRGBA GLVolume::DISABLED_COLOR    = ColorRGBA::DARK_GRAY();
ColorRGBA GLVolume::SLA_SUPPORT_COLOR = ColorRGBA::LIGHT_GRAY();
ColorRGBA GLVolume::SLA_PAD_COLOR     = {0.0f, 0.2f, 0.0f, 1.0f};
// BBS
ColorRGBA GLVolume::NEUTRAL_COLOR     = {0.8f, 0.8f, 0.8f, 1.0f};
ColorRGBA GLVolume::UNPRINTABLE_COLOR = {0.0f, 0.0f, 0.0f, 0.5f};

ColorRGBA GLVolume::MODEL_MIDIFIER_COL   = {1.0f, 1.0f, 0.0f, 0.6f};
ColorRGBA GLVolume::MODEL_NEGTIVE_COL    = {0.3f, 0.3f, 0.3f, 0.4f};
ColorRGBA GLVolume::SUPPORT_ENFORCER_COL = {0.3f, 0.3f, 1.0f, 0.4f};
ColorRGBA GLVolume::SUPPORT_BLOCKER_COL  = {1.0f, 0.3f, 0.3f, 0.4f};

ColorRGBA GLVolume::MODEL_HIDDEN_COL = {0.f, 0.f, 0.f, 0.3f};

std::array<ColorRGBA, 5> GLVolume::MODEL_COLOR = {
    {{1.0f, 1.0f, 0.0f, 1.f}, {1.0f, 0.5f, 0.5f, 1.f}, {0.5f, 1.0f, 0.5f, 1.f}, {0.5f, 0.5f, 1.0f, 1.f}, {1.0f, 1.0f, 0.0f, 1.f}}};

void GLVolume::update_render_colors()
{
    GLVolume::DISABLED_COLOR    = GUI::ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Model_Disable]);
    GLVolume::NEUTRAL_COLOR     = GUI::ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Model_Neutral]);
    GLVolume::MODEL_COLOR[0]    = GUI::ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Modifier]);
    GLVolume::MODEL_COLOR[1]    = GUI::ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Negtive_Volume]);
    GLVolume::MODEL_COLOR[2]    = GUI::ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Support_Enforcer]);
    GLVolume::MODEL_COLOR[3]    = GUI::ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Support_Blocker]);
    GLVolume::UNPRINTABLE_COLOR = GUI::ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Model_Unprintable]);
}

void GLVolume::load_render_colors()
{
    RenderColor::colors[RenderCol_Model_Disable]     = GUI::ImGuiWrapper::to_ImVec4(GLVolume::DISABLED_COLOR);
    RenderColor::colors[RenderCol_Model_Neutral]     = GUI::ImGuiWrapper::to_ImVec4(GLVolume::NEUTRAL_COLOR);
    RenderColor::colors[RenderCol_Modifier]          = GUI::ImGuiWrapper::to_ImVec4(GLVolume::MODEL_COLOR[0]);
    RenderColor::colors[RenderCol_Negtive_Volume]    = GUI::ImGuiWrapper::to_ImVec4(GLVolume::MODEL_COLOR[1]);
    RenderColor::colors[RenderCol_Support_Enforcer]  = GUI::ImGuiWrapper::to_ImVec4(GLVolume::MODEL_COLOR[2]);
    RenderColor::colors[RenderCol_Support_Blocker]   = GUI::ImGuiWrapper::to_ImVec4(GLVolume::MODEL_COLOR[3]);
    RenderColor::colors[RenderCol_Model_Unprintable] = GUI::ImGuiWrapper::to_ImVec4(GLVolume::UNPRINTABLE_COLOR);
}

GLVolume::GLVolume(float r, float g, float b, float a)
    : m_sla_shift_z(0.0)
    , m_sinking_contours(*this)
    // geometry_id == 0 -> invalid
    , geometry_id(std::pair<size_t, size_t>(0, 0))
    , extruder_id(0)
    , selected(false)
    , disabled(false)
    , printable(true)
    , visible(true)
    , is_active(true)
    , zoom_to_volumes(true)
    , shader_outside_printer_detection_enabled(false)
    , is_outside(false)
    , partly_inside(false)
    , hover(HS_None)
    , is_modifier(false)
    , is_wipe_tower(false)
    , is_extrusion_path(false)
    , force_transparent(false)
    , force_native_color(false)
    , force_neutral_color(false)
    , force_sinking_contours(false)
    , picking(false)
    , tverts_range(0, size_t(-1))
    , m_tvertsRangeLod(0, size_t(-1))
{
    color = {r, g, b, a};
    set_render_color(color);
    mmuseg_ts = 0;
}

GLVolume::~GLVolume()
{
    if (image_map_source_preview_job)
        image_map_source_preview_job->cancel.store(true, std::memory_order_relaxed);
}

std::optional<float> GLVolume::source_color_preview_progress() const
{
    if (!image_map_source_preview_job)
        return std::nullopt;
    const SourceColorPreviewStatus status = image_map_source_preview_job->status.load(std::memory_order_acquire);
    if (status != SourceColorPreviewStatus::Running && status != SourceColorPreviewStatus::Ready &&
        status != SourceColorPreviewStatus::Uploading && status != SourceColorPreviewStatus::Uploaded)
        return std::nullopt;
    return float(std::clamp(image_map_source_preview_job->progress.load(std::memory_order_relaxed), 0, 100)) / 100.f;
}

// BBS
float GLVolume::explosion_ratio      = 1.0;
float GLVolume::last_explosion_ratio = 1.0;

void GLVolume::set_render_color()
{
    bool outside = is_outside || is_below_printbed();

    if (force_native_color || force_neutral_color) {
#ifdef ENABBLE_OUTSIDE_COLOR
        if (outside && shader_outside_printer_detection_enabled)
            set_render_color(OUTSIDE_COLOR);
        else {
#endif
            if (force_native_color)
                set_render_color(color);
            else
                set_render_color(NEUTRAL_COLOR);
#ifdef ENABLE_OUTSIDE_COLOR
        }
#endif
    } else {
        /* BBS
        if (hover == HS_Select)
            set_render_color(HOVER_SELECT_COLOR);
        else if (hover == HS_Deselect)
            set_render_color(HOVER_DESELECT_COLOR);
        else if (selected)
            set_render_color(outside ? SELECTED_OUTSIDE_COLOR : SELECTED_COLOR);
        else if (disabled)
        */
        if (disabled)
            set_render_color(DISABLED_COLOR);
#ifdef ENABLE_OUTSIDE_COLOR
        else if (is_outside && shader_outside_printer_detection_enabled)
            set_render_color(OUTSIDE_COLOR);
#endif
        else {
            // to make black not too hard too see
            ColorRGBA new_color = adjust_color_for_rendering(color);
            set_render_color(new_color);
        }
    }

    if (force_transparent) {
        if (color.a() < FullyTransparentMaterialThreshold) {
            render_color.a(FullTransparentModdifiedToFixAlpha);
        } else {
            render_color.a(color.a());
        }
    }

    // BBS set unprintable color
    if (!printable) {
        render_color = UNPRINTABLE_COLOR;
    }

    // BBS set invisible color
    if (!visible) {
        render_color = MODEL_HIDDEN_COL;
    }
}

ColorRGBA color_from_model_volume(const ModelVolume& model_volume)
{
    ColorRGBA color;
    if (model_volume.is_negative_volume())
        return GLVolume::MODEL_NEGTIVE_COL;
    else if (model_volume.is_modifier())
#if ENABLE_MODIFIERS_ALWAYS_TRANSPARENT
        return GLVolume::MODEL_MIDIFIER_COL;
#else
        color = {0.2f, 1.0f, 0.2f, 1.0f};
#endif // ENABLE_MODIFIERS_ALWAYS_TRANSPARENT
    else if (model_volume.is_support_blocker())
        return GLVolume::SUPPORT_BLOCKER_COL;
    else if (model_volume.is_support_enforcer())
        return GLVolume::SUPPORT_ENFORCER_COL;
    return color;
}

static std::vector<ColorRGBA> mixed_filament_preview_gradient_colors(
    const MixedFilamentDefinition&     definition,
    const MixedFilamentDisplayContext& display_context)
{
    const std::vector<wxColour> preview_colors =
        GUI::build_mixed_filament_gradient_preview(definition, display_context).sampled_colors;
    std::vector<ColorRGBA> colors;
    colors.reserve(preview_colors.size());
    for (const wxColour& color : preview_colors) {
        colors.emplace_back(float(color.Red()) / 255.f,
                            float(color.Green()) / 255.f,
                            float(color.Blue()) / 255.f,
                            1.f);
    }
    return colors;
}

static ColorRGBA interpolate_preview_gradient_color(const ColorRGBA& a, const ColorRGBA& b, double local)
{
    ColorRGBA out;
    local = std::clamp(local, 0.0, 1.0);
    for (size_t channel = 0; channel < 4; ++channel)
        out[channel] = float(double(a[channel]) * (1.0 - local) + double(b[channel]) * local);
    return out;
}

static ColorRGBA sample_preview_gradient_color(const std::vector<ColorRGBA>& colors,
                                               const std::vector<double>&    positions,
                                               double                        t)
{
    if (colors.empty())
        return ColorRGBA::WHITE();
    if (colors.size() == 1)
        return colors.front();

    const size_t expected_stops = 2 * colors.size() - 1;
    if (positions.size() == expected_stops) {
        const double progress = std::clamp(t, 0.0, std::nextafter(1.0, 0.0));
        for (size_t idx = 0; idx + 1 < colors.size(); ++idx) {
            const double p_start = std::clamp(positions[2 * idx], 0.0, 1.0);
            const double p_mid   = std::clamp(positions[2 * idx + 1], p_start, 1.0);
            const double p_end   = std::clamp(positions[2 * idx + 2], p_mid, 1.0);
            if (progress > p_end && idx + 2 < colors.size())
                continue;

            double local = 0.0;
            if (p_end <= p_start + EPSILON)
                local = 0.0;
            else if (progress <= p_mid)
                local = 0.5 * std::clamp((progress - p_start) / std::max(EPSILON, p_mid - p_start), 0.0, 1.0);
            else
                local = 0.5 + 0.5 * std::clamp((progress - p_mid) / std::max(EPSILON, p_end - p_mid), 0.0, 1.0);
            return interpolate_preview_gradient_color(colors[idx], colors[idx + 1], local);
        }
        return colors.back();
    }

    const double scaled = std::clamp(t, 0.0, std::nextafter(1.0, 0.0)) * double(colors.size() - 1);
    const size_t idx    = std::min<size_t>(colors.size() - 2, size_t(std::floor(scaled)));
    const double local  = scaled - double(idx);
    return interpolate_preview_gradient_color(colors[idx], colors[idx + 1], local);
}

static bool render_preview_gradient_model(GUI::GLModel&                         model,
                                          const std::pair<size_t, size_t>&       tverts_range,
                                          GLShaderProgram*                      shader,
                                          const BoundingBoxf3&                  box,
                                          const std::array<float, 2>&           z_range,
                                          const std::vector<ColorRGBA>&         colors,
                                          const std::vector<double>&            positions,
                                          float                                 alpha)
{
    if (shader == nullptr || !model.is_initialized() || colors.size() < 2)
        return false;

    const double z_min = box.min.z();
    const double z_max = box.max.z();
    const double z_height = z_max - z_min;
    const double clip_min = std::max<double>(z_min, z_range[0]);
    const double clip_max = std::min<double>(z_max, z_range[1]);
    if (z_height <= EPSILON || clip_max <= clip_min + EPSILON)
        return false;

    bool rendered = false;
    const size_t band_count = std::clamp<size_t>(colors.size() * 16, 24, 64);
    for (size_t band_idx = 0; band_idx < band_count; ++band_idx) {
        const double band_min = z_min + z_height * double(band_idx) / double(band_count);
        const double band_max = z_min + z_height * double(band_idx + 1) / double(band_count);
        const double draw_min = std::max<double>(band_min, clip_min);
        const double draw_max = std::min<double>(band_max, clip_max);
        if (draw_max <= draw_min + EPSILON)
            continue;

        ColorRGBA band_color = adjust_color_for_rendering(sample_preview_gradient_color(colors, positions,
                                                                                       (double(band_idx) + 0.5) / double(band_count)));
        band_color.a(alpha);
        model.set_color(band_color);
        shader->set_uniform("z_range", std::array<float, 2>{float(draw_min), float(draw_max)});
        if (tverts_range == std::make_pair<size_t, size_t>(0, -1))
            model.render();
        else
            model.render(tverts_range);
        rendered = true;
    }
    return rendered;
}

static bool volume_has_surface_segmentation(const GLVolume& volume)
{
    ModelObjectPtrs& model_objects = GUI::wxGetApp().model().objects;
    if (volume.object_idx() < 0 || volume.object_idx() >= model_objects.size())
        return false;

    const ModelObject* model_object = model_objects[volume.object_idx()];
    if (model_object == nullptr || volume.volume_idx() < 0 || volume.volume_idx() >= model_object->volumes.size())
        return false;

    const ModelVolume* model_volume = model_object->volumes[volume.volume_idx()];
    return model_volume != nullptr && model_volume->is_mm_painted();
}

struct ImageMapPreviewPalette
{
    const MixedFilamentManager* manager{nullptr};
    size_t                      num_physical{0};
    size_t                      num_total{0};

    unsigned int resolve(const ImageMap::PaletteEntry& entry) const
    {
        if (manager != nullptr && entry.mixed_filament_stable_id != 0) {
            const std::optional<unsigned int> stable = manager->filament_id_from_stable_id(entry.mixed_filament_stable_id, num_physical);
            if (stable && *stable >= 1 && *stable <= num_total)
                return *stable;
        }
        return entry.fallback_filament_id >= 1 && entry.fallback_filament_id <= num_total ? entry.fallback_filament_id : 0u;
    }

    size_t signature(const ImageMap::VolumeData& data, unsigned int base_filament_id) const
    {
        size_t seed = base_filament_id;
        auto hash_combine = [&seed](size_t value) {
            seed ^= value + size_t(0x9e3779b9) + (seed << 6) + (seed >> 2);
        };
        hash_combine(num_physical);
        hash_combine(num_total);
        for (const ImageMap::Zone& zone : data.zones) {
            hash_combine(zone.enabled ? 1u : 0u);
            for (const ImageMap::PaletteEntry& entry : zone.palette)
                hash_combine(resolve(entry));
        }
        return seed;
    }
};

static ImageMapPreviewPalette image_map_preview_palette()
{
    ImageMapPreviewPalette palette;
    if (GUI::wxGetApp().preset_bundle == nullptr)
        return palette;
    palette.manager      = &GUI::wxGetApp().preset_bundle->mixed_filaments;
    palette.num_physical = size_t(std::max(GUI::wxGetApp().filaments_cnt(), 0));
    palette.num_total    = palette.manager->total_filaments(palette.num_physical);
    return palette;
}

bool GLVolume::SimplifyMesh(const TriangleMesh& mesh, std::shared_ptr<GUI::GLModel> model, std::shared_ptr<std::atomic<bool>> readyFlag, LODLevel lod) const
{
    return SimplifyMesh(mesh.its, model, readyFlag, lod);
}

bool GLVolume::SimplifyMesh(const indexed_triangle_set& its, std::shared_ptr<GUI::GLModel> model, std::shared_ptr<std::atomic<bool>> readyFlag, LODLevel lod) const
{
    if (its.indices.size() == 0 || its.vertices.size() == 0)
    {
        return false;
    }

    auto itsCopy = std::make_unique<indexed_triangle_set>(its);

    float maxError = std::numeric_limits<float>::max();
    if (lod == LODLevel::Middle)
    {
        maxError = MIDDLE_LOD_NORMAL_FACE_MAX_ERROR;
        if (its.indices.size() > SUPER_LARGE_FACES)
        {
            maxError = MIDDLE_LOD_SUPER_LARGE_FACE_MAX_ERROR;
        }
        else if(its.indices.size() > LARGE_FACES)
        {
            maxError = MIDDLE_LOD_LARGE_FACE_MAX_ERROR;
        }
    }
    if (lod == LODLevel::Small)
    {
        maxError = SMALL_LOD_NORMAL_FACE_MAX_ERROR;
        if (its.indices.size() > SUPER_LARGE_FACES)
        {
            maxError = SMALL_LOD_SUPER_LARGE_FACE_MAX_ERROR;
        }
        else if(its.indices.size() > LARGE_FACES)
        {
            maxError = SMALL_LOD_LARGE_FACE_MAX_ERROR;
        }
    }

    TriangleMesh originMesh(*itsCopy);

    // Run simplification in background thread (async, detached)
    // Ref: https://people.eecs.berkeley.edu/~jrs/meshpapers/GarlandHeckbert2.pdf
    std::thread worker = std::thread(
        [model, readyFlag, maxError, originMesh](std::unique_ptr<indexed_triangle_set> itsPtr) {
            int      initFaceCount  = itsPtr->indices.size();
            uint32_t triangleCount  = 0;
            float    maxErrCopy     = maxError;

            its_quadric_edge_collapse(*itsPtr, triangleCount, &maxErrCopy);

            // Validate simplification quality
            int endFaceCount = (*itsPtr).indices.size();
            if (initFaceCount < INIT_FACE_LOW_COUNT || (initFaceCount < FINAL_FACE_LOW_COUNT && endFaceCount < initFaceCount * QEM_FACE_RATIO))
            {
                BOOST_LOG_TRIVIAL(info) << "LOD simplify: rejected (too few faces) init=" << initFaceCount << " end=" << endFaceCount;
                return;
            }

            TriangleMesh simplifiedMesh(*itsPtr);
            Vec3f        originMin  = originMesh.stats().min - Vec3f(AABB_RANGE_EPSILON, AABB_RANGE_EPSILON, AABB_RANGE_EPSILON);
            Vec3f        originMax  = originMesh.stats().max + Vec3f(AABB_RANGE_EPSILON, AABB_RANGE_EPSILON, AABB_RANGE_EPSILON);

            // Ensure simplified mesh stays within original bounding box
            if (originMin.x() < simplifiedMesh.stats().min.x() &&
                originMin.y() < simplifiedMesh.stats().min.y() &&
                originMin.z() < simplifiedMesh.stats().min.z() &&
                originMax.x() > simplifiedMesh.stats().max.x() &&
                originMax.y() > simplifiedMesh.stats().max.y() &&
                originMax.z() > simplifiedMesh.stats().max.z()) {
                if (model && model.use_count() >= 2) {
                    // The model is render-disabled until the main thread sees
                    // readyFlag (GLModel.hpp threading contract), so this
                    // write is exclusive to this thread.
                    model->init_from(simplifiedMesh);
                    BOOST_LOG_TRIVIAL(info) << "LOD simplify: completed successfully, faces=" << initFaceCount
                                            << " -> " << endFaceCount
                                            << " (use_count=" << model.use_count() << ")";
                } else {
                    BOOST_LOG_TRIVIAL(info) << "LOD simplify: skipped init (use_count="
                                                << (model ? model.use_count() : 0) << ")";
                }
            } else {
                BOOST_LOG_TRIVIAL(info) << "LOD simplify: rejected (out of AABB bounds)";
            }

            // Last touch of the model: hand it over to the main thread. The
            // release store pairs with the acquire load in
            // promote_ready_lod_models(), making the init_from() writes above
            // visible before enable_render() is called.
            if (readyFlag)
                readyFlag->store(true, std::memory_order_release);
        },
        std::move(itsCopy));

    if (worker.joinable())
    {
        worker.detach();
    }
    return true;
}

void GLVolume::set_bounding_boxes_as_dirty()
{
    // Force immediate LOD re-evaluation
    m_lodUpdateIndex      = LOD_UPDATE_FREQUENCY;
    m_transformed_bounding_box.reset();
    m_transformed_convex_hull_bounding_box.reset();
    m_transformed_non_sinking_bounding_box.reset();
}

void GLVolume::promote_ready_lod_models()
{
    // The LOD models stay render-disabled while their background thread may
    // still be writing them. Once the worker signals completion (release
    // store in SimplifyMesh), the acquire load below makes its writes
    // visible, and enable_render() hands the model over to the main thread.
    if (m_modelMiddle && m_lodMiddleReady && m_modelMiddle->is_render_disabled() && m_lodMiddleReady->load(std::memory_order_acquire))
        m_modelMiddle->enable_render();
    if (m_modelSmall && m_lodSmallReady && m_modelSmall->is_render_disabled() && m_lodSmallReady->load(std::memory_order_acquire))
        m_modelSmall->enable_render();
}

Transform3d GLVolume::world_matrix() const
{
    Transform3d m          = m_instance_transformation.get_matrix() * m_volume_transformation.get_matrix();
    Vec3d       ofs2ass    = m_offset_to_assembly * (GLVolume::explosion_ratio - 1.0);
    Vec3d       volofs2obj = m_volume_transformation.get_offset() * (GLVolume::explosion_ratio - 1.0);

    m.translation()(2) += m_sla_shift_z;
    m.translate(ofs2ass + volofs2obj);
    return m;
}

bool GLVolume::is_left_handed() const
{
    const Vec3d& m1 = m_instance_transformation.get_mirror();
    const Vec3d& m2 = m_volume_transformation.get_mirror();
    return m1.x() * m1.y() * m1.z() * m2.x() * m2.y() * m2.z() < 0.;
}

const BoundingBoxf3& GLVolume::transformed_bounding_box() const
{
    if (!m_transformed_bounding_box.has_value() || last_explosion_ratio != explosion_ratio) {
        const BoundingBoxf3& box = bounding_box();
        assert(box.defined || box.min.x() >= box.max.x() || box.min.y() >= box.max.y() || box.min.z() >= box.max.z());
        std::optional<BoundingBoxf3>* trans_box = const_cast<std::optional<BoundingBoxf3>*>(&m_transformed_bounding_box);
        *trans_box                              = box.transformed(world_matrix());
        last_explosion_ratio                    = explosion_ratio;
    }
    return *m_transformed_bounding_box;
}

const BoundingBoxf3& GLVolume::transformed_convex_hull_bounding_box() const
{
    if (!m_transformed_convex_hull_bounding_box.has_value()) {
        std::optional<BoundingBoxf3>* trans_box = const_cast<std::optional<BoundingBoxf3>*>(&m_transformed_convex_hull_bounding_box);
        *trans_box                              = transformed_convex_hull_bounding_box(world_matrix());
    }
    return *m_transformed_convex_hull_bounding_box;
}

BoundingBoxf3 GLVolume::transformed_convex_hull_bounding_box(const Transform3d& trafo) const
{
    return (m_convex_hull && !m_convex_hull->empty()) ? m_convex_hull->transformed_bounding_box(trafo) : bounding_box().transformed(trafo);
}

BoundingBoxf3 GLVolume::transformed_non_sinking_bounding_box(const Transform3d& trafo) const
{
    auto* plater = GUI::wxGetApp().plater();
    if (!plater)
        return bounding_box().transformed(trafo);

    const auto& objects = plater->model().objects;
    int         obj_idx = object_idx();
    if (obj_idx < 0 || obj_idx >= (int) objects.size() || !objects[obj_idx])
        return bounding_box().transformed(trafo);

    const auto& volumes = objects[obj_idx]->volumes;
    int         vol_idx = volume_idx();
    if (vol_idx < 0 || vol_idx >= (int) volumes.size())
        return bounding_box().transformed(trafo);

    return volumes[vol_idx]->mesh().transformed_bounding_box(trafo, 0.0);
}

const BoundingBoxf3& GLVolume::transformed_non_sinking_bounding_box() const
{
    if (!m_transformed_non_sinking_bounding_box.has_value()) {
        std::optional<BoundingBoxf3>* trans_box = const_cast<std::optional<BoundingBoxf3>*>(&m_transformed_non_sinking_bounding_box);
        const Transform3d&            trafo     = world_matrix();
        *trans_box                              = transformed_non_sinking_bounding_box(trafo);
    }
    return *m_transformed_non_sinking_bounding_box;
}

void GLVolume::set_range(double min_z, double max_z)
{
    this->tverts_range.first  = 0;
    this->tverts_range.second = this->model.indices_count();

    if (!this->print_zs.empty()) {
        // The Z layer range is specified.
        // First test whether the Z span of this object is not out of (min_z, max_z) completely.
        if (this->print_zs.front() > max_z || this->print_zs.back() < min_z)
            this->tverts_range.second = 0;
        else {
            // Then find the lowest layer to be displayed.
            size_t i = 0;
            for (; i < this->print_zs.size() && this->print_zs[i] < min_z; ++i)
                ;
            if (i == this->print_zs.size())
                // This shall not happen.
                this->tverts_range.second = 0;
            else {
                // Remember start of the layer.
                this->tverts_range.first = this->offsets[i];
                // Some layers are above $min_z. Which?
                for (; i < this->print_zs.size() && this->print_zs[i] <= max_z; ++i)
                    ;
                if (i < this->print_zs.size())
                    this->tverts_range.second = this->offsets[i];
            }
        }
    }
}

void GLVolume::render()
{
    if (!is_active)
        return;

    GLShaderProgram* shader = GUI::wxGetApp().get_current_shader();
    if (shader == nullptr)
        return;

    ModelObjectPtrs&       model_objects = GUI::wxGetApp().model().objects;
    std::vector<ColorRGBA> colors        = get_extruders_colors();

    simple_render(shader, model_objects, colors);
}
// BBS add render for simple case
void GLVolume::simple_render(GLShaderProgram*            shader,
                             ModelObjectPtrs&            model_objects,
                             std::vector<ColorRGBA>&     extruder_colors,
                             bool                        ban_light,
                             const std::array<float, 2>* z_range)
{
    if (this->is_left_handed())
        glFrontFace(GL_CW);
    glsafe(::glCullFace(GL_BACK));

    bool         color_volume                 = false;
    bool         source_color_volume          = false;
    bool         palette_source_color_volume  = false;
    bool         adaptive_source_color_volume = false;
    ModelObject* model_object                 = nullptr;
    ModelVolume* model_volume                 = nullptr;
    do {
        if ((!printable) || object_idx() >= model_objects.size())
            break;
        model_object = model_objects[object_idx()];

        if (volume_idx() >= model_object->volumes.size())
            break;
        model_volume                                                     = model_object->volumes[volume_idx()];
        const bool interactive_image_map_preview = image_map_preview_override_data != nullptr;
        const std::shared_ptr<const ImageMap::VolumeData> image_map_data = image_map_preview_override_data ?
                                                                               image_map_preview_override_data :
                                                                               model_volume->image_map_data();
        if (model_volume->mmu_segmentation_facets.empty() && (!image_map_data || image_map_data->empty()))
            break;

        std::optional<ImageMap::RenderMode> image_map_render_mode;
        if (image_map_data) {
            const auto zone_it = std::find_if(image_map_data->zones.begin(), image_map_data->zones.end(),
                                              [](const ImageMap::Zone& zone) { return zone.enabled; });
            if (zone_it != image_map_data->zones.end())
                image_map_render_mode = zone_it->render_mode;
        }
        const bool quantized_image_map = image_map_render_mode == ImageMap::RenderMode::NormalMix;
        // A settings override is specifically a printable-result prediction:
        // otherwise the global "Original texture" preference bypasses the
        // modulation filters and makes live quality controls appear inert.
        const bool predicted_image_map = interactive_image_map_preview || GUI::image_map_preview_predicted_colors();
        // Filament-ID thumbnails require exact per-filament geometry. Normal
        // interactive rendering uses the bounded asynchronous preview below.
        if (image_map_render_mode && !ban_light) {
            if (!picking) {
                std::vector<ImageMap::ContinuousColorComponent> preview_components = !predicted_image_map || quantized_image_map ?
                                                                                         std::vector<ImageMap::ContinuousColorComponent>() :
                                                                                         source_color_preview_components();
                const ImageMapPreviewPalette                    preview_palette    = image_map_preview_palette();
                const unsigned int                              base_filament_id   = unsigned(
                    std::clamp(model_volume->extruder_id(), 1, int(std::max<size_t>(1, preview_palette.num_total))));
                std::vector<SourceColorPreviewAssignment> assignments;
                if (predicted_image_map &&
                    (quantized_image_map || *image_map_render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles)) {
                    std::optional<MixedFilamentDisplayContext> adaptive_display_context;
                    if (*image_map_render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles) {
                        std::vector<std::string> physical_colors;
                        physical_colors.reserve(preview_components.size());
                        for (const ImageMap::ContinuousColorComponent& component : preview_components)
                            physical_colors.emplace_back(component.color_hex);
                        adaptive_display_context = GUI::build_mixed_filament_display_context(physical_colors);
                    }
                    for (const ImageMap::Zone& zone : image_map_data->zones) {
                        if (!zone.enabled || zone.render_mode != *image_map_render_mode)
                            continue;
                        const uint32_t zone_index = uint32_t(&zone - image_map_data->zones.data());
                        for (const ImageMap::PaletteEntry& entry : zone.palette) {
                            const unsigned int filament_id = preview_palette.resolve(entry);
                            if (filament_id == 0)
                                continue;
                            RGBA display_color = *image_map_render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles ?
                                                     RGBA{0.55f, 0.55f, 0.55f, 1.f} :
                                                     entry.target_color;
                            if (quantized_image_map && filament_id <= extruder_colors.size()) {
                                const ColorRGBA adjusted = adjust_color_for_rendering(extruder_colors[filament_id - 1]);
                                display_color            = {adjusted.r(), adjusted.g(), adjusted.b(), adjusted.a()};
                            }
                            std::vector<ImageMap::ContinuousColorComponent> assignment_components;
                            if (*image_map_render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles) {
                                if (filament_id >= 1 && filament_id <= preview_components.size())
                                    display_color = source_color_preview_rgba(preview_components[filament_id - 1].color_hex);
                            }
                            if (*image_map_render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles &&
                                preview_palette.manager != nullptr && adaptive_display_context) {
                                const std::optional<MixedFilamentDefinition> definition =
                                    preview_palette.manager->mixed_filament_definition_from_id(filament_id, preview_palette.num_physical);
                                if (definition) {
                                    display_color = source_color_preview_rgba(
                                        compute_mixed_filament_display_color(*definition, *adaptive_display_context));
                                    for (const MixedFilamentWeightedComponent& component : definition->recipe.blend.components) {
                                        const unsigned int component_id = component.filament.id;
                                        const bool already_added = std::any_of(assignment_components.begin(), assignment_components.end(),
                                                                               [component_id, &preview_components](
                                                                                   const ImageMap::ContinuousColorComponent& existing) {
                                                                                   return component_id >= 1 &&
                                                                                          component_id <= preview_components.size() &&
                                                                                          existing.color_hex ==
                                                                                              preview_components[component_id - 1].color_hex;
                                                                               });
                                        if (component.percent > 0 && component_id >= 1 && component_id <= preview_components.size() &&
                                            !already_added)
                                            assignment_components.push_back(preview_components[component_id - 1]);
                                    }
                                }
                            }
                            assignments.push_back(
                                {zone_index, entry.target_color, filament_id, display_color, std::move(assignment_components)});
                        }
                    }
                }
                size_t preview_signature = source_color_preview_signature(preview_components);
                preview_signature ^= size_t(*image_map_render_mode) + size_t(0x9e3779b9) + (preview_signature << 6) +
                                     (preview_signature >> 2);
                const size_t assignment_signature = source_color_preview_assignment_signature(assignments);
                preview_signature ^= assignment_signature + size_t(0x9e3779b9) + (preview_signature << 6) + (preview_signature >> 2);
                const size_t palette_signature = preview_palette.signature(*image_map_data, base_filament_id);
                preview_signature ^= palette_signature + size_t(0x9e3779b9) + (preview_signature << 6) + (preview_signature >> 2);
                const size_t zone_signature = source_color_preview_zone_signature(*image_map_data);
                preview_signature ^= zone_signature + size_t(0x9e3779b9) + (preview_signature << 6) + (preview_signature >> 2);
                preview_signature ^= size_t(predicted_image_map) + size_t(0x9e3779b9) + (preview_signature << 6) +
                                     (preview_signature >> 2);
                preview_signature ^= size_t(interactive_image_map_preview) + size_t(0x9e3779b9) + (preview_signature << 6) +
                                     (preview_signature >> 2);
                const Transform3d preview_mesh_to_world = world_matrix();
                // Rotation and scaling change which texture direction lies in
                // a physical layer. Rebuild the prediction when either
                // changes; translation does not affect the filter geometry.
                for (Eigen::Index row = 0; row < 3; ++row) {
                    for (Eigen::Index column = 0; column < 3; ++column) {
                        const size_t transform_value = std::hash<double>{}(preview_mesh_to_world.linear()(row, column));
                        preview_signature ^= transform_value + size_t(0x9e3779b9) + (preview_signature << 6) +
                                             (preview_signature >> 2);
                    }
                }
                if (image_map_data != image_map_source_preview_data || preview_signature != image_map_source_preview_signature) {
                    // Keep the last completed preview visible while its
                    // replacement is computed. Clearing it here made every
                    // debounced settings edit blank the object at 2%.
                    if (image_map_source_preview_job)
                        image_map_source_preview_job->cancel.store(true, std::memory_order_relaxed);
                    image_map_source_preview_data      = image_map_data;
                    image_map_source_preview_signature = preview_signature;
                    image_map_source_preview_job       = start_source_color_preview(model_volume->mesh_ptr(), image_map_data,
                                                                                    std::move(preview_components), std::move(assignments),
                                                                                    model_volume->name, *image_map_render_mode,
                                                                                    quantized_image_map, predicted_image_map,
                                                                                    preview_mesh_to_world,
                                                                                    interactive_image_map_preview);
                }

                if (image_map_source_preview_job) {
                    const SourceColorPreviewStatus status = image_map_source_preview_job->status.load(std::memory_order_acquire);
                    if (status == SourceColorPreviewStatus::Ready) {
                        // Upload exactly once on the first render that observes
                        // the completed worker result. The previous inverted
                        // exchange skipped this render and could leave a modal
                        // settings preview permanently parked at 99%.
                        if (!image_map_source_preview_job->ready_presented.exchange(true, std::memory_order_acq_rel)) {
                            std::unique_ptr<GUI::GLModel::Geometry> geometry;
                            std::vector<SourceTexturePreviewPart>   texture_parts;
                            {
                                std::lock_guard<std::mutex> lock(image_map_source_preview_job->result_mutex);
                                geometry      = std::move(image_map_source_preview_job->geometry);
                                texture_parts = std::move(image_map_source_preview_job->texture_parts);
                            }
                            // Swap the complete result as a unit only after
                            // the worker has finished. A preview may contain
                            // both background geometry and texture overlays.
                            image_map_source_model.reset();
                            image_map_source_texture_models.clear();
                            image_map_source_textures.clear();
                            image_map_source_texture_transparent_wraps.clear();
                            if (geometry && geometry->vertices_count() != 0 && geometry->indices_count() != 0) {
                                image_map_source_model.init_from(std::move(*geometry));
                            }
                            if (!texture_parts.empty()) {
                                image_map_source_texture_models.resize(texture_parts.size());
                                image_map_source_textures.resize(texture_parts.size());
                                image_map_source_texture_transparent_wraps.resize(texture_parts.size());
                                for (size_t part_index = 0; part_index < texture_parts.size(); ++part_index) {
                                    SourceTexturePreviewPart& part = texture_parts[part_index];
                                    image_map_source_texture_transparent_wraps[part_index] = {
                                        part.wrap_u == ImageMap::WrapMode::Transparent,
                                        part.wrap_v == ImageMap::WrapMode::Transparent};
                                    if (!part.geometry || part.geometry->is_empty() || part.rgba.empty())
                                        continue;
                                    image_map_source_texture_models[part_index].init_from(std::move(*part.geometry));
                                    auto texture = std::make_unique<GUI::GLTexture>();
                                    if (!texture->load_from_raw_data(std::move(part.rgba), part.width, part.height, false, false)) {
                                        image_map_source_texture_models[part_index].reset();
                                        continue;
                                    }
                                    glsafe(::glBindTexture(GL_TEXTURE_2D, texture->get_id()));
                                    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
                                    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
                                    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                                                             part.wrap_u == ImageMap::WrapMode::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE));
                                    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                                                             part.wrap_v == ImageMap::WrapMode::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE));
                                    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
                                    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
                                    image_map_source_textures[part_index] = std::move(texture);
                                }
                            }
                            image_map_source_preview_job->progress.store(99, std::memory_order_relaxed);
                            image_map_source_preview_job->status.store(SourceColorPreviewStatus::Uploading, std::memory_order_release);
                        }
                    } else if (status == SourceColorPreviewStatus::Uploaded) {
                        if (!image_map_source_preview_job->uploaded_presented.exchange(true, std::memory_order_acq_rel))
                            image_map_source_preview_job.reset();
                    } else if (status == SourceColorPreviewStatus::Cancelled || status == SourceColorPreviewStatus::Failed) {
                        image_map_source_preview_job.reset();
                    }
                }

                const bool has_texture_preview = image_map_source_texture_models.size() == image_map_source_textures.size() &&
                                                 image_map_source_texture_models.size() ==
                                                     image_map_source_texture_transparent_wraps.size() &&
                                                 std::any_of(image_map_source_texture_models.begin(), image_map_source_texture_models.end(),
                                                             [this](const GUI::GLModel& texture_model) {
                                                                 const size_t index = size_t(&texture_model -
                                                                                             image_map_source_texture_models.data());
                                                                 return texture_model.is_initialized() &&
                                                                        image_map_source_textures[index] != nullptr &&
                                                                        image_map_source_textures[index]->get_id() != 0;
                                                             });
                if (image_map_source_model.is_initialized() || has_texture_preview) {
                    color_volume                = true;
                    source_color_volume         = true;
                    palette_source_color_volume = predicted_image_map &&
                                                  (quantized_image_map ||
                                                   *image_map_render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles);
                    adaptive_source_color_volume = predicted_image_map &&
                                                   *image_map_render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles;
                } else if (image_map_source_preview_job) {
                    // Do not show the photographic source texture as if it
                    // were an attainable print while the first KM/K-S preview
                    // is still being prepared.
                    source_color_volume = true;
                }
            }
            // Exact facet reconstruction remains a slice-time task.
            break;
        }

        const ImageMapPreviewPalette preview_palette  = image_map_preview_palette();
        const unsigned int           base_filament_id = unsigned(
            std::clamp(model_volume->extruder_id(), 1, int(std::max<size_t>(1, preview_palette.num_total))));
        const size_t image_map_palette_signature = image_map_data ? preview_palette.signature(*image_map_data, base_filament_id) : 0;
        color_volume                             = true;
        if (model_volume->mmu_segmentation_facets.timestamp() != mmuseg_ts || image_map_data != image_map_preview_data ||
            image_map_palette_signature != image_map_preview_palette_signature) {
            mmuseg_models.clear();
            std::unique_ptr<FacetsAnnotation> preview_facets = model_volume->mmu_segmentation_facets.copy_for_slicing();
            if (image_map_data) {
                const ImageMap::FacetRasterization rasterized = ImageMap::rasterize_facets(model_volume->mesh(), *image_map_data,
                                                                                           base_filament_id,
                                                                                           [&preview_palette](
                                                                                               const ImageMap::PaletteEntry& entry) {
                                                                                               return preview_palette.resolve(entry);
                                                                                           });
                for (const ImageMap::RasterizedFacet& facet : rasterized.facets) {
                    if (!facet.encoded_states.empty())
                        preview_facets->set_triangle_from_string(int(facet.triangle_index), facet.encoded_states);
                }
                if (rasterized.unresolved_palette_entries != 0) {
                    BOOST_LOG_TRIVIAL(warning) << "Image-map viewport palette resolution fell back to the volume filament"
                                               << " unresolved=" << rasterized.unresolved_palette_entries
                                               << " volume=" << model_volume->name;
                }
            }
            std::vector<indexed_triangle_set> its_per_color;
            preview_facets->get_facets(*model_volume, its_per_color);
            mmuseg_models.resize(its_per_color.size());
            for (int idx = 0; idx < its_per_color.size(); idx++) {
                mmuseg_models[idx].init_from(its_per_color[idx]);
            }

            mmuseg_ts                           = model_volume->mmu_segmentation_facets.timestamp();
            image_map_preview_data              = image_map_data;
            image_map_preview_palette_signature = image_map_palette_signature;
        }
    } while (0);

    if (source_color_volume) {
        const bool has_texture_preview = image_map_source_texture_models.size() == image_map_source_textures.size() &&
                                         image_map_source_texture_models.size() == image_map_source_texture_transparent_wraps.size() &&
                                         std::any_of(image_map_source_texture_models.begin(), image_map_source_texture_models.end(),
                                                     [this](const GUI::GLModel& texture_model) {
                                                         const size_t index = size_t(&texture_model -
                                                                                     image_map_source_texture_models.data());
                                                         return texture_model.is_initialized() &&
                                                                image_map_source_textures[index] != nullptr &&
                                                                image_map_source_textures[index]->get_id() != 0;
                                                     });
        if (has_texture_preview && GUI::wxGetApp().plater() != nullptr) {
            if (image_map_source_model.is_initialized()) {
                ColorRGBA background_color = ColorRGBA::WHITE();
                background_color.a(render_color.a());
                image_map_source_model.set_color(background_color);
                if (shader != nullptr && palette_source_color_volume) {
                    shader->set_uniform("image_map_cycle_preview", true);
                    shader->set_uniform("image_map_highlight_filament_id",
                                        adaptive_source_color_volume ? int(image_map_highlight_filament_id) : 0);
                }
                image_map_source_model.render();
                if (shader != nullptr && palette_source_color_volume) {
                    shader->set_uniform("image_map_cycle_preview", false);
                    shader->set_uniform("image_map_highlight_filament_id", 0);
                }
            }

            GLShaderProgram* texture_shader = GUI::wxGetApp().get_shader("image_map_texture_preview");
            if (texture_shader != nullptr) {
                const GUI::Camera& camera             = GUI::wxGetApp().plater()->get_camera();
                const Transform3d  model_matrix       = world_matrix();
                const Transform3d  view_model_matrix  = camera.get_view_matrix() * model_matrix;
                const Matrix3d     view_normal_matrix = camera.get_view_matrix().matrix().block(0, 0, 3, 3) *
                                                    model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
                const std::array<float, 2> texture_z_range = z_range != nullptr ? *z_range :
                                                                                  std::array<float, 2>{std::numeric_limits<float>::lowest(),
                                                                                                       std::numeric_limits<float>::max()};
                ColorRGBA                  source_color    = ColorRGBA::WHITE();
                source_color.a(render_color.a());

                texture_shader->start_using();
                texture_shader->set_uniform("view_model_matrix", view_model_matrix);
                texture_shader->set_uniform("projection_matrix", camera.get_projection_matrix());
                texture_shader->set_uniform("view_normal_matrix", view_normal_matrix);
                texture_shader->set_uniform("volume_world_matrix", model_matrix);
                texture_shader->set_uniform("z_range", texture_z_range);
                texture_shader->set_uniform("uniform_texture", 0);
                texture_shader->set_uniform("image_map_cycle_preview", palette_source_color_volume);
                texture_shader->set_uniform("image_map_highlight_filament_id",
                                            adaptive_source_color_volume ? int(image_map_highlight_filament_id) : 0);
                glsafe(::glActiveTexture(GL_TEXTURE0));
                const bool blend_enabled = glIsEnabled(GL_BLEND) == GL_TRUE;
                const bool polygon_offset_enabled = glIsEnabled(GL_POLYGON_OFFSET_FILL) == GL_TRUE;
                GLint      depth_function = GL_LESS;
                GLint      blend_src_rgb  = GL_ONE;
                GLint      blend_dst_rgb  = GL_ZERO;
                GLint      blend_src_alpha = GL_ONE;
                GLint      blend_dst_alpha = GL_ZERO;
                GLfloat    polygon_offset_factor = 0.f;
                GLfloat    polygon_offset_units  = 0.f;
                glsafe(::glGetIntegerv(GL_DEPTH_FUNC, &depth_function));
                glsafe(::glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb));
                glsafe(::glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb));
                glsafe(::glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_alpha));
                glsafe(::glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_alpha));
                glsafe(::glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &polygon_offset_factor));
                glsafe(::glGetFloatv(GL_POLYGON_OFFSET_UNITS, &polygon_offset_units));
                glsafe(::glEnable(GL_BLEND));
                glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
                glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
                glsafe(::glPolygonOffset(-1.f, -1.f));
                glsafe(::glDepthFunc(GL_LEQUAL));
                for (size_t index = 0; index < image_map_source_texture_models.size(); ++index) {
                    if (!image_map_source_texture_models[index].is_initialized() || image_map_source_textures[index] == nullptr ||
                        image_map_source_textures[index]->get_id() == 0)
                        continue;
                    texture_shader->set_uniform("transparent_wrap_u", image_map_source_texture_transparent_wraps[index][0]);
                    texture_shader->set_uniform("transparent_wrap_v", image_map_source_texture_transparent_wraps[index][1]);
                    glsafe(::glBindTexture(GL_TEXTURE_2D, image_map_source_textures[index]->get_id()));
                    image_map_source_texture_models[index].set_color(source_color);
                    image_map_source_texture_models[index].render();
                }
                glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
                glsafe(::glDepthFunc(depth_function));
                glsafe(::glPolygonOffset(polygon_offset_factor, polygon_offset_units));
                if (!polygon_offset_enabled)
                    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
                glsafe(::glBlendFuncSeparate(blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha));
                if (!blend_enabled)
                    glsafe(::glDisable(GL_BLEND));
                texture_shader->stop_using();
                if (shader != nullptr)
                    shader->start_using();
            }
            if (image_map_source_preview_job &&
                image_map_source_preview_job->status.load(std::memory_order_acquire) == SourceColorPreviewStatus::Uploading) {
                image_map_source_preview_job->progress.store(100, std::memory_order_relaxed);
                image_map_source_preview_job->status.store(SourceColorPreviewStatus::Uploaded, std::memory_order_release);
            }
        } else if (image_map_source_model.is_initialized()) {
            ColorRGBA source_color = ColorRGBA::WHITE();
            source_color.a(render_color.a());
            image_map_source_model.set_color(source_color);
            if (shader != nullptr && palette_source_color_volume) {
                shader->set_uniform("image_map_cycle_preview", true);
                shader->set_uniform("image_map_highlight_filament_id",
                                    adaptive_source_color_volume ? int(image_map_highlight_filament_id) : 0);
            }
            image_map_source_model.render();
            if (image_map_source_preview_job &&
                image_map_source_preview_job->status.load(std::memory_order_acquire) == SourceColorPreviewStatus::Uploading) {
                image_map_source_preview_job->progress.store(100, std::memory_order_relaxed);
                image_map_source_preview_job->status.store(SourceColorPreviewStatus::Uploaded, std::memory_order_release);
            }
            if (shader != nullptr && palette_source_color_volume) {
                shader->set_uniform("image_map_cycle_preview", false);
                shader->set_uniform("image_map_highlight_filament_id", 0);
            }
        }
    } else if (color_volume && !picking) {
        // when force_transparent, we need to keep the alpha
        if (force_native_color && render_color.is_transparent()) {
            for (auto& extruder_color : extruder_colors)
                extruder_color.a(render_color.a());
        }

        const std::array<float, 2>  full_z_range{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max()};
        const std::array<float, 2>& active_z_range = z_range != nullptr ? *z_range : full_z_range;
        const BoundingBoxf3         box            = transformed_bounding_box();

        for (int idx = 0; idx < mmuseg_models.size(); idx++) {
            GUI::GLModel& m = mmuseg_models[idx];
            if (!m.is_initialized())
                continue;

            int filament_id = idx;
            if (idx == 0) {
                filament_id = model_volume->extruder_id();
                if (filament_id <= 0)
                    filament_id = 1;
            }
            const size_t filament_idx = filament_id > 0 ? size_t(filament_id - 1) : size_t(0);

            const bool has_gradient =
                !picking &&
                filament_idx < preview_gradient_colors_by_extruder.size() &&
                preview_gradient_colors_by_extruder[filament_idx].size() >= 2;
            if (has_gradient) {
                float alpha = filament_idx < extruder_colors.size() ? extruder_colors[filament_idx].a() : 1.0f;
                if (force_native_color && render_color.is_transparent())
                    alpha = render_color.a();
                if (ban_light)
                    alpha = (255 - (filament_id - 1)) / 255.0f;

                static const std::vector<double> empty_positions;
                const std::vector<double>& positions =
                    filament_idx < preview_gradient_positions_by_extruder.size() ?
                        preview_gradient_positions_by_extruder[filament_idx] :
                        empty_positions;
                if (render_preview_gradient_model(m, tverts_range, shader, box, active_z_range,
                                                  preview_gradient_colors_by_extruder[filament_idx], positions, alpha)) {
                    shader->set_uniform("z_range", active_z_range);
                    continue;
                }
            }

            if (shader) {
                if (idx == 0) {
                    // to make black not too hard too see
                    const size_t color_idx = filament_id > 0 && size_t(filament_id - 1) < extruder_colors.size() ?
                        size_t(filament_id - 1) :
                        size_t(0);
                    ColorRGBA new_color = extruder_colors.empty() ?
                        ColorRGBA::WHITE() :
                        adjust_color_for_rendering(extruder_colors[color_idx]);
                    if (ban_light) {
                        new_color[3] = (255 - (filament_id - 1)) / 255.0f;
                    }
                    m.set_color(new_color);
                    // shader->set_uniform("uniform_color", new_color);
                } else {
                    if (idx <= extruder_colors.size()) {
                        // to make black not too hard too see
                        ColorRGBA new_color = adjust_color_for_rendering(extruder_colors[idx - 1]);
                        if (ban_light) {
                            new_color[3] = (255 - (idx - 1)) / 255.0f;
                        }
                        m.set_color(new_color);
                        // shader->set_uniform("uniform_color", new_color);
                    } else {
                        // to make black not too hard too see
                        ColorRGBA new_color = extruder_colors.empty() ?
                            ColorRGBA::WHITE() :
                            adjust_color_for_rendering(extruder_colors[0]);
                        if (ban_light) {
                            new_color[3] = (255 - 0) / 255.0f;
                        }
                        m.set_color(new_color);
                        // shader->set_uniform("uniform_color", new_color);
                    }
                }
            }
            if (tverts_range == std::make_pair<size_t, size_t>(0, -1))
                m.render();
            else
                m.render(this->tverts_range);
        }
    } else {
        // Select LOD model based on current LOD level
        static int lodRenderLogCounter = 0;
        lodRenderLogCounter++;
        if (!picking) {
            // DEBUG: color-code LOD levels for visual verification
            // GREEN = HIGH (original), BLUE = MIDDLE, RED = SMALL
            if (m_curLodLevel == LODLevel::Small && m_modelSmall && !m_modelSmall->is_render_disabled() && m_modelSmall->is_initialized()) {
                if (lodRenderLogCounter % 180 == 0)
                    BOOST_LOG_TRIVIAL(debug) << "LOD: SMALL '" << name << "'";
                m_modelSmall->set_color(render_color);
                //m_modelSmall->set_color(ColorRGBA::GREEN());
                m_modelSmall->render();
            } else if (m_curLodLevel == LODLevel::Middle && m_modelMiddle && !m_modelMiddle->is_render_disabled() && m_modelMiddle->is_initialized()) {
                if (lodRenderLogCounter % 180 == 0)
                    BOOST_LOG_TRIVIAL(debug) << "LOD: MID '" << name << "'";
                m_modelMiddle->set_color(render_color);
                //m_modelMiddle->set_color(ColorRGBA::BLUE());
                m_modelMiddle->render();
            } else {
                if (lodRenderLogCounter % 180 == 0) {
                    BOOST_LOG_TRIVIAL(debug) << "LOD: HIGH fallback '" << name
                                              << "' lv=" << static_cast<int>(m_curLodLevel)
                                              << " s=" << (m_modelSmall ? (int)(!m_modelSmall->is_render_disabled() && m_modelSmall->is_initialized()) : -1)
                                              << " m=" << (m_modelMiddle ? (int)(!m_modelMiddle->is_render_disabled() && m_modelMiddle->is_initialized()) : -1);
                }
                // model.set_color() already called in render loop line 1301
                //model.set_color(ColorRGBA::RED());
                if (tverts_range == std::make_pair<size_t, size_t>(0, -1))
                    model.render();
                else
                    model.render(this->tverts_range);
            }
        } else {
            // Picking: always use full-resolution model
            if (tverts_range == std::make_pair<size_t, size_t>(0, -1))
                model.render();
            else
                model.render(this->tverts_range);
        }
    }
    if (this->is_left_handed())
        glFrontFace(GL_CCW);
}

bool GLVolume::is_sla_support() const { return this->composite_id.volume_id == -int(slaposSupportTree); }
bool GLVolume::is_sla_pad() const { return this->composite_id.volume_id == -int(slaposPad); }

bool GLVolume::is_sinking() const
{
    if (is_modifier || GUI::wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA)
        return false;
    const BoundingBoxf3& box = transformed_convex_hull_bounding_box();
    return box.min.z() < SINKING_Z_THRESHOLD && box.max.z() >= SINKING_Z_THRESHOLD;
}

bool GLVolume::is_below_printbed() const { return transformed_convex_hull_bounding_box().max.z() < 0.0; }

void GLVolume::render_sinking_contours() { m_sinking_contours.render(); }

GLWipeTowerVolume::GLWipeTowerVolume(const std::vector<ColorRGBA>& colors) : GLVolume() { m_colors = colors; }

void GLWipeTowerVolume::render()
{
    if (!is_active)
        return;

    if (m_colors.size() == 0 || m_colors.size() != model_per_colors.size())
        return;

    if (this->is_left_handed())
        glFrontFace(GL_CW);
    glsafe(::glCullFace(GL_BACK));

    for (int i = 0; i < m_colors.size(); i++) {
        if (!picking) {
            ColorRGBA new_color = adjust_color_for_rendering(m_colors[i]);
            this->model_per_colors[i].set_color(new_color);
        } else {
            this->model_per_colors[i].set_color(model.get_color());
        }
        this->model_per_colors[i].render();
    }

    if (this->is_left_handed())
        glFrontFace(GL_CCW);
}

bool GLWipeTowerVolume::IsTransparent()
{
    for (size_t i = 0; i < m_colors.size(); i++) {
        if (m_colors[i].is_transparent()) {
            return true;
        }
    }
    return false;
}

std::vector<int> GLVolumeCollection::load_object(const ModelObject*      model_object,
                                                 int                     obj_idx,
                                                 const std::vector<int>& instance_idxs,
                                                 const std::string&      color_by,
                                                 bool                    opengl_initialized,
                                                 bool                    need_raycaster,
                                                 bool                    lodEnabled)
{
    std::vector<int> volumes_idx;
    for (int volume_idx = 0; volume_idx < int(model_object->volumes.size()); ++volume_idx)
        for (int instance_idx : instance_idxs)
            volumes_idx.emplace_back(this->GLVolumeCollection::load_object_volume(
                model_object, obj_idx, volume_idx, instance_idx, color_by,
                opengl_initialized, false, false, need_raycaster, lodEnabled));
    return volumes_idx;
}

int GLVolumeCollection::load_object_volume(const ModelObject* model_object,
                                           int                obj_idx,
                                           int                volume_idx,
                                           int                instance_idx,
                                           const std::string& color_by,
                                           bool               opengl_initialized,
                                           bool               in_assemble_view,
                                           bool               use_loaded_id,
                                           bool               need_raycaster,
                                           bool               lodEnabled)
{
    const ModelVolume*   model_volume = model_object->volumes[volume_idx];
    const int            extruder_id  = model_volume->extruder_id();
    const ModelInstance* instance     = model_object->instances[instance_idx];
    auto                 color        = GLVolume::MODEL_COLOR[((color_by == "volume") ? volume_idx : obj_idx) % 4];
    color.a(model_volume->is_model_part() ? 0.7f : 0.4f);

    std::shared_ptr<const TriangleMesh> meshSharedPtr = model_volume->mesh_ptr();
    const TriangleMesh*                 meshPtr       = meshSharedPtr.get();
    this->volumes.emplace_back(new GLVolume(color));
    GLVolume& v = *this->volumes.back();
    v.set_color(color_from_model_volume(*model_volume));
    v.name = model_volume->name;

    // LOD mesh sharing: if another volume already loaded this mesh, reuse its LOD data
    v.m_oriMesh = meshPtr;
    auto iter = g_meshVolumesMap.find(meshPtr);
    if (iter != g_meshVolumesMap.end()) {
        MeshLodEntry& entry = iter->second;
        if (!entry.volumes.empty()) {
            GLVolume* firstVolume = *entry.volumes.begin();
            // Share LOD models via shared_ptr (ref-counted, safe GPU buffer sharing)
            v.m_modelMiddle = firstVolume->m_modelMiddle;
            v.m_modelSmall  = firstVolume->m_modelSmall;
            // Share the readiness flags together with the models: while a
            // flag is false its model may still be written by the background
            // thread and must stay render-disabled.
            v.m_lodMiddleReady = firstVolume->m_lodMiddleReady;
            v.m_lodSmallReady  = firstVolume->m_lodSmallReady;
            // Note: model (main mesh) is always created per-volume since it's a value type
            // This avoids dangling GPU buffer issues when one volume is destroyed
        }
        entry.volumes.emplace(&v);
    } else {
        MeshLodEntry entry;
        entry.mesh = meshSharedPtr; // keep the mesh (and thus the map key) alive
        entry.volumes.emplace(&v);
        g_meshVolumesMap.emplace(meshPtr, std::move(entry));
    }

    // Always init the main model (GLModel is a value type, not shared)
    const TriangleMesh& mesh = *meshPtr;
#if ENABLE_SMOOTH_NORMALS
    v.model.init_from(mesh, true);
#else
    v.model.init_from(mesh);
#endif // ENABLE_SMOOTH_NORMALS

    // Generate LOD simplified models only once (shared via shared_ptr)
    if (lodEnabled && !v.m_modelMiddle && !v.m_modelSmall) {
        BOOST_LOG_TRIVIAL(info) << "LOD: Creating simplified models for '" << v.name
                                << "' faces=" << mesh.its.indices.size();
        v.m_modelMiddle = std::make_shared<GUI::GLModel>();
        // Keep rendering disabled until the background thread finishes
        // init_from() (GLModel.hpp threading contract); the main thread
        // re-enables it in promote_ready_lod_models() once the ready flag
        // is observed.
        v.m_modelMiddle->disable_render();
        v.m_lodMiddleReady = std::make_shared<std::atomic<bool>>(false);
        v.SimplifyMesh(mesh, v.m_modelMiddle, v.m_lodMiddleReady, LODLevel::Middle);

        v.m_modelSmall = std::make_shared<GUI::GLModel>();
        v.m_modelSmall->disable_render();
        v.m_lodSmallReady = std::make_shared<std::atomic<bool>>(false);
        v.SimplifyMesh(mesh, v.m_modelSmall, v.m_lodSmallReady, LODLevel::Small);
    } else if (!lodEnabled) {
        BOOST_LOG_TRIVIAL(info) << "LOD: Disabled for '" << v.name << "'";
    }

    if (need_raycaster) {
        v.mesh_raycaster = std::make_unique<GUI::MeshRaycaster>(meshSharedPtr);
    }
    v.composite_id = GLVolume::CompositeID(obj_idx, volume_idx, instance_idx);

    if (model_volume->is_model_part()) {
        // GLVolume will reference a convex hull from model_volume!
        v.set_convex_hull(model_volume->get_convex_hull_shared_ptr());
        if (extruder_id != -1)
            v.extruder_id = extruder_id;
    }
    v.is_modifier                              = !model_volume->is_model_part();
    v.shader_outside_printer_detection_enabled = model_volume->is_model_part();
    if (in_assemble_view) {
        v.set_instance_transformation(instance->get_assemble_transformation());
        v.set_offset_to_assembly(instance->get_offset_to_assembly());
    } else
        v.set_instance_transformation(instance->get_transformation());
    v.set_volume_transformation(model_volume->get_transformation());
    // use object's instance id
    if (use_loaded_id && (instance->loaded_id > 0))
        v.model_object_ID = instance->loaded_id;
    else
        v.model_object_ID = instance->id().id;

    return int(this->volumes.size() - 1);
}

// Load SLA auxiliary GLVolumes (for support trees or pad).
// This function produces volumes for multiple instances in a single shot,
// as some object specific mesh conversions may be expensive.
void GLVolumeCollection::load_object_auxiliary(const SLAPrintObject* print_object,
                                               int                   obj_idx,
                                               // pairs of <instance_idx, print_instance_idx>
                                               const std::vector<std::pair<size_t, size_t>>& instances,
                                               SLAPrintObjectStep                            milestone,
                                               // Timestamp of the last change of the milestone
                                               size_t timestamp)
{
    assert(print_object->is_step_done(milestone));
    Transform3d mesh_trafo_inv = print_object->trafo().inverse();
    // Get the support mesh.
    TriangleMesh mesh = print_object->get_mesh(milestone);
    mesh.transform(mesh_trafo_inv);
    // Convex hull is required for out of print bed detection.
    TriangleMesh convex_hull = mesh.convex_hull_3d();
    for (const std::pair<size_t, size_t>& instance_idx : instances) {
        const ModelInstance& model_instance = *print_object->model_object()->instances[instance_idx.first];
        this->volumes.emplace_back(new GLVolume((milestone == slaposPad) ? GLVolume::SLA_PAD_COLOR : GLVolume::SLA_SUPPORT_COLOR));
        GLVolume& v = *this->volumes.back();
#if ENABLE_SMOOTH_NORMALS
        v.model.init_from(mesh, true);
#else
        v.model.init_from(mesh);
        v.model.set_color((milestone == slaposPad) ? GLVolume::SLA_PAD_COLOR : GLVolume::SLA_SUPPORT_COLOR);
        v.mesh_raycaster = std::make_unique<GUI::MeshRaycaster>(std::make_shared<const TriangleMesh>(mesh));
#endif // ENABLE_SMOOTH_NORMALS
        v.composite_id = GLVolume::CompositeID(obj_idx, -int(milestone), (int) instance_idx.first);
        v.geometry_id  = std::pair<size_t, size_t>(timestamp, model_instance.id().id);
        // Create a copy of the convex hull mesh for each instance. Use a move operator on the last instance.
        if (&instance_idx == &instances.back())
            v.set_convex_hull(std::move(convex_hull));
        else
            v.set_convex_hull(convex_hull);
        v.is_modifier                              = false;
        v.shader_outside_printer_detection_enabled = (milestone == slaposSupportTree);
        v.set_instance_transformation(model_instance.get_transformation());
        // Leave the volume transformation at identity.
        // v.set_volume_transformation(model_volume->get_transformation());
    }
}

int GLVolumeCollection::load_wipe_tower_preview(
    int obj_idx, float pos_x, float pos_y, float width, float depth, float height, float rotation_angle, bool size_unknown, float brim_width)
{
    int plate_idx = obj_idx - 1000;

    if (depth < 0.01f)
        return int(this->volumes.size() - 1);
    if (height == 0.0f)
        height = 0.1f;

    std::vector<ColorRGBA> extruder_colors = get_extruders_colors();
    std::vector<ColorRGBA> colors;
    GUI::PartPlateList&    ppl              = GUI::wxGetApp().plater()->get_partplate_list();
    std::vector<int>       plate_extruders  = ppl.get_plate(plate_idx)->get_extruders(true);
    TriangleMesh           wipe_tower_shell = make_cube(width, depth, height);
    for (int extruder_id : plate_extruders) {
        if (extruder_id <= extruder_colors.size())
            colors.push_back(extruder_colors[extruder_id - 1]);
        else
            colors.push_back(extruder_colors[0]);
    }

    // Orca: make it transparent
    for (auto& color : colors)
        color.a(0.66f);
    volumes.emplace_back(new GLWipeTowerVolume(colors));
    GLWipeTowerVolume& v = *dynamic_cast<GLWipeTowerVolume*>(volumes.back());
    v.model_per_colors.resize(colors.size());
    for (int i = 0; i < colors.size(); i++) {
        TriangleMesh color_part = make_cube(width, depth / colors.size(), height);
        color_part.translate({0.f, depth * i / colors.size(), 0.});
        v.model_per_colors[i].init_from(color_part);
    }
    v.model.init_from(wipe_tower_shell);
    v.mesh_raycaster = std::make_unique<GUI::MeshRaycaster>(std::make_shared<const TriangleMesh>(wipe_tower_shell));
    v.set_convex_hull(wipe_tower_shell);
    v.set_volume_offset(Vec3d(pos_x, pos_y, 0.0));
    v.set_volume_rotation(Vec3d(0., 0., (M_PI / 180.) * rotation_angle));
    v.composite_id                             = GLVolume::CompositeID(obj_idx, 0, 0);
    v.geometry_id.first                        = 0;
    v.geometry_id.second                       = wipe_tower_instance_id().id + (obj_idx - 1000);
    v.is_wipe_tower                            = true;
    v.shader_outside_printer_detection_enabled = !size_unknown;
    return int(volumes.size() - 1);
}

GLVolume* GLVolumeCollection::new_toolpath_volume(const ColorRGBA& rgba)
{
    GLVolume* out          = new_nontoolpath_volume(rgba);
    out->is_extrusion_path = true;
    return out;
}

GLVolume* GLVolumeCollection::new_nontoolpath_volume(const ColorRGBA& rgba)
{
    GLVolume* out          = new GLVolume(rgba);
    out->is_extrusion_path = false;
    this->volumes.emplace_back(out);
    return out;
}

void GLVolumeCollection::release_volume(GLVolume* volume)
{
    if (volume == nullptr || volume->m_oriMesh == nullptr)
        return;
    auto iter = g_meshVolumesMap.find(volume->m_oriMesh);
    if (iter == g_meshVolumesMap.end())
        return;
    MeshLodEntry& entry = iter->second;
    entry.volumes.erase(volume);
    if (entry.volumes.empty())
        // Last holder is gone: drop the entry together with its owning
        // reference to the mesh, so the key address can be reused safely.
        g_meshVolumesMap.erase(iter);
}

GLVolumeWithIdAndZList volumes_to_render(const GLVolumePtrs&                  volumes,
                                         GLVolumeCollection::ERenderType      type,
                                         const Transform3d&                   view_matrix,
                                         std::function<bool(const GLVolume&)> filter_func)
{
    GLVolumeWithIdAndZList list;
    list.reserve(volumes.size());

    for (unsigned int i = 0; i < (unsigned int) volumes.size(); ++i) {
        GLVolume* volume                = volumes[i];
        bool      is_transparent        = volume->render_color.is_transparent();
        auto      tempGlwipeTowerVolume = dynamic_cast<GLWipeTowerVolume*>(volume);
        if (tempGlwipeTowerVolume) {
            is_transparent = tempGlwipeTowerVolume->IsTransparent();
        }
        if (((type == GLVolumeCollection::ERenderType::Opaque && !is_transparent) ||
             (type == GLVolumeCollection::ERenderType::Transparent && is_transparent) || type == GLVolumeCollection::ERenderType::All) &&
            (!filter_func || filter_func(*volume)))
            list.emplace_back(std::make_pair(volume, std::make_pair(i, 0.0)));
    }

    if (type == GLVolumeCollection::ERenderType::Transparent && list.size() > 1) {
        for (GLVolumeWithIdAndZ& volume : list) {
            volume.second.second = volume.first->bounding_box().transformed(view_matrix * volume.first->world_matrix()).max(2);
        }

        std::sort(list.begin(), list.end(),
                  [](const GLVolumeWithIdAndZ& v1, const GLVolumeWithIdAndZ& v2) -> bool { return v1.second.second < v2.second.second; });
    } else if (type == GLVolumeCollection::ERenderType::Opaque && list.size() > 1) {
        std::sort(list.begin(), list.end(), [](const GLVolumeWithIdAndZ& v1, const GLVolumeWithIdAndZ& v2) -> bool {
            return v1.first->selected && !v2.first->selected;
        });
    }

    return list;
}

int GLVolumeCollection::get_selection_support_threshold_angle(bool& enable_support) const
{
    const DynamicPrintConfig& glb_cfg = GUI::wxGetApp().preset_bundle->prints.get_edited_preset().config;
    enable_support                    = glb_cfg.opt_bool("enable_support");
    int support_threshold_angle       = glb_cfg.opt_int("support_threshold_angle");
    return support_threshold_angle;
}

void GLVolumeCollection::render(GLVolumeCollection::ERenderType      type,
                                bool                                 disable_cullface,
                                const GUI::Camera&                   camera,
                                std::function<bool(const GLVolume&)> filter_func,
                                bool                                 partly_inside_enable) const
{
    const Transform3d& view_matrix = camera.get_view_matrix();
    const Transform3d& projection_matrix = camera.get_projection_matrix();
    GLVolumeWithIdAndZList to_render = volumes_to_render(volumes, type, view_matrix, filter_func);
    if (to_render.empty())
        return;

    GLShaderProgram* shader = GUI::wxGetApp().get_current_shader();
    if (shader == nullptr)
        return;

    GLShaderProgram* sink_shader  = GUI::wxGetApp().get_shader("flat");
    GLShaderProgram* edges_shader = GUI::wxGetApp().get_shader("flat");

    if (type == ERenderType::Transparent) {
        glsafe(::glEnable(GL_BLEND));
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    }

    glsafe(::glCullFace(GL_BACK));
    if (disable_cullface)
        glsafe(::glDisable(GL_CULL_FACE));

    // Set static camera state for LOD evaluation in GLVolume rendering
    GLVolume::s_curZoom = camera.get_zoom();
    GLVolume::s_curViewProjMatrix = (projection_matrix.matrix() * view_matrix.matrix()).eval();
    GLVolume::s_curViewport = camera.get_viewport();

    // Evaluate LOD level for each volume once per frame
    float curZoom = GLVolume::s_curZoom;
    bool  shouldEvaluate = (std::abs(curZoom - GLVolume::s_lastCameraZoomValue) > ZOOM_THRESHOLD);
    if (shouldEvaluate)
    {
        GLVolume::s_lastCameraZoomValue = curZoom;
    }
    for (GLVolumeWithIdAndZ& volume : to_render)
    {
        GLVolume* v = volume.first;
        // Hand over LOD models whose background initialization finished.
        // Must run every frame, on the main thread only.
        v->promote_ready_lod_models();
        if (!v->picking && (shouldEvaluate || ++v->m_lodUpdateIndex >= LOD_UPDATE_FREQUENCY))
        {
            v->m_lodUpdateIndex = 0;
            LODLevel prevLod = v->m_curLodLevel;
            v->m_curLodLevel = CalcVolumeBoxInScreenBiggerThanThreshold(
                v->transformed_bounding_box(), GLVolume::s_curViewProjMatrix,
                GLVolume::s_curViewport[2], GLVolume::s_curViewport[3]);
            if (prevLod != v->m_curLodLevel) {
                BOOST_LOG_TRIVIAL(debug) << "LOD level changed: " << static_cast<int>(prevLod)
                                           << " -> " << static_cast<int>(v->m_curLodLevel)
                                           << " (zoom=" << curZoom << ", name=" << v->name << ")";
            }
        }
    }

    for (GLVolumeWithIdAndZ& volume : to_render) {
        //CPU Frustum culling
        auto _worldAABB = volume.first->transformed_bounding_box();
        if (!camera.GetFrustum().Intersects(_worldAABB))
        {
            continue;
        }
#if ENABLE_MODIFIERS_ALWAYS_TRANSPARENT
        if (type == ERenderType::Transparent) {
            volume.first->force_transparent = true;
            // BOOST_LOG_TRIVIAL(info) << boost::format("transparent rendering...");
        }
        // else
        //     BOOST_LOG_TRIVIAL(info) << boost::format("opaque rendering...");
#endif // ENABLE_MODIFIERS_ALWAYS_TRANSPARENT
        volume.first->set_render_color();
#if ENABLE_MODIFIERS_ALWAYS_TRANSPARENT
        if (type == ERenderType::Transparent)
            volume.first->force_transparent = false;
#endif // ENABLE_MODIFIERS_ALWAYS_TRANSPARENT

        // render sinking contours of non-hovered volumes
        shader->stop_using();
        if (sink_shader != nullptr) {
            sink_shader->start_using();
            if (m_show_sinking_contours) {
                if (volume.first->is_sinking() && !volume.first->is_below_printbed() && volume.first->hover == GLVolume::HS_None &&
                    !volume.first->force_sinking_contours) {
                    volume.first->render_sinking_contours();
                }
            }
            sink_shader->stop_using();
        }
        shader->start_using();

        if (!volume.first->model.is_initialized())
            shader->set_uniform("uniform_color", volume.first->render_color);
        shader->set_uniform("z_range", m_z_range);
        shader->set_uniform("clipping_plane", m_clipping_plane);
        shader->set_uniform("use_color_clip_plane", m_use_color_clip_plane);
        shader->set_uniform("color_clip_plane", m_color_clip_plane);
        shader->set_uniform("uniform_color_clip_plane_1", m_color_clip_plane_colors[0]);
        shader->set_uniform("uniform_color_clip_plane_2", m_color_clip_plane_colors[1]);
        // BBS set print_volume to render volume
        // shader->set_uniform("print_volume.type", static_cast<int>(m_render_volume.type));
        // shader->set_uniform("print_volume.xy_data", m_render_volume.data);
        // shader->set_uniform("print_volume.z_data", m_render_volume.zs);

        if (volume.first->partly_inside && partly_inside_enable) {
            // only partly inside volume need to be painted with boundary check
            shader->set_uniform("print_volume.type", static_cast<int>(m_print_volume.type));
            shader->set_uniform("print_volume.xy_data", m_print_volume.data);
            shader->set_uniform("print_volume.z_data", m_print_volume.zs);
        } else {
            // use -1 ad a invalid type
            shader->set_uniform("print_volume.type", -1);
        }

        bool enable_support;
        int  support_threshold_angle = get_selection_support_threshold_angle(enable_support);

        float normal_z = -::cos(Geometry::deg2rad((float) support_threshold_angle));

        shader->set_uniform("volume_world_matrix", volume.first->world_matrix());
        shader->set_uniform("slope.actived", m_slope.isGlobalActive && !volume.first->is_modifier && !volume.first->is_wipe_tower);
        shader->set_uniform("slope.volume_world_normal_matrix",
                            static_cast<Matrix3f>(
                                volume.first->world_matrix().matrix().block(0, 0, 3, 3).inverse().transpose().cast<float>()));
        shader->set_uniform("slope.normal_z", normal_z);

#if ENABLE_ENVIRONMENT_MAP
        unsigned int environment_texture_id  = GUI::wxGetApp().plater()->get_environment_texture_id();
        bool         use_environment_texture = environment_texture_id > 0 && GUI::wxGetApp().app_config->get("use_environment_map") == "1";
        shader->set_uniform("use_environment_tex", use_environment_texture);
        if (use_environment_texture)
            glsafe(::glBindTexture(GL_TEXTURE_2D, environment_texture_id));
#endif // ENABLE_ENVIRONMENT_MAP
        glcheck();

        const Transform3d model_matrix = volume.first->world_matrix();
        shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
        shader->set_uniform("projection_matrix", projection_matrix);
        const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) *
                                            model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
        shader->set_uniform("view_normal_matrix", view_normal_matrix);

        bool rendered_preview_gradient = false;
        const bool use_preview_gradient =
            volume.first->preview_gradient_colors.size() >= 2 &&
            volume.first->model.is_initialized() &&
            volume.first->visible &&
            volume.first->printable &&
            !volume.first->disabled &&
            !volume.first->is_modifier &&
            !volume.first->is_wipe_tower &&
            !volume.first->picking &&
            !m_use_color_clip_plane &&
            !volume_has_surface_segmentation(*volume.first);
        if (use_preview_gradient) {
            rendered_preview_gradient = render_preview_gradient_model(volume.first->model,
                                                                      volume.first->tverts_range,
                                                                      shader,
                                                                      volume.first->transformed_bounding_box(),
                                                                      m_z_range,
                                                                      volume.first->preview_gradient_colors,
                                                                      volume.first->preview_gradient_positions,
                                                                      volume.first->render_color.a());
            if (rendered_preview_gradient)
                shader->set_uniform("z_range", m_z_range);
        }

        if (!rendered_preview_gradient)
            volume.first->model.set_color(volume.first->render_color);

        const bool has_segmented_preview_gradient =
            !rendered_preview_gradient &&
            volume_has_surface_segmentation(*volume.first) &&
            !volume.first->preview_gradient_colors_by_extruder.empty() &&
            !volume.first->picking &&
            !m_use_color_clip_plane;

        // BBS: add outline related logic
        if (has_segmented_preview_gradient) {
            ModelObjectPtrs& model_objects = GUI::wxGetApp().model().objects;
            std::vector<ColorRGBA> extruder_colors = get_extruders_colors();
            volume.first->simple_render(shader, model_objects, extruder_colors, false, &m_z_range);
        } else if (!rendered_preview_gradient) {
            volume.first->render();
        }

#if ENABLE_ENVIRONMENT_MAP
        if (use_environment_texture)
            glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
#endif // ENABLE_ENVIRONMENT_MAP

        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
    }

    if (m_show_sinking_contours) {
        shader->stop_using();
        if (sink_shader != nullptr) {
            sink_shader->start_using();
            for (GLVolumeWithIdAndZ& volume : to_render) {
                // render sinking contours of hovered/displaced volumes
                if (volume.first->is_sinking() && !volume.first->is_below_printbed() &&
                    (volume.first->hover != GLVolume::HS_None || volume.first->force_sinking_contours)) {
                    glsafe(::glDepthFunc(GL_ALWAYS));
                    volume.first->render_sinking_contours();
                    glsafe(::glDepthFunc(GL_LESS));
                }
            }
            sink_shader->start_using();
        }
        shader->start_using();
    }

    if (disable_cullface)
        glsafe(::glEnable(GL_CULL_FACE));

    if (type == ERenderType::Transparent)
        glsafe(::glDisable(GL_BLEND));
}

bool GLVolumeCollection::check_outside_state(const BuildVolume& build_volume, ModelInstanceEPrintVolumeState* out_state) const
{
    if (GUI::wxGetApp().plater() == NULL || GUI::wxGetApp().is_recreating_gui()) {
        if (out_state != nullptr)
            *out_state = ModelInstancePVS_Inside;
        return false;
    }

    const Model& model        = GUI::wxGetApp().plater()->model();
    auto         volume_below = [](GLVolume& volume) -> bool {
        return volume.object_idx() != -1 && volume.volume_idx() != -1 && volume.is_below_printbed();
    };
    // Volume is partially below the print bed, thus a pre-calculated convex hull cannot be used.
    auto volume_sinking = [](GLVolume& volume) -> bool {
        return volume.object_idx() != -1 && volume.volume_idx() != -1 && volume.is_sinking();
    };
    // Cached bounding box of a volume above the print bed.
    auto volume_bbox = [volume_sinking](GLVolume& volume) -> BoundingBoxf3 {
        return volume_sinking(volume) ? volume.transformed_non_sinking_bounding_box() : volume.transformed_convex_hull_bounding_box();
    };
    // Cached 3D convex hull of a volume above the print bed.
    auto volume_convex_mesh = [volume_sinking, &model](GLVolume& volume) -> const TriangleMesh& {
        return volume_sinking(volume) ? model.objects[volume.object_idx()]->volumes[volume.volume_idx()]->mesh() : *volume.convex_hull();
    };

    ModelInstanceEPrintVolumeState overall_state     = ModelInstancePVS_Inside;
    bool                           contained_min_one = false;

    // BBS: add instance judge logic, besides to original volume judge logic
    std::map<int64_t, ModelInstanceEPrintVolumeState> model_state;

    GUI::PartPlate*                   curr_plate   = GUI::wxGetApp().plater()->get_partplate_list().get_selected_plate();
    const Pointfs&                    pp_bed_shape = curr_plate->get_shape();
    BuildVolume                       plate_build_volume(pp_bed_shape, build_volume.printable_height());
    const std::vector<BoundingBoxf3>& exclude_areas = curr_plate->get_exclude_areas();

    for (GLVolume* volume : this->volumes) {
        // Snapmaker: 初始化螺旋抬升边界状态（在循环开始时就清除所有标志）
        if (volume != nullptr)
            volume->near_boundary_for_spiral_lift = false;

        if (!volume->is_modifier &&
            (volume->shader_outside_printer_detection_enabled || (!volume->is_wipe_tower && volume->composite_id.volume_id >= 0))) {
            BuildVolume::ObjectState state;
            if (volume_below(*volume))
                state = BuildVolume::ObjectState::Below;
            else {
                switch (plate_build_volume.type()) {
                case BuildVolume_Type::Rectangle: {
                    // FIXME this test does not evaluate collision of a build volume bounding box with non-convex objects.
                    const BoundingBoxf3& bb = volume_bbox(*volume);
                    state                   = plate_build_volume.volume_state_bbox(bb);
                } break;
                case BuildVolume_Type::Circle:
                case BuildVolume_Type::Convex:
                // FIXME doing test on convex hull until we learn to do test on non-convex polygons efficiently.
                case BuildVolume_Type::Custom:
                    state = plate_build_volume.object_state(volume_convex_mesh(*volume).its, volume->world_matrix().cast<float>(),
                                                            volume_sinking(*volume));
                    break;
                default:
                    // Ignore, don't produce any collision.
                    state = BuildVolume::ObjectState::Inside;
                    break;
                }
                assert(state != BuildVolume::ObjectState::Below);
            }

            int64_t comp_id    = ((int64_t) volume->composite_id.object_id << 32) | ((int64_t) volume->composite_id.instance_id);
            volume->is_outside = state != BuildVolume::ObjectState::Inside;

            // Snapmaker: 检测模型是否距离床边界太近（螺旋抬升风险）
            // 只对矩形床进行检测（Snapmaker U1），只检测可打印的对象
            // 只检测完全在床内的对象（state == Inside），避免对跨越边界的对象误报
            if (plate_build_volume.type() == BuildVolume_Type::Rectangle && volume->composite_id.volume_id >= 0 &&
                state == BuildVolume::ObjectState::Inside && volume->printable) {
                constexpr double     SPIRAL_LIFT_SAFETY_MARGIN = 3.5; // mm
                const BoundingBoxf3& bb                        = volume_bbox(*volume);
                const BoundingBoxf3& bed_bb                    = plate_build_volume.bounding_volume();

                // 计算模型边界框与床边界的最小距离
                double dist_left   = std::abs(bb.min.x() - bed_bb.min.x());
                double dist_right  = std::abs(bed_bb.max.x() - bb.max.x());
                double dist_bottom = std::abs(bb.min.y() - bed_bb.min.y());
                double dist_top    = std::abs(bed_bb.max.y() - bb.max.y());

                double min_distance = std::min({dist_left, dist_right, dist_bottom, dist_top});
                // 如果最小距离小于安全余量，触发警告
                if (min_distance < SPIRAL_LIFT_SAFETY_MARGIN) {
                    volume->near_boundary_for_spiral_lift = true;
                }
            }

            // volume->partly_inside = (state == BuildVolume::ObjectState::Colliding);
            if (volume->printable) {
                if (overall_state == ModelInstancePVS_Inside && volume->is_outside) {
                    overall_state = ModelInstancePVS_Fully_Outside;
                }

                if (overall_state == ModelInstancePVS_Fully_Outside && volume->is_outside &&
                    (state == BuildVolume::ObjectState::Colliding)) {
                    overall_state = ModelInstancePVS_Partly_Outside;
                }
                contained_min_one |= !volume->is_outside;
            }

            ModelInstanceEPrintVolumeState volume_state;
            // if (volume->is_outside && (plate_build_volume.bounding_volume().intersects(volume->bounding_box())))
            if (volume->is_outside && (state == BuildVolume::ObjectState::Colliding))
                volume_state = ModelInstancePVS_Partly_Outside;
            else if (volume->is_outside)
                volume_state = ModelInstancePVS_Fully_Outside;
            else
                volume_state = ModelInstancePVS_Inside;

            if (model_state.find(comp_id) != model_state.end()) {
                if (model_state[comp_id] != ModelInstancePVS_Partly_Outside) {
                    if (volume_state == ModelInstancePVS_Partly_Outside)
                        model_state[comp_id] = ModelInstancePVS_Partly_Outside;
                    else if (model_state[comp_id] != volume_state) {
                        model_state[comp_id] = ModelInstancePVS_Partly_Outside;
                    }
                }
            } else {
                model_state[comp_id] = volume_state;
            }

            if (model_state[comp_id] == ModelInstancePVS_Partly_Outside) {
                overall_state = ModelInstancePVS_Partly_Outside;
                BOOST_LOG_TRIVIAL(debug) << "instance includes " << volume->name << " is partially outside of bed";
            }
        }
    }

    for (GLVolume* volume : this->volumes) {
        if (!volume->is_modifier &&
            (volume->shader_outside_printer_detection_enabled || (!volume->is_wipe_tower && volume->composite_id.volume_id >= 0))) {
            int64_t comp_id = ((int64_t) volume->composite_id.object_id << 32) | ((int64_t) volume->composite_id.instance_id);
            if (model_state.find(comp_id) != model_state.end()) {
                if (model_state[comp_id] == ModelInstancePVS_Partly_Outside) {
                    volume->partly_inside = true;
                } else
                    volume->partly_inside = false;
            }
        }
    }

    if (out_state != nullptr)
        *out_state = overall_state;

    return contained_min_one;
}

void GLVolumeCollection::reset_outside_state()
{
    for (GLVolume* volume : this->volumes) {
        if (volume != nullptr) {
            volume->is_outside                    = false;
            volume->partly_inside                 = false;
            volume->near_boundary_for_spiral_lift = false; // Snapmaker: 初始化螺旋抬升边界状态
        }
    }
}

// Snapmaker: 检查是否有任何 volume 靠近边界（螺旋抬升风险）
bool GLVolumeCollection::is_any_volume_near_boundary_for_spiral_lift() const
{
    for (const GLVolume* volume : this->volumes) {
        if (volume != nullptr && volume->near_boundary_for_spiral_lift)
            return true;
    }
    return false;
}

void GLVolumeCollection::update_colors_by_extruder(const DynamicPrintConfig* config, bool is_update_alpha)
{
    using ColorItem = std::pair<std::string, ColorRGBA>;
    std::vector<ColorItem> colors;
    std::vector<std::vector<ColorRGBA>> preview_gradient_colors;
    std::vector<std::vector<double>>    preview_gradient_positions;

    if (static_cast<PrinterTechnology>(config->opt_int("printer_technology")) == ptSLA) {
        const std::string& txt_color = config->opt_string("material_colour").empty() ?
                                           print_config_def.get("material_colour")->get_default_value<ConfigOptionString>()->value :
                                           config->opt_string("material_colour");
        ColorRGBA          rgba;
        if (decode_color(txt_color, rgba))
            colors.push_back({txt_color, rgba});
    } else {
        const ConfigOptionStrings* filamemts_opt = dynamic_cast<const ConfigOptionStrings*>(config->option("filament_colour"));
        if (filamemts_opt == nullptr)
            return;

        std::vector<std::string> filament_colors = filamemts_opt->values;
        if (filament_colors.empty())
            return;

        const std::vector<std::string> physical_filament_colors = filament_colors;
        const MixedFilamentDisplayContext gradient_display_context =
            GUI::build_mixed_filament_display_context(physical_filament_colors);
        preview_gradient_colors.resize(filament_colors.size());
        preview_gradient_positions.resize(filament_colors.size());

        // Include visible mixed (virtual) filament colors so volume extruder IDs
        // assigned to mixed rows render correctly in Prepare view.
        if (GUI::wxGetApp().preset_bundle != nullptr) {
            const MixedFilamentManager& mixed_mgr = GUI::wxGetApp().preset_bundle->mixed_filaments;
            const auto mixed_colors = mixed_mgr.display_colors();
            const auto mixed_definitions = mixed_mgr.mixed_filament_definitions(physical_filament_colors.size());
            size_t visible_mixed_idx = 0;
            for (const MixedFilamentDefinition& definition : mixed_definitions) {
                if (definition.visibility.tombstoned)
                    continue;
                const std::string display_color =
                    visible_mixed_idx < mixed_colors.size() ? mixed_colors[visible_mixed_idx] : definition.presentation.display_color;
                std::vector<ColorRGBA> gradient_colors =
                    mixed_filament_preview_gradient_colors(definition, gradient_display_context);
                filament_colors.emplace_back(display_color);
                // Engine-aware samples are uniformly spaced and already encode
                // the original stop curve, so no second position transform is needed.
                preview_gradient_positions.emplace_back();
                preview_gradient_colors.emplace_back(std::move(gradient_colors));
                ++visible_mixed_idx;
            }
        }
        if (preview_gradient_colors.size() < filament_colors.size())
            preview_gradient_colors.resize(filament_colors.size());
        if (preview_gradient_positions.size() < filament_colors.size())
            preview_gradient_positions.resize(filament_colors.size());

        colors.resize(filament_colors.size());

        for (size_t i = 0; i < filament_colors.size(); ++i) {
            ColorRGBA          rgba;
            const std::string& fil_color = filament_colors[i];
            if (decode_color(fil_color, rgba))
                colors[i] = {fil_color, rgba};
        }
    }

    for (GLVolume* volume : volumes) {
        if (volume == nullptr || volume->is_modifier || volume->is_wipe_tower || volume->volume_idx() < 0)
            continue;

        int extruder_id = volume->extruder_id - 1;
        if (extruder_id < 0 || (int) colors.size() <= extruder_id)
            extruder_id = 0;

        const ColorItem& color = colors[extruder_id];
        volume->preview_gradient_colors =
            extruder_id >= 0 && size_t(extruder_id) < preview_gradient_colors.size() ?
                preview_gradient_colors[size_t(extruder_id)] :
                std::vector<ColorRGBA>();
        volume->preview_gradient_positions =
            extruder_id >= 0 && size_t(extruder_id) < preview_gradient_positions.size() ?
                preview_gradient_positions[size_t(extruder_id)] :
                std::vector<double>();
        volume->preview_gradient_colors_by_extruder = preview_gradient_colors;
        volume->preview_gradient_positions_by_extruder = preview_gradient_positions;
        if (!color.first.empty()) {
            if (!is_update_alpha) {
                float old_a   = volume->color.a();
                volume->color = color.second;
                volume->color.a(old_a);
            } else {
                volume->color = color.second;
            }
        }
    }
}

void GLVolumeCollection::set_transparency(float alpha)
{
    for (GLVolume* volume : volumes) {
        if (volume == nullptr || volume->is_modifier || volume->is_wipe_tower || (volume->volume_idx() < 0))
            continue;

        volume->color.a(alpha);
    }
}

std::vector<double> GLVolumeCollection::get_current_print_zs(bool active_only) const
{
    // Collect layer top positions of all volumes.
    std::vector<double> print_zs;
    for (GLVolume* vol : this->volumes) {
        if (!active_only || vol->is_active)
            append(print_zs, vol->print_zs);
    }
    std::sort(print_zs.begin(), print_zs.end());

    // Replace intervals of layers with similar top positions with their average value.
    int n = int(print_zs.size());
    int k = 0;
    for (int i = 0; i < n;) {
        int      j    = i + 1;
        coordf_t zmax = print_zs[i] + EPSILON;
        for (; j < n && print_zs[j] <= zmax; ++j)
            ;
        print_zs[k++] = (j > i + 1) ? (0.5 * (print_zs[i] + print_zs[j - 1])) : print_zs[i];
        i             = j;
    }
    if (k < n)
        print_zs.erase(print_zs.begin() + k, print_zs.end());

    return print_zs;
}

size_t GLVolumeCollection::cpu_memory_used() const
{
    size_t memsize = sizeof(*this) + this->volumes.capacity() * sizeof(GLVolume);
    for (const GLVolume* volume : this->volumes)
        memsize += volume->cpu_memory_used();
    return memsize;
}

size_t GLVolumeCollection::gpu_memory_used() const
{
    size_t memsize = 0;
    for (const GLVolume* volume : this->volumes)
        memsize += volume->gpu_memory_used();
    return memsize;
}

std::string GLVolumeCollection::log_memory_info() const
{
    return " (GLVolumeCollection RAM: " + format_memsize_MB(this->cpu_memory_used()) +
           " GPU: " + format_memsize_MB(this->gpu_memory_used()) + " Both: " + format_memsize_MB(this->gpu_memory_used()) + ")";
}

std::optional<float> GLVolumeCollection::source_color_preview_progress() const
{
    float  progress_sum = 0.f;
    size_t pending_count = 0;
    for (const GLVolume *volume : volumes) {
        const std::optional<float> progress = volume->source_color_preview_progress();
        if (!progress)
            continue;
        progress_sum += *progress;
        ++pending_count;
    }
    return pending_count == 0 ? std::nullopt : std::optional<float>(progress_sum / float(pending_count));
}

static void thick_lines_to_geometry(const Lines&               lines,
                                    const std::vector<double>& widths,
                                    const std::vector<double>& heights,
                                    bool                       closed,
                                    double                     top_z,
                                    GUI::GLModel::Geometry&    geometry)
{
    assert(!lines.empty());
    if (lines.empty())
        return;

    enum Direction : unsigned char { Left, Right, Top, Bottom };

    // right, left, top, bottom
    std::array<int, 4> idx_prev    = {-1, -1, -1, -1};
    std::array<int, 4> idx_initial = {-1, -1, -1, -1};

    double bottom_z_prev = 0.0;
    Vec2d  b1_prev(Vec2d::Zero());
    Vec2d  v_prev(Vec2d::Zero());
    double len_prev         = 0.0;
    double width_initial    = 0.0;
    double bottom_z_initial = 0.0;

    // loop once more in case of closed loops
    const size_t lines_end = closed ? (lines.size() + 1) : lines.size();
    for (size_t ii = 0; ii < lines_end; ++ii) {
        const size_t i        = (ii == lines.size()) ? 0 : ii;
        const Line&  line     = lines[i];
        const double bottom_z = top_z - heights[i];
        const double middle_z = 0.5 * (top_z + bottom_z);
        const double width    = widths[i];

        const bool is_first   = (ii == 0);
        const bool is_last    = (ii == lines_end - 1);
        const bool is_closing = closed && is_last;

        const Vec2d  v   = unscale(line.vector()).normalized();
        const double len = unscale<double>(line.length());

        const Vec2d a  = unscale(line.a);
        const Vec2d b  = unscale(line.b);
        Vec2d       a1 = a;
        Vec2d       a2 = a;
        Vec2d       b1 = b;
        Vec2d       b2 = b;
        {
            const double dist = 0.5 * width; // scaled
            const double dx   = dist * v.x();
            const double dy   = dist * v.y();
            a1 += Vec2d(+dy, -dx);
            a2 += Vec2d(-dy, +dx);
            b1 += Vec2d(+dy, -dx);
            b2 += Vec2d(-dy, +dx);
        }

        // calculate new XY normals
        const Vec2d xy_right_normal = unscale(line.normal()).normalized();

        std::array<int, 4> idx_a    = {0, 0, 0, 0};
        std::array<int, 4> idx_b    = {0, 0, 0, 0};
        int                idx_last = int(geometry.vertices_count());

        const bool bottom_z_different = bottom_z_prev != bottom_z;
        bottom_z_prev                 = bottom_z;

        if (!is_first && bottom_z_different) {
            // Found a change of the layer thickness -> Add a cap at the end of the previous segment.
            geometry.add_triangle(idx_b[Bottom], idx_b[Left], idx_b[Top]);
            geometry.add_triangle(idx_b[Bottom], idx_b[Top], idx_b[Right]);
        }

        // Share top / bottom vertices if possible.
        if (is_first) {
            idx_a[Top] = idx_last++;
            geometry.add_vertex(Vec3f(a.x(), a.y(), top_z), Vec3f(0.0f, 0.0f, 1.0f));
        } else
            idx_a[Top] = idx_prev[Top];

        if (is_first || bottom_z_different) {
            // Start of the 1st line segment or a change of the layer thickness while maintaining the print_z.
            idx_a[Bottom] = idx_last++;
            geometry.add_vertex(Vec3f(a.x(), a.y(), bottom_z), Vec3f(0.0f, 0.0f, -1.0f));
            idx_a[Left] = idx_last++;
            geometry.add_vertex(Vec3f(a2.x(), a2.y(), middle_z), Vec3f(-xy_right_normal.x(), -xy_right_normal.y(), 0.0f));
            idx_a[Right] = idx_last++;
            geometry.add_vertex(Vec3f(a1.x(), a1.y(), middle_z), Vec3f(xy_right_normal.x(), xy_right_normal.y(), 0.0f));
        } else
            idx_a[Bottom] = idx_prev[Bottom];

        if (is_first) {
            // Start of the 1st line segment.
            width_initial    = width;
            bottom_z_initial = bottom_z;
            idx_initial      = idx_a;
        } else {
            // Continuing a previous segment.
            // Share left / right vertices if possible.
            const double v_dot = v_prev.dot(v);
            // To reduce gpu memory usage, we try to reuse vertices
            // To reduce the visual artifacts, due to averaged normals, we allow to reuse vertices only when any of two adjacent edges
            // is longer than a fixed threshold.
            // The following value is arbitrary, it comes from tests made on a bunch of models showing the visual artifacts
            const double len_threshold = 2.5;

            // Generate new vertices if the angle between adjacent edges is greater than 45 degrees or thresholds conditions are met
            const bool sharp = (v_dot < 0.707) || (len_prev > len_threshold) || (len > len_threshold);
            if (sharp) {
                if (!bottom_z_different) {
                    // Allocate new left / right points for the start of this segment as these points will receive their own normals to
                    // indicate a sharp turn.
                    idx_a[Right] = idx_last++;
                    geometry.add_vertex(Vec3f(a1.x(), a1.y(), middle_z), Vec3f(xy_right_normal.x(), xy_right_normal.y(), 0.0f));
                    idx_a[Left] = idx_last++;
                    geometry.add_vertex(Vec3f(a2.x(), a2.y(), middle_z), Vec3f(-xy_right_normal.x(), -xy_right_normal.y(), 0.0f));
                    if (cross2(v_prev, v) > 0.0) {
                        // Right turn. Fill in the right turn wedge.
                        geometry.add_triangle(idx_prev[Right], idx_a[Right], idx_prev[Top]);
                        geometry.add_triangle(idx_prev[Right], idx_prev[Bottom], idx_a[Right]);
                    } else {
                        // Left turn. Fill in the left turn wedge.
                        geometry.add_triangle(idx_prev[Left], idx_prev[Top], idx_a[Left]);
                        geometry.add_triangle(idx_prev[Left], idx_a[Left], idx_prev[Bottom]);
                    }
                }
            } else {
                if (!bottom_z_different) {
                    // The two successive segments are nearly collinear.
                    idx_a[Left]  = idx_prev[Left];
                    idx_a[Right] = idx_prev[Right];
                }
            }
            if (is_closing) {
                if (!sharp) {
                    if (!bottom_z_different) {
                        // Closing a loop with smooth transition. Unify the closing left / right vertices.
                        geometry.set_vertex(idx_initial[Left], geometry.extract_position_3(idx_prev[Left]),
                                            geometry.extract_normal_3(idx_prev[Left]));
                        geometry.set_vertex(idx_initial[Right], geometry.extract_position_3(idx_prev[Right]),
                                            geometry.extract_normal_3(idx_prev[Right]));
                        geometry.remove_vertex(geometry.vertices_count() - 1);
                        geometry.remove_vertex(geometry.vertices_count() - 1);
                        // Replace the left / right vertex indices to point to the start of the loop.
                        const size_t indices_count = geometry.indices_count();
                        for (size_t u = indices_count - 24; u < indices_count; ++u) {
                            const unsigned int id = geometry.extract_index(u);
                            if (id == (unsigned int) idx_prev[Left])
                                geometry.set_index(u, (unsigned int) idx_initial[Left]);
                            else if (id == (unsigned int) idx_prev[Right])
                                geometry.set_index(u, (unsigned int) idx_initial[Right]);
                        }
                    }
                }
                // This is the last iteration, only required to solve the transition.
                break;
            }
        }

        // Only new allocate top / bottom vertices, if not closing a loop.
        if (is_closing)
            idx_b[Top] = idx_initial[Top];
        else {
            idx_b[Top] = idx_last++;
            geometry.add_vertex(Vec3f(b.x(), b.y(), top_z), Vec3f(0.0f, 0.0f, 1.0f));
        }

        if (is_closing && width == width_initial && bottom_z == bottom_z_initial)
            idx_b[Bottom] = idx_initial[Bottom];
        else {
            idx_b[Bottom] = idx_last++;
            geometry.add_vertex(Vec3f(b.x(), b.y(), bottom_z), Vec3f(0.0f, 0.0f, -1.0f));
        }
        // Generate new vertices for the end of this line segment.
        idx_b[Left] = idx_last++;
        geometry.add_vertex(Vec3f(b2.x(), b2.y(), middle_z), Vec3f(-xy_right_normal.x(), -xy_right_normal.y(), 0.0f));
        idx_b[Right] = idx_last++;
        geometry.add_vertex(Vec3f(b1.x(), b1.y(), middle_z), Vec3f(xy_right_normal.x(), xy_right_normal.y(), 0.0f));

        idx_prev      = idx_b;
        bottom_z_prev = bottom_z;
        b1_prev       = b1;
        v_prev        = v;
        len_prev      = len;

        if (bottom_z_different && (closed || (!is_first && !is_last))) {
            // Found a change of the layer thickness -> Add a cap at the beginning of this segment.
            geometry.add_triangle(idx_a[Bottom], idx_a[Right], idx_a[Top]);
            geometry.add_triangle(idx_a[Bottom], idx_a[Top], idx_a[Left]);
        }

        if (!closed) {
            // Terminate open paths with caps.
            if (is_first) {
                geometry.add_triangle(idx_a[Bottom], idx_a[Right], idx_a[Top]);
                geometry.add_triangle(idx_a[Bottom], idx_a[Top], idx_a[Left]);
            }
            // We don't use 'else' because both cases are true if we have only one line.
            if (is_last) {
                geometry.add_triangle(idx_b[Bottom], idx_b[Left], idx_b[Top]);
                geometry.add_triangle(idx_b[Bottom], idx_b[Top], idx_b[Right]);
            }
        }

        // Add quads for a straight hollow tube-like segment.
        // bottom-right face
        geometry.add_triangle(idx_a[Bottom], idx_b[Bottom], idx_b[Right]);
        geometry.add_triangle(idx_a[Bottom], idx_b[Right], idx_a[Right]);
        // top-right face
        geometry.add_triangle(idx_a[Right], idx_b[Right], idx_b[Top]);
        geometry.add_triangle(idx_a[Right], idx_b[Top], idx_a[Top]);
        // top-left face
        geometry.add_triangle(idx_a[Top], idx_b[Top], idx_b[Left]);
        geometry.add_triangle(idx_a[Top], idx_b[Left], idx_a[Left]);
        // bottom-left face
        geometry.add_triangle(idx_a[Left], idx_b[Left], idx_b[Bottom]);
        geometry.add_triangle(idx_a[Left], idx_b[Bottom], idx_a[Bottom]);
    }
}

// caller is responsible for supplying NO lines with zero length
static void thick_lines_to_geometry(const Lines3&              lines,
                                    const std::vector<double>& widths,
                                    const std::vector<double>& heights,
                                    bool                       closed,
                                    GUI::GLModel::Geometry&    geometry)
{
    assert(!lines.empty());
    if (lines.empty())
        return;

    enum Direction : unsigned char { Left, Right, Top, Bottom };

    // left, right, top, bottom
    std::array<int, 4> idx_prev    = {-1, -1, -1, -1};
    std::array<int, 4> idx_initial = {-1, -1, -1, -1};

    double z_prev        = 0.0;
    double len_prev      = 0.0;
    Vec3d  n_right_prev  = Vec3d::Zero();
    Vec3d  n_top_prev    = Vec3d::Zero();
    Vec3d  unit_v_prev   = Vec3d::Zero();
    double width_initial = 0.0;

    // new vertices around the line endpoints
    // left, right, top, bottom
    std::array<Vec3d, 4> a = {Vec3d::Zero(), Vec3d::Zero(), Vec3d::Zero(), Vec3d::Zero()};
    std::array<Vec3d, 4> b = {Vec3d::Zero(), Vec3d::Zero(), Vec3d::Zero(), Vec3d::Zero()};

    // loop once more in case of closed loops
    const size_t lines_end = closed ? (lines.size() + 1) : lines.size();
    for (size_t ii = 0; ii < lines_end; ++ii) {
        const size_t i = (ii == lines.size()) ? 0 : ii;

        const Line3& line   = lines[i];
        const double height = heights[i];
        const double width  = widths[i];

        const Vec3d  unit_v = unscale(line.vector()).normalized();
        const double len    = unscale<double>(line.length());

        Vec3d n_top   = Vec3d::Zero();
        Vec3d n_right = Vec3d::Zero();

        if (line.a.x() == line.b.x() && line.a.y() == line.b.y()) {
            // vertical segment
            n_top   = Vec3d::UnitY();
            n_right = Vec3d::UnitX();
            if (line.a.z() < line.b.z())
                n_right = -n_right;
        } else {
            // horizontal segment
            n_right = unit_v.cross(Vec3d::UnitZ()).normalized();
            n_top   = n_right.cross(unit_v).normalized();
        }

        const Vec3d rl_displacement = 0.5 * width * n_right;
        const Vec3d tb_displacement = 0.5 * height * n_top;
        const Vec3d l_a             = unscale(line.a);
        const Vec3d l_b             = unscale(line.b);

        a[Right]  = l_a + rl_displacement;
        a[Left]   = l_a - rl_displacement;
        a[Top]    = l_a + tb_displacement;
        a[Bottom] = l_a - tb_displacement;
        b[Right]  = l_b + rl_displacement;
        b[Left]   = l_b - rl_displacement;
        b[Top]    = l_b + tb_displacement;
        b[Bottom] = l_b - tb_displacement;

        const Vec3d n_bottom = -n_top;
        const Vec3d n_left   = -n_right;

        std::array<int, 4> idx_a    = {0, 0, 0, 0};
        std::array<int, 4> idx_b    = {0, 0, 0, 0};
        int                idx_last = int(geometry.vertices_count());

        const bool z_different = (z_prev != l_a.z());
        z_prev                 = l_b.z();

        // Share top / bottom vertices if possible.
        if (ii == 0) {
            idx_a[Top] = idx_last++;
            geometry.add_vertex((Vec3f) a[Top].cast<float>(), (Vec3f) n_top.cast<float>());
        } else
            idx_a[Top] = idx_prev[Top];

        if (ii == 0 || z_different) {
            // Start of the 1st line segment or a change of the layer thickness while maintaining the print_z.
            idx_a[Bottom] = idx_last++;
            geometry.add_vertex((Vec3f) a[Bottom].cast<float>(), (Vec3f) n_bottom.cast<float>());
            idx_a[Left] = idx_last++;
            geometry.add_vertex((Vec3f) a[Left].cast<float>(), (Vec3f) n_left.cast<float>());
            idx_a[Right] = idx_last++;
            geometry.add_vertex((Vec3f) a[Right].cast<float>(), (Vec3f) n_right.cast<float>());
        } else
            idx_a[Bottom] = idx_prev[Bottom];

        if (ii == 0) {
            // Start of the 1st line segment.
            width_initial = width;
            idx_initial   = idx_a;
        } else {
            // Continuing a previous segment.
            // Share left / right vertices if possible.
            const double v_dot         = unit_v_prev.dot(unit_v);
            const bool   is_right_turn = n_top_prev.dot(unit_v_prev.cross(unit_v)) > 0.0;

            // To reduce gpu memory usage, we try to reuse vertices
            // To reduce the visual artifacts, due to averaged normals, we allow to reuse vertices only when any of two adjacent edges
            // is longer than a fixed threshold.
            // The following value is arbitrary, it comes from tests made on a bunch of models showing the visual artifacts
            const double len_threshold = 2.5;

            // Generate new vertices if the angle between adjacent edges is greater than 45 degrees or thresholds conditions are met
            const bool is_sharp = v_dot < 0.707 || len_prev > len_threshold || len > len_threshold;
            if (is_sharp) {
                // Allocate new left / right points for the start of this segment as these points will receive their own normals to indicate
                // a sharp turn.
                idx_a[Right] = idx_last++;
                geometry.add_vertex((Vec3f) a[Right].cast<float>(), (Vec3f) n_right.cast<float>());
                idx_a[Left] = idx_last++;
                geometry.add_vertex((Vec3f) a[Left].cast<float>(), (Vec3f) n_left.cast<float>());

                if (is_right_turn) {
                    // Right turn. Fill in the right turn wedge.
                    geometry.add_triangle(idx_prev[Right], idx_a[Right], idx_prev[Top]);
                    geometry.add_triangle(idx_prev[Right], idx_prev[Bottom], idx_a[Right]);
                } else {
                    // Left turn. Fill in the left turn wedge.
                    geometry.add_triangle(idx_prev[Left], idx_prev[Top], idx_a[Left]);
                    geometry.add_triangle(idx_prev[Left], idx_a[Left], idx_prev[Bottom]);
                }
            } else {
                // The two successive segments are nearly collinear.
                idx_a[Left]  = idx_prev[Left];
                idx_a[Right] = idx_prev[Right];
            }

            if (ii == lines.size()) {
                if (!is_sharp) {
                    // Closing a loop with smooth transition. Unify the closing left / right vertices.
                    geometry.set_vertex(idx_initial[Left], geometry.extract_position_3(idx_prev[Left]),
                                        geometry.extract_normal_3(idx_prev[Left]));
                    geometry.set_vertex(idx_initial[Right], geometry.extract_position_3(idx_prev[Right]),
                                        geometry.extract_normal_3(idx_prev[Right]));
                    geometry.remove_vertex(geometry.vertices_count() - 1);
                    geometry.remove_vertex(geometry.vertices_count() - 1);
                    // Replace the left / right vertex indices to point to the start of the loop.
                    const size_t indices_count = geometry.indices_count();
                    for (size_t u = indices_count - 24; u < indices_count; ++u) {
                        const unsigned int id = geometry.extract_index(u);
                        if (id == (unsigned int) idx_prev[Left])
                            geometry.set_index(u, (unsigned int) idx_initial[Left]);
                        else if (id == (unsigned int) idx_prev[Right])
                            geometry.set_index(u, (unsigned int) idx_initial[Right]);
                    }
                }

                // This is the last iteration, only required to solve the transition.
                break;
            }
        }

        // Only new allocate top / bottom vertices, if not closing a loop.
        if (closed && ii + 1 == lines.size())
            idx_b[Top] = idx_initial[Top];
        else {
            idx_b[Top] = idx_last++;
            geometry.add_vertex((Vec3f) b[Top].cast<float>(), (Vec3f) n_top.cast<float>());
        }

        if (closed && ii + 1 == lines.size() && width == width_initial)
            idx_b[Bottom] = idx_initial[Bottom];
        else {
            idx_b[Bottom] = idx_last++;
            geometry.add_vertex((Vec3f) b[Bottom].cast<float>(), (Vec3f) n_bottom.cast<float>());
        }

        // Generate new vertices for the end of this line segment.
        idx_b[Left] = idx_last++;
        geometry.add_vertex((Vec3f) b[Left].cast<float>(), (Vec3f) n_left.cast<float>());
        idx_b[Right] = idx_last++;
        geometry.add_vertex((Vec3f) b[Right].cast<float>(), (Vec3f) n_right.cast<float>());

        idx_prev     = idx_b;
        n_right_prev = n_right;
        n_top_prev   = n_top;
        unit_v_prev  = unit_v;
        len_prev     = len;

        if (!closed) {
            // Terminate open paths with caps.
            if (i == 0) {
                geometry.add_triangle(idx_a[Bottom], idx_a[Right], idx_a[Top]);
                geometry.add_triangle(idx_a[Bottom], idx_a[Top], idx_a[Left]);
            }

            // We don't use 'else' because both cases are true if we have only one line.
            if (i + 1 == lines.size()) {
                geometry.add_triangle(idx_b[Bottom], idx_b[Left], idx_b[Top]);
                geometry.add_triangle(idx_b[Bottom], idx_b[Top], idx_b[Right]);
            }
        }

        // Add quads for a straight hollow tube-like segment.
        // bottom-right face
        geometry.add_triangle(idx_a[Bottom], idx_b[Bottom], idx_b[Right]);
        geometry.add_triangle(idx_a[Bottom], idx_b[Right], idx_a[Right]);
        // top-right face
        geometry.add_triangle(idx_a[Right], idx_b[Right], idx_b[Top]);
        geometry.add_triangle(idx_a[Right], idx_b[Top], idx_a[Top]);
        // top-left face
        geometry.add_triangle(idx_a[Top], idx_b[Top], idx_b[Left]);
        geometry.add_triangle(idx_a[Top], idx_b[Left], idx_a[Left]);
        // bottom-left face
        geometry.add_triangle(idx_a[Left], idx_b[Left], idx_b[Bottom]);
        geometry.add_triangle(idx_a[Left], idx_b[Bottom], idx_a[Bottom]);
    }
}

void _3DScene::thick_lines_to_verts(const Lines&               lines,
                                    const std::vector<double>& widths,
                                    const std::vector<double>& heights,
                                    bool                       closed,
                                    double                     top_z,
                                    GUI::GLModel::Geometry&    geometry)
{
    thick_lines_to_geometry(lines, widths, heights, closed, top_z, geometry);
}

void _3DScene::thick_lines_to_verts(const Lines3&              lines,
                                    const std::vector<double>& widths,
                                    const std::vector<double>& heights,
                                    bool                       closed,
                                    GUI::GLModel::Geometry&    geometry)
{
    thick_lines_to_geometry(lines, widths, heights, closed, geometry);
}

// Fill in the qverts and tverts with quads and triangles for the extrusion_path.
void _3DScene::extrusionentity_to_verts(const ExtrusionPath&    extrusion_path,
                                        float                   print_z,
                                        const Point&            copy,
                                        GUI::GLModel::Geometry& geometry)
{
    Polyline polyline = extrusion_path.polyline;
    polyline.remove_duplicate_points();
    polyline.translate(copy);
    const Lines         lines = polyline.lines();
    std::vector<double> widths(lines.size(), extrusion_path.width);
    std::vector<double> heights(lines.size(), extrusion_path.height);
    thick_lines_to_verts(lines, widths, heights, false, print_z, geometry);
}

// Fill in the qverts and tverts with quads and triangles for the extrusion_loop.
void _3DScene::extrusionentity_to_verts(const ExtrusionLoop&    extrusion_loop,
                                        float                   print_z,
                                        const Point&            copy,
                                        GUI::GLModel::Geometry& geometry)
{
    Lines               lines;
    std::vector<double> widths;
    std::vector<double> heights;
    for (const ExtrusionPath& extrusion_path : extrusion_loop.paths) {
        Polyline polyline = extrusion_path.polyline;
        polyline.remove_duplicate_points();
        polyline.translate(copy);
        const Lines lines_this = polyline.lines();
        append(lines, lines_this);
        widths.insert(widths.end(), lines_this.size(), extrusion_path.width);
        heights.insert(heights.end(), lines_this.size(), extrusion_path.height);
    }
    thick_lines_to_verts(lines, widths, heights, true, print_z, geometry);
}

// Fill in the qverts and tverts with quads and triangles for the extrusion_multi_path.
void _3DScene::extrusionentity_to_verts(const ExtrusionMultiPath& extrusion_multi_path,
                                        float                     print_z,
                                        const Point&              copy,
                                        GUI::GLModel::Geometry&   geometry)
{
    Lines               lines;
    std::vector<double> widths;
    std::vector<double> heights;
    for (const ExtrusionPath& extrusion_path : extrusion_multi_path.paths) {
        Polyline polyline = extrusion_path.polyline;
        polyline.remove_duplicate_points();
        polyline.translate(copy);
        const Lines lines_this = polyline.lines();
        append(lines, lines_this);
        widths.insert(widths.end(), lines_this.size(), extrusion_path.width);
        heights.insert(heights.end(), lines_this.size(), extrusion_path.height);
    }
    thick_lines_to_verts(lines, widths, heights, false, print_z, geometry);
}

void _3DScene::extrusionentity_to_verts(const ExtrusionEntityCollection& extrusion_entity_collection,
                                        float                            print_z,
                                        const Point&                     copy,
                                        GUI::GLModel::Geometry&          geometry)
{
    for (const ExtrusionEntity* extrusion_entity : extrusion_entity_collection.entities)
        extrusionentity_to_verts(extrusion_entity, print_z, copy, geometry);
}

void _3DScene::extrusionentity_to_verts(const ExtrusionEntity*  extrusion_entity,
                                        float                   print_z,
                                        const Point&            copy,
                                        GUI::GLModel::Geometry& geometry)
{
    if (extrusion_entity != nullptr) {
        auto* extrusion_path = dynamic_cast<const ExtrusionPath*>(extrusion_entity);
        if (extrusion_path != nullptr)
            extrusionentity_to_verts(*extrusion_path, print_z, copy, geometry);
        else {
            auto* extrusion_loop = dynamic_cast<const ExtrusionLoop*>(extrusion_entity);
            if (extrusion_loop != nullptr)
                extrusionentity_to_verts(*extrusion_loop, print_z, copy, geometry);
            else {
                auto* extrusion_multi_path = dynamic_cast<const ExtrusionMultiPath*>(extrusion_entity);
                if (extrusion_multi_path != nullptr)
                    extrusionentity_to_verts(*extrusion_multi_path, print_z, copy, geometry);
                else {
                    auto* extrusion_entity_collection = dynamic_cast<const ExtrusionEntityCollection*>(extrusion_entity);
                    if (extrusion_entity_collection != nullptr)
                        extrusionentity_to_verts(*extrusion_entity_collection, print_z, copy, geometry);
                    else
                        throw Slic3r::RuntimeError("Unexpected extrusion_entity type in to_verts()");
                }
            }
        }
    }
}

} // namespace Slic3r
