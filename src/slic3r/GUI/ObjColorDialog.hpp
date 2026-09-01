#ifndef _OBJ_COLOR_DIALOG_H_
#define _OBJ_COLOR_DIALOG_H_

#include "GUI_Utils.hpp"
#include "MixedColorMatchHelpers.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/slider.h>
#include <wx/timer.h>
#include <wx/tglbtn.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/radiobut.h>
#include <wx/msgdlg.h>
class Button;
class Label;
class ComboBox;

struct ColorDistValue
{
    int   id;
    float distance;
};

class ObjColorPanel : public wxPanel
{
public:
    ObjColorPanel(wxWindow*                       parent,
                  std::vector<Slic3r::RGBA>&      input_colors,
                  bool                            is_single_color,
                  Slic3r::ObjColorImportContext&  import_context,
                  const std::vector<std::string>& extruder_colours,
                  std::vector<unsigned char>&     filament_ids,
                  unsigned char&                  first_extruder_id,
                  const std::string&              obj_filename = {});

    void msw_rescale();
    bool is_ok();
    bool update_filament_ids();

    // Per-row state for the 3-column Standard mapping table
    struct ColorTableRow
    {
        wxPanel*       row_panel{nullptr};
        wxButton*      color_icon{nullptr};       // color swatch button
        wxStaticText*  rgba_text{nullptr};         // "R:x G:y B:z" label
        wxRadioButton* existing_radio{nullptr};    // "Existing Filament" radio
        ComboBox*      existing_combo{nullptr};    // physical filament selector
        wxRadioButton* mix_radio{nullptr};         // "Generated Mix" radio
        wxButton*      mix_preview_icon{nullptr};  // read-only generated mix swatch preview
        wxStaticText*  mix_preview_text{nullptr};  // "Mix" label
    };

private:
    struct SliderSettingControl
    {
        wxSlider*   slider{nullptr};
        wxTextCtrl* input{nullptr};
        double      minimum{0.0};
        double      step{1.0};

        double value() const;
        void   enable(bool enabled) const;
    };

    // -----------------------------------------------------------------------
    // UI construction helpers
    // -----------------------------------------------------------------------
    void        build_source_section(wxWindow* parent, wxBoxSizer* parent_sizer);
    void        build_method_section(wxWindow* parent, wxBoxSizer* parent_sizer);
    void        build_standard_body(wxWindow* settings_parent, wxBoxSizer* settings_sizer,
                                    wxWindow* mapping_parent, wxBoxSizer* mapping_sizer);
    SliderSettingControl create_slider_setting_row(wxWindow* parent,
                                                    wxBoxSizer* parent_sizer,
                                                    const wxString& label,
                                                    double minimum,
                                                    double maximum,
                                                    double initial,
                                                    double step,
                                                    int digits,
                                                    const wxString& suffix,
                                                    const wxString& tooltip);
    void        rebuild_color_table();
    wxPanel*    create_help_icon(wxWindow* parent, const wxString& tooltip);

    wxBoxSizer* create_extruder_icon_and_rgba_sizer(wxWindow* parent, int id, const wxColour& color);
    wxBoxSizer* create_image_map_btn_sizer(wxWindow* parent);
    std::string get_color_str(const wxColour& color);
    ComboBox*   create_physical_filament_combo(wxWindow* parent, int row_id);

    // -----------------------------------------------------------------------
    // Mapping logic
    // -----------------------------------------------------------------------
    void        deal_default_strategy_new();
    void        reset_to_generated_mixes();
    void        apply_all_row_existing(int filament_idx);
    void        set_row_mode(int row_id, bool wants_mix, int physical_filament_idx = -1);
    void        on_row_existing_radio(int row_id);
    void        on_row_mix_radio(int row_id);
    void        on_row_combo_changed(int row_id);
    void        on_all_existing_click();
    void        on_all_generated_click();
    void        update_quantized_button_states();
    void        update_all_buttons_state();
    void        update_quantization_accuracy_warning();
    int         max_quantized_color_count() const;
    bool        rebuild_normal_color_match_plan();
    const Slic3r::GUI::NormalColorMatchPlanEntry* normal_color_match_plan_entry(size_t row) const;
    void        refresh_generated_mix_previews();

    // Kept for adaptive mode and internal use
    void deal_add_btn();
    void deal_reset_btn();
    void deal_algo(int cluster_number, bool redraw_ui = false);

