#include "HexColourDialog.hpp"

#include "I18N.hpp"

#ifdef _WIN32
#include <windows.h>
#include <CommCtrl.h>
#include <ColorDlg.h>
#endif

#include <algorithm>

namespace Slic3r
{
namespace GUI
{

#ifdef _WIN32
namespace
{

constexpr int HEX_EDIT_ID = 0x7F10;
constexpr UINT_PTR HEX_EDIT_SUBCLASS_ID = 0x484558;

RECT GetChildRect(HWND dialog, int controlId)
{
    RECT rect {};
    HWND control = ::GetDlgItem(dialog, controlId);
    if (control == nullptr || !::GetWindowRect(control, &rect))
        return RECT {};

    ::MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

void MoveChildDown(HWND dialog, int controlId, int offset)
{
    HWND control = ::GetDlgItem(dialog, controlId);
    if (control == nullptr)
        return;

    const RECT rect = GetChildRect(dialog, controlId);
    ::SetWindowPos(control, nullptr, rect.left, rect.top + offset, rect.right - rect.left,
                   rect.bottom - rect.top, SWP_NOACTIVATE | SWP_NOZORDER);
}

LRESULT CALLBACK HexEditSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                 UINT_PTR subclassId, DWORD_PTR referenceData)
{
    HexColourDialog* dialog = reinterpret_cast<HexColourDialog*>(referenceData);
    const LRESULT result = ::DefSubclassProc(window, message, wParam, lParam);

    switch (message)
    {
    case WM_CHAR:
    case WM_PASTE:
    case WM_CUT:
    case WM_CLEAR:
    case WM_UNDO:
        if (dialog != nullptr)
            dialog->UpdateHexFromNativeEdit(reinterpret_cast<WXHWND>(window));
        break;
    case WM_NCDESTROY:
        ::RemoveWindowSubclass(window, HexEditSubclass, subclassId);
        break;
    default:
        break;
    }

    return result;
}

} // namespace
#endif

HexColourDialog::HexColourDialog(wxWindow* parent, const wxColourData* data)
    : wxColourDialog(parent, data)
{
    _hexColour = GetColourData().GetColour();
}

int HexColourDialog::ShowModal()
{
    _hexColour = GetColourData().GetColour();
    _hexEdited = false;
    Bind(wxEVT_COLOUR_CHANGED, &HexColourDialog::OnColourChanged, this);

    const int result = wxColourDialog::ShowModal();

    Unbind(wxEVT_COLOUR_CHANGED, &HexColourDialog::OnColourChanged, this);
    if (result == wxID_OK && _hexEdited && _hexColour.IsOk())
        GetColourData().SetColour(_hexColour);

#ifdef _WIN32
    _nativeDialog = nullptr;
    _nativeHexEdit = nullptr;
#endif
    return result;
}

void HexColourDialog::OnColourChanged(wxColourDialogEvent& event)
{
    const wxColour color = event.GetColour();
    if (!color.IsOk())
        return;

    _hexColour = color;
    _hexEdited = false;

#ifdef _WIN32
    if (_nativeHexEdit != nullptr && !_syncingHex)
    {
        _syncingHex = true;
        const wxString hex = color.GetAsString(wxC2S_HTML_SYNTAX);
        ::SetWindowTextW(reinterpret_cast<HWND>(_nativeHexEdit), hex.wc_str());
        _syncingHex = false;
    }
#endif
    event.Skip();
}

#ifdef _WIN32
void HexColourDialog::MSWOnInitDone(WXHWND dialogHandle)
{
    wxColourDialog::MSWOnInitDone(dialogHandle);

    HWND dialog = reinterpret_cast<HWND>(dialogHandle);
    HWND addButton = ::GetDlgItem(dialog, COLOR_ADD);
    if (addButton == nullptr)
        return;

    const RECT addRect = GetChildRect(dialog, COLOR_ADD);
    const int buttonHeight = addRect.bottom - addRect.top;
    const int buttonWidth = addRect.right - addRect.left;
    if (buttonHeight <= 0 || buttonWidth <= 0)
        return;

    const UINT dpi = ::GetDpiForWindow(dialog);
    const int margin = std::max(4, ::MulDiv(4, static_cast<int>(dpi), 96));
    const int rowHeight = buttonHeight;
    const int addedHeight = rowHeight + margin;

    RECT windowRect {};
    ::GetWindowRect(dialog, &windowRect);
    const int windowWidth = windowRect.right - windowRect.left;
    const int windowHeight = windowRect.bottom - windowRect.top;
    const int newTop = std::max(0, static_cast<int>(windowRect.top) - addedHeight / 2);
    ::SetWindowPos(dialog, nullptr, windowRect.left, newTop, windowWidth, windowHeight + addedHeight,
                   SWP_NOACTIVATE | SWP_NOZORDER);

    MoveChildDown(dialog, IDOK, addedHeight);
    MoveChildDown(dialog, IDCANCEL, addedHeight);
    MoveChildDown(dialog, COLOR_ADD, addedHeight);

    const int labelWidth = ::MulDiv(38, static_cast<int>(dpi), 96);
    HWND label = ::CreateWindowExW(0, L"STATIC", _L("Hex:").wc_str(),
                                   WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                                   addRect.left, addRect.top, labelWidth, rowHeight,
                                   dialog, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    HWND edit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_UPPERCASE,
                                  addRect.left + labelWidth, addRect.top,
                                  std::max(1, buttonWidth - labelWidth), rowHeight,
                                  dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(HEX_EDIT_ID)),
                                  ::GetModuleHandleW(nullptr), nullptr);
    if (label == nullptr || edit == nullptr)
        return;

