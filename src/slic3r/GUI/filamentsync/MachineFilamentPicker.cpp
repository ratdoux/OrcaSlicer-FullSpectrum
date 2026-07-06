#include "MachineFilamentPicker.hpp"

#include <cmath>

#include <wx/dcclient.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/dcgraph.h>

#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/MixedFilamentBadge.hpp"

namespace
{

// --- Popup layout ---
constexpr int g_popupMinWidth = 67;  // DIP — minimum width (matches FilamentColorMapBox card width)
constexpr int g_popupHeight   = 106; // DIP

// --- Item row ---
constexpr int g_itemRowH      = 16; // DIP — row height
constexpr int g_itemStepY     = 22; // DIP — vertical step between rows
constexpr int g_firstRowY     = 12; // DIP — top of first row (after shadow)
constexpr int g_itemGap       = 6;  // DIP — gap between rows
constexpr int g_selectionInsetX = 3; // DIP — left/right inset for selected row background
constexpr int g_contentOffsetX  = -2; // DIP — keep row content clear of the right edge

// --- Checkmark (draw with path, roughly 8×8 DIP) ---
constexpr int g_checkmarkX = 9;
constexpr int g_checkmarkY = 4;  // offset from row top
constexpr int g_checkmarkW = 8;
constexpr int g_checkmarkH = 8;

// --- Colour circle (filament index) ---
constexpr int g_circleRadius = 7;  // DIP (design: 6.57143)
constexpr int g_circleCx     = 30; // DIP from left edge

// --- Text label ---
constexpr int g_textX = 42;           // DIP from left edge
constexpr int g_textRightPadding = 6; // DIP — right margin after text

// --- Colours (from design spec) ---
const wxColour g_selectionBg(0xBE, 0xE1, 0xDB);  // #BEE1DB
const wxColour g_checkmarkColor(0x7B, 0x82, 0x82);   // #7B8282
const wxColour g_textColor(0x33, 0x33, 0x33);         // #333333
const wxColour g_circleStroke(0xDB, 0xDB, 0xDA);      // #DBDBDA
// ============================================================
// Helper: check whether a filament entry is the NONE placeholder
// ============================================================
bool isNoneEntry(const Slic3r::GUI::FilamentData& data)
{
    return data.m_type.empty() || data.m_type == "NONE";
}

// ============================================================
// Helper: determine which row is under a mouse-y coordinate,
//         returns -1 when the click is not on any row.
// ============================================================
int hitTestRow(int yDip, int itemCount)
{
    if (yDip < g_firstRowY) {
        return -1;
    }
    int row = (yDip - g_firstRowY) / g_itemStepY;
    if (row >= itemCount) {
        return -1;
    }
    // Allow a little tolerance into the gap below the last row
    int top = g_firstRowY + row * g_itemStepY;
    if (yDip > top + g_itemRowH + g_itemGap) {
        return -1;
    }
    return row;
}

} // namespace

namespace Slic3r
{
namespace GUI
{

// ============================================================
// Inner panel that does all custom drawing
// ============================================================
class PickerContentPanel : public wxPanel
{
public:
    PickerContentPanel(wxWindow* parent,
                       const std::vector<FilamentData>& dataList,
                       unsigned int selectedIndex)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                  wxFULL_REPAINT_ON_RESIZE)
        , m_dataList(dataList)
        , m_selectedIndex(selectedIndex)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &PickerContentPanel::onPaint, this);
        Bind(wxEVT_LEFT_DOWN, &PickerContentPanel::onLeftDown, this);
        Bind(wxEVT_MOUSEWHEEL, &PickerContentPanel::onMouseWheel, this);

        int maxTextWidthPx = 0;
        {
            wxClientDC dc(this);
            dc.SetFont(Label::Body_10);
            for (const auto& data : m_dataList) {
                wxString typeStr = wxString::FromUTF8(data.m_type);
                int tw = dc.GetTextExtent(typeStr).x;
                if (tw > maxTextWidthPx) {
                    maxTextWidthPx = tw;
                }
            }
        }
        int minWidthPx = FromDIP(g_popupMinWidth);
        int contentWidthPx = FromDIP(g_textX + g_contentOffsetX) + maxTextWidthPx + FromDIP(g_textRightPadding);
        int actualWidthPx = std::max(minWidthPx, contentWidthPx);

