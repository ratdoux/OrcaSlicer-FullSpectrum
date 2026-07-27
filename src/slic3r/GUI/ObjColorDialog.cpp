#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
//#include "libslic3r/FlushVolCalc.hpp"
#include "ObjColorDialog.hpp"
#include "BitmapCache.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/FilamentCardMixed.hpp"
#include "slic3r/Utils/ColorSpaceConvert.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "libslic3r/Config.hpp"
#include "BitmapComboBox.hpp"
#include "Widgets/ComboBox.hpp"
#include <wx/choicdlg.h>
#include <wx/filedlg.h>
#include <wx/sizer.h>

#include "libslic3r/ObjColorUtils.hpp"
#include "libslic3r/ImageMap/Sampling.hpp"
#include "MixedColorMatchHelpers.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

int objcolor_scale(const int val) { return val * Slic3r::GUI::wxGetApp().em_unit() / 10; }
int OBJCOLOR_ITEM_WIDTH() { return objcolor_scale(30); }
static const wxColour g_text_color = wxColour(107, 107, 107, 255);
const int HEADER_BORDER  = 5;
const int CONTENT_BORDER = 3;
const int PANEL_WIDTH = 430;
const int COLOR_LABEL_WIDTH = 165;

#undef  ICON_SIZE
#define ICON_SIZE                 wxSize(FromDIP(16), FromDIP(16))
#define MIN_OBJCOLOR_DIALOG_WIDTH FromDIP(460)
#define FIX_SCROLL_HEIGTH         FromDIP(400)
#define BTN_SIZE                  wxSize(FromDIP(58), FromDIP(24))
#define BTN_GAP                   FromDIP(20)

static void update_ui(wxWindow* window)
{
    Slic3r::GUI::wxGetApp().UpdateDarkUI(window);
}

static std::vector<wxColour> wx_spectrum_colors(const std::vector<RGBA>& representative)
{
    std::vector<wxColour> colors;
    colors.reserve(representative.size());
    for (const RGBA& color : representative) {
        colors.emplace_back(std::clamp(int(std::lround(color[0] * 255.f)), 0, 255),
                            std::clamp(int(std::lround(color[1] * 255.f)), 0, 255),
                            std::clamp(int(std::lround(color[2] * 255.f)), 0, 255));
    }
    return colors;
}

static std::vector<wxColour> sampled_spectrum_colors(const std::vector<RGBA>& input_colors)
{
    return wx_spectrum_colors(ImageMap::representative_source_colors(input_colors));
}

static const char g_min_cluster_color = 1;
//static const char g_max_cluster_color = 15;
static const char g_max_color = 16;
const  StateColor ok_btn_bg(std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed),
                     std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
                     std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));
const StateColor  ok_btn_disable_bg(std::pair<wxColour, int>(wxColour(205, 201, 201), StateColor::Pressed),
                                   std::pair<wxColour, int>(wxColour(205, 201, 201), StateColor::Hovered),
                                   std::pair<wxColour, int>(wxColour(205, 201, 201), StateColor::Normal));
wxBoxSizer* ObjColorDialog::create_btn_sizer(long flags)
{
    auto btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->AddStretchSpacer();

    StateColor ok_btn_bd(
        std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal)
    );
    StateColor ok_btn_text(
        std::pair<wxColour, int>(wxColour(255, 255, 254), StateColor::Normal)
    );
    StateColor cancel_btn_bg(
        std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Normal)
    );
    StateColor cancel_btn_bd_(
        std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Normal)
    );
    StateColor cancel_btn_text(
        std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Normal)
    );
    StateColor calc_btn_bg(
        std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal)
    );
    StateColor calc_btn_bd(
        std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal)
    );
    StateColor calc_btn_text(
        std::pair<wxColour, int>(wxColour(255, 255, 254), StateColor::Normal)
    );
    if (flags & wxOK) {
        Button* ok_btn = new Button(this, _L("OK"));
        ok_btn->SetMinSize(BTN_SIZE);
        ok_btn->SetCornerRadius(FromDIP(12));
        ok_btn->Enable(false);
        ok_btn->SetBackgroundColor(ok_btn_disable_bg);
        ok_btn->SetBorderColor(ok_btn_bd);
        ok_btn->SetTextColor(ok_btn_text);
        ok_btn->SetFocus();
        ok_btn->SetId(wxID_OK);
        btn_sizer->Add(ok_btn, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, BTN_GAP);
        m_button_list[wxOK] = ok_btn;
    }
    if (flags & wxCANCEL) {
        Button* cancel_btn = new Button(this, _L("Cancel"));
        cancel_btn->SetMinSize(BTN_SIZE);
        cancel_btn->SetCornerRadius(FromDIP(12));
        cancel_btn->SetBackgroundColor(cancel_btn_bg);
        cancel_btn->SetBorderColor(cancel_btn_bd_);
        cancel_btn->SetTextColor(cancel_btn_text);
        cancel_btn->SetId(wxID_CANCEL);
        btn_sizer->Add(cancel_btn, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, BTN_GAP);
        m_button_list[wxCANCEL] = cancel_btn;
    }
    return btn_sizer;
}

void ObjColorDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    for (auto button_item : m_button_list)
    {
        if (button_item.first == wxRESET)
        {
            button_item.second->SetMinSize(wxSize(FromDIP(75), FromDIP(24)));
            button_item.second->SetCornerRadius(FromDIP(12));
        }
        if (button_item.first == wxOK) {
            button_item.second->SetMinSize(BTN_SIZE);
            button_item.second->SetCornerRadius(FromDIP(12));
        }
        if (button_item.first == wxCANCEL) {
            button_item.second->SetMinSize(BTN_SIZE);
            button_item.second->SetCornerRadius(FromDIP(12));
        }
    }
    m_panel_ObjColor->msw_rescale();
    this->Refresh();
};

