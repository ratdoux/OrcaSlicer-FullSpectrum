#include "FilamentColorMapBox.hpp"

#include <wx/dcclient.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/dcmemory.h>
#include <wx/settings.h>

#include "slic3r/GUI/BitmapCache.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/MixedFilamentBadge.hpp"

namespace
{

// ============================================================
// Layout constants
// ============================================================

constexpr int g_cardWidth = 67;

constexpr int g_topBarHeight   = 26;
constexpr int g_bodyOverlap    = 1;
constexpr int g_bodyY          = g_topBarHeight - g_bodyOverlap; // 25

constexpr int g_bodyHeightEnabled  = 60; // 25 + 60 = 85
constexpr int g_bodyHeightDisabled = 46; // 25 + 46 = 71

constexpr int g_cardHeightEnabled  = g_bodyY + g_bodyHeightEnabled;  // 85
constexpr int g_cardHeightDisabled = g_bodyY + g_bodyHeightDisabled; // 71

constexpr int g_cornerRadius = 6;

constexpr int g_circleDiameter = 20;
constexpr int g_circleYOffset  = 5;

constexpr int g_arrowSize      = 14;
constexpr int g_arrowBotMargin = 4;

constexpr int g_topTextY = 7;

constexpr int g_nameTextY        = 29;
constexpr int g_bodyBorderWidth  = 1;

// ============================================================
// Colours
// ============================================================
const wxColour g_bodyBorderColor(0xDB, 0xDB, 0xDA);
const wxColour g_bodyBorderHoverColor(0xA0, 0xA0, 0xA0);
const wxColour g_bodyTextColor(0x33, 0x33, 0x33);
const wxColour g_disabledBodyBg(0xE8, 0xE8, 0xE8);
const wxColour g_cardBg(0xFF, 0xFF, 0xFF);

wxString formatTopLabel(const Slic3r::GUI::FilamentData& data)
{
    unsigned int displayIndex = data.m_index + 1;
    if (data.m_type.empty())
        return wxString::Format("%u NONE", displayIndex);
    return wxString::Format("%u %s", displayIndex, data.m_type);
}

Slic3r::GUI::BitmapCache& getIconCache()
{
    static Slic3r::GUI::BitmapCache s_cache;
    return s_cache;
}

const wxBitmap& getDropdownArrowBitmap(int sizePxHasDpi)
{
    static wxBitmap s_bmp;
    static int      s_cachedPx = -1;
    if (s_cachedPx != sizePxHasDpi || !s_bmp.IsOk()) {
        wxBitmap* loaded = getIconCache().load_svg("filament_dropdown_arrow", sizePxHasDpi, sizePxHasDpi);
        if (loaded && loaded->IsOk()) {
            s_bmp       = *loaded;
            s_cachedPx = sizePxHasDpi;
        }
    }
    return s_bmp;
}

} // namespace

namespace Slic3r
{
namespace GUI
{

FilamentColorMapBox::FilamentColorMapBox(wxWindow* parent,
                                         const FilamentData& aboveData,
                                         const FilamentData& belowData)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
              wxBORDER_NONE | wxFULL_REPAINT_ON_RESIZE)
    , m_aboveFilament(aboveData)
    , m_belowFilament(belowData)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(g_cardBg);
    updateSizing();
    Bind(wxEVT_PAINT, &FilamentColorMapBox::onPaint, this);
    Bind(wxEVT_LEFT_DOWN, &FilamentColorMapBox::onLeftDown, this);
    Bind(wxEVT_ENTER_WINDOW, &FilamentColorMapBox::onMouseEnter, this);
    Bind(wxEVT_LEAVE_WINDOW, &FilamentColorMapBox::onMouseLeave, this);
    Bind(wxEVT_MOTION, &FilamentColorMapBox::onMouseMove, this);
}

