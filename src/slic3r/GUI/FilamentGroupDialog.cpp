#include "FilamentGroupDialog.hpp"

#include "FlowTypeHelper.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "HighFlowCompat.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "wxExtensions.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StaticBox.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/dnd.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

#include <algorithm>
#include <functional>
#include <set>

namespace Slic3r { namespace GUI {

namespace {

const char *DRAG_PREFIX = "sm_filament:";

// Colour-chip grid geometry, shared by the group boxes and the scroll sizing.
constexpr int kChipCols    = 8;   // chips per row
constexpr int kChipCellW   = 39;  // chip cell width  (DIP)
constexpr int kChipCellH   = 38;  // chip cell height (DIP)
constexpr int kChipVGap    = 8;   // gap between chip rows (DIP)
constexpr int kVisibleRows = 3;   // rows visible before the group scrolls

// 20x20 colour block with the filament number inside and the material name below.
class FilamentChip : public wxPanel
{
public:
    FilamentChip(wxWindow *parent, size_t filament_idx, const wxColour &color, const wxString &label)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition,
                  wxSize(parent->FromDIP(kChipCellW), parent->FromDIP(kChipCellH)))
        , m_idx(filament_idx)
        , m_color(color)
        , m_label(label)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &FilamentChip::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, &FilamentChip::on_left_down, this);
    }

private:
    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();

        const int block = FromDIP(20);
        const int x     = (GetSize().x - block) / 2;
        dc.SetPen(wxPen(wxColour("#DBDBDA")));
        dc.SetBrush(wxBrush(m_color));
        dc.DrawRectangle(x, 0, block, block);

        // White number on dark colours, black on light ones.
        const double lum = 0.299 * m_color.Red() + 0.587 * m_color.Green() + 0.114 * m_color.Blue();
        dc.SetTextForeground(lum < 160.0 ? *wxWHITE : *wxBLACK);
        dc.SetFont(Label::Body_13);
        const wxString num  = wxString::Format("%zu", m_idx + 1);
        const wxSize   next = dc.GetTextExtent(num);
        dc.DrawText(num, x + (block - next.x) / 2, (block - next.y) / 2);

        // Material label. #242424 is the registered "primary text" dark-map key
        // (-> #E4E4E7 in dark); the previous #333333 had no dark mapping and stayed
        // near-invisible on the dark card. Figma dark node 29771:88711.
        dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#242424")));
        dc.SetFont(Label::Body_10);
        wxSize lext = dc.GetTextExtent(m_label);
        if (lext.x > GetSize().x)
        {
            dc.SetFont(Label::Body_9);
            lext = dc.GetTextExtent(m_label);
        }
        if (lext.x > GetSize().x)
        {
            dc.SetFont(Label::Body_8);
            lext = dc.GetTextExtent(m_label);
        }
        dc.DrawText(m_label, (GetSize().x - lext.x) / 2, block + FromDIP(2));
    }

    void on_left_down(wxMouseEvent &)
    {
        wxTextDataObject data(wxString(DRAG_PREFIX) + wxString::Format("%zu", m_idx));
        wxDropSource     source(data, this);
        source.DoDragDrop(wxDrag_CopyOnly);
    }

    size_t   m_idx;
    wxColour m_color;
    wxString m_label;
};

// 20x20 swap button that swaps the contents of the standard and high flow groups.
class SwapButton : public wxPanel
{
public:
    SwapButton(wxWindow *parent, std::function<void()> on_click)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition,
                  wxSize(parent->FromDIP(20), parent->FromDIP(20)))
        , m_on_click(std::move(on_click))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        SetToolTip(_L("Swap the two groups"));
        m_bitmap = ScalableBitmap(this, "icon_swap_groups", 14);
        Bind(wxEVT_PAINT, &SwapButton::on_paint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
            if (m_on_click)
                m_on_click();
        });
    }

private:
    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();

        // Figma node 29771:88805: the icon sits on a circular #36363d card (light
        // mode: #F4F4F4). GDI has no anti-aliasing, so on MSW draw the circle onto an
        // off-screen bitmap through a wxGCDC and blit it, matching StaticBox::render.
        const wxSize   size      = GetSize();
        const int      d         = std::min(size.x, size.y);
        const wxColour circle_bg = StateColor::darkModeColorFor(wxColour("#F4F4F4"));
        const wxPoint  origin((size.x - d) / 2, (size.y - d) / 2);