    HFONT font = reinterpret_cast<HFONT>(::SendMessageW(addButton, WM_GETFONT, 0, 0));
    if (font != nullptr)
    {
        ::SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        ::SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }

    ::SendMessageW(edit, EM_SETLIMITTEXT, 7, 0);
    const wxColour initial = GetColourData().GetColour();
    const wxString initialHex = initial.IsOk() ? initial.GetAsString(wxC2S_HTML_SYNTAX) : "#000000";
    _syncingHex = true;
    ::SetWindowTextW(edit, initialHex.wc_str());
    _syncingHex = false;
    ::SetWindowSubclass(edit, HexEditSubclass, HEX_EDIT_SUBCLASS_ID, reinterpret_cast<DWORD_PTR>(this));

    _nativeDialog = reinterpret_cast<WXHWND>(dialog);
    _nativeHexEdit = reinterpret_cast<WXHWND>(edit);
}

void HexColourDialog::UpdateHexFromNativeEdit(WXHWND editHandle)
{
    if (_syncingHex || editHandle == nullptr)
        return;

    wchar_t buffer[16] {};
    ::GetWindowTextW(reinterpret_cast<HWND>(editHandle), buffer, static_cast<int>(std::size(buffer)));
    wxString text(buffer);
    text.Trim().Trim(false);
    if (text.length() == 6 && !text.StartsWith("#"))
        text.Prepend("#");

    const wxColour color(text);
    if (text.length() != 7 || !text.StartsWith("#") || !color.IsOk())
    {
        _hexEdited = false;
        return;
    }

    _hexColour = color;
    _hexEdited = true;

    HWND dialog = reinterpret_cast<HWND>(_nativeDialog);
    if (dialog == nullptr)
        return;

    _syncingHex = true;
    ::SetDlgItemInt(dialog, COLOR_RED, color.Red(), FALSE);
    ::SetDlgItemInt(dialog, COLOR_GREEN, color.Green(), FALSE);
    ::SetDlgItemInt(dialog, COLOR_BLUE, color.Blue(), FALSE);
    HWND blueEdit = ::GetDlgItem(dialog, COLOR_BLUE);
    ::SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(COLOR_BLUE, EN_CHANGE), reinterpret_cast<LPARAM>(blueEdit));
    _syncingHex = false;
}
#endif

} // namespace GUI
} // namespace Slic3r
