#ifndef slic3r_GUI_MixedFilamentDialog_hpp_
#define slic3r_GUI_MixedFilamentDialog_hpp_

#include <memory> 
#include <functional>
#include <wx/wx.h>
#include <wx/scrolwin.h>

#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/HSLColorPicker.hpp"
#include "Widgets/MixedFilamentRatioPanel.hpp"
#include "GUI_Utils.hpp"

namespace Slic3r::GUI {


class MixedFilamentDialog : public DPIDialog
{
public:
    enum class Action { Add, Edit };
    enum class Tab { Mix, Pattern, Gradient };
    enum class MixMethod { ManualRatio, ByColor };
 
    MixedFilamentDialog(wxWindow* parent, Action action, std::vector<std::pair<std::string, std::string>>& physical_filaments);


protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    Action m_action{Action::Add};
    Tab    m_current_tab{Tab::Mix};
    MixMethod m_mix_method{MixMethod::ManualRatio};

    double m_min_weight_ratio{0.15}; // 0...1 (e.g. 0.5 for 50%)

    int max_filament = 4; // mix 2-4 physical filaments
    const int min_filament = 2;

    int m_width_fixed;
    int m_height_start;
    int m_height_min;
    int m_clr_swatch_size;
    std::vector<std::pair<std::string, std::string>>& m_physical_filaments;

    std::vector<int>    m_selected_filaments;
    std::vector<double>  m_selected_filaments_weights; // 0...1 (e.g. 0.5 for 50%)
    std::vector<wxColor> m_selected_filaments_colors;

    void build_ui(wxWindow* parent);

    void build_mix_method_ui(wxPanel* parent, wxBoxSizer* parent_sizer);
    void build_color_picker_ui(wxPanel* parent, wxBoxSizer* parent_sizer);
    void build_pattern_selector_ui(wxPanel* parent, wxBoxSizer* parent_sizer);
    void build_material_ui(wxPanel* parent, wxBoxSizer* parent_sizer);
    void build_ratio_ui(wxPanel* parent, wxBoxSizer* parent_sizer);
    void build_recommendations_ui(wxPanel* parent, wxBoxSizer* parent_sizer);
    void setup_card_panel(wxPanel* panel);
    void build_preview_ui(wxPanel* parent, wxBoxSizer* parent_sizer);
    void update_preview();
    void update_preview(const std::vector<int>& filaments, const std::vector<double>& weights);

    // Layer stack solver for preview visualization (temp — replaceable)
    struct LayerStackEntry {
        int    filament_index; // index into m_selected_filaments
        double scale;          // 0..1 width scaling (1.0 = full width)
    };
    static std::vector<LayerStackEntry> compute_layer_stack(
        const std::vector<double>& weights, int total_layers = 20);

    void update_material_buttons_visibility();
    void update_material_panel();
    void update_material_title_preview();

    void setup_collapsible_section(
        wxPanel* title_panel,
        wxBoxSizer* title_sizer,
        wxStaticText* title_text,
        bool& collapsed_var,
        const std::vector<wxWindow*>& body_windows,
        const std::vector<wxWindow*>& action_controls = {},
        std::function<void()> on_toggle = nullptr
    );

    wxColour getTabBorderColor(bool is_selected, bool is_hovered) const;
    wxColour getTabBackgroundColor(bool is_selected, bool is_hovered) const;
    wxColour getTabTextColor(bool is_selected, bool is_hovered) const;
    void     paintTabBtn(
        wxPanel* panel,
        bool     round_left,
        bool     round_right,
        double   radius,
        wxString label,
        wxString icon_name ,
        bool     is_selected,
        bool     is_hovered);
    void update_tabs();
    bool m_mix_tab_hovered = false;
    bool m_pattern_tab_hovered = false;
    bool m_gradient_tab_hovered = false;

    // fallback, when MinSize/MaxSize constraints are not sufficient, 
    // restrict to only vertical resizing with a minimum height
    void on_sizing(wxSizeEvent& event);

    // content is filled by m_physical_filaments
    void add_material_combobox(wxPanel* parent, wxBoxSizer* sizer, int selected_filament_index = -1);
    void remove_material_combobox();
    void on_selected_filaments_changed(int index);
    int  find_first_free_filament() const;
    void refresh_material_combobox_items();
    void refresh_material_weight_labels();
    void update_min_weight_slider_bounds();
    void apply_min_weight(int new_percentage);

    void fill_recommendations(wxPanel* container, wxBoxSizer* container_sizer);
    void set_active_mix(const std::vector<int>& physical_filaments, const std::vector<double>& weights);

    std::vector<double> get_default_weights(int filament_count);
    std::vector<wxColor> get_selected_filaments_colors(const std::vector<int>& filament_indices) const;

    // Decoupled mix presets for color matching (used by both recommendations UI and color picker matching)
    struct MixPreset {
        std::vector<int>    filament_indices;
        std::vector<double> weights;
        wxColor             mixed_color;
    };
    std::vector<MixPreset> m_mix_presets;
    void generate_mix_presets();
    static wxColor compute_mixed_color(const std::vector<std::pair<std::string, std::string>>& filaments,
                                       const std::vector<int>& indices, const std::vector<double>& weights);
    const MixPreset* find_closest_mix(const wxColour& target) const;
    void update_color_match(const wxColour& selected_color, bool update_active_mix = false);
    void sync_color_picker_to_mix();
    bool m_syncing_from_color_picker{false}; // if current mix is synced from color picker, to prevent circular updates when user changes color or mix