ObjColorDialog::ObjColorDialog(wxWindow*                       parent,
                               std::vector<Slic3r::RGBA>&      input_colors,
                               bool                            is_single_color,
                               Slic3r::ObjColorImportContext&  import_context,
                               const std::vector<std::string>& extruder_colours,
                               std::vector<unsigned char>&     filament_ids,
                               unsigned char&                  first_extruder_id)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe),
                wxID_ANY,
                import_context.mode == ObjColorImportMode::ImageMap ? _(L("Import OBJ Image Map")) : _(L("Import OBJ Colors")),
                wxDefaultPosition,
                wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE /* | wxRESIZE_BORDER*/)
    , m_filament_ids(filament_ids)
    , m_first_extruder_id(first_extruder_id)
{
    std::string icon_path = (boost::format("%1%/images/Snapmaker_OrcaTitle.ico") % Slic3r::resources_dir()).str();
    SetIcon(wxIcon(Slic3r::encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));

    auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));

    this->SetBackgroundColour(*wxWHITE);
    this->SetMinSize(wxSize(MIN_OBJCOLOR_DIALOG_WIDTH, -1));

    m_panel_ObjColor = new ObjColorPanel(this, input_colors, is_single_color, import_context, extruder_colours, filament_ids,
                                         first_extruder_id);

    auto main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_line_top, 0, wxEXPAND, 0);
    // set min sizer width according to extruders count
    auto sizer_width = (int) (2.8 * OBJCOLOR_ITEM_WIDTH());
    sizer_width      = sizer_width > MIN_OBJCOLOR_DIALOG_WIDTH ? sizer_width : MIN_OBJCOLOR_DIALOG_WIDTH;
    main_sizer->SetMinSize(wxSize(sizer_width, -1));
    main_sizer->Add(m_panel_ObjColor, 1, wxEXPAND | wxALL, 0);

    auto btn_sizer = create_btn_sizer(wxOK | wxCANCEL);
    {
        m_button_list[wxOK]->Bind(wxEVT_UPDATE_UI, ([this](wxUpdateUIEvent &e) {
           if (m_panel_ObjColor->is_ok() == m_button_list[wxOK]->IsEnabled()) { return; }
           m_button_list[wxOK]->Enable(m_panel_ObjColor->is_ok());
           m_button_list[wxOK]->SetBackgroundColor(m_panel_ObjColor->is_ok() ? ok_btn_bg : ok_btn_disable_bg);
         }));
    }
    main_sizer->Add(btn_sizer, 0, wxBOTTOM | wxRIGHT | wxEXPAND, BTN_GAP);
    SetSizer(main_sizer);
    main_sizer->SetSizeHints(this);

    if (this->FindWindowById(wxID_OK, this)) {
        this->FindWindowById(wxID_OK, this)->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {// if OK button is clicked..
              if (m_panel_ObjColor->update_filament_ids())
                  EndModal(wxID_OK);
            }, wxID_OK);
    }
    if (this->FindWindowById(wxID_CANCEL, this)) {
        update_ui(static_cast<wxButton*>(this->FindWindowById(wxID_CANCEL, this)));
        this->FindWindowById(wxID_CANCEL, this)->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxCANCEL); });
    }
    this->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) { EndModal(wxCANCEL); });

    wxGetApp().UpdateDlgDarkUI(this);
}
RGBA     convert_to_rgba(const wxColour &color)
{
    RGBA rgba;
    rgba[0] = std::clamp(color.Red() / 255.f, 0.f, 1.f);
    rgba[1] = std::clamp(color.Green() / 255.f, 0.f, 1.f);
    rgba[2] = std::clamp(color.Blue() / 255.f, 0.f, 1.f);
    rgba[3] = std::clamp(color.Alpha() / 255.f, 0.f, 1.f);
    return rgba;
}
wxColour convert_to_wxColour(const RGBA &color)
{
    auto     r = std::clamp((int) (color[0] * 255.f), 0, 255);
    auto     g = std::clamp((int) (color[1] * 255.f), 0, 255);
    auto     b = std::clamp((int) (color[2] * 255.f), 0, 255);
    auto     a = std::clamp((int) (color[3] * 255.f), 0, 255);
    wxColour wx_color(r, g, b, a);
    return wx_color;
}
// This panel contains all control widgets for both simple and advanced mode (these reside in separate sizers)
ObjColorPanel::ObjColorPanel(wxWindow*                       parent,
                             std::vector<Slic3r::RGBA>&      input_colors,
                             bool                            is_single_color,
                             Slic3r::ObjColorImportContext&  import_context,
                             const std::vector<std::string>& extruder_colours,
                             std::vector<unsigned char>&     filament_ids,
                             unsigned char&                  first_extruder_id)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize /*,wxBORDER_RAISED*/)
    , m_input_colors(input_colors)
    , m_is_image_map(import_context.mode == ObjColorImportMode::ImageMap)
    , m_import_context(import_context)
    , m_first_extruder_id(first_extruder_id)
    , m_filament_ids(filament_ids)
{
    if (input_colors.size() == 0) {
        return;
    }
    for (const std::string& color : extruder_colours) {
        m_colours.push_back(wxColor(color));
    }
    // deal input_colors
    m_input_colors_size = input_colors.size();
    for (size_t i = 0; i < input_colors.size(); i++) {
        if (color_is_equal(input_colors[i] , UNDEFINE_COLOR) && !m_colours.empty()) { // not define color range:0~1
            input_colors[i]=convert_to_rgba(m_colours[0]);
        }
    }
    if (m_is_image_map)
        m_source_spectrum_colours = sampled_spectrum_colors(input_colors);
    if (is_single_color && input_colors.size() >=1) {
        m_cluster_colors_from_algo.emplace_back(input_colors[0]);
        m_cluster_colours.emplace_back(convert_to_wxColour(input_colors[0]));
        m_cluster_labels_from_algo.reserve(m_input_colors_size);
        for (size_t i = 0; i < m_input_colors_size; i++) {
            m_cluster_labels_from_algo.emplace_back(0);
        }
        m_cluster_map_filaments.resize(m_cluster_colors_from_algo.size());
        m_color_num_recommend = m_color_cluster_num_by_algo = m_cluster_colors_from_algo.size();
    } else {//cluster deal
        deal_algo(-1);
    }
    //end first cluster
    //draw ui
    auto sizer_width = FromDIP(300);
    // Create two switched panels with their own sizers
    m_sizer_simple = new wxBoxSizer(wxVERTICAL);
    m_page_simple  = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_page_simple->SetSizer(m_sizer_simple);
    m_page_simple->SetBackgroundColour(*wxWHITE);

    update_ui(m_page_simple);
    // BBS
    m_sizer_simple->AddSpacer(FromDIP(10));
    if (m_is_image_map) {
        auto* description = new wxStaticText(
            m_page_simple, wxID_ANY,
            wxString::Format(
                 _L("The selected OBJ surface colors were sampled into %llu printable regions.\n"
                    "Choose normal mixed colors, a shared perimeter-modulated sequence, or localized mixed-filament cycles with perimeter modulation."),
                static_cast<unsigned long long>(input_colors.size())));
        description->Wrap(FromDIP(PANEL_WIDTH - 30));
        m_sizer_simple->Add(description, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));

        auto* mode_sizer = new wxBoxSizer(wxHORIZONTAL);
        mode_sizer->Add(new wxStaticText(m_page_simple, wxID_ANY, _L("Image mapping method:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT,
                        FromDIP(8));
        m_image_map_mode_ctrl = new wxChoice(m_page_simple, wxID_ANY);
        m_image_map_mode_ctrl->Append(_L("Normal mixed filaments"));
        m_image_map_mode_ctrl->Append(_L("One filament per layer - perimeter modulation"));
        m_image_map_mode_ctrl->Append(_L("Adaptive localized cycles - perimeter modulation"));
        const int initial_mode = import_context.image_map_render_mode == ObjImageMapRenderMode::PerimeterModulationV2 ? 1 :
                                 import_context.image_map_render_mode == ObjImageMapRenderMode::AdaptiveLocalizedCycles ? 2 : 0;
        m_image_map_mode_ctrl->SetSelection(initial_mode);
        m_image_map_mode_ctrl->SetToolTip(
            _L("Perimeter modulation uses one shared physical-filament sequence and one tool per layer. "
               "Adaptive localized cycles cluster the source into localized color regions, then share a sparse KM/K-S mixed-filament "
               "cycle wherever the same physical recipe is sufficient. Perimeter modulation refines the sampled texture color; this may "
               "require more toolchanges."));
        mode_sizer->Add(m_image_map_mode_ctrl, 1, wxALIGN_CENTER_VERTICAL);
        mode_sizer->AddSpacer(FromDIP(8));
        mode_sizer->Add(create_image_map_btn_sizer(m_page_simple), 0, wxALIGN_CENTER_VERTICAL);
        m_sizer_simple->Add(mode_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));
        m_image_map_mode_ctrl->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            if (uses_adaptive_local_cycles_image_map())
                deal_add_btn();
            rebuild_adaptive_cycle_spectrum_table();
            update_image_map_mode_ui();
        });
    }
    // BBS: for tunning flush volumes
    {
        //color cluster results
        wxBoxSizer *  specify_cluster_sizer               = new wxBoxSizer(wxHORIZONTAL);
        m_color_cluster_title = new wxStaticText(m_page_simple, wxID_ANY, _L("Quantized colors:"));
        m_color_cluster_title->SetFont(Label::Head_14);
        specify_cluster_sizer->Add(m_color_cluster_title, 0, wxALIGN_CENTER | wxALL, FromDIP(5));

        m_color_cluster_num_by_user_ebox = new wxTextCtrl(m_page_simple, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(25), -1), wxTE_PROCESS_ENTER);
        m_color_cluster_num_by_user_ebox->SetValue(std::to_string(m_color_cluster_num_by_algo).c_str());
        {//event
            auto on_apply_color_cluster_text_modify = [this](wxEvent &e) {
                wxString str        = m_color_cluster_num_by_user_ebox->GetValue();
                int      number = wxAtoi(str);
                if (number > m_color_num_recommend || number < g_min_cluster_color) {
                    number = number < g_min_cluster_color ? g_min_cluster_color : m_color_num_recommend;
                    str    = wxString::Format(("%d"), number);
                    m_color_cluster_num_by_user_ebox->SetValue(str);
                    MessageDialog dlg(nullptr, wxString::Format(_L("The color count should be in range [%d, %d]."), g_min_cluster_color, m_color_num_recommend),
                                      _L("Warning"), wxICON_WARNING | wxOK);
                    dlg.ShowModal();
                }
                e.Skip();
            };
            m_color_cluster_num_by_user_ebox->Bind(wxEVT_TEXT_ENTER, on_apply_color_cluster_text_modify);
            m_color_cluster_num_by_user_ebox->Bind(wxEVT_KILL_FOCUS, on_apply_color_cluster_text_modify);
            m_color_cluster_num_by_user_ebox->Bind(wxEVT_COMMAND_TEXT_UPDATED, [this](wxCommandEvent &) {
                wxString str        = m_color_cluster_num_by_user_ebox->GetValue();
                int    number = wxAtof(str);
                if (number > m_color_num_recommend || number < g_min_cluster_color) {
                    number = number < g_min_cluster_color ? g_min_cluster_color : m_color_num_recommend;
                    str    = wxString::Format(("%d"), number);
                    m_color_cluster_num_by_user_ebox->SetValue(str);
                    m_color_cluster_num_by_user_ebox->SetInsertionPointEnd();
                }
                if (m_last_cluster_num != number) {
                    deal_algo(number, true);
                    Layout();
                    //Fit();
                    Refresh();
                    Update();
                    m_last_cluster_num = number;
                }
            });
            m_color_cluster_num_by_user_ebox->Bind(wxEVT_CHAR, [this](wxKeyEvent &e) {
                int keycode = e.GetKeyCode();
                wxString input_char = wxString::Format("%c", keycode);
                long     value;
                if (!input_char.ToLong(&value))
                    return;
                e.Skip();
            });
        }
        specify_cluster_sizer->AddSpacer(FromDIP(2));
        specify_cluster_sizer->Add(m_color_cluster_num_by_user_ebox, 0, wxALIGN_CENTER | wxALL, 0);
        specify_cluster_sizer->AddSpacer(FromDIP(15));
        wxStaticText *recommend_color_cluster_title = new wxStaticText(m_page_simple, wxID_ANY, "(" + std::to_string(m_color_num_recommend) + " " + _L("Recommended ") + ")");
        specify_cluster_sizer->Add(recommend_color_cluster_title, 0, wxALIGN_CENTER | wxALL, 0);

        specify_cluster_sizer->AddSpacer(FromDIP(18));
        auto *minimum_weight_label = new wxStaticText(m_page_simple, wxID_ANY, _L("Minimum mix component:"));
        specify_cluster_sizer->Add(minimum_weight_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        m_min_component_percent_ctrl = new wxSpinCtrl(m_page_simple, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                                      wxSize(FromDIP(58), -1), wxSP_ARROW_KEYS, 1, 49, 15);
        m_min_component_percent_ctrl->SetToolTip(_L("Smallest physical-filament percentage allowed in generated mixed colors."));
        m_min_component_percent_ctrl->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) {
            if (uses_adaptive_local_cycles_image_map()) {
                deal_add_btn();
                rebuild_adaptive_cycle_spectrum_table();
                update_image_map_mode_ui();
            }
        });
        specify_cluster_sizer->Add(m_min_component_percent_ctrl, 0, wxALIGN_CENTER_VERTICAL);

        m_quantized_settings_sizer = specify_cluster_sizer;
        m_sizer_simple->Add(specify_cluster_sizer, 0, wxEXPAND | wxLEFT, FromDIP(20));

        wxBoxSizer *  current_filaments_title_sizer  = new wxBoxSizer(wxHORIZONTAL);
        wxStaticText *current_filaments_title = new wxStaticText(m_page_simple, wxID_ANY, _L("Physical filament colors:"));
        current_filaments_title->SetFont(Label::Head_14);
        current_filaments_title_sizer->Add(current_filaments_title, 0, wxALIGN_CENTER | wxALL, FromDIP(5));
        m_physical_title_sizer = current_filaments_title_sizer;
        m_sizer_simple->Add(current_filaments_title_sizer, 0, wxEXPAND | wxLEFT, FromDIP(20));

        wxBoxSizer *  current_filaments_sizer = new wxBoxSizer(wxHORIZONTAL);
        current_filaments_sizer->AddSpacer(FromDIP(10));
        for (size_t i = 0; i < m_colours.size(); i++) {
            auto extruder_icon_sizer = create_extruder_icon_and_rgba_sizer(m_page_simple, i, m_colours[i]);
            current_filaments_sizer->Add(extruder_icon_sizer, 0, wxALIGN_CENTER | wxALIGN_CENTER_VERTICAL, FromDIP(10));
        }
        m_physical_colors_sizer = current_filaments_sizer;
        m_sizer_simple->Add(current_filaments_sizer, 0, wxEXPAND | wxLEFT, FromDIP(20));

        if (m_is_image_map) {
            m_image_map_spectrum_sizer = new wxBoxSizer(wxHORIZONTAL);
            wxString spectrum_label;
            switch (m_import_context.source) {
            case ObjColorImportSource::ImageTexture: spectrum_label = _L("Texture colors"); break;
            case ObjColorImportSource::VertexColors: spectrum_label = _L("Vertex colors"); break;
            case ObjColorImportSource::FaceColors:   spectrum_label = _L("Material colors"); break;
            }
            auto* spectrum_card = new FilamentCardImageMap(
                m_page_simple, spectrum_label, m_source_spectrum_colours, false);
            spectrum_card->SetMinSize(wxSize(FromDIP(PANEL_WIDTH - 20), FromDIP(34)));
            m_image_map_spectrum_sizer->Add(spectrum_card, 1, wxEXPAND);
            m_sizer_simple->Add(m_image_map_spectrum_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));
        }
        //colors table
        m_scrolledWindow = new wxScrolledWindow(m_page_simple,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxSB_VERTICAL);
        m_sizer_simple->Add(m_scrolledWindow, 0, wxEXPAND | wxALL, FromDIP(5));
        draw_table();

        m_adaptive_spectrum_window = new wxScrolledWindow(
            m_page_simple, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSB_VERTICAL);
        m_adaptive_spectrum_window->SetBackgroundColour(m_page_simple->GetBackgroundColour());
        m_adaptive_spectrum_window->Hide();
        m_sizer_simple->Add(m_adaptive_spectrum_window, 0, wxEXPAND | wxALL, FromDIP(5));
        rebuild_adaptive_cycle_spectrum_table();
        // buttons
        wxBoxSizer*   quick_set_sizer = new wxBoxSizer(wxHORIZONTAL);
        wxStaticText* quick_set_title = new wxStaticText(m_page_simple, wxID_ANY, _L("Quick set:"));
        quick_set_title->SetFont(Label::Head_12);
        quick_set_sizer->Add(quick_set_title, 0, wxALIGN_CENTER | wxALL, 0);
        quick_set_sizer->AddSpacer(FromDIP(10));

        auto calc_approximate_match_btn_sizer = create_approximate_match_btn_sizer(m_page_simple);
        auto calc_add_btn_sizer               = create_add_btn_sizer(m_page_simple);
        auto calc_reset_btn_sizer             = create_reset_btn_sizer(m_page_simple);
        quick_set_sizer->Add(calc_add_btn_sizer, 0, wxALIGN_CENTER | wxALL, 0);
        quick_set_sizer->AddSpacer(FromDIP(10));
        quick_set_sizer->Add(calc_approximate_match_btn_sizer, 0, wxALIGN_CENTER | wxALL, 0);
        quick_set_sizer->AddSpacer(FromDIP(10));
        if (!m_is_image_map) {
            auto image_map_btn_sizer = create_image_map_btn_sizer(m_page_simple);
            quick_set_sizer->Add(image_map_btn_sizer, 0, wxALIGN_CENTER | wxALL, 0);
            quick_set_sizer->AddSpacer(FromDIP(10));
        }
        quick_set_sizer->Add(calc_reset_btn_sizer, 0, wxALIGN_CENTER | wxALL, 0);
        quick_set_sizer->AddSpacer(FromDIP(10));
        m_quick_set_sizer = quick_set_sizer;
        m_sizer_simple->Add(quick_set_sizer, 0, wxEXPAND | wxLEFT, FromDIP(30));

        wxBoxSizer* warning_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_warning_text            = new wxStaticText(m_page_simple, wxID_ANY, "");
        if (!m_import_context.warning_message.empty())
            m_warning_text->SetLabelText(wxString::FromUTF8(m_import_context.warning_message));
        m_warning_text->Wrap(FromDIP(PANEL_WIDTH - 40));
        warning_sizer->Add(m_warning_text, 1, wxEXPAND | wxALL, 0);
        m_sizer_simple->Add(warning_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(30));

        m_sizer_simple->AddSpacer(10);
    }
    deal_default_strategy();
    rebuild_adaptive_cycle_spectrum_table();
    // page_simple//page_advanced
    m_sizer = new wxBoxSizer(wxVERTICAL);
    m_sizer->Add(m_page_simple, 0, wxEXPAND, 0);

    m_sizer->SetSizeHints(this);
    SetSizer(m_sizer);
    this->Layout();
    update_image_map_mode_ui();
}

