#include "GLGizmoImageProjection.hpp"

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/BitmapCache.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/ObjColorDialog.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/SidebarFilamentMenu.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "libslic3r/Format/ImportedTexture.hpp"
#include "libslic3r/ImageMap/Sampling.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <boost/filesystem/path.hpp>

#include <GL/glew.h>
#include <wx/filedlg.h>
#include <wx/glcanvas.h>

namespace Slic3r::GUI {

namespace {

RGBA to_rgba(const ColorRGBA &color)
{
    return {color.r(), color.g(), color.b(), color.a()};
}

std::pair<Vec3d, Vec3d> projection_axes(const Vec3d &source_normal, const Vec3d &source_up, double rotation_degrees)
{
    const Vec3d normal = source_normal.normalized();
    Vec3d       up     = source_up - normal * source_up.dot(normal);
    if (!up.allFinite() || up.squaredNorm() <= 1e-12) {
        const Vec3d reference = std::abs(normal.z()) < 0.9 ? Vec3d::UnitZ() : Vec3d::UnitY();
        up = reference - normal * reference.dot(normal);
    }
    up.normalize();
    Vec3d right = up.cross(normal).normalized();
    up          = normal.cross(right).normalized();
    const double angle         = rotation_degrees * PI / 180.0;
    const Vec3d  rotated_right = right * std::cos(angle) + up * std::sin(angle);
    const Vec3d  rotated_up    = up * std::cos(angle) - right * std::sin(angle);
    return {rotated_right, rotated_up};
}

} // namespace

GLGizmoImageProjection::GLGizmoImageProjection(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{
}

bool GLGizmoImageProjection::on_init()
{
    m_grabbers.resize(GrabberCount);
    m_grabbers[MoveGrabber].extensions = EGrabberExtension(int(EGrabberExtension::PosX) | int(EGrabberExtension::PosY));
    m_grabbers[WidthGrabber].extensions  = EGrabberExtension::PosX;
    m_grabbers[HeightGrabber].extensions = EGrabberExtension::PosY;
    set_grabbers_enabled(false);
    return true;
}

std::string GLGizmoImageProjection::on_get_name() const
{
    return _u8L("Project image");
}

bool GLGizmoImageProjection::on_is_selectable() const
{
    return wxGetApp().preset_bundle != nullptr &&
           wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF;
}

GLVolume *GLGizmoImageProjection::selected_gl_volume() const
{
    const Selection              &selection = m_parent.get_selection();
    const Selection::IndicesList &indices   = selection.get_volume_idxs();
    if (indices.size() != 1)
        return nullptr;
    GLVolume *gl_volume = const_cast<GLVolume *>(selection.get_volume(*indices.begin()));
    if (gl_volume == nullptr || gl_volume->object_idx() < 0 || gl_volume->volume_idx() < 0)
        return nullptr;
    const ModelObjectPtrs &objects = wxGetApp().model().objects;
    if (size_t(gl_volume->object_idx()) >= objects.size() || objects[size_t(gl_volume->object_idx())] == nullptr ||
        size_t(gl_volume->volume_idx()) >= objects[size_t(gl_volume->object_idx())]->volumes.size())
        return nullptr;
    const ModelVolume *model_volume = objects[size_t(gl_volume->object_idx())]->volumes[size_t(gl_volume->volume_idx())];
    return model_volume != nullptr && model_volume->is_model_part() ? gl_volume : nullptr;
}

bool GLGizmoImageProjection::on_is_activable() const
{
    return selected_gl_volume() != nullptr;
}

std::string GLGizmoImageProjection::get_tooltip() const
{
    switch (m_hover_id) {
    case MoveGrabber: return _u8L("Move projection");
    case WidthGrabber: return _u8L("Adjust projection width");
    case HeightGrabber: return _u8L("Adjust projection height");
    case ScaleGrabber: return _u8L("Scale projection");
    case RotateGrabber: return _u8L("Rotate projection");
    default: return {};
    }
}

void GLGizmoImageProjection::on_set_state()
{
    if (m_state == Off) {
        m_placed      = false;
        m_object_idx  = -1;
        m_volume_idx  = -1;
        m_frame.reset();
        m_preview_quad.reset();
        m_preview_texture.reset();
        m_preview_texture_dirty = false;
        m_surface_pointer_down   = false;
        m_drag_state             = DragState{};
        set_grabbers_enabled(false);
        m_frame_dirty = true;
    }
}

void GLGizmoImageProjection::data_changed(bool)
{
    GLVolume *volume = selected_gl_volume();
    if (volume == nullptr || (m_placed && (volume->object_idx() != m_object_idx || volume->volume_idx() != m_volume_idx))) {
        m_placed      = false;
        m_object_idx  = -1;
        m_volume_idx  = -1;
        m_surface_pointer_down = false;
        set_grabbers_enabled(false);
        m_frame_dirty = true;
    }
}

bool GLGizmoImageProjection::choose_image()
{
    wxFileDialog dialog((wxWindow *) wxGetApp().mainframe, _L("Choose an image to project"), wxEmptyString, wxEmptyString,
                        _L("Image files (*.png;*.jpg;*.jpeg)|*.png;*.jpg;*.jpeg|PNG files (*.png)|*.png|JPEG files "
                           "(*.jpg;*.jpeg)|*.jpg;*.jpeg"),
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return false;

    std::vector<uint8_t> rgba;
    uint32_t             width  = 0;
    uint32_t             height = 0;
    const std::string    path   = into_u8(dialog.GetPath());
    if (!decode_imported_texture_rgba_from_file(path, rgba, width, height)) {
        MessageDialog error((wxWindow *) wxGetApp().mainframe, _L("The selected PNG or JPEG could not be decoded."),
                            _L("Project image"), wxOK | wxICON_ERROR);
        error.ShowModal();
        return false;
    }

    m_image_path   = path;
    m_rgba         = std::move(rgba);
    m_image_width  = width;
    m_image_height = height;
    m_palette.clear();
    reset_projection_controls();
    m_placed                = false;
    m_surface_pointer_down  = false;
    m_object_idx            = -1;
    m_volume_idx            = -1;
    set_grabbers_enabled(false);
    m_preview_texture.reset();
    m_preview_texture_dirty = true;
    mark_frame_dirty();
    return true;
}

bool GLGizmoImageProjection::configure_image_mapping(const std::string &image_path,
                                                      const std::vector<uint8_t> &rgba,
                                                      uint32_t width,
                                                      uint32_t height)
{
    const size_t pixel_count = size_t(width) * size_t(height);
    if (pixel_count == 0 || rgba.size() != pixel_count * 4)
        return false;

    constexpr size_t max_dialog_samples = 65'536;
    const size_t     sample_count       = std::min(pixel_count, max_dialog_samples);
    std::vector<RGBA> input_colors;
    input_colors.reserve(sample_count);
    for (size_t sample_index = 0; sample_index < sample_count; ++sample_index) {
        const size_t pixel_index = sample_index * pixel_count / sample_count;
        const size_t offset      = pixel_index * 4;
        if (rgba[offset + 3] <= 5)
            continue;
        input_colors.push_back({float(rgba[offset]) / 255.f, float(rgba[offset + 1]) / 255.f,
                                float(rgba[offset + 2]) / 255.f, float(rgba[offset + 3]) / 255.f});
    }
    if (input_colors.empty()) {
        for (size_t pixel_index = 0; pixel_index < pixel_count; ++pixel_index) {
            const size_t offset = pixel_index * 4;
            if (rgba[offset + 3] <= 5)
                continue;
            input_colors.push_back({float(rgba[offset]) / 255.f, float(rgba[offset + 1]) / 255.f,
                                    float(rgba[offset + 2]) / 255.f, float(rgba[offset + 3]) / 255.f});
            break;
        }
    }
    if (input_colors.empty()) {
        MessageDialog error((wxWindow *) wxGetApp().mainframe, _L("The selected image is fully transparent."), _L("Project image"),
                            wxOK | wxICON_ERROR);
        error.ShowModal();
        return false;
    }

    ObjColorImportContext context;
    context.source                              = ObjColorImportSource::ImageTexture;
    context.mode                                = ObjColorImportMode::ImageMap;
    context.detected_texture_available          = true;
    context.image_map_render_mode               = m_render_mode == ImageMap::RenderMode::PerimeterModulationV2 ?
                                                      ObjImageMapRenderMode::PerimeterModulationV2 :
                                                  m_render_mode == ImageMap::RenderMode::AdaptiveLocalizedCycles ?
                                                      ObjImageMapRenderMode::AdaptiveLocalizedCycles :
                                                      ObjImageMapRenderMode::NormalMix;
    context.image_map_adaptive_modulation_mode  = m_adaptive_modulation_mode == ImageMap::AdaptiveModulationMode::LocalZHeight ?
                                                      ObjAdaptiveModulationMode::LocalZHeight :
                                                      ObjAdaptiveModulationMode::Perimeter;
    context.image_map_color_mix_model = m_color_mix_model == ImageMap::ColorMixModel::FilamentMixer ?
                                            ObjImageMapColorMixModel::FilamentMixer :
                                            ObjImageMapColorMixModel::FullSpectrumKmKs;
    context.image_map_minimum_component_percent = m_minimum_component_percent;
    context.image_map_synchronize_whole_object_cadence = m_synchronize_whole_object_cadence;
    context.image_map_modulation_sample_spacing_mm      = m_modulation_sample_spacing_mm;
    context.image_map_disable_broad_path_smoothing      = m_disable_broad_path_smoothing;
    context.image_map_gaussian_smoothing_strength       = m_gaussian_smoothing_strength;
    context.image_map_first_path_smoothing_strength     = m_first_path_smoothing_strength;
    context.image_map_second_path_smoothing_strength    = m_second_path_smoothing_strength;
    context.image_map_tone_gamma                        = m_tone_gamma;
    context.image_map_overhang_contrast_percent         = m_overhang_contrast_percent;
    context.image_map_exposure_ev                       = m_image_exposure_ev;
    context.image_map_contrast_percent                  = m_image_contrast_percent;
    context.image_map_saturation_percent                = m_image_saturation_percent;
    context.image_map_edge_boost_percent                = m_image_edge_boost_percent;

    const std::vector<RGBA> representatives = ImageMap::representative_source_colors(input_colors, 2, max_dialog_samples);
    const bool              is_single_color = representatives.size() <= 1;
    std::vector<unsigned char> filament_ids;
    unsigned char              first_extruder_id = 1;
    if (GLVolume *gl_volume = selected_gl_volume()) {
        if (ModelVolume *volume = get_model_volume(*gl_volume, wxGetApp().model().objects))
            first_extruder_id = static_cast<unsigned char>(std::clamp(volume->extruder_id(), 1, 255));
    }
    const std::vector<std::string> extruder_colors =
        wxGetApp().plater()->get_extruder_colors_from_plater_config(nullptr, false);
    const std::string filename = boost::filesystem::path(image_path).filename().string();
    ObjColorDialog dialog(nullptr, input_colors, is_single_color, context, extruder_colors, filament_ids, first_extruder_id, filename,
                          _L("Project Image"));
    if (dialog.ShowModal() != wxID_OK)
        return false;

    m_render_mode = context.image_map_render_mode == ObjImageMapRenderMode::PerimeterModulationV2 ?
                        ImageMap::RenderMode::PerimeterModulationV2 :
                    context.image_map_render_mode == ObjImageMapRenderMode::AdaptiveLocalizedCycles ?
                        ImageMap::RenderMode::AdaptiveLocalizedCycles :
                        ImageMap::RenderMode::NormalMix;
    m_adaptive_modulation_mode = context.image_map_adaptive_modulation_mode == ObjAdaptiveModulationMode::LocalZHeight ?
                                     ImageMap::AdaptiveModulationMode::LocalZHeight :
                                     ImageMap::AdaptiveModulationMode::Perimeter;
    m_color_mix_model = context.image_map_color_mix_model == ObjImageMapColorMixModel::FilamentMixer ?
                            ImageMap::ColorMixModel::FilamentMixer :
                            ImageMap::ColorMixModel::FullSpectrumKmKs;
    m_minimum_component_percent = context.image_map_minimum_component_percent;
    m_synchronize_whole_object_cadence = context.image_map_synchronize_whole_object_cadence;
    m_modulation_sample_spacing_mm      = context.image_map_modulation_sample_spacing_mm;
    m_disable_broad_path_smoothing      = context.image_map_disable_broad_path_smoothing;
    m_gaussian_smoothing_strength       = context.image_map_gaussian_smoothing_strength;
    m_first_path_smoothing_strength     = context.image_map_first_path_smoothing_strength;
    m_second_path_smoothing_strength    = context.image_map_second_path_smoothing_strength;
    m_tone_gamma                        = context.image_map_tone_gamma;
    m_overhang_contrast_percent         = context.image_map_overhang_contrast_percent;
    m_image_exposure_ev                 = context.image_map_exposure_ev;
    m_image_contrast_percent            = context.image_map_contrast_percent;
    m_image_saturation_percent          = context.image_map_saturation_percent;
    m_image_edge_boost_percent          = context.image_map_edge_boost_percent;
    m_palette.clear();
    const size_t palette_size = std::min(context.image_map_palette_colors.size(), context.image_map_palette_filament_ids.size());
    m_palette.reserve(palette_size);
    for (size_t palette_index = 0; palette_index < palette_size; ++palette_index) {
        const uint64_t stable_id = palette_index < context.image_map_palette_mixed_stable_ids.size() ?
                                       context.image_map_palette_mixed_stable_ids[palette_index] :
                                       0;
        m_palette.push_back({context.image_map_palette_colors[palette_index], stable_id,
                             unsigned(context.image_map_palette_filament_ids[palette_index])});
    }
    return !m_palette.empty();
}

void GLGizmoImageProjection::reset_projection_controls()
{
    m_width_mm              = 30.f;
    m_height_mm             = m_image_width > 0 ? 30.f * float(m_image_height) / float(m_image_width) : 30.f;
    m_lock_aspect           = true;
    m_flip_horizontal       = false;
    m_flip_vertical         = false;
    m_offset_x_mm           = 0.f;
    m_offset_y_mm           = 0.f;
    m_rotation_degrees      = 0.f;
    m_depth_mm              = 10.f;
    m_surface_angle_degrees = 75.f;
}

bool GLGizmoImageProjection::place_from_mouse(const Vec2d &mouse_pos)
{
    GLVolume *volume = selected_gl_volume();
    if (volume == nullptr || volume->mesh_raycaster == nullptr || m_rgba.empty())
        return false;

    Vec3f  hit;
    Vec3f  normal;
    size_t facet_idx = 0;
    const Transform3d world_matrix = volume->world_matrix();
    if (!volume->mesh_raycaster->unproject_on_mesh(mouse_pos, world_matrix, wxGetApp().plater()->get_camera(), hit, normal, nullptr,
                                                    &facet_idx))
        return false;

    m_center        = hit.cast<double>();
    m_normal        = normal.cast<double>().normalized();
    m_up            = world_matrix.linear().inverse() * Vec3d::UnitZ();
    m_offset_x_mm   = 0.f;
    m_offset_y_mm   = 0.f;
    m_seed_triangle = uint32_t(facet_idx);
    m_object_idx    = volume->object_idx();
    m_volume_idx    = volume->volume_idx();
    m_placed        = true;
    set_grabbers_enabled(true);
    mark_frame_dirty();
    return true;
}

Vec3d GLGizmoImageProjection::projection_center() const
{
    const auto [right, up] = projection_axes(m_normal, m_up, m_rotation_degrees);
    return m_center + right * double(m_offset_x_mm) + up * double(m_offset_y_mm);
}

bool GLGizmoImageProjection::on_mouse(const wxMouseEvent &mouse_event)
{
    if (m_rgba.empty()) {
        m_surface_pointer_down = false;
        return false;
    }

    // A surface placement gesture belongs entirely to this gizmo. In
    // particular, keep consuming its drag events so GLCanvas3D does not turn
    // the same mouse motion into camera orbiting.
    if (m_surface_pointer_down) {
        if (mouse_event.Dragging() && mouse_event.LeftIsDown()) {
            place_from_mouse(Vec2d(double(mouse_event.GetX()), double(mouse_event.GetY())));
            return true;
        }

        const bool gesture_finished = mouse_event.LeftUp() || mouse_event.Leaving() || !mouse_event.LeftIsDown();
        if (gesture_finished)
            m_surface_pointer_down = false;
        return mouse_event.LeftUp() || mouse_event.Leaving();
    }

    if (m_placed && use_grabbers(mouse_event))
        return true;
    if (!mouse_event.LeftDown() || mouse_event.CmdDown() || mouse_event.AltDown())
        return false;

    if (!place_from_mouse(Vec2d(double(mouse_event.GetX()), double(mouse_event.GetY()))))
        return false;

    m_surface_pointer_down = true;
    return true;
}

bool GLGizmoImageProjection::mouse_position_on_projection_plane(const Linef3 &mouse_ray,
                                                                 const DragState &drag_state,
                                                                 Vec3d &local_hit) const
{
    const Vec3d world_point  = drag_state.world_matrix * drag_state.projection_center;
    const Vec3d world_right  = drag_state.world_matrix.linear() * drag_state.right;
    const Vec3d world_up     = drag_state.world_matrix.linear() * drag_state.up;
    Vec3d       world_normal = world_right.cross(world_up);
    if (!world_normal.allFinite() || world_normal.squaredNorm() <= 1e-20)
        return false;
    world_normal.normalize();

    const Vec3d ray_direction = mouse_ray.unit_vector();
    const double denominator  = world_normal.dot(ray_direction);
    if (!std::isfinite(denominator) || std::abs(denominator) <= 1e-6)
        return false;
    const double distance = world_normal.dot(world_point - mouse_ray.a) / denominator;
    const Vec3d  world_hit = mouse_ray.a + distance * ray_direction;
    local_hit = drag_state.world_matrix.inverse() * world_hit;
    return local_hit.allFinite();
}

void GLGizmoImageProjection::on_start_dragging()
{
    GLVolume *volume = selected_gl_volume();
    if (!m_placed || volume == nullptr || m_hover_id < MoveGrabber || m_hover_id >= GrabberCount)
        return;

    const auto [right, up]          = projection_axes(m_normal, m_up, m_rotation_degrees);
    m_drag_state.valid              = false;
    m_drag_state.offset_x_mm        = m_offset_x_mm;
    m_drag_state.offset_y_mm        = m_offset_y_mm;
    m_drag_state.width_mm           = m_width_mm;
    m_drag_state.height_mm          = m_height_mm;
    m_drag_state.rotation_degrees   = m_rotation_degrees;
    m_drag_state.projection_center  = projection_center();
    m_drag_state.right              = right;
    m_drag_state.up                 = up;
    m_drag_state.normal             = right.cross(up).normalized();
    m_drag_state.world_matrix       = volume->world_matrix();

    wxPoint mouse_position = m_parent.get_wxglcanvas()->ScreenToClient(wxGetMousePosition());
    const Linef3 mouse_ray = m_parent.mouse_ray(Point(mouse_position.x, mouse_position.y));
    m_drag_state.valid = mouse_position_on_projection_plane(mouse_ray, m_drag_state, m_drag_state.mouse_hit);
}

void GLGizmoImageProjection::on_dragging(const UpdateData &data)
{
    if (!m_drag_state.valid)
        return;

    Vec3d current_hit;
    if (!mouse_position_on_projection_plane(data.mouse_ray, m_drag_state, current_hit))
        return;

    const Vec3d delta = current_hit - m_drag_state.mouse_hit;
    switch (m_hover_id) {
    case MoveGrabber:
        m_offset_x_mm = m_drag_state.offset_x_mm + float(delta.dot(m_drag_state.right));
        m_offset_y_mm = m_drag_state.offset_y_mm + float(delta.dot(m_drag_state.up));
        break;
    case WidthGrabber: {
        const float width = std::max(0.1f, m_drag_state.width_mm + 2.f * float(delta.dot(m_drag_state.right)));
        m_width_mm        = width;
        break;
    }
    case HeightGrabber: {
        const float height = std::max(0.1f, m_drag_state.height_mm + 2.f * float(delta.dot(m_drag_state.up)));
        m_height_mm        = height;
        break;
    }
    case ScaleGrabber: {
        const Vec3d start = m_drag_state.mouse_hit - m_drag_state.projection_center;
        const Vec3d now   = current_hit - m_drag_state.projection_center;
        const double start_radius = std::hypot(start.dot(m_drag_state.right), start.dot(m_drag_state.up));
        const double now_radius   = std::hypot(now.dot(m_drag_state.right), now.dot(m_drag_state.up));
        if (start_radius > 1e-6 && std::isfinite(now_radius)) {
            const float ratio = float(std::max(0.01, now_radius / start_radius));
            m_width_mm        = std::max(0.1f, m_drag_state.width_mm * ratio);
            m_height_mm       = std::max(0.1f, m_drag_state.height_mm * ratio);
        }
        break;
    }
    case RotateGrabber: {
        Vec3d start = m_drag_state.mouse_hit - m_drag_state.projection_center;
        Vec3d now   = current_hit - m_drag_state.projection_center;
        start -= m_drag_state.normal * start.dot(m_drag_state.normal);
        now   -= m_drag_state.normal * now.dot(m_drag_state.normal);
        if (start.squaredNorm() > 1e-12 && now.squaredNorm() > 1e-12) {
            start.normalize();
            now.normalize();
            const double angle = std::atan2(m_drag_state.normal.dot(start.cross(now)), start.dot(now));
            m_rotation_degrees = m_drag_state.rotation_degrees + float(angle * 180.0 / PI);
        }
        break;
    }
    default: return;
    }

    while (m_rotation_degrees > 180.f)
        m_rotation_degrees -= 360.f;
    while (m_rotation_degrees < -180.f)
        m_rotation_degrees += 360.f;
    mark_frame_dirty();
}

void GLGizmoImageProjection::on_stop_dragging()
{
    m_drag_state = DragState{};
    mark_frame_dirty();
}

void GLGizmoImageProjection::set_grabbers_enabled(bool enabled)
{
    for (Grabber &grabber : m_grabbers)
        grabber.enabled = enabled;
}

void GLGizmoImageProjection::update_grabbers(const GLVolume &volume)
{
    if (!m_placed) {
        set_grabbers_enabled(false);
        return;
    }

    const auto [right, up] = projection_axes(m_normal, m_up, m_rotation_degrees);
    Transform3d plane      = Transform3d::Identity();
    plane.linear().col(0)  = right;
    plane.linear().col(1)  = up;
    plane.linear().col(2)  = right.cross(up).normalized();
    plane.translation()    = projection_center();
    const Transform3d grabber_matrix = volume.world_matrix() * plane;
    for (Grabber &grabber : m_grabbers) {
        grabber.enabled = true;
        grabber.matrix  = grabber_matrix;
    }

    const double half_width  = 0.5 * double(m_width_mm);
    const double half_height = 0.5 * double(m_height_mm);
    const double fixed_grabber_world_size = double(Grabber::FixedGrabberSize * INV_ZOOM);
    const double rotate_gap = std::max({3.0, 0.15 * double(m_height_mm), 0.75 * fixed_grabber_world_size});
    // Keep rotation away from the height handle's long +Y cone. The minimum
    // screen-space separation also prevents overlap on very narrow images.
    const double rotate_lateral_offset = std::max(half_width, 2.5 * fixed_grabber_world_size);
    m_grabbers[MoveGrabber].center   = Vec3d::Zero();
    m_grabbers[WidthGrabber].center  = Vec3d(half_width, 0.0, 0.0);
    m_grabbers[HeightGrabber].center = Vec3d(0.0, half_height, 0.0);
    m_grabbers[ScaleGrabber].center  = Vec3d(half_width, half_height, 0.0);
    m_grabbers[RotateGrabber].center = Vec3d(-rotate_lateral_offset, half_height + rotate_gap, 0.0);

    m_grabbers[MoveGrabber].color        = ColorRGBA(0.f, 0.72f, 0.64f, 1.f);
    m_grabbers[MoveGrabber].hover_color  = ColorRGBA(0.2f, 1.f, 0.9f, 1.f);
    m_grabbers[WidthGrabber].color       = AXES_COLOR[0];
    m_grabbers[WidthGrabber].hover_color = AXES_HOVER_COLOR[0];
    m_grabbers[HeightGrabber].color      = AXES_COLOR[1];
    m_grabbers[HeightGrabber].hover_color = AXES_HOVER_COLOR[1];
    m_grabbers[ScaleGrabber].color        = GRABBER_UNIFORM_COL;
    m_grabbers[ScaleGrabber].hover_color  = GRABBER_UNIFORM_HOVER_COL;
    m_grabbers[RotateGrabber].color       = AXES_COLOR[2];
    m_grabbers[RotateGrabber].hover_color = AXES_HOVER_COLOR[2];
}

void GLGizmoImageProjection::update_rotation_grabber_picker()
{
    Grabber &grabber = m_grabbers[RotateGrabber];
    if (!grabber.enabled || grabber.picking_id < 0)
        return;

    PickingModel &cube = grabber.get_cube();
    if (cube.mesh_raycaster == nullptr)
        return;

    const double picker_size = 1.5 * double(Grabber::FixedGrabberSize * INV_ZOOM);
    const Transform3d picker_matrix =
        grabber.matrix * Geometry::assemble_transform(grabber.center, grabber.angles, picker_size * Vec3d::Ones());
    if (grabber.raycasters[0] == nullptr) {
        grabber.raycasters[0] =
            m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, grabber.picking_id, *cube.mesh_raycaster, picker_matrix);
    } else {
        grabber.raycasters[0]->set_transform(picker_matrix);
    }
}

void GLGizmoImageProjection::init_rotation_arrows()
{
    if (m_rotation_arrows.is_initialized())
        return;

    GLModel::Geometry geometry;
    geometry.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3};
    geometry.color  = AXES_COLOR[2];

    constexpr int    segments     = 14;
    constexpr double inner_radius = 0.68;
    constexpr double outer_radius = 0.90;
    constexpr double head_radius  = 1.18;
    auto add_arc = [&](double start_degrees, double end_degrees) {
        const unsigned int first_vertex = unsigned(geometry.vertices_count());
        for (int segment = 0; segment <= segments; ++segment) {
            const double t     = double(segment) / double(segments);
            const double angle = (start_degrees + t * (end_degrees - start_degrees)) * PI / 180.0;
            const Vec3f  radial(float(std::cos(angle)), float(std::sin(angle)), 0.f);
            geometry.add_vertex(Vec3f(float(inner_radius) * radial));
            geometry.add_vertex(Vec3f(float(outer_radius) * radial));
        }
        for (int segment = 0; segment < segments; ++segment) {
            const unsigned int inner0 = first_vertex + unsigned(2 * segment);
            const unsigned int outer0 = inner0 + 1;
            const unsigned int inner1 = inner0 + 2;
            const unsigned int outer1 = inner0 + 3;
            geometry.add_triangle(inner0, outer0, outer1);
            geometry.add_triangle(inner0, outer1, inner1);
        }

        const double end_angle = end_degrees * PI / 180.0;
        const Vec3f  radial(float(std::cos(end_angle)), float(std::sin(end_angle)), 0.f);
        const Vec3f  tangent(-radial.y(), radial.x(), 0.f);
        const Vec3f  tip = float(head_radius) * radial + 0.12f * tangent;
        const unsigned int head = unsigned(geometry.vertices_count());
        geometry.add_vertex(tip);
        geometry.add_vertex(Vec3f(float(inner_radius - 0.12) * radial - 0.30f * tangent));
        geometry.add_vertex(Vec3f(float(outer_radius + 0.12) * radial - 0.30f * tangent));
        geometry.add_triangle(head, head + 1, head + 2);
    };

    // Two arrowed portions of one circular motion read cleanly at gizmo size
    // while leaving the center open as the rotation drag target.
    add_arc(18.0, 152.0);
    add_arc(198.0, 332.0);
    m_rotation_arrows.init_from(std::move(geometry));
}

void GLGizmoImageProjection::render_rotation_arrows(const Camera &camera)
{
    init_rotation_arrows();
    if (!m_rotation_arrows.is_initialized())
        return;

    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const Grabber &grabber = m_grabbers[RotateGrabber];
    const double radius = 0.78 * double(Grabber::FixedGrabberSize * INV_ZOOM);
    const Transform3d icon_matrix =
        grabber.matrix * Geometry::assemble_transform(grabber.center + Vec3d(0.0, 0.0, 0.12), grabber.angles, radius * Vec3d::Ones());
    m_rotation_arrows.set_color(m_hover_id == RotateGrabber ? grabber.hover_color : grabber.color);

    const bool depth_test_enabled = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * icon_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    glsafe(::glDisable(GL_DEPTH_TEST));
    m_rotation_arrows.render();
    if (depth_test_enabled)
        glsafe(::glEnable(GL_DEPTH_TEST));
    shader->stop_using();
}

void GLGizmoImageProjection::mark_frame_dirty()
{
    m_frame_dirty = true;
    m_parent.set_as_dirty();
}

void GLGizmoImageProjection::rebuild_frame()
{
    m_frame.reset();
    m_preview_quad.reset();
    m_frame_dirty = false;
    if (!m_placed)
        return;

    const Vec3d normal          = m_normal.normalized();
    const auto [right, up] = projection_axes(m_normal, m_up, m_rotation_degrees);
    const Vec3d center          = projection_center();

    const Vec3d preview_offset = normal * 0.06;
    const Vec3d offset         = normal * 0.09;
    const Vec3d rx     = right * (0.5 * double(m_width_mm));
    const Vec3d uy     = up * (0.5 * double(m_height_mm));
    const std::array<Vec3d, 4> corners = {center - rx - uy + offset, center + rx - uy + offset,
                                           center + rx + uy + offset, center - rx + uy + offset};

    std::array<Vec2f, 4> texture_coordinates = {
        Vec2f(0.f, 0.f), Vec2f(1.f, 0.f), Vec2f(1.f, 1.f), Vec2f(0.f, 1.f)};
    if (m_flip_horizontal) {
        for (Vec2f &uv : texture_coordinates)
            uv.x() = 1.f - uv.x();
    }
    if (m_flip_vertical) {
        for (Vec2f &uv : texture_coordinates)
            uv.y() = 1.f - uv.y();
    }
    GLModel::Geometry preview_geometry;
    preview_geometry.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3T2};
    preview_geometry.color  = ColorRGBA::WHITE();
    preview_geometry.reserve_vertices(4);
    preview_geometry.reserve_indices(6);
    const std::array<Vec3d, 4> preview_corners = {center - rx - uy + preview_offset, center + rx - uy + preview_offset,
                                                   center + rx + uy + preview_offset, center - rx + uy + preview_offset};
    for (size_t corner = 0; corner < preview_corners.size(); ++corner)
        preview_geometry.add_vertex(Vec3f(preview_corners[corner].cast<float>()), texture_coordinates[corner]);
    preview_geometry.add_triangle(0, 1, 2);
    preview_geometry.add_triangle(0, 2, 3);
    m_preview_quad.init_from(std::move(preview_geometry));