        wxSize sz(actualWidthPx, FromDIP(g_popupHeight));
        SetSize(sz);
        SetMinSize(sz);
        SetMaxSize(sz);
    }

    void setSelectedIndex(unsigned int idx)
    {
        m_selectedIndex = idx;
        Refresh();
    }

    void bindSelectionCallback(std::function<void(unsigned int)> cb)
    {
        m_selectionCallback = std::move(cb);
    }

private:
    void onPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC bufDC(this);
        wxGCDC dc(bufDC);
        render(dc);
    }

    void render(wxDC& dc)
    {
        wxSize panelSize = GetClientSize();
        int contentW = panelSize.x;
        int contentH = panelSize.y;

        // ---- Background fill (clear) ----
        dc.SetBackground(*wxWHITE);
        dc.Clear();

        // ---- White content area (inset 1px so border isn't clipped by popup edges on macOS) ----
        dc.SetPen(wxPen(wxColour(0xE0, 0xE0, 0xE0), 1));
        dc.SetBrush(wxBrush(*wxWHITE));
        dc.DrawRectangle(1, 1, contentW - 2, contentH - 2);

        // ---- Draw each row ----
        int itemCount = static_cast<int>(m_dataList.size());

        wxFont indexFont = GetFont();
        indexFont.SetPointSize(7);
        indexFont.SetWeight(wxFONTWEIGHT_BOLD);

        wxFont labelFont = Label::Body_10;

        for (int i = 0; i < itemCount; ++i) {
            int rowY = g_firstRowY + i * g_itemStepY;
            drawRow(dc, i, rowY, contentW, indexFont, labelFont);
        }
    }

    void drawRow(wxDC& dc, int index, int rowYDip, int contentWidthPx,
                 const wxFont& indexFont, const wxFont& labelFont)
    {
        const FilamentData& data = m_dataList[index];
        bool isSelected = (static_cast<unsigned int>(index) == m_selectedIndex);

        int x = 0; // all coordinates in DIP
        int y = rowYDip;

        // ---- Selection background ----
        if (isSelected) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(g_selectionBg));
            const int insetX = FromDIP(g_selectionInsetX);
            dc.DrawRectangle(FromDIP(x) + insetX, FromDIP(y),
                             contentWidthPx - insetX * 2,
                             FromDIP(g_itemRowH));
        }

        // ---- Checkmark (selected only) ----
        if (isSelected) {
            int cx = FromDIP(g_checkmarkX + g_contentOffsetX);
            int cy = FromDIP(y + g_checkmarkY);
            int cw = FromDIP(g_checkmarkW);
            int ch = FromDIP(g_checkmarkH);

            drawCheckmark(dc, cx, cy, cw, ch);
        }

        bool isNone = isNoneEntry(data);

        // ---- Colour circle (bitmap) ----
        int circleCxPx = FromDIP(g_circleCx + g_contentOffsetX);
        int circleCyPx = FromDIP(y + g_itemRowH / 2);
        int circleR    = FromDIP(g_circleRadius);
        int circleD    = circleR * 2;

        std::vector<wxColour> circleColors = getAllColors(data.m_color);
        bool circleIsGradient = data.m_color.NormalizedMode() == FilamentColorMode::Gradient;
        const wxColour circleBorder = isNone ? wxColour(0xCC, 0xCC, 0xCC) : g_circleStroke;
        wxBitmap* circleBmp = get_color_block_bitmap_cached(circleColors, circleIsGradient,
            circleD, circleD, wxEmptyString, circleBorder,
            CornerRadius::Uniform(circleR));
        if (circleBmp && circleBmp->IsOk())
            dc.DrawBitmap(*circleBmp, circleCxPx - circleR, circleCyPx - circleR);

        // Border circle on top
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.SetPen(wxPen(circleBorder, FromDIP(1)));
        dc.DrawCircle(circleCxPx, circleCyPx, circleR);

        // ---- Filament type text ----
        dc.SetFont(labelFont);
        dc.SetTextForeground(isNone ? wxColour(0xBB, 0xBB, 0xBB) : g_textColor);
        wxString typeStr = wxString::FromUTF8(data.m_type.empty() ? "NONE" : data.m_type);
        int textX = FromDIP(g_textX + g_contentOffsetX);
        int textH = dc.GetTextExtent(typeStr).y;
        int textY = FromDIP(y) + static_cast<int>(std::round((FromDIP(g_itemRowH) - textH) / 2.0));

        // ---- Index number inside circle (vertically aligned with filament text) ----
        dc.SetFont(indexFont);
        dc.SetTextForeground(isNone ? wxColour(0xBB, 0xBB, 0xBB) : getTextColour(getMainColor(data.m_color)));
        wxString idxStr = wxString::Format("%d", data.m_index + 1);
        wxSize   idxExtent = dc.GetTextExtent(idxStr);
        int halfW = static_cast<int>(std::round(idxExtent.x / 2.0));
        int halfH = static_cast<int>(std::round(idxExtent.y / 2.0));
        dc.DrawText(idxStr, circleCxPx - halfW, circleCyPx - halfH);

        dc.SetFont(labelFont);
        dc.SetTextForeground(isNone ? wxColour(0xBB, 0xBB, 0xBB) : g_textColor);
        dc.DrawText(typeStr, textX, textY);
    }

    void drawCheckmark(wxDC& dc, int x, int y, int w, int h)
    {
        // Simple checkmark: two-line polyline ✓
        wxPoint pts[3];
        pts[0] = wxPoint(x + w * 0.2, y + h * 0.5);
        pts[1] = wxPoint(x + w * 0.45, y + h * 0.75);
        pts[2] = wxPoint(x + w * 0.9, y + h * 0.2);

        dc.SetPen(wxPen(g_checkmarkColor, FromDIP(1)));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawLines(3, pts);
    }

    void onLeftDown(wxMouseEvent& evt)
    {
        int row = hitTestRow(evt.GetY() / GetDPIScaleFactor(),
                             static_cast<int>(m_dataList.size()));
        if (row < 0) {
            return;
        }

        // Ignore clicks on NONE (empty slot) entries
        if (isNoneEntry(m_dataList[row])) {
            return;
        }

        m_selectedIndex = static_cast<unsigned int>(row);
        Refresh();

        if (m_selectionCallback) {
            m_selectionCallback(m_selectedIndex);
        }
    }

    void onMouseWheel(wxMouseEvent&)
    {
        auto* popup = static_cast<wxPopupTransientWindow*>(GetParent());
        if (popup) {
            popup->Dismiss();
        }
    }

    std::vector<FilamentData> m_dataList;
    unsigned int              m_selectedIndex = 0;
    std::function<void(unsigned int)> m_selectionCallback = nullptr;
};