void ObjColorPanel::msw_rescale()
{
    for (unsigned int i = 0; i < m_extruder_icon_list.size(); ++i) {
        auto bitmap = *get_extruder_color_icon(m_colours[i].GetAsString(wxC2S_HTML_SYNTAX).ToStdString(), std::to_string(i + 1), FromDIP(16), FromDIP(16));
        m_extruder_icon_list[i]->SetBitmap(bitmap);
    }
   /* for (unsigned int i = 0; i < m_color_cluster_icon_list.size(); ++i) {
        auto bitmap = *get_extruder_color_icon(m_cluster_colours[i].GetAsString(wxC2S_HTML_SYNTAX).ToStdString(), std::to_string(i + 1), FromDIP(16), FromDIP(16));
        m_color_cluster_icon_list[i]->SetBitmap(bitmap);
    }*/
}

bool ObjColorPanel::is_ok() {
    if (m_is_image_map && uses_layer_sequence_image_map())
        return m_colours.size() >= 2 && !m_source_spectrum_colours.empty();
    if (m_is_image_map && uses_adaptive_local_cycles_image_map())
        return adaptive_cycle_mixes_ready() && m_adaptive_cycle_preview_valid &&
               std::all_of(m_adaptive_cycle_spectrum_colours.begin(), m_adaptive_cycle_spectrum_colours.end(),
                           [](const std::vector<wxColour>& colors) { return !colors.empty(); });
    for (auto item : m_result_icon_list) {
        if (item->bitmap_combox->IsShown()) {
            auto selection = item->bitmap_combox->GetSelection();
            if (selection < 1) {
                return false;
            }
        }
    }
    return true;
}

