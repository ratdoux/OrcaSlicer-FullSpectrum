#pragma once

#include <wx/colordlg.h>

namespace Slic3r
{
namespace GUI
{

/**
 * The platform color dialog with an additive exact-hex field on Windows.
 *
 * All native palette, HSL/RGB, luminosity, and custom-color behavior remains
 * owned by wxColourDialog. The hex value is synchronized with the native
 * selection and becomes the returned color when edited directly.
 */
class HexColourDialog : public wxColourDialog
{
public:
    HexColourDialog(wxWindow* parent, const wxColourData* data = nullptr);

    int ShowModal() override;

#ifdef _WIN32
    void MSWOnInitDone(WXHWND dialogHandle) override;
    void UpdateHexFromNativeEdit(WXHWND editHandle);
#endif

private:
    void OnColourChanged(wxColourDialogEvent& event);

private:
    wxColour _hexColour;
    bool _hexEdited { false };
    bool _syncingHex { false };

#ifdef _WIN32
    WXHWND _nativeDialog { nullptr };
    WXHWND _nativeHexEdit { nullptr };
#endif
};

} // namespace GUI
} // namespace Slic3r
