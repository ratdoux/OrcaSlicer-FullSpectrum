#ifndef slic3r_GUI_MFDBatchRecommendedAccordion_hpp_
#define slic3r_GUI_MFDBatchRecommendedAccordion_hpp_

#include "Widgets/Accordion.hpp"
#include <wx/wrapsizer.h>
#include <vector>
#include <functional>
#include <memory>
#include <unordered_map>

namespace Slic3r::GUI {

struct BatchMixItem;
class BatchSwatchTile;
class PhysicalFilamentTile;
class BatchCheckBox;

class CollapsibleSubSection : public wxPanel
{
public:
    CollapsibleSubSection(wxWindow* parent, const wxString& title);
    ~CollapsibleSubSection() override = default;

    wxPanel* get_body() const { return m_body; }

    void set_check_state(int state);
    int get_check_state() const;
    void set_info(int add_count, int del_count);
    void toggle();
    bool is_collapsed() const { return m_collapsed; }

    std::function<void(bool collapsed)> on_toggle;
    std::function<void()> on_check_clicked;

private:
    wxPanel* m_header{ nullptr };
    wxPanel* m_body{ nullptr };
    wxStaticText* m_lbl_title{ nullptr };
    wxStaticText* m_lbl_info{ nullptr };
    BatchCheckBox* m_check_box{ nullptr };

    bool m_collapsed{ false };

    void build_ui(const wxString& title);
    void on_header_click(wxMouseEvent& e);
    void on_paint(wxPaintEvent& e);
};

class MFDBatchRecommendedAccordion : public Accordion
{
public:
    MFDBatchRecommendedAccordion(wxWindow* parent, std::vector<BatchMixItem>& mix_items,
                                 const std::vector<std::pair<std::string, std::string>>& physical_filaments);
    ~MFDBatchRecommendedAccordion() override = default;

    void set_on_item_toggled(std::function<void()> cb) { m_on_item_toggled = std::move(cb); }

    void update_states();
    const std::vector<bool>& get_physical_enabled() const { return m_physical_enabled; }
    void wrap_explainer(int client_width);

private:
    wxStaticText* m_explainer_lbl{ nullptr };
    std::vector<BatchMixItem>& m_mix_items;
    const std::vector<std::pair<std::string, std::string>>& m_physical_filaments;
    std::function<void()> m_on_item_toggled;

    std::vector<bool> m_physical_enabled;

    wxWrapSizer* m_phys_sizer{ nullptr };
    std::vector<PhysicalFilamentTile*> m_phys_tiles;

    // Subaccordions
    CollapsibleSubSection* m_sub_50_50{ nullptr };
    CollapsibleSubSection* m_sub_34_66{ nullptr };
    CollapsibleSubSection* m_sub_3way{ nullptr };

    // Subaccordion wrap sizers
    wxWrapSizer* m_wrap_50_50{ nullptr };
    wxWrapSizer* m_wrap_34_66{ nullptr };
    wxWrapSizer* m_wrap_3way{ nullptr };

    // Subaccordion items maps
    std::vector<std::pair<BatchMixItem*, BatchSwatchTile*>> m_items_50_50;
    std::vector<std::pair<BatchMixItem*, BatchSwatchTile*>> m_items_34_66;
    std::vector<std::pair<BatchMixItem*, BatchSwatchTile*>> m_items_3way;

    void build_ui();
    void build_physical_row();
    void build_subaccordions();
    void build_tiles_for_subsection(CollapsibleSubSection* sub, wxWrapSizer* sizer,
                                     int mix_type, std::vector<std::pair<BatchMixItem*, BatchSwatchTile*>>& items_list);
    void update_subaccordion_state(CollapsibleSubSection* sub, const std::vector<std::pair<BatchMixItem*, BatchSwatchTile*>>& items_list);
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_MFDBatchRecommendedAccordion_hpp_