bool ObjColorPanel::update_filament_ids()
{
    const int existing_filament_count = static_cast<int>(m_colours.size());
    std::map<int, int> appended_filament_id_map;
    bool created_mixed_filament = false;

    auto report_progress = [this](size_t current, size_t total) {
        if (!m_import_context.image_map_progress_fn)
            return true;
        total = std::max<size_t>(total, 1);
        const size_t stride = std::max<size_t>(total / 100, 1);
        if (current != 0 && current < total && current % stride != 0)
            return true;
        return report_image_map_progress(ObjImageMapProgressStage::CreateMixedFilaments,
                                         std::min(current, total), total);
    };

    auto physical_color_strings = [this]() {
        std::vector<std::string> colors;
        colors.reserve(m_colours.size());
        for (const wxColour &color : m_colours)
            colors.emplace_back(color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
        return colors;
    };

    if (m_is_image_map && uses_layer_sequence_image_map()) {
        if (m_source_spectrum_colours.empty() && m_cluster_colours.empty())
            return false;
        if (!report_progress(0, 1))
            return false;

        const std::vector<std::string> physical_colors = physical_color_strings();
        const wxColour cadence_color = m_cluster_colours.empty() ? m_source_spectrum_colours.front() : m_cluster_colours.front();

        const MixedColorMatchCreationResult match = create_mixed_filament_color_match(
            cadence_color,
            physical_colors,
            min_component_percent(),
            g_max_color,
            MixedColorMatchEncoding::PerimeterModulatedLayerSequence);
        if (!report_progress(1, 1))
            return false;
        if (!match.valid || match.filament_id == 0 || match.filament_id > unsigned(g_max_color)) {
            m_warning_text->SetLabelText(
                _L("Unable to create a shared layer-sequence color within the 16 printable color slots."));
            return false;
        }

        const unsigned char cadence_filament_id = static_cast<unsigned char>(match.filament_id);
        created_mixed_filament = match.created;

        m_filament_ids.assign(size_t(m_input_colors_size), cadence_filament_id);
        m_first_extruder_id = cadence_filament_id;
        store_layer_sequence_image_map_palette(cadence_filament_id, cadence_color);
        if (created_mixed_filament && wxGetApp().plater() != nullptr)
            wxGetApp().plater()->on_filaments_change(m_colours.size());
        return !m_filament_ids.empty();
    }

    if (m_is_image_map && uses_adaptive_local_cycles_image_map() && !adaptive_cycle_mixes_ready()) {
        m_warning_text->SetLabelText(
            _L("Unable to generate every adaptive cycle within the 16 printable color slots. Reduce the adaptive color region count."));
        return false;
    }

    if (!m_new_add_colors.empty()) {
        std::vector<int> selected_appended_indices;
        selected_appended_indices.reserve(m_cluster_map_filaments.size());
        for (int mapped_filament_id : m_cluster_map_filaments) {
            if (mapped_filament_id > existing_filament_count) {
                selected_appended_indices.emplace_back(mapped_filament_id);
            }
        }

        std::sort(selected_appended_indices.begin(), selected_appended_indices.end());
        selected_appended_indices.erase(std::unique(selected_appended_indices.begin(), selected_appended_indices.end()), selected_appended_indices.end());

        size_t mixed_progress = 0;
        const size_t mixed_progress_total = selected_appended_indices.size() + size_t(m_input_colors_size);
        const std::vector<std::string> physical_colors = physical_color_strings();
        if (!report_progress(0, mixed_progress_total))
            return false;
        for (int combo_selection : selected_appended_indices) {
            const int new_color_idx = combo_selection - existing_filament_count - 1;
            if (new_color_idx < 0 || new_color_idx >= static_cast<int>(m_new_add_colors.size())) {
                continue;
            }

            const MixedColorMatchEncoding encoding = uses_adaptive_local_cycles_image_map() ?
                                                         MixedColorMatchEncoding::AdaptiveLocalizedCycles :
                                                         m_is_image_map ? MixedColorMatchEncoding::SurfaceBias :
                                                                          MixedColorMatchEncoding::LayerRatio;
            const MixedColorMatchCreationResult match = create_mixed_filament_color_match(
                m_new_add_colors[new_color_idx],
                physical_colors,
                min_component_percent(),
                g_max_color,
                encoding);
            if (!match.valid || match.filament_id == 0 || match.filament_id > unsigned(g_max_color)) {
                m_warning_text->SetLabelText(_L("Unable to create a printable mixed color for one or more OBJ colors."));
                return false;
            }
            appended_filament_id_map.emplace(combo_selection, int(match.filament_id));
            created_mixed_filament |= match.created;
            if (!report_progress(++mixed_progress, mixed_progress_total))
                return false;
        }
    }

    auto resolve_filament_id = [&appended_filament_id_map](int mapped_filament_id) {
        const auto it = appended_filament_id_map.find(mapped_filament_id);
        const int resolved_filament_id = it == appended_filament_id_map.end() ? mapped_filament_id : it->second;
        return static_cast<unsigned char>(resolved_filament_id);
    };

    m_filament_ids.clear();
    m_filament_ids.reserve(m_input_colors_size);
    for (size_t i = 0; i < m_input_colors_size; i++) {
        auto label = m_cluster_labels_from_algo[i];
        m_filament_ids.emplace_back(resolve_filament_id(m_cluster_map_filaments[label]));
        if (!report_progress(appended_filament_id_map.size() + i + 1,
                             appended_filament_id_map.size() + size_t(m_input_colors_size)))
            return false;
    }
    m_first_extruder_id = resolve_filament_id(m_cluster_map_filaments[0]);
    if (m_is_image_map) {
        std::vector<unsigned char> cluster_filament_ids;
        cluster_filament_ids.reserve(m_cluster_map_filaments.size());
        for (int mapped_filament_id : m_cluster_map_filaments)
            cluster_filament_ids.emplace_back(resolve_filament_id(mapped_filament_id));
        store_image_map_palette(cluster_filament_ids);
    }
    if (created_mixed_filament && wxGetApp().plater() != nullptr)
        wxGetApp().plater()->on_filaments_change(m_colours.size());
    return !m_filament_ids.empty();
}

void ObjColorPanel::store_image_map_palette(const std::vector<unsigned char>& cluster_filament_ids)
{
    if (uses_layer_sequence_image_map())
        m_import_context.image_map_render_mode = ObjImageMapRenderMode::PerimeterModulationV2;
    else if (uses_adaptive_local_cycles_image_map())
        m_import_context.image_map_render_mode = ObjImageMapRenderMode::AdaptiveLocalizedCycles;
    else
        m_import_context.image_map_render_mode = ObjImageMapRenderMode::NormalMix;
    m_import_context.image_map_minimum_component_percent = min_component_percent();
    m_import_context.image_map_palette_colors.clear();
    m_import_context.image_map_palette_filament_ids.clear();
    m_import_context.image_map_palette_mixed_stable_ids.clear();
    m_import_context.image_map_palette_colors.reserve(m_cluster_colours.size());
    m_import_context.image_map_palette_filament_ids.reserve(m_cluster_colours.size());
    m_import_context.image_map_palette_mixed_stable_ids.reserve(m_cluster_colours.size());

    PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    for (size_t cluster_idx = 0; cluster_idx < m_cluster_colours.size() && cluster_idx < cluster_filament_ids.size(); ++cluster_idx) {
        const wxColour &color = m_cluster_colours[cluster_idx];
        m_import_context.image_map_palette_colors.push_back(
            RGBA{float(color.Red()) / 255.f, float(color.Green()) / 255.f, float(color.Blue()) / 255.f, float(color.Alpha()) / 255.f});
        const unsigned char filament_id = cluster_filament_ids[cluster_idx];
        m_import_context.image_map_palette_filament_ids.push_back(filament_id);

        uint64_t stable_id = 0;
        if (preset_bundle != nullptr && filament_id > m_colours.size()) {
            const std::optional<MixedFilamentDefinition> definition =
                preset_bundle->mixed_filaments.mixed_filament_definition_from_id(filament_id, m_colours.size());
            if (definition)
                stable_id = definition->identity.stable_id;
        }
        m_import_context.image_map_palette_mixed_stable_ids.push_back(stable_id);
    }
}

void ObjColorPanel::store_layer_sequence_image_map_palette(unsigned char filament_id, const wxColour& representative_color)
{
    m_import_context.image_map_render_mode              = ObjImageMapRenderMode::PerimeterModulationV2;
    m_import_context.image_map_minimum_component_percent = min_component_percent();
    m_import_context.image_map_palette_colors.assign(1, convert_to_rgba(representative_color));
    m_import_context.image_map_palette_filament_ids.assign(1, filament_id);

    uint64_t stable_id = 0;
    if (wxGetApp().preset_bundle != nullptr && filament_id > m_colours.size()) {
        const std::optional<MixedFilamentDefinition> definition =
            wxGetApp().preset_bundle->mixed_filaments.mixed_filament_definition_from_id(filament_id, m_colours.size());
        if (definition)
            stable_id = definition->identity.stable_id;
    }
    m_import_context.image_map_palette_mixed_stable_ids.assign(1, stable_id);
}

wxBoxSizer *ObjColorPanel::create_approximate_match_btn_sizer(wxWindow *parent)
{
    auto       btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    StateColor calc_btn_bg(std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed), std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
                           std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));
    StateColor calc_btn_bd(std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));
    StateColor calc_btn_text(std::pair<wxColour, int>(wxColour(255, 255, 254), StateColor::Normal));
    //create btn
    m_quick_approximate_match_btn = new Button(parent, _L("Physical match"));
    m_quick_approximate_match_btn->SetToolTip(_L("Map every quantized OBJ color to the nearest physical filament."));
    auto cur_btn         = m_quick_approximate_match_btn;
    cur_btn->SetFont(Label::Body_13);
    cur_btn->SetMinSize(wxSize(FromDIP(60), FromDIP(20)));
    cur_btn->SetCornerRadius(FromDIP(10));
    cur_btn->SetBackgroundColor(calc_btn_bg);
    cur_btn->SetBorderColor(calc_btn_bd);
    cur_btn->SetTextColor(calc_btn_text);
    cur_btn->SetFocus();
    btn_sizer->Add(cur_btn, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 0);
    cur_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        deal_approximate_match_btn();
    });
    return btn_sizer;
}

wxBoxSizer *ObjColorPanel::create_add_btn_sizer(wxWindow *parent)
{
    auto       btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    StateColor calc_btn_bg(std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed), std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
                           std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));
    StateColor calc_btn_bd(std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));
    StateColor calc_btn_text(std::pair<wxColour, int>(wxColour(255, 255, 254), StateColor::Normal));
    // create btn
    m_quick_add_btn = new Button(parent, _L("Generate mixes"));
    m_quick_add_btn->SetToolTip(_L("Generate mixed filaments that match all quantized OBJ colors."));
    auto cur_btn    = m_quick_add_btn;
    cur_btn->SetFont(Label::Body_13);
    cur_btn->SetMinSize(wxSize(FromDIP(60), FromDIP(20)));
    cur_btn->SetCornerRadius(FromDIP(10));
    cur_btn->SetBackgroundColor(calc_btn_bg);
    cur_btn->SetBorderColor(calc_btn_bd);
    cur_btn->SetTextColor(calc_btn_text);
    cur_btn->SetFocus();
    btn_sizer->Add(cur_btn, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 0);
    cur_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        deal_add_btn();
    });
    return btn_sizer;
}

wxBoxSizer *ObjColorPanel::create_reset_btn_sizer(wxWindow *parent)
{
    auto       btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    StateColor calc_btn_bg(std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed), std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
                           std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));
    StateColor calc_btn_bd(std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));
    StateColor calc_btn_text(std::pair<wxColour, int>(wxColour(255, 255, 254), StateColor::Normal));
    // create btn
    m_quick_reset_btn = new Button(parent, _L("Reset"));
    m_quick_reset_btn->SetToolTip(_L("Reset mapped extruders."));
    auto cur_btn      = m_quick_reset_btn;
    cur_btn->SetFont(Label::Body_13);
    cur_btn->SetMinSize(wxSize(FromDIP(60), FromDIP(20)));
    cur_btn->SetCornerRadius(FromDIP(10));
    cur_btn->SetBackgroundColor(calc_btn_bg);
    cur_btn->SetBorderColor(calc_btn_bd);
    cur_btn->SetTextColor(calc_btn_text);
    cur_btn->SetFocus();
    btn_sizer->Add(cur_btn, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 0);
    cur_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { deal_reset_btn(); });
    return btn_sizer;
}

wxBoxSizer* ObjColorPanel::create_image_map_btn_sizer(wxWindow* parent)
{
    auto       btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    StateColor calc_btn_bg(std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed),
                           std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
                           std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));
    StateColor calc_btn_bd(std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));
    StateColor calc_btn_text(std::pair<wxColour, int>(wxColour(255, 255, 254), StateColor::Normal));

    m_image_map_btn = new Button(parent, m_is_image_map ? _L("Image source...") : _L("Image map..."));
    m_image_map_btn->SetToolTip(
        _L("Open the image-map importer using a detected texture, a selected texture image, or colors stored in the OBJ."));
    m_image_map_btn->SetFont(Label::Body_13);
    m_image_map_btn->SetMinSize(wxSize(FromDIP(78), FromDIP(20)));
    m_image_map_btn->SetCornerRadius(FromDIP(10));
    m_image_map_btn->SetBackgroundColor(calc_btn_bg);
    m_image_map_btn->SetBorderColor(calc_btn_bd);
    m_image_map_btn->SetTextColor(calc_btn_text);
    btn_sizer->Add(m_image_map_btn, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 0);
    m_image_map_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { choose_image_map_source(); });
    return btn_sizer;
}

