#pragma once

#include <functional>

#include <wx/panel.h>

class wxMouseEvent;

namespace Slic3r
{
namespace GUI
{

// Custom scrollbar for the filament sync dialog.
// 10 DIP wide track, 4 DIP wide thumb with 10 DIP corner radius.
class FilamentScrollBar : public wxPanel
{
public:
    // Pixels per mouse-wheel tick
    static constexpr int s_scrollStepLines = 30;

    // bgColor: background colour for the track area.
    FilamentScrollBar(wxWindow* parent, const wxColour& bgColor, wxWindowID id = wxID_ANY);

    void setScrollRange(int contentHeight, int viewportHeight);
    void setScrollOffset(int offset);
    int  getScrollOffset() const { return m_scrollOffset; }

    void setOnScroll(std::function<void(int)> callback) { m_onScroll = std::move(callback); }

private:
    void onPaint(wxPaintEvent& evt);
    void onMouseDown(wxMouseEvent& evt);
    void onMouseUp(wxMouseEvent& evt);
    void onMouseMove(wxMouseEvent& evt);

    int m_contentHeight    = 0;
    int m_viewportHeight   = 0;
    int m_scrollOffset     = 0;
    int m_thumbHeight      = 0;
    int m_thumbY           = 0;
    int m_dragStartY       = 0;
    int m_dragStartOffset  = 0;

    std::function<void(int)> m_onScroll;

    int  calcThumbHeight() const;
    int  calcThumbY() const;
    void onScroll(int newOffset);
};

} // namespace GUI
} // namespace Slic3r
