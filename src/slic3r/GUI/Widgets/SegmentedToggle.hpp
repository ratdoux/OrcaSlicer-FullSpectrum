#pragma once

#include <functional>
#include <vector>

#include <wx/panel.h>
#include <wx/string.h>

#include "StateHandler.hpp"

class Button;
class StaticBox;

namespace Slic3r
{
namespace GUI
{

class SegmentedToggle : public wxPanel
{
public:
    using SelectionCallback = std::function<void(int index)>;

    SegmentedToggle(wxWindow* parent,
                    const std::vector<wxString>& options,
                    int selectedIndex = 0);

    void setSelected(int index);
    int  getSelected() const;

    void bindSelectionCallback(SelectionCallback cb);

private:
    void onButtonClicked(int index);

    StaticBox*             m_pContainer = nullptr;
    std::vector<Button*>   m_buttons;
    int                    m_selectedIndex = 0;
    SelectionCallback      m_selectionCallback = nullptr;

    StateColor m_unselectedBg;
    StateColor m_unselectedFg;
    StateColor m_selectedBg;
    StateColor m_selectedFg;
};

} // namespace GUI
} // namespace Slic3r