void ObjColorPanel::choose_image_map_source()
{
    struct ImageSourceChoice
    {
        wxString             label;
        ObjColorImportSource source;
        bool                 select_texture_file{false};
    };

    std::vector<ImageSourceChoice> source_choices;
    if (m_import_context.detected_texture_available) {
        source_choices.push_back({_L("Use the detected OBJ texture"), ObjColorImportSource::ImageTexture, false});
    }
    if (m_import_context.texture_coordinates_available) {
        source_choices.push_back({_L("Select a PNG or JPEG texture..."), ObjColorImportSource::ImageTexture, true});
    }
    if (m_import_context.vertex_colors_available) {
        source_choices.push_back({_L("Use OBJ vertex colors"), ObjColorImportSource::VertexColors, false});
    }
    if (m_import_context.face_colors_available) {
        source_choices.push_back({_L("Use OBJ material colors"), ObjColorImportSource::FaceColors, false});
    }

    if (source_choices.empty()) {
        MessageDialog dialog(this,
                             _L("This OBJ has no UV coordinates, vertex colors, or material colors that can be used for image mapping."),
                             _L("OBJ image map"), wxOK | wxICON_INFORMATION);
        dialog.ShowModal();
        return;
    }

    wxArrayString labels;
    int           default_selection = 0;
    for (size_t choice_idx = 0; choice_idx < source_choices.size(); ++choice_idx) {
        labels.Add(source_choices[choice_idx].label);
        if (source_choices[choice_idx].source == m_import_context.source &&
            !(m_import_context.source == ObjColorImportSource::ImageTexture && source_choices[choice_idx].select_texture_file)) {
            default_selection = int(choice_idx);
        }
    }

    wxSingleChoiceDialog source_dialog(this, _L("Choose the surface color source for image mapping."), _L("OBJ image map source"), labels);
    source_dialog.SetSelection(default_selection);
    if (source_dialog.ShowModal() != wxID_OK)
        return;

    const int selection = source_dialog.GetSelection();
    if (selection < 0 || selection >= int(source_choices.size()))
        return;
    const ImageSourceChoice& choice = source_choices[size_t(selection)];

    std::string texture_file;
    if (choice.select_texture_file) {
        wxFileDialog texture_dialog(
            this, _L("Choose an image texture for the OBJ"), wxEmptyString, wxEmptyString,
            _L("Image files (*.png;*.jpg;*.jpeg)|*.png;*.jpg;*.jpeg|PNG files (*.png)|*.png|JPEG files (*.jpg;*.jpeg)|*.jpg;*.jpeg"),
            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (texture_dialog.ShowModal() != wxID_OK)
            return;
        texture_file = into_u8(texture_dialog.GetPath());
    }

    m_import_context.source_change_requested = true;
    m_import_context.requested_source        = choice.source;
    m_import_context.requested_mode          = ObjColorImportMode::ImageMap;
    m_import_context.requested_texture_file  = std::move(texture_file);
    m_import_context.warning_message.clear();
    if (auto* dialog = dynamic_cast<wxDialog*>(GetParent()))
        dialog->EndModal(wxID_APPLY);
}

wxBoxSizer* ObjColorPanel::create_extruder_icon_and_rgba_sizer(wxWindow* parent, int id, const wxColour& color)
{
    auto      icon_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxButton* icon       = new wxButton(parent, wxID_ANY, {}, wxDefaultPosition, ICON_SIZE, wxBORDER_NONE | wxBU_AUTODRAW);
    icon->SetBitmap(
        *get_extruder_color_icon(color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString(), std::to_string(id + 1), FromDIP(16), FromDIP(16)));
    icon->SetCanFocus(false);
    m_extruder_icon_list.emplace_back(icon);
    icon_sizer->Add(icon, 0, wxALIGN_CENTER | wxALIGN_CENTER_VERTICAL, FromDIP(10)); // wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM

    icon_sizer->AddSpacer(FromDIP(5));
    return icon_sizer;
}

std::string ObjColorPanel::get_color_str(const wxColour& color)
{
    std::string str = ("R:" + std::to_string(color.Red()) +
                          std::string(" G:") + std::to_string(color.Green()) +
                          std::string(" B:") + std::to_string(color.Blue()) +
                          std::string(" A:") + std::to_string(color.Alpha()));
    return str;
}

ComboBox *ObjColorPanel::CreateEditorCtrl(wxWindow *parent, int id) // wxRect labelRect,, const wxVariant &value
{
    const double            em          = Slic3r::GUI::wxGetApp().em_unit();
    bool                    thin_icon   = false;
    const int               icon_width  = lround((thin_icon ? 2 : 4.4) * em);
    const int               icon_height = lround(2 * em);
    m_combox_icon_width                 = icon_width;
    m_combox_icon_height                = icon_height;
    std::vector<wxBitmap *> icons;
    icons.reserve(m_colours.size());
    for (size_t index = 0; index < m_colours.size(); ++index) {
        icons.emplace_back(get_extruder_color_icon(
            m_colours[index].GetAsString(wxC2S_HTML_SYNTAX).ToStdString(), std::to_string(index + 1), icon_width, icon_height));
    }
    wxColour undefined_color(0,255,0,255);
    icons.insert(icons.begin(), get_extruder_color_icon(undefined_color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString(), std::to_string(-1), icon_width, icon_height));
    if (icons.empty())
        return nullptr;

    ::ComboBox *c_editor = new ::ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(m_combox_width), -1), 0, nullptr,
                                          wxCB_READONLY | CB_NO_DROP_ICON | CB_NO_TEXT);
    c_editor->SetMinSize(wxSize(FromDIP(m_combox_width), -1));
    c_editor->SetMaxSize(wxSize(FromDIP(m_combox_width), -1));
    c_editor->GetDropDown().SetUseContentWidth(true);
    for (size_t i = 0; i < icons.size(); i++) {
        c_editor->Append(wxString::Format("%d", i), *icons[i]);
        if (i == 0) {
            c_editor->SetItemTooltip(i,undefined_color.GetAsString(wxC2S_HTML_SYNTAX));
        } else {
            c_editor->SetItemTooltip(i, m_colours[i-1].GetAsString(wxC2S_HTML_SYNTAX));
        }
    }
    c_editor->SetSelection(0);
    c_editor->SetName(wxString::Format("%d", id));
    c_editor->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &evt) {
        auto *com_box = static_cast<ComboBox *>(evt.GetEventObject());
        int   i       = atoi(com_box->GetName().c_str());
        if (i < m_cluster_map_filaments.size()) { m_cluster_map_filaments[i] = com_box->GetSelection(); }
        evt.StopPropagation();
    });
    return c_editor;
}

void ObjColorPanel::deal_approximate_match_btn()
{
    if (!m_new_add_colors.empty()) {
        deal_reset_btn();
    }

    auto calc_color_distance = [](wxColour c1, wxColour c2) {
        float lab[2][3];
        RGB2Lab(c1.Red(), c1.Green(), c1.Blue(), &lab[0][0], &lab[0][1], &lab[0][2]);
        RGB2Lab(c2.Red(), c2.Green(), c2.Blue(), &lab[1][0], &lab[1][1], &lab[1][2]);

        return DeltaE76(lab[0][0], lab[0][1], lab[0][2], lab[1][0], lab[1][1], lab[1][2]);
    };
    m_warning_text->SetLabelText("");
    if (m_result_icon_list.size() == 0) { return; }
    auto map_count = static_cast<int>(m_colours.size());
    if (map_count < 1) { return; }
    for (size_t i = 0; i < m_cluster_colours.size(); i++) {
        auto    c = m_cluster_colours[i];
        std::vector<ColorDistValue> color_dists;
        color_dists.resize(map_count);
        for (size_t j = 0; j < map_count; j++) {
            auto tip_color       = m_result_icon_list[0]->bitmap_combox->GetItemTooltip(j+1);
            wxColour candidate_c(tip_color);
            color_dists[j].distance = calc_color_distance(c, candidate_c);
            color_dists[j].id = j + 1;
        }
        std::sort(color_dists.begin(), color_dists.end(), [](ColorDistValue &a, ColorDistValue& b) {
            return a.distance < b.distance;
            });
        auto new_index= color_dists[0].id;
        m_result_icon_list[i]->bitmap_combox->SetSelection(new_index);
        m_cluster_map_filaments[i] = new_index;
    }
    update_keep_color_buttons();
}

bool ObjColorPanel::colors_are_equal(const wxColour &lhs, const wxColour &rhs)
{
    return lhs.Red() == rhs.Red() && lhs.Green() == rhs.Green() && lhs.Blue() == rhs.Blue() && lhs.Alpha() == rhs.Alpha();
}

int ObjColorPanel::find_filament_selection_by_color(const wxColour &color) const
{
    for (size_t i = 0; i < m_colours.size(); ++i) {
        if (colors_are_equal(m_colours[i], color)) {
            return static_cast<int>(i + 1);
        }
    }

    for (size_t i = 0; i < m_new_add_colors.size(); ++i) {
        if (colors_are_equal(m_new_add_colors[i], color)) {
            return static_cast<int>(m_colours.size() + i + 1);
        }
    }

    return 0;
}