#ifdef __WXMSW__
        wxMemoryDC memdc(&dc);
        if (memdc.IsOk()) {
            wxBitmap layer(size.x, size.y);
            memdc.SelectObject(layer);
            memdc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
            memdc.Clear();
            {
                wxGCDC gcdc(memdc);
                gcdc.SetPen(*wxTRANSPARENT_PEN);
                gcdc.SetBrush(wxBrush(circle_bg));
                gcdc.DrawEllipse(origin.x, origin.y, d, d);
            }
            memdc.SelectObject(wxNullBitmap);
            dc.DrawBitmap(layer, 0, 0);
        } else {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(circle_bg));
            dc.DrawEllipse(origin.x, origin.y, d, d);
        }
#else
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(circle_bg));
        dc.DrawEllipse(origin.x, origin.y, d, d);
#endif
        const wxBitmap &bmp = m_bitmap.bmp();
        dc.DrawBitmap(bmp, (size.x - bmp.GetWidth()) / 2, (size.y - bmp.GetHeight()) / 2);
    }

    ScalableBitmap       m_bitmap;
    std::function<void()> m_on_click;
};

class GroupDropTarget : public wxTextDropTarget
{
public:
    GroupDropTarget(FilamentGroupDialog *dialog, bool high_flow) : m_dialog(dialog), m_high_flow(high_flow) {}

    bool OnDropText(wxCoord, wxCoord, const wxString &text) override
    {
        const wxString prefix = wxString(DRAG_PREFIX);
        if (!text.StartsWith(prefix))
            return false;
        unsigned long idx = 0;
        if (!text.Mid(prefix.size()).ToULong(&idx))
            return false;
        m_dialog->move_filament(size_t(idx), m_high_flow);
        return true;
    }

private:
    FilamentGroupDialog *m_dialog;
    bool                 m_high_flow;
};

// Resize the scrollable region's virtual area to the chip grid so the vertical
// scrollbar appears once the colours overflow the fixed visible height.
void update_group_scroll(wxScrolledWindow *scroll, wxFlexGridSizer *grid)
{
    scroll->Layout();
    const int count     = int(grid->GetItemCount());
    const int rows      = (count + kChipCols - 1) / kChipCols;
    const int grid_w    = scroll->FromDIP(kChipCellW) * kChipCols;
    const int content_h = rows > 0 ? scroll->FromDIP(kChipCellH) * rows + scroll->FromDIP(kChipVGap) * (rows - 1) : 0;
    scroll->SetVirtualSize(grid_w, content_h);
}

} // anonymous namespace

