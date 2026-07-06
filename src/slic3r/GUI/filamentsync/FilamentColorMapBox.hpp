#pragma once

#include <wx/panel.h>

#include "FilamentData.hpp"

namespace Slic3r
{
namespace GUI
{

class FilamentColorMapBox : public wxPanel
{
public:
    enum ButtonType
    {
        Above,
        Below
    };

    FilamentColorMapBox(wxWindow* parent, const FilamentData& aboveData, const FilamentData& belowData);

    void bindButton(FilamentInfoCallback cb, ButtonType type = ButtonType::Below);
    void setEnable(bool bEnable, ButtonType type = ButtonType::Below);

    void updateAboveData(const FilamentData& data);
    void updateBelowData(const FilamentData& data);

    const FilamentData& getAboveData() const;
    const FilamentData& getBelowData() const;

private:
    void onPaint(wxPaintEvent& event);
    void onLeftDown(wxMouseEvent& event);
    void onMouseMove(wxMouseEvent& event);
    void onMouseEnter(wxMouseEvent& event);
    void onMouseLeave(wxMouseEvent& event);
    void updateSizing();

    FilamentData m_aboveFilament;
    FilamentData m_belowFilament;

    bool m_bAboveEnabled = false;
    bool m_bBelowEnabled = true;

    // Hover tracking: -1=none, 0=above, 1=below
    int m_hoveredZone = -1;

    FilamentInfoCallback m_aboveCallback = nullptr;
    FilamentInfoCallback m_belowCallback = nullptr;
};

} // namespace GUI
} // namespace Slic3r