int ObjColorPanel::append_new_filament_option(const wxColour &color, std::vector<unsigned int>* component_filament_ids)
{
    if (m_colours.size() < 2 || m_colours.size() + m_new_add_colors.size() >= g_max_color) {
        return 0;
    }

    std::vector<std::string> physical_colors;
    physical_colors.reserve(m_colours.size());
    for (const wxColour &physical_color : m_colours)
        physical_colors.emplace_back(physical_color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
    const MixedFilamentDisplayContext context = build_mixed_filament_display_context(physical_colors);
    const MixedColorMatchRecipeResult recipe = build_best_color_match_recipe(
        physical_colors,
        color,
        min_component_percent(),
        context.physical_tds,
        context.physical_material_ids,
        MixedFilamentManager::color_engine(),
        MixedFilamentManager::use_td_for_color_prediction());
    if (!recipe.valid)
        return 0;

    if (component_filament_ids != nullptr) {
        component_filament_ids->clear();
        const MixedFilamentDefinition definition = mixed_filament_definition_from_color_match_recipe(
            recipe, physical_colors.size(), MixedColorMatchEncoding::AdaptiveLocalizedCycles);
        for (const MixedFilamentWeightedComponent& component : definition.recipe.blend.components) {
            if (component.percent > 0 && component.filament.id >= 1 && component.filament.id <= physical_colors.size())
                component_filament_ids->push_back(component.filament.id);
        }
    }

    return append_new_filament_color_option(color);
}

int ObjColorPanel::append_new_filament_color_option(const wxColour& color)
{
    if (m_colours.size() + m_new_add_colors.size() >= g_max_color)
        return 0;

    m_new_add_colors.emplace_back(color);
    const int selection = static_cast<int>(m_colours.size() + m_new_add_colors.size());
    auto *    bitmap    = get_extruder_color_icon(color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString(), std::to_string(selection), m_combox_icon_width, m_combox_icon_height);

    for (auto *item : m_result_icon_list) {
        if (item->bitmap_combox == nullptr) {
            continue;
        }

        item->bitmap_combox->Append(wxString::Format("%d", item->bitmap_combox->GetCount()), *bitmap);
        item->bitmap_combox->SetItemTooltip(item->bitmap_combox->GetCount() - 1, color.GetAsString(wxC2S_HTML_SYNTAX));
    }

    return selection;
}

void ObjColorPanel::update_keep_color_buttons()
{
    for (size_t i = 0; i < m_result_icon_list.size(); ++i) {
        auto *item = m_result_icon_list[i];
        if (item->keep_color_btn == nullptr) {
            continue;
        }

        const bool has_cluster_color = i < m_cluster_colours.size();
        const bool show_keep_color   = has_cluster_color && find_filament_selection_by_color(m_cluster_colours[i]) == 0;
        item->keep_color_btn->Show(show_keep_color);
        item->keep_color_btn->Enable(show_keep_color);
    }

    if (m_scrolledWindow != nullptr) {
        m_scrolledWindow->Layout();
    }
    Layout();
}

void ObjColorPanel::deal_keep_color_btn(int id)
{
    if (id < 0 || id >= static_cast<int>(m_cluster_colours.size())) {
        return;
    }

    int selection = find_filament_selection_by_color(m_cluster_colours[id]);
    if (selection == 0) {
        selection = append_new_filament_option(m_cluster_colours[id]);
    }

    if (selection == 0) {
        m_warning_text->SetLabelText(_L("The generated colors would exceed the 16 printable color slots."));
        return;
    }

    m_result_icon_list[id]->bitmap_combox->SetSelection(selection);
    m_cluster_map_filaments[id] = selection;
    m_warning_text->SetLabelText(_L("Note: The color has been selected, you can choose OK \nto continue or manually adjust it."));
    update_keep_color_buttons();
}

void ObjColorPanel::show_sizer(wxSizer *sizer, bool show)
{
    wxSizerItemList items = sizer->GetChildren();
    for (wxSizerItemList::iterator it = items.begin(); it != items.end(); ++it) {
        wxSizerItem *item   = *it;
        if (wxWindow *window = item->GetWindow()) {
            window->Show(show);
        }
        if (wxSizer *son_sizer = item->GetSizer()) {
            show_sizer(son_sizer, show);
        }
    }
}

void ObjColorPanel::redraw_part_table() {
    //show all and set -1
    deal_reset_btn();
    for (size_t i = 0; i < m_row_sizer_list.size(); i++) {
        show_sizer(m_row_sizer_list[i], true);
    }
    if (m_cluster_colours.size() < m_row_sizer_list.size()) { // show part
        for (size_t i = m_cluster_colours.size(); i < m_row_sizer_list.size(); i++) {
            show_sizer(m_row_sizer_list[i], false);
            //m_row_panel_list[i]->Show(false); // show_sizer(m_left_color_cluster_boxsizer_list[i],false);
           // m_result_icon_list[i]->bitmap_combox->Show(false);
        }
    } else if (m_cluster_colours.size() > m_row_sizer_list.size()) {
        for (size_t i = m_row_sizer_list.size(); i < m_cluster_colours.size(); i++) {
            int      id                       = i;
            wxPanel *row_panel = new wxPanel(m_scrolledWindow);
            row_panel->SetBackgroundColour((i+1) % 2 == 0 ? *wxWHITE : wxColour(238, 238, 238));
            auto row_sizer = new wxBoxSizer(wxHORIZONTAL);
            row_panel->SetSizer(row_sizer);

            row_panel->SetMinSize(wxSize(FromDIP(PANEL_WIDTH), -1));
            row_panel->SetMaxSize(wxSize(FromDIP(PANEL_WIDTH), -1));

            auto cluster_color_icon_sizer = create_color_icon_and_rgba_sizer(row_panel, id, m_cluster_colours[id]);
            row_sizer->Add(cluster_color_icon_sizer, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
            row_sizer->AddStretchSpacer();
            row_sizer->Add(create_result_button_sizer(row_panel, id), 0, wxALIGN_CENTER_VERTICAL, 0);

            m_row_sizer_list.emplace_back(row_sizer);
            m_gridsizer->Add(row_panel, 0, wxALIGN_LEFT | wxALL, FromDIP(HEADER_BORDER));
        }
        m_gridsizer->Layout();
    }
    for (size_t i = 0; i < m_cluster_colours.size(); i++) { // update data
        // m_color_cluster_icon_list//m_color_cluster_text_list
        update_color_icon_and_rgba_sizer(i, m_cluster_colours[i]);
    }
    update_keep_color_buttons();
    m_scrolledWindow->Refresh();
}

void ObjColorPanel::draw_table()
{
    auto row                = std::max(m_cluster_colours.size(), m_colours.size()) + 1;
    m_gridsizer             = new wxGridSizer(row, 1, 1, 3); //(int rows, int cols, int vgap, int hgap );

    m_color_cluster_icon_list.clear();
    m_extruder_icon_list.clear();
    float row_height = 0;
    for (size_t ii = 0; ii < row; ii++) {
        wxPanel *row_panel = new wxPanel(m_scrolledWindow);
        row_panel->SetBackgroundColour(ii % 2 == 0 ? *wxWHITE : wxColour(238, 238, 238));
        auto row_sizer = new wxBoxSizer(wxHORIZONTAL);
        row_panel->SetSizer(row_sizer);

        row_panel->SetMinSize(wxSize(FromDIP(PANEL_WIDTH), -1));
        row_panel->SetMaxSize(wxSize(FromDIP(PANEL_WIDTH), -1));
        if (ii == 0) {
            wxStaticText *colors_left_title = new wxStaticText(row_panel, wxID_ANY, _L("Cluster colors"));
            colors_left_title->SetFont(Label::Head_14);
            row_sizer->Add(colors_left_title, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(40));
            row_sizer->AddStretchSpacer();

            wxStaticText *colors_middle_title = new wxStaticText(row_panel, wxID_ANY, _L("Map Filament"));
            colors_middle_title->SetFont(Label::Head_14);
            row_sizer->Add(colors_middle_title, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(HEADER_BORDER));
        } else {
            int id = ii - 1;
            if (id < m_cluster_colours.size()) {
                auto cluster_color_icon_sizer = create_color_icon_and_rgba_sizer(row_panel, id, m_cluster_colours[id]);
                row_sizer->Add(cluster_color_icon_sizer, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
                row_sizer->AddStretchSpacer();
                row_sizer->Add(create_result_button_sizer(row_panel, id), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(CONTENT_BORDER));
            }
        }
        row_height = row_panel->GetSize().GetHeight();
        if (ii>=1) {
            m_row_sizer_list.emplace_back(row_sizer);
        }
        m_gridsizer->Add(row_panel, 0, wxALIGN_LEFT | wxALL, FromDIP(HEADER_BORDER));
    }
    m_scrolledWindow->SetSizer(m_gridsizer);
    int totalHeight = row_height *(row+1) * 2;
    m_scrolledWindow->SetVirtualSize(MIN_OBJCOLOR_DIALOG_WIDTH, totalHeight);
    auto look = FIX_SCROLL_HEIGTH;
    if (totalHeight > FIX_SCROLL_HEIGTH) {
        m_scrolledWindow->SetMinSize(wxSize(MIN_OBJCOLOR_DIALOG_WIDTH, FIX_SCROLL_HEIGTH));
        m_scrolledWindow->SetMaxSize(wxSize(MIN_OBJCOLOR_DIALOG_WIDTH, FIX_SCROLL_HEIGTH));
    }
    else {
        m_scrolledWindow->SetMinSize(wxSize(MIN_OBJCOLOR_DIALOG_WIDTH, totalHeight));
    }
    m_scrolledWindow->EnableScrolling(false, true);
    m_scrolledWindow->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_DEFAULT);//wxSHOW_SB_ALWAYS
    m_scrolledWindow->SetScrollRate(20, 20);
    update_keep_color_buttons();
}

void ObjColorPanel::deal_algo(char cluster_number, bool redraw_ui)
{
    if (m_last_cluster_number == cluster_number) {
        return;
    }
    const char previous_cluster_number = m_last_cluster_number;
    m_last_cluster_number = cluster_number;
    QuantKMeans quant(10);
    const size_t existing_mixed = wxGetApp().preset_bundle != nullptr ?
        wxGetApp().preset_bundle->mixed_filaments.visible_count() : 0;
    const int printable_mix_slots = std::max(1,
        int(g_max_color) - int(m_colours.size()) - int(existing_mixed));
    const bool completed = quant.apply(
        m_input_colors,
        m_cluster_colors_from_algo,
        m_cluster_labels_from_algo,
        int(cluster_number),
        printable_mix_slots,
        2,
        [this](int current, int total) {
            return report_image_map_progress(ObjImageMapProgressStage::QuantizeColors,
                                             size_t(std::max(current, 0)),
                                             size_t(std::max(total, 1)));
        });
    if (!completed) {
        m_last_cluster_number = previous_cluster_number;
        return;
    }
    m_cluster_colours.clear();
    m_cluster_colours.reserve(m_cluster_colors_from_algo.size());
    for (size_t i = 0; i < m_cluster_colors_from_algo.size(); i++) {
        m_cluster_colours.emplace_back(convert_to_wxColour(m_cluster_colors_from_algo[i]));
    }
    if (m_cluster_colours.size() == 0) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",m_cluster_colours.size() = 0\n";
        return;
    }
    m_cluster_map_filaments.resize(m_cluster_colors_from_algo.size());
    m_color_cluster_num_by_algo = m_cluster_colors_from_algo.size();
    if (cluster_number == -1) {
        m_color_num_recommend = m_color_cluster_num_by_algo;
    }
    //redraw ui
    if (redraw_ui) {
        redraw_part_table();
        deal_default_strategy();
        rebuild_adaptive_cycle_spectrum_table();
    }
}

void ObjColorPanel::deal_default_strategy()
{
    if (m_colours.size() < 2) {
        deal_approximate_match_btn();
        return;
    }

    deal_add_btn();
    m_warning_text->SetLabelText(_L("Mixed-filament matches were selected. Adjust the quantization or mappings if needed."));
}

int ObjColorPanel::min_component_percent() const
{
    return m_min_component_percent_ctrl != nullptr ? std::clamp(m_min_component_percent_ctrl->GetValue(), 1, 49) : 15;
}

bool ObjColorPanel::uses_layer_sequence_image_map() const
{
    return m_image_map_mode_ctrl != nullptr && m_image_map_mode_ctrl->GetSelection() == 1;
}

bool ObjColorPanel::uses_adaptive_local_cycles_image_map() const
{
    return m_image_map_mode_ctrl != nullptr && m_image_map_mode_ctrl->GetSelection() == 2;
}

bool ObjColorPanel::adaptive_cycle_mixes_ready() const
{
    if (m_colours.size() < 2 || m_cluster_map_filaments.size() < m_cluster_colors_from_algo.size() ||
        m_cluster_colors_from_algo.empty() || !m_adaptive_cycle_preview_valid)
        return false;
    return std::all_of(m_cluster_map_filaments.begin(),
                       m_cluster_map_filaments.begin() + m_cluster_colors_from_algo.size(),
                       [](int filament_id) { return filament_id > 0; });
}

void ObjColorPanel::update_adaptive_cycle_spectra(const AdaptiveColorMatchPreviewResult* supplied_preview)
{
    m_adaptive_cycle_spectrum_colours.clear();
    m_adaptive_cycle_display_component_filament_ids.clear();
    m_adaptive_cycle_display_filament_ids.clear();
    m_adaptive_cycle_display_region_counts.clear();
    m_adaptive_direct_physical_region_count = 0;
    m_adaptive_cycle_preview_valid          = false;

    const bool adaptive_requested =
        uses_adaptive_local_cycles_image_map() ||
        (m_image_map_mode_ctrl == nullptr &&
         m_import_context.image_map_render_mode == ObjImageMapRenderMode::AdaptiveLocalizedCycles);
    if (!m_is_image_map || !adaptive_requested || m_cluster_colors_from_algo.empty())
        return;

    const std::vector<std::vector<RGBA>> representative = ImageMap::representative_labeled_source_colors(
        m_input_colors, m_cluster_labels_from_algo, m_cluster_colors_from_algo.size(), 64, 512);
    std::vector<std::string> physical_colors;
    physical_colors.reserve(m_colours.size());
    for (const wxColour& color : m_colours)
        physical_colors.emplace_back(color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
    const MixedFilamentDisplayContext context = build_mixed_filament_display_context(physical_colors);

    std::vector<wxColour> target_colors;
    target_colors.reserve(m_cluster_colors_from_algo.size());
    for (const RGBA& target : m_cluster_colors_from_algo)
        target_colors.emplace_back(convert_to_wxColour(target));
    AdaptiveColorMatchPreviewResult computed_preview;
    if (supplied_preview == nullptr) {
        computed_preview = preview_adaptive_localized_color_matches(
            target_colors, physical_colors, min_component_percent(), g_max_color);
        supplied_preview = &computed_preview;
    }
    const AdaptiveColorMatchPreviewResult& preview = *supplied_preview;
    if (!preview.valid)
        return;

    m_adaptive_cycle_preview_valid          = true;
    m_adaptive_direct_physical_region_count = preview.direct_physical_target_count;
    m_adaptive_cycle_spectrum_colours.reserve(preview.mixed_cycles.size());
    m_adaptive_cycle_display_component_filament_ids.reserve(preview.mixed_cycles.size());
    m_adaptive_cycle_display_filament_ids.reserve(preview.mixed_cycles.size());
    m_adaptive_cycle_display_region_counts.reserve(preview.mixed_cycles.size());

    for (const AdaptiveColorMatchPreviewCycle& cycle : preview.mixed_cycles) {
        std::vector<unsigned int> component_filament_ids;
        for (const MixedFilamentWeightedComponent& component : cycle.definition.recipe.blend.components) {
            if (component.percent > 0 && component.filament.id >= 1 && component.filament.id <= m_colours.size())
                component_filament_ids.emplace_back(component.filament.id);
        }

        std::vector<RGBA> represented_colors;
        for (const size_t target_index : cycle.target_indices) {
            if (target_index >= representative.size())
                continue;
            represented_colors.insert(represented_colors.end(),
                                      representative[target_index].begin(),
                                      representative[target_index].end());
        }
        represented_colors = ImageMap::representative_source_colors(represented_colors, 64, 512);
        std::vector<wxColour> attainable =
            build_adaptive_cycle_attainable_colors(component_filament_ids, represented_colors, context);

        m_adaptive_cycle_spectrum_colours.emplace_back(
            attainable.empty() ? wx_spectrum_colors(represented_colors) : std::move(attainable));
        m_adaptive_cycle_display_component_filament_ids.emplace_back(std::move(component_filament_ids));
        m_adaptive_cycle_display_filament_ids.emplace_back(cycle.filament_id);
        m_adaptive_cycle_display_region_counts.emplace_back(cycle.target_indices.size());
    }
}

void ObjColorPanel::rebuild_adaptive_cycle_spectrum_table()
{
    if (m_adaptive_spectrum_window == nullptr)
        return;

    m_adaptive_spectrum_window->Freeze();
    wxSizer* spectrum_sizer = m_adaptive_spectrum_window->GetSizer();
    if (spectrum_sizer == nullptr) {
        spectrum_sizer = new wxBoxSizer(wxVERTICAL);
        m_adaptive_spectrum_window->SetSizer(spectrum_sizer);
    } else {
        spectrum_sizer->Clear(true);
    }

    for (size_t cycle_index = 0; cycle_index < m_adaptive_cycle_spectrum_colours.size(); ++cycle_index) {
        std::vector<FilamentCardImageMap::ComponentFilament> component_filaments;
        if (cycle_index < m_adaptive_cycle_display_component_filament_ids.size()) {
            for (unsigned int filament_id : m_adaptive_cycle_display_component_filament_ids[cycle_index]) {
                if (filament_id >= 1 && filament_id <= m_colours.size())
                    component_filaments.emplace_back(filament_id, m_colours[filament_id - 1]);
            }
        }
        const unsigned int filament_id = cycle_index < m_adaptive_cycle_display_filament_ids.size() ?
                                             m_adaptive_cycle_display_filament_ids[cycle_index] :
                                             unsigned(m_colours.size() + cycle_index + 1);
        const size_t region_count = cycle_index < m_adaptive_cycle_display_region_counts.size() ?
                                        m_adaptive_cycle_display_region_counts[cycle_index] :
                                        size_t(0);
        auto* card = new FilamentCardImageMap(
            m_adaptive_spectrum_window,
            wxString::Format(_L("Mixed filament %u — %llu regions"),
                             filament_id,
                             static_cast<unsigned long long>(region_count)),
            m_adaptive_cycle_spectrum_colours[cycle_index],
            false,
            wxString::Format(
                _L("%llu adaptive color regions use this cycle. The gradient shows their KM/K-S-predicted attainable colors; "
                   "out-of-gamut image colors are projected to the closest attainable color."),
                static_cast<unsigned long long>(region_count)),
            std::move(component_filaments));
        card->SetMinSize(wxSize(FromDIP(PANEL_WIDTH - 20), FromDIP(34)));
        spectrum_sizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
    }

    const int total_height = std::max(FromDIP(40), int(m_adaptive_cycle_spectrum_colours.size()) * FromDIP(39));
    const int visible_height = std::min(total_height, FIX_SCROLL_HEIGTH);
    m_adaptive_spectrum_window->SetMinSize(wxSize(MIN_OBJCOLOR_DIALOG_WIDTH, visible_height));
    m_adaptive_spectrum_window->SetMaxSize(wxSize(MIN_OBJCOLOR_DIALOG_WIDTH, visible_height));
    m_adaptive_spectrum_window->SetVirtualSize(MIN_OBJCOLOR_DIALOG_WIDTH, total_height);
    m_adaptive_spectrum_window->EnableScrolling(false, true);
    m_adaptive_spectrum_window->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_DEFAULT);
    m_adaptive_spectrum_window->SetScrollRate(20, 20);
    m_adaptive_spectrum_window->Layout();
    m_adaptive_spectrum_window->FitInside();
    m_adaptive_spectrum_window->Thaw();
}

bool ObjColorPanel::report_image_map_progress(ObjImageMapProgressStage stage, size_t current, size_t total)
{
    if (!m_import_context.image_map_progress_fn)
        return true;
    const bool keep_going = m_import_context.image_map_progress_fn(stage, current, total);
    if (!keep_going) {
        if (auto* dialog = dynamic_cast<wxDialog*>(wxGetTopLevelParent(this)); dialog != nullptr && dialog->IsModal())
            dialog->EndModal(wxID_CANCEL);
    }
    return keep_going;
}

void ObjColorPanel::update_image_map_mode_ui()
{
    if (!m_is_image_map || m_image_map_mode_ctrl == nullptr)
        return;

    const bool layer_sequence = uses_layer_sequence_image_map();
    const bool adaptive_local_cycles = uses_adaptive_local_cycles_image_map();
    if (m_color_cluster_title != nullptr) {
        m_color_cluster_title->SetLabelText(
            adaptive_local_cycles ? _L("Adaptive color regions:") : _L("Quantized colors:"));
        m_color_cluster_title->SetToolTip(
            adaptive_local_cycles ?
                _L("Number of localized source-color regions. Several regions may share one physical mixed-filament cycle.") :
                _L("Number of source-color clusters mapped to printable filaments."));
    }
    if (m_quantized_settings_sizer != nullptr)
        m_quantized_settings_sizer->ShowItems(!layer_sequence);
    if (m_physical_title_sizer != nullptr)
        m_physical_title_sizer->ShowItems(!layer_sequence);
    if (m_physical_colors_sizer != nullptr)
        m_physical_colors_sizer->ShowItems(!layer_sequence);
    if (m_scrolledWindow != nullptr)
        m_scrolledWindow->Show(!layer_sequence && !adaptive_local_cycles);
    if (m_adaptive_spectrum_window != nullptr)
        m_adaptive_spectrum_window->Show(adaptive_local_cycles);
    if (m_quick_set_sizer != nullptr)
        m_quick_set_sizer->ShowItems(!layer_sequence && !adaptive_local_cycles);
    if (m_image_map_spectrum_sizer != nullptr)
        m_image_map_spectrum_sizer->ShowItems(layer_sequence);

    if (m_warning_text != nullptr) {
        if (!m_import_context.warning_message.empty()) {
            m_warning_text->SetLabelText(wxString::FromUTF8(m_import_context.warning_message));
        } else if (layer_sequence) {
            m_warning_text->SetLabelText(
                _L("Colors are sampled from the image source. One shared physical-filament sequence will reproduce them by perimeter exposure."));
        } else if (adaptive_local_cycles) {
            if (!m_adaptive_cycle_preview_valid) {
                m_warning_text->SetLabelText(
                    _L("Unable to preview the adaptive region-to-cycle mapping."));
            } else if (!adaptive_cycle_mixes_ready()) {
                m_warning_text->SetLabelText(
                    _L("Unable to generate every adaptive cycle within the 16 printable color slots. Reduce the adaptive color region count."));
            } else {
                wxString summary = wxString::Format(
                    _L("%llu adaptive color regions map to %llu unique mixed-filament cycles"),
                    static_cast<unsigned long long>(m_cluster_colors_from_algo.size()),
                    static_cast<unsigned long long>(m_adaptive_cycle_display_filament_ids.size()));
                if (m_adaptive_direct_physical_region_count > 0) {
                    summary += wxString::Format(
                        _L(" and %llu regions use a physical filament directly"),
                        static_cast<unsigned long long>(m_adaptive_direct_physical_region_count));
                }
                summary += _L(". Each gradient combines all source colors assigned to that cycle and shows the KM/K-S-predicted "
                              "attainable result. Perimeter modulation refines their exposure.");
                m_warning_text->SetLabelText(summary);
            }
        } else if (m_colours.size() >= 2) {
            m_warning_text->SetLabelText(
                _L("Mixed-filament matches were selected. Adjust the quantization or mappings if needed."));
        } else {
            m_warning_text->SetLabelText(wxEmptyString);
        }
        // wxStaticText reports the unwrapped label as its best width. Without
        // an explicit wrap, the adaptive summary makes the top-level Fit()
        // expand the import dialog across the screen.
        m_warning_text->Wrap(FromDIP(PANEL_WIDTH - 40));
    }

    m_page_simple->Layout();
    Layout();
    if (wxWindow* top_level = wxGetTopLevelParent(this); top_level != nullptr && top_level->GetSizer() != nullptr) {
        top_level->Layout();
        top_level->Fit();
    }
}

void ObjColorPanel::deal_add_btn()
{
    if (m_colours.size() >= g_max_color) { return; }
    deal_reset_btn();

    if (uses_adaptive_local_cycles_image_map()) {
        std::vector<std::string> physical_colors;
        physical_colors.reserve(m_colours.size());
        for (const wxColour& color : m_colours)
            physical_colors.emplace_back(color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString());

        std::vector<wxColour> target_colors;
        target_colors.reserve(m_cluster_colors_from_algo.size());
        for (const RGBA& target : m_cluster_colors_from_algo)
            target_colors.emplace_back(convert_to_wxColour(target));

        const AdaptiveColorMatchPreviewResult preview = preview_adaptive_localized_color_matches(
            target_colors, physical_colors, min_component_percent(), g_max_color);
        if (!preview.valid) {
            update_adaptive_cycle_spectra(&preview);
            m_warning_text->SetLabelText(_L("Unable to generate the adaptive region-to-cycle mapping."));
            update_keep_color_buttons();
            return;
        }

        std::map<unsigned int, int> cycle_selections;
        for (const AdaptiveColorMatchPreviewCycle& cycle : preview.mixed_cycles) {
            if (cycle.target_indices.empty() || cycle.target_indices.front() >= target_colors.size())
                continue;
            const int selection = append_new_filament_color_option(target_colors[cycle.target_indices.front()]);
            if (selection == 0) {
                m_warning_text->SetLabelText(
                    _L("The adaptive cycles would exceed the 16 printable color slots."));
                update_keep_color_buttons();
                return;
            }
            cycle_selections.emplace(cycle.filament_id, selection);
        }

        for (size_t region_index = 0;
             region_index < preview.target_filament_ids.size() && region_index < m_result_icon_list.size();
             ++region_index) {
            const unsigned int preview_filament_id = preview.target_filament_ids[region_index];
            int selection = int(preview_filament_id);
            if (preview_filament_id > m_colours.size()) {
                const auto found = cycle_selections.find(preview_filament_id);
                if (found == cycle_selections.end()) {
                    m_warning_text->SetLabelText(
                        _L("Unable to map one or more adaptive color regions to a physical cycle."));
                    update_keep_color_buttons();
                    return;
                }
                selection = found->second;
            }
            m_result_icon_list[region_index]->bitmap_combox->SetSelection(selection);
            m_cluster_map_filaments[region_index] = selection;
        }

        update_adaptive_cycle_spectra(&preview);
        update_keep_color_buttons();
        return;
    }

    bool is_exceed = false;
    std::vector<int> appended_selections;
    appended_selections.reserve(m_cluster_colors_from_algo.size());
    for (size_t i = 0; i < m_cluster_colors_from_algo.size(); i++) {
        const wxColour cur_color  = convert_to_wxColour(m_cluster_colors_from_algo[i]);
        const int selection = append_new_filament_option(cur_color);
        if (selection == 0) {
            is_exceed = true;
            break;
        }
        appended_selections.emplace_back(selection);
    }
    if (is_exceed) {
        deal_approximate_match_btn();
        m_warning_text->SetLabelText(_L("Some colors could not be generated within the 16 printable color slots."));
        return;
    }

    for (size_t i = 0; i < m_cluster_colours.size() && i < appended_selections.size(); i++) {
        m_result_icon_list[i]->bitmap_combox->SetSelection(appended_selections[i]);
        m_cluster_map_filaments[i] = appended_selections[i];
    }

    update_adaptive_cycle_spectra();
    update_keep_color_buttons();
}

void ObjColorPanel::deal_reset_btn()
{
    for (size_t i = 0; i < m_result_icon_list.size(); ++i) {
        auto *item = m_result_icon_list[i];
        // delete redundant bitmap
        while (item->bitmap_combox->GetCount() > m_colours.size()+ 1) {
            item->bitmap_combox->DeleteOneItem(item->bitmap_combox->GetCount() - 1);
        }
        item->bitmap_combox->SetSelection(0);
        if (i < m_cluster_map_filaments.size()) {
            m_cluster_map_filaments[i] = 0;
        }
    }
    m_new_add_colors.clear();
    m_warning_text->SetLabelText("");
    update_keep_color_buttons();
}

wxBoxSizer *ObjColorPanel::create_result_button_sizer(wxWindow *parent, int id)
{
    for (size_t i = m_result_icon_list.size(); i < id + 1; i++) {
        m_result_icon_list.emplace_back(new ButtonState());
    }

    m_result_icon_list[id]->bitmap_combox = CreateEditorCtrl(parent,id);
    auto *result_sizer = new wxBoxSizer(wxHORIZONTAL);
    result_sizer->Add(m_result_icon_list[id]->bitmap_combox, 0, wxALIGN_CENTER_VERTICAL, 0);

    StateColor calc_btn_bg(std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed), std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
                           std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));
    StateColor calc_btn_bd(std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));
    StateColor calc_btn_text(std::pair<wxColour, int>(wxColour(255, 255, 254), StateColor::Normal));

    auto *keep_color_btn = new Button(parent, _L("Create mix"));
    keep_color_btn->SetToolTip(_L("Generate a mixed filament for this quantized color."));
    keep_color_btn->SetFont(Label::Body_13);
    keep_color_btn->SetMinSize(wxSize(FromDIP(88), FromDIP(24)));
    keep_color_btn->SetCornerRadius(FromDIP(12));
    keep_color_btn->SetBackgroundColor(calc_btn_bg);
    keep_color_btn->SetBorderColor(calc_btn_bd);
    keep_color_btn->SetTextColor(calc_btn_text);
    keep_color_btn->Bind(wxEVT_BUTTON, [this, id](wxCommandEvent &) {
        deal_keep_color_btn(id);
    });
    m_result_icon_list[id]->keep_color_btn = keep_color_btn;

    result_sizer->AddSpacer(FromDIP(8));
    result_sizer->Add(keep_color_btn, 0, wxALIGN_CENTER_VERTICAL, 0);
    return result_sizer;
}