void FilamentColorMapBox::bindButton(FilamentInfoCallback cb, ButtonType type)
{
    if (type == ButtonType::Above)
        m_aboveCallback = std::move(cb);
    else
        m_belowCallback = std::move(cb);
}

void FilamentColorMapBox::setEnable(bool bEnable, ButtonType type)
{
    if (type == ButtonType::Above) {
        m_bAboveEnabled = bEnable;
    } else {
        m_bBelowEnabled = bEnable;
    }
    updateSizing();
    Refresh();
}

void FilamentColorMapBox::updateAboveData(const FilamentData& data)
{
    m_aboveFilament = data;
    Refresh();
}

void FilamentColorMapBox::updateBelowData(const FilamentData& data)
{
    m_belowFilament = data;
    Refresh();
}

const FilamentData& FilamentColorMapBox::getAboveData() const
{
    return m_aboveFilament;
}

const FilamentData& FilamentColorMapBox::getBelowData() const
{
    return m_belowFilament;
}

void FilamentColorMapBox::updateSizing()
{
    int totalH = m_bBelowEnabled ? g_cardHeightEnabled : g_cardHeightDisabled;
    wxSize sz = FromDIP(wxSize(g_cardWidth, totalH));
    SetMinSize(sz);
    SetMaxSize(sz);
}