    void                      choose_image_map_source();
    int                       min_component_percent() const;
    bool                      uses_layer_sequence_image_map() const;
    bool                      uses_adaptive_local_cycles_image_map() const;
    bool                      uses_adaptive_local_z_height_modulation() const;
    Slic3r::ObjImageMapColorMixModel selected_image_map_color_mix_model() const;
    float                     simple_pm_sample_spacing_mm() const;
    void                      store_simple_pm_quality_settings();
    void                      store_image_processing_settings();
    void                      schedule_image_map_preview_update();
    void                      update_simple_pm_quality_ui();
    bool                      simple_pm_uses_manual_filaments() const;
    int                       simple_pm_requested_filament_count() const;
    std::vector<unsigned int> simple_pm_component_ids() const;
    void                      update_simple_pm_filament_selection_hint();
    bool                      adaptive_cycle_mixes_ready() const;
    bool                      report_image_map_progress(Slic3r::ObjImageMapProgressStage stage, size_t current, size_t total);
    void                      update_adaptive_cycle_spectra(const Slic3r::GUI::AdaptiveColorMatchPreviewResult* preview = nullptr);
    void                      rebuild_adaptive_cycle_spectrum_table();
    void                      update_image_map_mode_ui();
    void                      store_image_map_palette(const std::vector<unsigned char>& cluster_filament_ids);
    void                      store_layer_sequence_image_map_palette(unsigned char filament_id, const wxColour& representative_color);
    int                       find_filament_selection_by_color(const wxColour& color) const;
    int         append_new_filament_option(const wxColour& color, std::vector<unsigned int>* component_filament_ids = nullptr);
    int         append_new_filament_color_option(const wxColour& color);
    static bool colors_are_equal(const wxColour& lhs, const wxColour& rhs);

    // Source label text for the current import_context.source
    wxString    source_display_label() const;

    // DeltaE76 close-match threshold: rows below this use "Existing Filament" by default
    static constexpr float k_close_match_threshold = 10.0f;

private:
    // -----------------------------------------------------------------------
    // Top-level layout panels
    // -----------------------------------------------------------------------
    wxPanel*           m_page_simple{nullptr};
    wxBoxSizer*        m_sizer{nullptr};
    wxBoxSizer*        m_sizer_simple{nullptr};

    // Method body sub-panels (one shown at a time)
    wxPanel*           m_standard_sub_panel{nullptr};
    wxPanel*           m_simple_pm_sub_panel{nullptr};
    wxPanel*           m_adaptive_sub_panel{nullptr};
    wxPanel*           m_standard_mapping_panel{nullptr};
    wxPanel*           m_simple_pm_mapping_panel{nullptr};
    wxPanel*           m_adaptive_mapping_panel{nullptr};

    // -----------------------------------------------------------------------
    // Section Source
    // -----------------------------------------------------------------------
    wxChoice*          m_source_choice{nullptr};      // dropdown: Detected, Specified, Material Colors
    wxStaticText*      m_source_filename_text{nullptr};
    wxStaticText*      m_filename_text{nullptr};      // "cube.obj"
    Button*            m_btn_browse_texture{nullptr};  // "Set Texture…" for Specified mode

    // Spectrum card for image texture source (layer-sequence mode)
    wxSizer* m_image_map_spectrum_sizer{nullptr};

    // -----------------------------------------------------------------------
    // Section Method (radio buttons replace wxChoice)
    // -----------------------------------------------------------------------
    wxRadioButton* m_method_standard_radio{nullptr};
    wxRadioButton* m_method_simple_pm_radio{nullptr};
    wxRadioButton* m_method_adaptive_radio{nullptr};
    ComboBox*      m_adaptive_modulation_choice{nullptr};
    ComboBox*      m_image_map_color_mix_model_choice{nullptr};
    ComboBox*      m_simple_pm_detail_choice{nullptr};
    wxCheckBox*    m_simple_pm_whole_object_cadence_checkbox{nullptr};
    wxCheckBox*    m_simple_pm_maximum_detail_checkbox{nullptr};
    SliderSettingControl m_simple_pm_gaussian_smoothing_ctrl;
    SliderSettingControl m_simple_pm_first_path_smoothing_ctrl;
    SliderSettingControl m_simple_pm_second_path_smoothing_ctrl;
    SliderSettingControl m_simple_pm_tone_gamma_ctrl;
    SliderSettingControl m_simple_pm_overhang_contrast_ctrl;
    SliderSettingControl m_image_exposure_ctrl;
    SliderSettingControl m_image_contrast_ctrl;
    SliderSettingControl m_image_saturation_ctrl;
    SliderSettingControl m_image_edge_boost_ctrl;
    wxTimer           m_image_map_preview_refresh_timer;
    int               m_image_map_preview_refresh_grace_ticks{0};

    // Simple perimeter modulation uses one shared cadence built from an
    // automatically selected subset, an explicit count, or a manual subset.
    ComboBox*                          m_simple_pm_filament_count_choice{nullptr};
    wxStaticText*                      m_simple_pm_filament_selection_hint{nullptr};
    std::vector<wxBitmapToggleButton*> m_simple_pm_filament_buttons;
    std::vector<bool>                  m_simple_pm_manual_filament_selected;