    const Vec3d u_direction = m_flip_horizontal ? -right : right;
    const Vec3d v_direction = m_flip_vertical ? -up : up;
    const double u_length   = std::max(0.5, 0.2 * double(m_width_mm));
    const double v_length   = std::max(0.5, 0.2 * double(m_height_mm));
    const double arrow_size = std::max(0.2, 0.04 * std::min(double(m_width_mm), double(m_height_mm)));
    const Vec3d axis_origin = center + offset;
    const Vec3d u_end       = axis_origin + u_direction * u_length;
    const Vec3d v_end       = axis_origin + v_direction * v_length;
    const std::array<Vec3d, 7> axes = {axis_origin,
                                       u_end,
                                       u_end - u_direction * arrow_size + v_direction * (0.5 * arrow_size),
                                       u_end - u_direction * arrow_size - v_direction * (0.5 * arrow_size),
                                       v_end,
                                       v_end - v_direction * arrow_size + u_direction * (0.5 * arrow_size),
                                       v_end - v_direction * arrow_size - u_direction * (0.5 * arrow_size)};

    GLModel::Geometry geometry;
    geometry.format = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3};
    geometry.color  = ColorRGBA(0.f, 0.72f, 0.64f, 1.f);
    geometry.reserve_vertices(corners.size() + axes.size());
    geometry.reserve_indices(20);
    for (const Vec3d &corner : corners)
        geometry.add_vertex(Vec3f(corner.cast<float>()));
    for (const Vec3d &point : axes)
        geometry.add_vertex(Vec3f(point.cast<float>()));
    geometry.add_line(0, 1);
    geometry.add_line(1, 2);
    geometry.add_line(2, 3);
    geometry.add_line(3, 0);
    geometry.add_line(4, 5);
    geometry.add_line(5, 6);
    geometry.add_line(5, 7);
    geometry.add_line(4, 8);
    geometry.add_line(8, 9);
    geometry.add_line(8, 10);
    m_frame.init_from(std::move(geometry));
}

