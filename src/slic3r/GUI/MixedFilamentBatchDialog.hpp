#ifndef slic3r_GUI_MixedFilamentBatchDialog_hpp_
#define slic3r_GUI_MixedFilamentBatchDialog_hpp_

#include <wx/wx.h>
#include <wx/scrolwin.h>
#include <vector>
#include <string>
#include <utility>
#include <functional>
#include "GUI_Utils.hpp"
#include "libslic3r/MixedFilament.hpp"

namespace Slic3r::GUI {

class MFDBatchActiveAccordion;
class MFDBatchRecommendedAccordion;

struct BatchMixKey
{
    // 1-based physical_id, percent
    std::vector<std::pair<int, int>> components;

    bool operator==(const BatchMixKey& other) const
    {
        return components == other.components;
    }
};

struct BatchMixItem
{
    BatchMixKey key;
    std::vector<int> physical_indices; // 0-based
    std::vector<int> percentages;       // weights (e.g. 50, 50)
    wxColor color;
    wxString tooltip;
    wxString display_id;
    bool is_recommended = false;
    bool is_existing = false;
    bool is_deleted = false;
    bool is_added = false;
};

class BatchSwatchTile : public wxPanel
{
public:
    BatchSwatchTile(wxWindow* parent, BatchMixItem* item, std::function<void()> on_toggled);
    ~BatchSwatchTile() override = default;

private:
    BatchMixItem* m_item;
    std::function<void()> m_on_toggled;
    bool m_hovered{ false };

    void on_paint(wxPaintEvent& event);
    void on_enter(wxMouseEvent& event);
    void on_leave(wxMouseEvent& event);
    void on_left_up(wxMouseEvent& event);
};

class PhysicalFilamentTile : public wxPanel
{
public:
    PhysicalFilamentTile(wxWindow* parent, size_t index, const std::string& color_hex, const std::string& name, std::vector<bool>& enabled_ref, std::function<void()> on_toggled);
    ~PhysicalFilamentTile() override = default;

    void update_tooltip();

private:
    size_t m_index;
    std::string m_color_hex;
    std::string m_name;
    std::vector<bool>& m_enabled_ref;
    std::function<void()> m_on_toggled;
    bool m_hovered{ false };

    void on_paint(wxPaintEvent& event);
    void on_enter(wxMouseEvent& event);
    void on_leave(wxMouseEvent& event);
    void on_left_up(wxMouseEvent& event);
};

class BatchCheckBox : public wxPanel
{
public:
    BatchCheckBox(wxWindow* parent, std::function<void()> on_clicked);
    ~BatchCheckBox() override = default;

    void set_state(int state);
    int get_state() const { return m_state; }

private:
    std::function<void()> m_on_clicked;
    int m_state{ 0 }; // 0 = empty, 1 = Box, 2 = checked
    bool m_hovered{ false };

    void on_paint(wxPaintEvent& event);
    void on_enter(wxMouseEvent& event);
    void on_leave(wxMouseEvent& event);
    void on_left_up(wxMouseEvent& event);
};

class MixedFilamentBatchDialog : public DPIDialog
{
public:
    MixedFilamentBatchDialog(
        wxWindow* parent,
        const std::vector<std::pair<std::string, std::string>>& physical_filaments);

    ~MixedFilamentBatchDialog() = default;

    void apply_batch_changes();

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    // State
    const std::vector<std::pair<std::string, std::string>>& m_physical_filaments;
    std::vector<double>                                      m_physical_tds;
    std::vector<std::string>                                 m_physical_material_ids;
    std::vector<BatchMixItem>                                m_mix_items;

    // UI elements
    wxScrolledWindow* m_scroll_win{ nullptr };
    wxBoxSizer*       m_content_sizer{ nullptr };
    wxPanel*          m_footer_panel{ nullptr };
    wxStaticText*     m_info_label{ nullptr };
    wxTimer           m_resize_timer;

    // Accordions
    MFDBatchActiveAccordion*      m_active_accordion{ nullptr };
    MFDBatchRecommendedAccordion* m_recommended_accordion{ nullptr };

    void build_ui();
    void generate_items();
    void update_footer_info();

    BatchMixKey make_mix_key(const std::vector<int>& physical_indices, const std::vector<int>& percentages) const;
    BatchMixKey make_mix_key(const MixedFilamentDefinition& def) const;
    wxColor compute_mixed_color(const std::vector<int>& physical_indices, const std::vector<int>& percentages) const;
    wxString format_tooltip(const std::vector<int>& physical_indices, const std::vector<int>& percentages, const wxColor& mixed_color) const;
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_MixedFilamentBatchDialog_hpp_
