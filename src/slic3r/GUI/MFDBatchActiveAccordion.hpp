#ifndef slic3r_GUI_MFDBatchActiveAccordion_hpp_
#define slic3r_GUI_MFDBatchActiveAccordion_hpp_

#include "Widgets/Accordion.hpp"
#include <wx/wrapsizer.h>
#include <vector>
#include <functional>
#include <memory>

namespace Slic3r::GUI {

struct BatchMixItem;

class MFDBatchActiveAccordion : public Accordion
{
public:
    MFDBatchActiveAccordion(wxWindow* parent, std::vector<BatchMixItem>& mix_items);
    ~MFDBatchActiveAccordion() override = default;

    void set_on_item_toggled(std::function<void()> cb) { m_on_item_toggled = std::move(cb); }

    void update_states();

private:
    std::vector<BatchMixItem>& m_mix_items;
    std::function<void()> m_on_item_toggled;

    wxWrapSizer* m_wrap_sizer{ nullptr };
    std::vector<wxPanel*> m_tiles;

    void build_ui();
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_MFDBatchActiveAccordion_hpp_