void GLGizmoImageProjection::on_render()
{
    GLVolume *volume = selected_gl_volume();
    if (!m_placed || volume == nullptr || volume->object_idx() != m_object_idx || volume->volume_idx() != m_volume_idx)
        return;
    if (m_frame_dirty)
        rebuild_frame();
    if (!m_frame.is_initialized())
        return;

    if (m_preview_texture_dirty) {
        m_preview_texture.reset();
        std::vector<unsigned char> preview_rgba = m_rgba;
        for (size_t offset = 3; offset < preview_rgba.size(); offset += 4)
            preview_rgba[offset] = uint8_t(std::lround(double(preview_rgba[offset]) * 0.78));
        if (!m_preview_texture.load_from_raw_data(std::move(preview_rgba), m_image_width, m_image_height, false, false))
            m_preview_texture.reset();
        if (m_preview_texture.get_id() != 0) {
            glsafe(::glBindTexture(GL_TEXTURE_2D, m_preview_texture.get_id()));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
            glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
        }
        m_preview_texture_dirty = false;
    }

    const Camera     &camera       = wxGetApp().plater()->get_camera();
    const Transform3d model_matrix = volume->world_matrix();
    const bool depth_test_enabled  = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
    const bool blend_enabled       = glIsEnabled(GL_BLEND) == GL_TRUE;
    const bool cull_face_enabled   = glIsEnabled(GL_CULL_FACE) == GL_TRUE;
    if (m_preview_quad.is_initialized() && m_preview_texture.get_id() != 0) {
        GLShaderProgram *texture_shader = wxGetApp().get_shader("image_projection_preview");
        if (texture_shader != nullptr) {
            texture_shader->start_using();
            texture_shader->set_uniform("view_model_matrix", camera.get_view_matrix() * model_matrix);
            texture_shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            texture_shader->set_uniform("uniform_texture", 0);
            glsafe(::glActiveTexture(GL_TEXTURE0));
            glsafe(::glBindTexture(GL_TEXTURE_2D, m_preview_texture.get_id()));
            glsafe(::glEnable(GL_BLEND));
            glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
            glsafe(::glDisable(GL_CULL_FACE));
            glsafe(::glDisable(GL_DEPTH_TEST));
            m_preview_quad.render();
            glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
            texture_shader->stop_using();
        }
    }

    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * model_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glLineWidth(2.f));
    m_frame.render();
    glsafe(::glLineWidth(1.f));
    glsafe(::glEnable(GL_DEPTH_TEST));
    shader->stop_using();

    if (!depth_test_enabled)
        glsafe(::glDisable(GL_DEPTH_TEST));
    if (!blend_enabled)
        glsafe(::glDisable(GL_BLEND));
    if (cull_face_enabled)
        glsafe(::glEnable(GL_CULL_FACE));
    else
        glsafe(::glDisable(GL_CULL_FACE));

    update_grabbers(*volume);
    render_grabbers(MoveGrabber, ScaleGrabber, Grabber::FixedGrabberSize, false);
    update_rotation_grabber_picker();
    render_rotation_arrows(camera);
}

