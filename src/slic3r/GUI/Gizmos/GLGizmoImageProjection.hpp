#ifndef slic3r_GLGizmoImageProjection_hpp_
#define slic3r_GLGizmoImageProjection_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLTexture.hpp"

#include "libslic3r/ImageMap/Projection.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {

class GLVolume;

namespace GUI {

class GLGizmoImageProjection : public GLGizmoBase
{
public:
    GLGizmoImageProjection(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);

    bool on_mouse(const wxMouseEvent &mouse_event) override;
    void data_changed(bool is_serializing) override;
    std::string get_tooltip() const override;

protected:
    bool         on_init() override;
    std::string  on_get_name() const override;
    bool         on_is_selectable() const override;
    bool         on_is_activable() const override;
    void         on_render() override;
    void         on_render_input_window(float x, float y, float bottom_limit) override;
    void         on_set_state() override;
    void         on_start_dragging() override;
    void         on_dragging(const UpdateData &data) override;
    void         on_stop_dragging() override;

private:
    enum GrabberId : int
    {
        MoveGrabber,
        WidthGrabber,
        HeightGrabber,
        ScaleGrabber,
        RotateGrabber,
        GrabberCount
    };

    struct DragState
    {
        bool        valid{false};
        float       offset_x_mm{0.f};
        float       offset_y_mm{0.f};
        float       width_mm{0.f};
        float       height_mm{0.f};
        float       rotation_degrees{0.f};
        Vec3d       projection_center{Vec3d::Zero()};
        Vec3d       right{Vec3d::UnitX()};
        Vec3d       up{Vec3d::UnitY()};
        Vec3d       normal{Vec3d::UnitZ()};
        Vec3d       mouse_hit{Vec3d::Zero()};
        Transform3d world_matrix{Transform3d::Identity()};
    };

    GLVolume    *selected_gl_volume() const;
    bool         choose_image();
    bool         configure_image_mapping(const std::string &image_path, const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height);
    bool         place_from_mouse(const Vec2d &mouse_pos);
    bool         place_at_default_position();
    bool         apply_projection();
    Vec3d        projection_center() const;
    bool         mouse_position_on_projection_plane(const Linef3 &mouse_ray, const DragState &drag_state, Vec3d &local_hit) const;
    void         update_grabbers(const GLVolume &volume);
    void         set_grabbers_enabled(bool enabled);
    void         reset_projection_controls();
    void         rebuild_frame();
    void         mark_frame_dirty();

    std::string          m_image_path;
    std::vector<uint8_t> m_rgba;
    uint32_t             m_image_width{0};
    uint32_t             m_image_height{0};
    ImageMap::RenderMode m_render_mode{ImageMap::RenderMode::NormalMix};
    ImageMap::AdaptiveModulationMode m_adaptive_modulation_mode{ImageMap::AdaptiveModulationMode::Perimeter};
    ImageMap::ColorMixModel m_color_mix_model{ImageMap::ColorMixModel::FullSpectrumKmKs};
    int                  m_minimum_component_percent{15};
    bool                 m_synchronize_whole_object_cadence{false};
    float                m_modulation_sample_spacing_mm{0.16f};
    bool                 m_disable_broad_path_smoothing{false};
    float                m_gaussian_smoothing_strength{1.f};
    float                m_first_path_smoothing_strength{1.f};
    float                m_second_path_smoothing_strength{1.f};
    float                m_tone_gamma{1.f};
    float                m_overhang_contrast_percent{100.f};
    float                m_image_exposure_ev{0.f};
    float                m_image_contrast_percent{100.f};
    float                m_image_saturation_percent{100.f};
    float                m_image_edge_boost_percent{0.f};
    std::vector<ImageMap::PaletteEntry> m_palette;

    bool     m_placed{false};
    bool     m_lock_aspect{true};
    bool     m_flip_horizontal{false};
    bool     m_flip_vertical{false};
    int      m_object_idx{-1};
    int      m_volume_idx{-1};
    uint32_t m_seed_triangle{0};
    Vec3d    m_center{Vec3d::Zero()};
    Vec3d    m_normal{Vec3d::UnitZ()};
    Vec3d    m_up{Vec3d::UnitY()};
    float    m_offset_x_mm{0.f};
    float    m_offset_y_mm{0.f};
    float    m_width_mm{30.f};
    float    m_height_mm{30.f};
    float    m_rotation_degrees{0.f};
    float    m_depth_mm{10.f};
    float    m_surface_angle_degrees{75.f};

    bool    m_frame_dirty{true};
    GLModel m_frame;
    GLModel m_preview_quad;
    GLTexture m_preview_texture;
    bool      m_preview_texture_dirty{false};
    DragState m_drag_state;
};

} // namespace GUI
} // namespace Slic3r

#endif