    // UI 
    wxPanel*          m_title_panel{nullptr};
    wxScrolledWindow* m_content_panel{nullptr};
    wxPanel* m_footer_panel{nullptr};

    wxBoxSizer* m_main_sizer{nullptr};
    wxBoxSizer* m_title_sizer{nullptr};
    wxBoxSizer* m_content_sizer{nullptr};
    wxBoxSizer* m_footer_sizer{nullptr};

    // Title
    wxPanel* m_mix_tab_btn{nullptr};
    wxPanel* m_pattern_tab_btn{nullptr};
    wxPanel* m_gradient_tab_btn{nullptr};

    // Content
    wxPanel*                    m_material_panel{nullptr};
    wxPanel*                    m_material_title_panel{nullptr};
    wxStaticText*               m_material_title_text{nullptr};
    ScalableButton*             m_add_material_btn{nullptr};
    ScalableButton*             m_delete_material_btn{nullptr};
    wxPanel*                    m_material_combobox_panel{nullptr};
    wxPanel*                    m_material_title_preview{nullptr};
    std::vector<wxPanel*>       m_material_title_swatches;
    std::vector<wxStaticText*>  m_material_title_percent_texts;
    std::vector<int>            m_last_preview_filaments;
    std::vector<ComboBox*>      m_material_comboboxes;
    std::vector<wxStaticText*>  m_material_weight_labels;

    // Material
    wxBoxSizer* m_material_sizer{nullptr};
    wxBoxSizer* m_material_title_sizer{nullptr};
    wxBoxSizer* m_material_combobox_sizer{nullptr};

    // Ratio
    wxPanel*    m_ratio_section_panel{nullptr};
    wxBoxSizer* m_ratio_section_sizer{nullptr};

    wxPanel*      m_mix_ratio_title_panel{nullptr};
    wxStaticText*   m_mix_ratio_title_text{nullptr};

    MixedFilamentRatioPanel* m_mix_ratio_panel{nullptr};
    wxBoxSizer*              m_mix_ratio_sizer{nullptr};

    wxPanel*      m_min_weight_panel{nullptr};
    wxStaticText* m_min_weight_label{nullptr};
    wxSlider*     m_min_weight_slider{nullptr};
    wxTextCtrl*   m_min_weight_value_input{nullptr};
    wxStaticText* m_min_weight_value_label{nullptr}; // "%" label next to input

    wxBoxSizer*   m_min_weight_sizer{nullptr};

    // Recommendations
    wxPanel*      m_recommendations_panel{nullptr};
    wxBoxSizer*   m_recommendations_sizer{nullptr};
    wxPanel*      m_recommendations_title_panel{nullptr};
    wxBoxSizer*   m_recommendations_title_sizer{nullptr};
    wxStaticText* m_recommendations_title_text{nullptr};
    wxPanel*      m_recommendations_mix_panel{nullptr};
    wxBoxSizer*   m_recommendations_mix_sizer{nullptr};

    // Preview
    wxPanel*      m_preview_panel{nullptr};
    wxBoxSizer*   m_preview_sizer{nullptr};
    wxPanel*      m_preview_title_panel{nullptr};
    wxStaticText* m_preview_title_text{nullptr};
    wxBoxSizer*   m_preview_title_sizer{nullptr};
    wxPanel*      m_preview_body{nullptr};
    wxPanel*      m_preview_layers_panel{nullptr};
    wxPanel*      m_preview_color_panel{nullptr};
    wxPanel*      m_preview_title_swatch{nullptr};
    wxPanel*      m_preview_title_layers{nullptr};
    std::vector<LayerStackEntry> m_preview_layer_stack;
    std::vector<wxColor>         m_preview_colors;

    wxPanel*        m_mix_method_panel{nullptr};
    wxBoxSizer*     m_mix_method_sizer{nullptr};
    wxRadioButton*  m_method_manual_radio{nullptr};
    wxRadioButton*  m_method_by_color_radio{nullptr};

    wxPanel*          m_color_picker_panel{nullptr};
    wxBoxSizer*       m_color_picker_sizer{nullptr};
    HSLColorPicker*   m_hsl_color_picker{nullptr};

    // Color Picker
    wxPanel*      m_color_picker_body{nullptr};
    wxPanel*      m_pattern_selector_body{nullptr};

    wxPanel*      m_match_section_panel{nullptr};
    wxTextCtrl*   m_matched_hex_display{nullptr};
    wxPanel*      m_matched_color_preview{nullptr};
    bool          m_match_section_hovered{false};
    double        m_current_deviation{-1.0};
    double        m_warning_deviation_threshold{65.0};
    double        m_max_deviation{441.673}; // std::sqrt(3 * 255 * 255)

    // Pattern
    wxPanel*        m_pattern_selector_panel{nullptr};
    wxBoxSizer*     m_pattern_selector_sizer{nullptr};


    bool m_color_picker_collapsed = false;
    bool m_pattern_selector_collapsed = false;
    bool m_material_collapsed = false;
    bool m_ratio_collapsed = false;
    bool m_recommendations_collapsed = false;
    bool m_preview_collapsed = false;
    
    // Footer
    
};


} // namespace Slic3r::GUI
#endif