bool GLGizmoImageProjection::apply_projection()
{
    GLVolume *gl_volume = selected_gl_volume();
    if (!m_placed || gl_volume == nullptr || gl_volume->object_idx() != m_object_idx || gl_volume->volume_idx() != m_volume_idx)
        return false;
    ModelVolume *volume = get_model_volume(*gl_volume, wxGetApp().model().objects);
    if (volume == nullptr)
        return false;

    const std::vector<ColorRGBA> filament_colors = get_extruders_colors();
    const size_t physical_count = std::min(filament_colors.size(), size_t(std::max(wxGetApp().filaments_cnt(), 0)));
    if (physical_count == 0) {
        MessageDialog error((wxWindow *) wxGetApp().mainframe, _L("No physical filament colors are configured for this project."),
                            _L("Project image"), wxOK | wxICON_ERROR);
        error.ShowModal();
        return false;
    }

    if (!configure_image_mapping(m_image_path, m_rgba, m_image_width, m_image_height))
        return false;

    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    ImageMap::TextureAsset texture;
    texture.stable_id   = "projected-image-" + std::to_string(tick);
    texture.display_name = boost::filesystem::path(m_image_path).filename().string();
    texture.width       = m_image_width;
    texture.height      = m_image_height;
    texture.rgba        = m_rgba;

    ImageMap::Zone zone;
    zone.stable_id                 = "image-projection-" + std::to_string(tick);
    zone.display_name              = texture.display_name;
    zone.render_mode               = m_render_mode;
    zone.adaptive_modulation_mode  = m_adaptive_modulation_mode;
    zone.color_mix_model           = m_color_mix_model;
    zone.synchronize_whole_object_cadence =
        m_render_mode == ImageMap::RenderMode::PerimeterModulationV2 && m_synchronize_whole_object_cadence;
    zone.minimum_component_percent = m_minimum_component_percent;
    zone.modulation_sample_spacing_mm = m_modulation_sample_spacing_mm;
    zone.disable_broad_path_smoothing = m_disable_broad_path_smoothing;
    zone.gaussian_smoothing_strength  = m_gaussian_smoothing_strength;
    zone.first_path_smoothing_strength = m_first_path_smoothing_strength;
    zone.second_path_smoothing_strength = m_second_path_smoothing_strength;
    zone.tone_gamma                   = m_tone_gamma;
    zone.overhang_contrast_percent    = m_overhang_contrast_percent;
    zone.image_exposure_ev            = m_image_exposure_ev;
    zone.image_contrast_percent       = m_image_contrast_percent;
    zone.image_saturation_percent     = m_image_saturation_percent;
    zone.image_edge_boost_percent     = m_image_edge_boost_percent;
    zone.palette                   = m_palette;

    const int base_filament_id = std::clamp(volume->extruder_id(), 1, int(physical_count));
    ImageMap::OrthographicProjection projection;
    projection.center                    = projection_center();
    projection.normal                    = m_normal;
    projection.up                        = m_up;
    projection.width_mm                  = m_width_mm;
    projection.height_mm                 = m_height_mm;
    projection.rotation_degrees          = m_rotation_degrees;
    projection.max_depth_mm              = m_depth_mm;
    projection.max_surface_angle_degrees = m_surface_angle_degrees;
    projection.flip_horizontal           = m_flip_horizontal;
    projection.flip_vertical             = m_flip_vertical;
    projection.seed_triangle             = m_seed_triangle;
    projection.background_color          = to_rgba(filament_colors[size_t(base_filament_id - 1)]);

    ImageMap::VolumeData data = volume->image_map_data() ? *volume->image_map_data() : ImageMap::VolumeData{};
    const ImageMap::ProjectionResult result =
        ImageMap::append_orthographic_projection(volume->mesh(), std::move(texture), std::move(zone), projection, data);
    if (!result) {
        MessageDialog error((wxWindow *) wxGetApp().mainframe, from_u8(result.error), _L("Project image"), wxOK | wxICON_ERROR);
        error.ShowModal();
        return false;
    }

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Project image"), UndoRedo::SnapshotType::GizmoAction);
    if (!volume->set_image_map_data(std::move(data))) {
        MessageDialog error((wxWindow *) wxGetApp().mainframe, _L("The generated image projection could not be attached to the volume."),
                            _L("Project image"), wxOK | wxICON_ERROR);
        error.ShowModal();
        return false;
    }
    wxGetApp().plater()->set_plater_dirty(true);
    wxGetApp().plater()->changed_object(*volume->get_object());
    wxGetApp().plater()->sidebar().filament_menu()->refresh_image_map_entries();
    m_placed = false;
    set_grabbers_enabled(false);
    mark_frame_dirty();
    return true;
}

