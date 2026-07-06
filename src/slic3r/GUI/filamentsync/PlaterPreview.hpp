#pragma once

#include <functional>

#include <wx/panel.h>
#include <wx/bitmap.h>
#include <wx/string.h>

class ComboBox;
class Label;

namespace Slic3r
{
namespace GUI
{

class PlaterPreview : public wxPanel
{
public:
    PlaterPreview(wxWindow* parent, unsigned int totalPlateCount = 1);

    void setOriginalPreview(const wxBitmap& thumbnail);
    void setCoverPreview(const wxBitmap& thumbnail);
    void updateCoverPreview(const wxBitmap& thumbnail);
    void setCoverLabel(const wxString& label);

    void setCurrentPlate(unsigned int plateIndex);
    unsigned int getCurrentPlate() const;

    void setTotalPlateCount(unsigned int count);

    void bindPlateSwitchCallback(std::function<void(unsigned int newPlateIndex)> cb);

private:
    void onLeftArrow(wxMouseEvent& event);
    void onRightArrow(wxMouseEvent& event);
    void onPlateComboBoxChanged(wxCommandEvent& event);

    void navigateTo(int index);
    void updateNavigation();

    void paintPreview(wxWindow* win, const wxBitmap& bmp);

    unsigned int m_currentPlateIndex = 0;
    unsigned int m_totalPlateCount   = 1;

    wxBitmap m_originalBitmap;
    wxBitmap m_coverBitmap;
    bool     m_isRescaling = false;

    wxPanel* m_pPreviewLeft  = nullptr;
    wxPanel* m_pPreviewRight = nullptr;
    wxPanel* m_pArrowLeft    = nullptr;
    wxPanel* m_pArrowRight   = nullptr;
    ComboBox* m_pPlateCombo  = nullptr;
    Label*   m_pDiskLabel    = nullptr;
    Label*   m_pLabelLeft    = nullptr;
    Label*   m_pLabelRight   = nullptr;

    std::function<void(unsigned int)> m_plateSwitchCallback = nullptr;
};

} // namespace GUI
} // namespace Slic3r