wxBoxSizer *ObjColorPanel::create_color_icon_and_rgba_sizer(wxWindow *parent, int id, const wxColour& color)
{
    auto      icon_sizer = new wxBoxSizer(wxHORIZONTAL);
    icon_sizer->AddSpacer(FromDIP(40));
    wxButton *icon       = new wxButton(parent, wxID_ANY, {}, wxDefaultPosition, ICON_SIZE, wxBORDER_NONE | wxBU_AUTODRAW);
    icon->SetBitmap(*get_extruder_color_icon(color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString(), std::to_string(id + 1), FromDIP(16), FromDIP(16)));
    icon->SetCanFocus(false);
    m_color_cluster_icon_list.emplace_back(icon);
    icon_sizer->Add(icon, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, 0); // wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM
    icon_sizer->AddSpacer(FromDIP(10));

    std::string   message    = get_color_str(color);
    wxStaticText *rgba_title = new wxStaticText(parent, wxID_ANY, message.c_str());
    m_color_cluster_text_list.emplace_back(rgba_title);
    rgba_title->SetMinSize(wxSize(FromDIP(COLOR_LABEL_WIDTH), -1));
    rgba_title->SetMaxSize(wxSize(FromDIP(COLOR_LABEL_WIDTH), -1));
    //rgba_title->SetFont(Label::Head_12);
    icon_sizer->Add(rgba_title, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, 0);
    return icon_sizer;
}

void ObjColorPanel::update_color_icon_and_rgba_sizer(int id, const wxColour &color)
{
    if (id < m_color_cluster_text_list.size()) {
        auto icon = m_color_cluster_icon_list[id];
        icon->SetBitmap(*get_extruder_color_icon(color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString(), std::to_string(id + 1), FromDIP(16), FromDIP(16)));
        std::string message = get_color_str(color);
        m_color_cluster_text_list[id]->SetLabelText(message.c_str());
    }
}