void GLGizmoImageProjection::on_render_input_window(float x, float y, float bottom_limit)
{
    const float approximate_height = m_imgui->scaled(23.f);
    y = std::min(y, bottom_limit - approximate_height);
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.f, 0.f);
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                                    ImGuiWindowFlags_NoTitleBar);

    if (m_imgui->bbl_button(m_rgba.empty() ? _L("Choose image...") : _L("Choose another image...")))
        choose_image();
    if (!m_rgba.empty()) {
        ImGui::SameLine();
        const std::string filename = boost::filesystem::path(m_image_path).filename().string();
        m_imgui->text(filename);
        ImGui::TextDisabled("%s", _u8L("Mapping method will be selected after Apply.").c_str());
        ImGui::Separator();
        const std::string placement_help =
            m_placed ? _u8L("Drag the handles to move, resize, or rotate the image. Click the model to move it to another surface.") :
                       _u8L("Click the target surface to place the image.");
        ImGui::TextWrapped("%s", placement_help.c_str());

        if (m_placed) {
            bool changed = false;
            ImGui::PushItemWidth(m_imgui->scaled(8.f));
            const float old_width = m_width_mm;
            if (ImGui::DragFloat(_u8L("Width (mm)").c_str(), &m_width_mm, 0.1f, 0.1f, 1000.f, "%.2f")) {
                m_width_mm = std::clamp(m_width_mm, 0.1f, 1000.f);
                if (m_lock_aspect && old_width > 0.f)
                    m_height_mm *= m_width_mm / old_width;
                changed = true;
            }
            const float old_height = m_height_mm;
            if (ImGui::DragFloat(_u8L("Height (mm)").c_str(), &m_height_mm, 0.1f, 0.1f, 1000.f, "%.2f")) {
                m_height_mm = std::clamp(m_height_mm, 0.1f, 1000.f);
                if (m_lock_aspect && old_height > 0.f)
                    m_width_mm *= m_height_mm / old_height;
                changed = true;
            }
            changed |= ImGui::Checkbox(_u8L("Link width and height fields").c_str(), &m_lock_aspect);
            changed |= ImGui::DragFloat(_u8L("Horizontal offset (mm)").c_str(), &m_offset_x_mm, 0.1f, -1000.f, 1000.f, "%.2f");
            changed |= ImGui::DragFloat(_u8L("Vertical offset (mm)").c_str(), &m_offset_y_mm, 0.1f, -1000.f, 1000.f, "%.2f");
            changed |= ImGui::DragFloat(_u8L("Rotation").c_str(), &m_rotation_degrees, 0.5f, -180.f, 180.f, "%.1f deg");
            if (m_imgui->bbl_button(_L("Rotate +90 deg"))) {
                m_rotation_degrees += 90.f;
                changed = true;
            }
            ImGui::SameLine();
            if (m_imgui->bbl_button(_L("Rotate -90 deg"))) {
                m_rotation_degrees -= 90.f;
                changed = true;
            }
            changed |= ImGui::Checkbox(_u8L("Flip horizontally").c_str(), &m_flip_horizontal);
            changed |= ImGui::Checkbox(_u8L("Flip vertically").c_str(), &m_flip_vertical);
            changed |= ImGui::DragFloat(_u8L("Projection depth (mm)").c_str(), &m_depth_mm, 0.1f, 0.1f, 1000.f, "%.2f");
            changed |= ImGui::DragFloat(_u8L("Surface angle limit").c_str(), &m_surface_angle_degrees, 0.5f, 1.f, 89.f, "%.1f deg");
            if (m_imgui->bbl_button(_L("Reset projection controls"))) {
                reset_projection_controls();
                changed = true;
            }
            ImGui::PopItemWidth();
            if (changed) {
                while (m_rotation_degrees > 180.f)
                    m_rotation_degrees -= 360.f;
                while (m_rotation_degrees < -180.f)
                    m_rotation_degrees += 360.f;
                m_offset_x_mm           = std::clamp(m_offset_x_mm, -1000.f, 1000.f);
                m_offset_y_mm           = std::clamp(m_offset_y_mm, -1000.f, 1000.f);
                m_depth_mm              = std::clamp(m_depth_mm, 0.1f, 1000.f);
                m_surface_angle_degrees = std::clamp(m_surface_angle_degrees, 1.f, 89.f);
                mark_frame_dirty();
            }

            ImGui::Separator();
            m_imgui->push_confirm_button_style();
            if (m_imgui->bbl_button(_L("Apply projection...")))
                apply_projection();
            m_imgui->pop_confirm_button_style();
        }
    }

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

} // namespace Slic3r::GUI