    // -----------------------------------------------------------------------
    // Section Color (Standard mode body)
    // -----------------------------------------------------------------------
    // Quantized-color count + min-component controls (shown in Standard & Adaptive)
    wxSizer*           m_quantized_settings_sizer{nullptr};
    wxSizer*           m_physical_title_sizer{nullptr};
    wxSizer*           m_physical_colors_sizer{nullptr};
    wxStaticText*      m_color_cluster_title{nullptr};
    wxTextCtrl*        m_color_cluster_num_by_user_ebox{nullptr};
    Button*            m_btn_quant_minus{nullptr};
    Button*            m_btn_quant_plus{nullptr};
    Button*            m_btn_recommended{nullptr};
    wxSpinCtrl*        m_min_component_percent_ctrl{nullptr};
    wxStaticText*      m_quantization_accuracy_warning{nullptr};

    // 3-column mapping table
    wxScrolledWindow*  m_scrolledWindow{nullptr};
    wxBoxSizer*        m_table_sizer{nullptr};

    // "All" row buttons
    Button*            m_btn_all_existing{nullptr};
    Button*            m_btn_all_generated{nullptr};

    // Per-row table state
    std::vector<ColorTableRow> m_color_table_rows;
    std::vector<bool>          m_row_wants_mix;    // true = generate mix; false = use existing
    int                        m_combox_icon_width{0};
    int                        m_combox_icon_height{0};

    // Warning / status text (shown below the active method body)
    wxStaticText*      m_warning_text{nullptr};
    wxStaticText*      m_adaptive_warning_text{nullptr};

    // -----------------------------------------------------------------------
    // Section Color — Adaptive mode body
    // -----------------------------------------------------------------------
    wxScrolledWindow*  m_adaptive_spectrum_window{nullptr};

    // -----------------------------------------------------------------------
    // Physical extruder swatch list (for msw_rescale)
    // -----------------------------------------------------------------------
    std::vector<wxButton*> m_extruder_icon_list;

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------
    std::string                    m_obj_filename;
    int                            m_last_cluster_num{-1};
    int                            m_last_cluster_number{-2};
    std::vector<Slic3r::RGBA>&     m_input_colors;
    int                            m_color_num_recommend{0};
    int                            m_color_cluster_num_by_algo{0};
    int                            m_input_colors_size{0};
    std::vector<wxColour>          m_colours;            // physical extruder colors
    std::vector<int>               m_cluster_map_filaments; // resolved filament id per cluster
    std::vector<wxColour>          m_cluster_colours;       // quantized cluster colors
    std::vector<wxColour>          m_source_spectrum_colours;
    std::vector<std::vector<wxColour>>          m_adaptive_cycle_spectrum_colours;
    std::vector<std::vector<unsigned int>>      m_adaptive_cycle_display_component_filament_ids;
    std::vector<unsigned int>                   m_adaptive_cycle_display_filament_ids;
    std::vector<size_t>                         m_adaptive_cycle_display_region_counts;
    size_t                         m_adaptive_direct_physical_region_count{0};
    bool                           m_adaptive_cycle_preview_valid{false};
    std::vector<wxColour>          m_new_add_colors;  // generated mixed colors (lazy)
    Slic3r::GUI::NormalColorMatchPlan m_normal_color_match_plan;
    std::vector<size_t>            m_normal_color_match_plan_rows;
    bool                           m_is_image_map{false};
    Slic3r::ObjColorImportContext& m_import_context;

    // algo result
    std::vector<Slic3r::RGBA> m_cluster_colors_from_algo;
    std::vector<int>          m_cluster_labels_from_algo;

    // result output
    unsigned char&              m_first_extruder_id;
    std::vector<unsigned char>& m_filament_ids;
};

class ObjColorDialog : public Slic3r::GUI::DPIDialog
{
public:
    ObjColorDialog(wxWindow*                       parent,
                   std::vector<Slic3r::RGBA>&      input_colors,
                   bool                            is_single_color,
                   Slic3r::ObjColorImportContext&  import_context,
                   const std::vector<std::string>& extruder_colours,
                   std::vector<unsigned char>&     filament_ids,
                   unsigned char&                  first_extruder_id,
                   const std::string&              obj_filename = {},
                   const wxString&                 dialog_title = wxString());
    wxBoxSizer* create_btn_sizer(long flags);
    void        on_dpi_changed(const wxRect& suggested_rect) override;

private:
    ObjColorPanel*                   m_panel_ObjColor = nullptr;
    std::unordered_map<int, Button*> m_button_list;
    std::vector<unsigned char>&      m_filament_ids;
    unsigned char&                   m_first_extruder_id;
};

#endif // _OBJ_COLOR_DIALOG_H_
