#ifndef slic3r_GUI_MFDTheme_hpp_
#define slic3r_GUI_MFDTheme_hpp_

#include <wx/colour.h>
#include <wx/window.h>

#include "Widgets/StateColor.hpp"

namespace Slic3r::GUI::MFDTheme {

inline wxColour dialog_background() { return StateColor::darkModeColorFor(*wxWHITE); }
inline wxColour content_background() { return StateColor::darkModeColorFor(wxColour("#F5F5F5")); }
inline wxColour card_background() { return StateColor::darkModeColorFor(*wxWHITE); }
inline wxColour card_border() { return StateColor::darkModeColorFor(wxColour("#EEEEEE")); }
inline wxColour divider() { return StateColor::darkModeColorFor(wxColour("#EBEBEB")); }
inline wxColour input_background() { return StateColor::darkModeColorFor(*wxWHITE); }
inline wxColour input_border() { return StateColor::darkModeColorFor(wxColour("#D1D5DC")); }
inline wxColour primary_text() { return StateColor::darkModeColorFor(wxColour("#242424")); }
inline wxColour secondary_text() { return StateColor::darkModeColorFor(wxColour("#4A4A4A")); }
inline wxColour muted_text() { return StateColor::darkModeColorFor(wxColour("#8F8F8F")); }
inline wxColour error_text() { return StateColor::darkModeColorFor(wxColour("#D32F2F")); }

inline void apply_window(wxWindow* window, const wxColour& bg = card_background())
{
    if (window)
        window->SetBackgroundColour(bg);
}

inline void apply_text(wxWindow* window, const wxColour& fg = primary_text(), const wxColour& bg = card_background())
{
    if (!window)
        return;
    window->SetForegroundColour(fg);
    window->SetBackgroundColour(bg);
}

inline void apply_input(wxWindow* window)
{
    if (!window)
        return;
    window->SetForegroundColour(primary_text());
    window->SetBackgroundColour(input_background());
}

} // namespace Slic3r::GUI::MFDTheme

#endif // slic3r_GUI_MFDTheme_hpp_
