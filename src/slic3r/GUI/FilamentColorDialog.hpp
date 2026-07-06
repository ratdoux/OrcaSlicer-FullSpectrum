#pragma once

#include "GUI_Utils.hpp"
#include "libslic3r/FilamentColorLibrary.hpp"

#include <string>
#include <utility>
#include <vector>

class wxStaticBitmap;
class wxStaticText;

namespace Slic3r
{
namespace GUI
{

/**
 * @brief Dialog for choosing a built-in filament color or a custom color.
 */
class FilamentColorDialog : public DPIDialog
{
public:
    /**
     * @brief Creates the filament color dialog.
     */
    FilamentColorDialog(wxWindow* parent, const FilamentColorInfo& filament, const FilamentColor& currentColor);

    /**
     * @brief Gets the selected color result.
     */
    const FilamentColor& Selection() const
    {
        return _selection;
    }

private:
    void BuildUi();
    void SelectFilamentColor(const FilamentColorItem& colorItem);
    void SelectCustomColor(const std::string& color);
    void UpdatePreview();
    void UpdateSwatchSelection();
    void OpenMoreColorDialog();
    void PlaceNearFilamentPanel();
    void UpdateRoundedShape();
    void BindDragWindow(wxWindow* window);
    void StartDrag(wxMouseEvent& event);
    void DragDialog(wxMouseEvent& event);
    void EndDrag(wxMouseEvent& event);
    void on_dpi_changed(const wxRect& suggestedRect) override;

private:
    FilamentColorInfo _filament;
    std::string _languageCode;
    FilamentColor _selection;
    std::string _selectedSku;
    std::string _highlightSku;
    std::vector<std::pair<wxWindow*, std::string>> _swatchBySku;
    wxStaticBitmap* _previewBitmap { nullptr };
    wxStaticText* _nameLabel { nullptr };
    wxStaticText* _skuLabel { nullptr };
    bool _dragPending { false };
    bool _isDragging { false };
    wxPoint _dragStartMouse;
    wxPoint _dragStartPosition;
};

} // namespace GUI
} // namespace Slic3r