// ============================================================
// MachineFilamentPicker public API
// ============================================================

MachineFilamentPicker::MachineFilamentPicker(wxWindow* parent,
                                             const std::vector<FilamentData>& dataList,
                                             unsigned int curIndex)
    : wxPopupTransientWindow(parent)
    , m_dataList(dataList)
    , m_selectedIndex(curIndex)
{
    auto* panel = new PickerContentPanel(this, m_dataList, curIndex);
    m_contentPanel = panel;

    panel->bindSelectionCallback([this](unsigned int idx) {
        m_selectedIndex = idx;

        if (m_selectionCallback && idx < m_dataList.size()) {
            m_selectionCallback(m_dataList[idx]);
        }

        Dismiss();
    });

    Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent&) {
        Dismiss();
    });

    wxSize panelSize = panel->GetSize();
    SetClientSize(panelSize);
}

FilamentData MachineFilamentPicker::getSelectedData() const
{
    if (m_selectedIndex < m_dataList.size()) {
        return m_dataList[m_selectedIndex];
    }
    return FilamentData();
}

unsigned int MachineFilamentPicker::getSelectedIndex() const
{
    return m_selectedIndex;
}

void MachineFilamentPicker::setSelectedIndex(unsigned int index)
{
    m_selectedIndex = index;
    if (m_contentPanel)
        static_cast<PickerContentPanel*>(m_contentPanel)->setSelectedIndex(index);
}

void MachineFilamentPicker::popupAt(const wxPoint& pos)
{
    int minWidthPx = FromDIP(g_popupMinWidth);
    int actualWidthPx = m_contentPanel ? m_contentPanel->GetSize().x : minWidthPx;
    int offsetX = (actualWidthPx - minWidthPx) / 2;
    SetPosition(wxPoint(pos.x - offsetX, pos.y));
    Popup();
}

void MachineFilamentPicker::bindSelectionCallback(FilamentInfoCallback cb)
{
    m_selectionCallback = std::move(cb);
}

void MachineFilamentPicker::bindOnDismissCallback(std::function<void()> cb)
{
    m_onDismissCallback = std::move(cb);
}

void MachineFilamentPicker::OnDismiss()
{
    if (m_onDismissCallback) {
        m_onDismissCallback();
    }

    CallAfter([this]() { Destroy(); });
}

} // namespace GUI
} // namespace Slic3r