void FilamentColorMapBox::onPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.Clear();

    wxGCDC gdc(dc);

    const wxSize sz    = GetClientSize();
    const int    w     = sz.x;
    const int    totalH = sz.y;
    const int    splitY = FromDIP(g_topBarHeight);
    const int    bodyH  = totalH - splitY;
    const int    radius = FromDIP(g_cornerRadius);

    const int borderW = FromDIP(g_bodyBorderWidth);

    // Hover border: use hover color when mouse is over an enabled zone
    bool hoverActive = m_hoveredZone >= 0
        && ((m_hoveredZone == 0) ? m_bAboveEnabled : m_bBelowEnabled);
    const wxColour& borderColour = hoverActive ? g_bodyBorderHoverColor : g_bodyBorderColor;

    // ---- 1. Full card body (white bg) ----
    gdc.SetPen(wxPen(borderColour, borderW));
    gdc.SetBrush(g_cardBg);
    gdc.DrawRoundedRectangle(0, 0, w, totalH, radius);

    // ---- 2. Top bar (above filament colour bitmap, clipped to top half) ----
    {
        std::vector<wxColour> aboveColors = getAllColors(m_aboveFilament.m_color);
        bool aboveIsGradient = m_aboveFilament.m_color.NormalizedMode() == FilamentColorMode::Gradient;
        const int barR = radius;
        CornerRadius topCorners = {barR, barR, 0, 0};
        wxBitmap* aboveBmp = get_color_block_bitmap_cached(aboveColors, aboveIsGradient,
            w, totalH, wxEmptyString, wxColour(), topCorners);
        wxDCClipper clip(gdc, wxRect(0, 0, w, splitY));
        if (aboveBmp && aboveBmp->IsOk())
            gdc.DrawBitmap(*aboveBmp, 0, 0);
    }

    // Redraw card border over the bitmap so it stays visible
    {
        wxDCClipper clip(gdc, wxRect(0, 0, w, splitY));
        gdc.SetPen(wxPen(borderColour, borderW));
        gdc.SetBrush(*wxTRANSPARENT_BRUSH);
        gdc.DrawRoundedRectangle(0, 0, w, totalH, radius);
    }

    // ---- 3. Separator line ----
    {
        gdc.SetPen(wxPen(borderColour, borderW));
        gdc.DrawLine(0, splitY, w, splitY);
    }

    // ---- 4. Top bar label ----
    {
        const wxColour textColour = m_bAboveEnabled
                                        ? getTextColour(getMainColor(m_aboveFilament.m_color))
                                        : g_bodyTextColor;
        gdc.SetTextForeground(textColour);
        gdc.SetFont(Label::Body_10);
        const wxString label = formatTopLabel(m_aboveFilament);
        const wxSize   te    = gdc.GetTextExtent(label);
        gdc.DrawText(label, (w - te.x) / 2, FromDIP(g_topTextY));
    }

    // ---- 5. Index circle (filled with machine filament colour bitmap) ----
    {
        const int circleD = FromDIP(g_circleDiameter);
        const int cx      = w / 2;
        const int cy      = splitY + FromDIP(g_circleYOffset) + circleD / 2;
        const int cr      = circleD / 2;

        std::vector<wxColour> belowColors = getAllColors(m_belowFilament.m_color);
        bool belowIsGradient = m_belowFilament.m_color.NormalizedMode() == FilamentColorMode::Gradient;
        wxBitmap* belowBmp = get_color_block_bitmap_cached(belowColors, belowIsGradient,
            circleD, circleD, wxEmptyString, borderColour,
            CornerRadius::Uniform(cr));
        if (belowBmp && belowBmp->IsOk())
            gdc.DrawBitmap(*belowBmp, cx - cr, cy - cr);

        // Border circle drawn on top of bitmap
        gdc.SetPen(wxPen(borderColour, borderW));
        gdc.SetBrush(*wxTRANSPARENT_BRUSH);
        gdc.DrawCircle(cx, cy, cr);

        gdc.SetTextForeground(getTextColour(getMainColor(m_belowFilament.m_color)));
        gdc.SetFont(Label::Head_12);
        const wxString num    = wxString::Format("%u", m_belowFilament.m_index + 1);
        const wxSize   numExt = gdc.GetTextExtent(num);
        gdc.DrawText(num, cx - numExt.x / 2, cy - numExt.y / 2);
    }

    // ---- 6. Below filament type / name ----
    {
        gdc.SetTextForeground(g_bodyTextColor);
        gdc.SetFont(Label::Body_10);
        const wxString type = m_belowFilament.m_type.empty()
                                  ? wxString("NONE")
                                  : wxString(m_belowFilament.m_type);
        const wxSize te = gdc.GetTextExtent(type);
        gdc.DrawText(type, (w - te.x) / 2,
                     splitY + FromDIP(g_nameTextY));
    }

    // ---- 7. Dropdown arrow (SVG, only when Below enabled) ----
    if (m_bBelowEnabled) {
        const wxBitmap& arrowBmp = getDropdownArrowBitmap(FromDIP(g_arrowSize));
        if (arrowBmp.IsOk()) {
            const int arrowW = arrowBmp.GetWidth();
            const int arrowH = arrowBmp.GetHeight();
            const int ax     = (w - arrowW) / 2;
            const int ay     = splitY + bodyH - arrowH - FromDIP(g_arrowBotMargin);
            gdc.DrawBitmap(arrowBmp, ax, ay);
        }
    }
}

void FilamentColorMapBox::onLeftDown(wxMouseEvent& event)
{
    int posY  = event.GetPosition().y;
    int splitY = FromDIP(g_topBarHeight);

    if (posY < splitY) {
        if (m_bAboveEnabled && m_aboveCallback)
            m_aboveCallback(m_aboveFilament);
    } else {
        if (m_bBelowEnabled && m_belowCallback)
            m_belowCallback(m_belowFilament);
    }
}

void FilamentColorMapBox::onMouseEnter(wxMouseEvent& event)
{
    onMouseMove(event);
}

void FilamentColorMapBox::onMouseLeave(wxMouseEvent&)
{
    if (m_hoveredZone == -1)
        return;
    m_hoveredZone = -1;
    Refresh();
}

void FilamentColorMapBox::onMouseMove(wxMouseEvent& event)
{
    int posY    = event.GetPosition().y;
    int splitY  = FromDIP(g_topBarHeight);
    int newZone = (posY < splitY) ? 0 : 1;
    if (newZone == m_hoveredZone)
        return;
    m_hoveredZone = newZone;
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
