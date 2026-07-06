#include "FilamentScrollBar.hpp"

#include <wx/dcclient.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/settings.h>

#include <algorithm>

#include "slic3r/GUI/Widgets/StateColor.hpp"

namespace
{

constexpr int g_scrollBarThumbW    = 4;  // DIP — thumb width
constexpr int g_scrollBarMinThumbH = 20; // DIP — minimum thumb height

// Thumb colour
const wxColour g_scrollBarThumbColor(0xB6, 0xB6, 0xB6);

} // namespace

namespace Slic3r
{
namespace GUI
{

FilamentScrollBar::FilamentScrollBar(wxWindow* parent, const wxColour& bgColor, wxWindowID id)
    : wxPanel(parent, id, wxDefaultPosition, wxDefaultSize,
              wxBORDER_NONE | wxFULL_REPAINT_ON_RESIZE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(bgColor);
    SetDoubleBuffered(true);
    Bind(wxEVT_PAINT, &FilamentScrollBar::onPaint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent&) {
        if (m_contentHeight > 0 && m_viewportHeight > 0) {
            m_thumbHeight = calcThumbHeight();
            m_thumbY      = calcThumbY();
            Refresh();
        }
    });
    Bind(wxEVT_LEFT_DOWN, &FilamentScrollBar::onMouseDown, this);
    Bind(wxEVT_LEFT_UP, &FilamentScrollBar::onMouseUp, this);
    Bind(wxEVT_MOTION, &FilamentScrollBar::onMouseMove, this);
}

void FilamentScrollBar::setScrollRange(int contentHeight, int viewportHeight)
{
    if (contentHeight <= 0 || viewportHeight <= 0)
        return;

    m_contentHeight  = contentHeight;
    m_viewportHeight = viewportHeight;

    int maxOffset = std::max(0, m_contentHeight - m_viewportHeight);
    if (m_scrollOffset > maxOffset)
        m_scrollOffset = maxOffset;
    if (m_scrollOffset < 0)
        m_scrollOffset = 0;

    m_thumbHeight = calcThumbHeight();
    m_thumbY      = calcThumbY();
    Refresh();
}

void FilamentScrollBar::setScrollOffset(int offset)
{
    int maxOffset = std::max(0, m_contentHeight - m_viewportHeight);
    int newOffset = std::max(0, std::min(offset, maxOffset));
    if (newOffset == m_scrollOffset)
        return;
    m_scrollOffset = newOffset;
    m_thumbY       = calcThumbY();
    Refresh();
}

int FilamentScrollBar::calcThumbHeight() const
{
    if (m_contentHeight <= m_viewportHeight)
        return GetClientSize().y;

    int   trackH = GetClientSize().y;
    float ratio  = static_cast<float>(m_viewportHeight) / static_cast<float>(m_contentHeight);
    return std::max(FromDIP(g_scrollBarMinThumbH), static_cast<int>(trackH * ratio));
}

int FilamentScrollBar::calcThumbY() const
{
    int trackH = GetClientSize().y;
    if (m_contentHeight <= m_viewportHeight || trackH <= m_thumbHeight)
        return 0;

    int   maxOffset = m_contentHeight - m_viewportHeight;
    float ratio     = static_cast<float>(m_scrollOffset) / static_cast<float>(maxOffset);
    return static_cast<int>((trackH - m_thumbHeight) * ratio);
}

void FilamentScrollBar::onPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.Clear();

    // If content fits viewport, nothing to draw
    if (m_contentHeight <= m_viewportHeight)
        return;

    wxSize sz = GetClientSize();
    if (sz.y <= 0)
        return;

    wxGCDC gdc(dc);

    int thumbW = FromDIP(g_scrollBarThumbW);
    int thumbX = (sz.x - thumbW) / 2; // center horizontally in the 10 DIP track

    gdc.SetPen(*wxTRANSPARENT_PEN);
    gdc.SetBrush(wxBrush(g_scrollBarThumbColor));

    if (m_thumbHeight <= thumbW) {
        // Thumb too short for a capsule — just draw a circle
        gdc.DrawEllipse(thumbX, m_thumbY + (m_thumbHeight - thumbW) / 2, thumbW, thumbW);
    } else {
        // Capsule = two half-circle caps connected by a rectangle
        int capR = thumbW / 2;
        // Top cap
        gdc.DrawEllipse(thumbX, m_thumbY, thumbW, thumbW);
        // Body
        gdc.DrawRectangle(thumbX, m_thumbY + capR, thumbW, m_thumbHeight - thumbW);
        // Bottom cap
        gdc.DrawEllipse(thumbX, m_thumbY + m_thumbHeight - thumbW, thumbW, thumbW);
    }
}

void FilamentScrollBar::onMouseDown(wxMouseEvent& evt)
{
    int y = evt.GetPosition().y;

    if (y >= m_thumbY && y <= m_thumbY + m_thumbHeight) {
        // Click on thumb — start drag
        m_dragStartY      = y;
        m_dragStartOffset = m_scrollOffset;
        CaptureMouse();
    } else {
        // Click in track — page scroll
        int trackH = GetClientSize().y;
        if (m_contentHeight <= m_viewportHeight || trackH <= 0)
            return;

        int maxOffset = m_contentHeight - m_viewportHeight;
        int pageStep  = m_viewportHeight;

        if (y < m_thumbY)
            onScroll(std::max(0, m_scrollOffset - pageStep));
        else
            onScroll(std::min(maxOffset, m_scrollOffset + pageStep));
    }
}

void FilamentScrollBar::onMouseUp(wxMouseEvent&)
{
    if (HasCapture())
        ReleaseMouse();
}

void FilamentScrollBar::onMouseMove(wxMouseEvent& evt)
{
    // Only act while we own the mouse capture (set in onMouseDown, released in onMouseUp).
    //   evt.Dragging() is not used because its button-state check may be unreliable
    //   after CaptureMouse() on some platforms.
    if (!HasCapture())
        return;

    int dy = evt.GetPosition().y - m_dragStartY;
    if (dy == 0)
        return;

    int trackH = GetClientSize().y;
    if (m_contentHeight <= m_viewportHeight || trackH <= m_thumbHeight)
        return;

    int   maxOffset     = m_contentHeight - m_viewportHeight;
    int   draggableArea = trackH - m_thumbHeight;
    float ratio         = static_cast<float>(dy) / static_cast<float>(draggableArea);
    int   newOffset     = m_dragStartOffset + static_cast<int>(ratio * maxOffset);

    onScroll(newOffset);
}

void FilamentScrollBar::onScroll(int newOffset)
{
    int maxOffset = std::max(0, m_contentHeight - m_viewportHeight);
    newOffset     = std::max(0, std::min(newOffset, maxOffset));

    // Notify callback BEFORE updating m_scrollOffset so the callback
    // can still detect the change and move the content.
    if (m_onScroll)
        m_onScroll(newOffset);

    m_scrollOffset = newOffset;
    m_thumbY       = calcThumbY();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