FilamentGroupDialog::FilamentGroupDialog(wxWindow *parent)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY,
                _L("Custom Filament Grouping"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    const wxColour dlg_bg = StateColor::darkModeColorFor(*wxWHITE);
    SetBackgroundColour(dlg_bg);
    SetFont(Label::Body_14);
    // Figma dark spec (node 29771:88711) uses pure-white text and a bright teal
    // Confirm button; the app's normal dark theme differs, so branch on the theme.
    const bool is_dark = wxGetApp().dark_mode();

    load_filaments();
    m_mapping = wxGetApp().preset_bundle->get_filament_volume_types();
    m_mapping.resize(wxGetApp().preset_bundle->filament_presets.size(), fvtStandard);

    auto *v_sizer = new wxBoxSizer(wxVERTICAL);

    auto *intro = new wxStaticText(this, wxID_ANY, _L("Slicing will follow the nozzle assignment below:"));
    // The hint is plain text on the dialog (no card). Match the dialog background so
    // wxMSW does not fill the static-text rectangle with the system face colour.
    intro->SetBackgroundColour(dlg_bg);
    intro->SetForegroundColour(is_dark ? wxColour("#FFFFFF") : wxColour("#4A4A4A"));
    v_sizer->Add(intro, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    // Group panel per Figma 27673:62102: #F3F3F3 rounded-8 background box,
    // title with 16px inset, chip grid with 8px inset. The chip grid lives in a
    // fixed-height scrolled window: with many filaments the colours overflow into
    // a scrollable region instead of being clipped (previously the box height was
    // pinned by SetMinSize, so any row past the ~2 that fit was cut off). A
    // vertical-scrollbar gutter is reserved so the 8-column grid still fits once
    // the scrollbar appears.
    auto make_group = [this, is_dark](const wxString &title, wxFlexGridSizer *&grid,
                             wxScrolledWindow *&scroll, bool high_flow) -> StaticBox * {
        const int grid_w = FromDIP(kChipCellW) * kChipCols;
        int       sb_w   = wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, this);
        if (sb_w <= 0)
            sb_w = FromDIP(16);
        const int scroll_w = grid_w + sb_w;
        const int scroll_h = FromDIP(kChipCellH) * kVisibleRows + FromDIP(kChipVGap) * (kVisibleRows - 1);
        const int box_w    = scroll_w + FromDIP(16); // 8px inset on each side

        StaticBox *box = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxSize(box_w, -1));
        box->SetCornerRadius(FromDIP(8));
        box->SetBorderWidth(0);
        // Figma dark node 29771:88711: the nozzle cards are #36363d. #F4F4F4 is the
        // registered dark-map key that resolves to #36363D; the bare #F3F3F3 had no
        // dark mapping, so the cards previously stayed light grey in dark mode.
        const wxColour box_bg = StateColor::darkModeColorFor(wxColour("#F4F4F4"));
        box->SetBackgroundColorNormal(box_bg);
        // Also set the plain wx background so children (title / chips) inherit it.
        box->SetBackgroundColour(box_bg);

        auto *box_sizer = new wxBoxSizer(wxVERTICAL);
        auto *label     = new wxStaticText(box, wxID_ANY, title);
        label->SetFont(Label::Body_14);
        label->SetBackgroundColour(box_bg);
        // Card title is white in the Figma dark design (node 29771:88711).
        label->SetForegroundColour(is_dark ? wxColour("#FFFFFF") : wxColour("#242424"));
        box_sizer->Add(label, 0, wxLEFT | wxTOP | wxRIGHT, FromDIP(16));
        box_sizer->AddSpacer(FromDIP(16));

        scroll = new wxScrolledWindow(box, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        scroll->SetBackgroundColour(box_bg);
        scroll->SetMinSize(wxSize(scroll_w, scroll_h));
        scroll->SetMaxSize(wxSize(scroll_w, scroll_h));
        scroll->EnableScrolling(false, true);
        scroll->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_DEFAULT);
        scroll->SetScrollRate(0, FromDIP(10));
        grid = new wxFlexGridSizer(0, kChipCols, FromDIP(kChipVGap), 0);
        scroll->SetSizer(grid); // chips left-aligned, matching the original layout
        box_sizer->Add(scroll, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        box->SetSizer(box_sizer);
        // Pin the width only; the height follows the fixed-height chip area so the
        // box (and thus the dialog) keeps a constant size regardless of chip count.
        box->SetMinSize(wxSize(box_w, wxDefaultCoord));
        // Accept chip drops anywhere on the card (title strip and chip area).
        box->SetDropTarget(new GroupDropTarget(this, high_flow));
        scroll->SetDropTarget(new GroupDropTarget(this, high_flow));
        return box;
    };

    auto *groups_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_std_box          = make_group(_L("Standard Nozzle"), m_std_grid, m_std_scroll, false);
    m_high_box         = make_group(_L("High Flow Nozzle"), m_high_grid, m_high_scroll, true);

    auto *swap = new SwapButton(this, [this]() { swap_groups(); });

    groups_sizer->Add(m_std_box, 0, wxALIGN_TOP);
    groups_sizer->Add(swap, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(16));
    groups_sizer->Add(m_high_box, 0, wxALIGN_TOP);
    v_sizer->Add(groups_sizer, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    auto *tip = new wxStaticText(this, wxID_ANY, _L("Tip: Drag and drop filaments to assign them to a different nozzle."));
    tip->SetBackgroundColour(dlg_bg);
    tip->SetForegroundColour(is_dark ? wxColour("#FFFFFF") : wxColour("#4A4A4A"));
    v_sizer->Add(tip, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_warning_sizer = new wxBoxSizer(wxVERTICAL);
    v_sizer->Add(m_warning_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    auto *dlg_btns = new DialogButtons(this, {"Cancel", "Confirm"});
    m_confirm_button = dlg_btns->GetCONFIRM();
    // Figma dark spec (node 29771:88711) keeps the Confirm button at the bright ORCA
    // teal instead of the app's muted dark accent. StateColor dark-maps #009688 ->
    // #00675b for every Button, so to brighten only this dialog we override the fill
    // with teals the dark map does not contain (perceptually the Figma #009688). This
    // modal is short-lived, so the default style is never re-applied over it.
    if (is_dark) {
        const wxColour teal(0, 150, 137);          // ~#009688, not a dark-map key
        const wxColour teal_hover(0, 170, 155);    // hover, slightly lighter
        const wxColour teal_pressed(0, 128, 116);  // pressed, slightly darker
        m_confirm_button->SetBackgroundColor(StateColor(
            std::make_pair(teal,                (int) StateColor::NotHovered),
            std::make_pair(wxColour("#DFDFDF"), (int) StateColor::Disabled),
            std::make_pair(teal_pressed,        (int) StateColor::Pressed),
            std::make_pair(teal_hover,          (int) StateColor::Hovered),
            std::make_pair(teal,                (int) StateColor::Normal),
            std::make_pair(teal,                (int) StateColor::Enabled)));
    }
    m_confirm_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        FlowType::apply_custom_mapping(m_mapping);
        EndModal(wxID_OK);
    });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    v_sizer->Add(dlg_btns, 0, wxEXPAND | wxALL, FromDIP(8));

    SetSizer(v_sizer);
    rebuild_chips();
    update_warnings();
    Fit();
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void FilamentGroupDialog::load_filaments()
{
    PresetBundle &bundle = *wxGetApp().preset_bundle;
    const auto   *colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour");

    // Requirement: list only the filaments actually used by objects in the scene
    // (across all plates), not every filament configured in the sidebar. When the
    // used set cannot be determined (empty scene), fall back to all filaments.
    std::set<int> used; // 1-based filament ids
    if (Plater *plater = wxGetApp().plater(); plater != nullptr)
        used = plater->get_partplate_list().get_extruders(true);

    for (size_t i = 0; i < bundle.filament_presets.size(); ++i) {
        if (!used.empty() && used.find(int(i) + 1) == used.end())
            continue;
        FilamentInfo info;
        info.filament_idx = i;
        info.color = colors != nullptr && i < colors->values.size() ? wxColour(from_u8(colors->values[i])) : *wxWHITE;
        const Preset *preset = bundle.filaments.find_preset(bundle.filament_presets[i], false);
        info.type_raw    = preset != nullptr ? preset->config.opt_string("filament_type", 0u) : std::string("PLA");
        info.preset_name = bundle.filament_presets[i];
        info.label       = from_u8(info.type_raw);
        m_filaments.push_back(info);
    }
}

void FilamentGroupDialog::move_filament(size_t filament_idx, bool to_high_flow)
{
    if (filament_idx >= m_mapping.size())
        return;
    const FilamentVolumeType target = to_high_flow ? fvtHighFlow : fvtStandard;
    if (m_mapping[filament_idx] == target)
        return;
    m_mapping[filament_idx] = target;
    // The rebuild destroys and recreates every chip in both grids; freezing
    // the dialog keeps those intermediate empty/partial states off screen so
    // dropping a chip does not flash.
    Freeze();
    rebuild_chips();
    update_warnings();
    Thaw();
}

void FilamentGroupDialog::swap_groups()
{
    for (const FilamentInfo &info : m_filaments)
        m_mapping[info.filament_idx] =
            m_mapping[info.filament_idx] == fvtHighFlow ? fvtStandard : fvtHighFlow;
    // Same flash concern as move_filament(): rebuild invisibly, repaint once.
    Freeze();
    rebuild_chips();
    update_warnings();
    Thaw();
}

void FilamentGroupDialog::rebuild_chips()
{
    m_std_grid->Clear(true);
    m_high_grid->Clear(true);
    for (const FilamentInfo &info : m_filaments) {
        const bool        high   = m_mapping[info.filament_idx] == fvtHighFlow;
        wxScrolledWindow *parent = high ? m_high_scroll : m_std_scroll;
        wxFlexGridSizer  *grid   = high ? m_high_grid : m_std_grid;
        grid->Add(new FilamentChip(parent, info.filament_idx, info.color, info.label));
    }
    update_group_scroll(m_std_scroll, m_std_grid);
    update_group_scroll(m_high_scroll, m_high_grid);
    m_std_box->Layout();
    m_high_box->Layout();
    Layout();
    Fit();
}

void FilamentGroupDialog::update_warnings()
{
    m_warning_sizer->Clear(true);

    std::vector<HighFlowCompat::CompatibilityResult> warnings;
    bool has_unsupported = false;
    for (const FilamentInfo &info : m_filaments)
    {
        if (m_mapping[info.filament_idx] != fvtHighFlow)
            continue;

        const HighFlowCompat::CompatibilityResult result = HighFlowCompat::check(info.type_raw, info.preset_name);
        if (result.level == HighFlowCompat::CompatibilityLevel::Compatible)
            continue;

        has_unsupported |= result.level == HighFlowCompat::CompatibilityLevel::Unsupported;
        const auto duplicate = std::find_if(warnings.begin(), warnings.end(), [&result](const auto &warning) {
            return warning.level == result.level && warning.material == result.material;
        });
        if (duplicate == warnings.end())
            warnings.push_back(result);
    }

    const std::string diameter =
        wxGetApp().preset_bundle->printers.get_edited_preset().config.opt_string("printer_variant");
    wxString not_recommended_materials;
    wxString unsupported_materials;
    for (const HighFlowCompat::CompatibilityResult &warning : warnings)
    {
        wxString material;
        if (warning.material == "CF/GF filaments")
            material = _L("CF/GF filaments");
        else
            material = from_u8(warning.material);

        wxString &materials = warning.level == HighFlowCompat::CompatibilityLevel::Unsupported ?
                                  unsupported_materials :
                                  not_recommended_materials;
        if (!materials.empty())
            materials += ", ";
        materials += material;
    }

    auto add_warning = [this](const wxString &message, bool is_error) {
        // Warning (non-error) colours use the registered dark-map keys #FFF3EB /
        // #FF842D; the previous #FFFAF2 / #FF8400 had no dark mapping and leaked
        // their light values into dark mode.
        const wxColour background = StateColor::darkModeColorFor(wxColour(is_error ? "#FDE8E8" : "#FFF3EB"));
        const wxColour foreground = StateColor::darkModeColorFor(wxColour(is_error ? "#D32F2F" : "#FF842D"));
        // Non-error ("not recommended") warnings use the orange round exclamation
        // from Figma 27673-62209 — not the triangle icon.
        const char *icon_name = is_error ? "error_icon_red_exclamation" : "warning_icon_orange_exclamation";

        StaticBox *bar = new StaticBox(this, wxID_ANY);
        bar->SetCornerRadius(FromDIP(4));
        bar->SetBorderWidth(0);
        bar->SetBackgroundColorNormal(background);
        bar->SetBackgroundColour(background);
        bar->SetMinSize(wxSize(-1, FromDIP(34)));

        auto *bar_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto *icon = new wxStaticBitmap(bar, wxID_ANY, create_scaled_bitmap(icon_name, bar, 14));
        auto *text = new wxStaticText(bar, wxID_ANY, message);
        text->Wrap(FromDIP(660));
        icon->SetBackgroundColour(background);
        text->SetBackgroundColour(background);
        text->SetForegroundColour(foreground);
        bar_sizer->Add(icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP | wxBOTTOM, FromDIP(8));
        bar_sizer->AddSpacer(FromDIP(10));
        bar_sizer->Add(text, 1, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM | wxRIGHT, FromDIP(8));
        bar->SetSizer(bar_sizer);
        m_warning_sizer->Add(bar, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    };

    if (!not_recommended_materials.empty())
    {
        add_warning(format_wxstr(_L("Warning:%1%mm High Flow nozzles are not recommended for %2%"),
                                 from_u8(diameter), not_recommended_materials),
                    false);
    }
    if (!unsupported_materials.empty())
    {
        add_warning(format_wxstr(_L("These filaments cannot be printed with the %1%mm high flow nozzle: %2%"),
                                 from_u8(diameter), unsupported_materials),
                    true);
    }

    m_confirm_button->Enable(!has_unsupported);
    Layout();
    Fit();
    Refresh();
}

void FilamentGroupDialog::on_dpi_changed(const wxRect &)
{
    Fit();
    Refresh();
}

}} // namespace Slic3r::GUI
