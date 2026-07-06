#pragma once

#include <functional>
#include <vector>

#include <wx/panel.h>
#include <wx/popupwin.h>

#include "FilamentData.hpp"

namespace Slic3r
{
namespace GUI
{

class MachineFilamentPicker : public wxPopupTransientWindow
{
public:
    MachineFilamentPicker(wxWindow* parent,
                          const std::vector<FilamentData>& dataList,
                          unsigned int curIndex);

    FilamentData getSelectedData() const;
    unsigned int getSelectedIndex() const;

    void setSelectedIndex(unsigned int index);

    void popupAt(const wxPoint& pos);

    void bindSelectionCallback(FilamentInfoCallback cb);
    void bindOnDismissCallback(std::function<void()> cb);

    void OnDismiss() override;

private:
    std::vector<FilamentData> m_dataList;
    unsigned int              m_selectedIndex = 0;

    wxPanel* m_contentPanel = nullptr; // owned by wxWindow parent-child, no delete

    FilamentInfoCallback  m_selectionCallback  = nullptr;
    std::function<void()> m_onDismissCallback = nullptr;
};

} // namespace GUI
} // namespace Slic3r
