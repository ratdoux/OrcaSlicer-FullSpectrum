#include "PlaterPreview.hpp"

#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include <wx/dcmemory.h>
#include <wx/sizer.h>
#include <wx/panel.h>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/BitmapCache.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/ComboBox.hpp"

namespace
{

// ============================================================
// Layout constants
// ============================================================
constexpr int g_platerWidth        = 499; // 220 + 16 + 263 (content width, no padding)
constexpr int g_platerHeight       = 279;
constexpr int g_previewGap         = 16;
constexpr int g_leftPreviewW       = 220;
constexpr int g_leftPreviewH       = 220;
constexpr int g_rightPreviewW      = 263;
constexpr int g_rightPreviewH      = 263;
constexpr int g_previewRadius      = 8;
constexpr int g_labelTopMargin     = 4;

constexpr int g_arrowSize          = 20;
constexpr int g_navItemGap         = 12;

constexpr int g_plateSelectorW        = 62;
constexpr int g_plateSelectorH        = 30;
constexpr int g_plateSelectorBorderW  = 1;
constexpr int g_plateSelectorRadius   = 4;


// ============================================================
// Colours
// ============================================================
const wxColour g_previewBg(0xD9, 0xD9, 0xD9);
const wxColour g_panelBg(0xFF, 0xFF, 0xFF);
const wxColour g_diskLabelColor(0x4A, 0x4A, 0x4A);
const wxColour g_platerBorderColor(0xF0, 0xF0, 0xF0);
const wxColour g_labelTextColor(0x24, 0x24, 0x24);

// ============================================================
// Rescaling
// ============================================================
constexpr int g_rescaleInsetMargin = 8;
constexpr int g_minClientSize      = 10;


Slic3r::GUI::BitmapCache& getIconCache()
{
    static Slic3r::GUI::BitmapCache s_cache;
    return s_cache;
}

const wxBitmap& getLeftArrowBitmap(int sizePx)
{
    static wxBitmap s_bmp;
    static int      s_cachedPx = -1;
    if (s_cachedPx != sizePx || !s_bmp.IsOk()) {
        wxBitmap* loaded = getIconCache().load_svg("filament_picker_left_arrow", sizePx, sizePx);
        if (loaded && loaded->IsOk()) {
            s_bmp      = *loaded;
            s_cachedPx = sizePx;
        }
    }
    return s_bmp;
}

const wxBitmap& getRightArrowBitmap(int sizePx)
{
    static wxBitmap s_bmp;
    static int      s_cachedPx = -1;
    if (s_cachedPx != sizePx || !s_bmp.IsOk()) {
        wxBitmap* loaded = getIconCache().load_svg("filament_picker_right_arrow", sizePx, sizePx);
        if (loaded && loaded->IsOk()) {
            s_bmp      = *loaded;
            s_cachedPx = sizePx;
        }
    }
    return s_bmp;
}

wxFont getMediumFont()
{
    wxFont f = Label::Body_14;
    f.SetWeight(wxFONTWEIGHT_MEDIUM);
    return f;
}

void drawArrowBitmap(wxDC& dc, wxWindow* win, const wxBitmap& bmp)
{
    if (!bmp.IsOk()) 
        return;

    if (win->IsThisEnabled()) {
        dc.DrawBitmap(bmp, 0, 0);
    } else {
        wxImage img = bmp.ConvertToImage();
        if (!img.HasAlpha()) 
            img.InitAlpha();
        unsigned char* alpha = img.GetAlpha();
        int px = img.GetWidth() * img.GetHeight();
        for (int i = 0; i < px; ++i) {
            alpha[i] = static_cast<unsigned char>(alpha[i] * 0.3);
        }
        dc.DrawBitmap(wxBitmap(img), 0, 0);
    }
}

} // namespace

namespace Slic3r
{
namespace GUI
{

PlaterPreview::PlaterPreview(wxWindow* parent, unsigned int totalPlateCount)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
              wxFULL_REPAINT_ON_RESIZE)
    , m_totalPlateCount(totalPlateCount > 0 ? totalPlateCount : 1)
{
    SetBackgroundColour(g_panelBg);

    auto* outerSizer = new wxBoxSizer(wxVERTICAL);

    // ---- Top row: two preview columns ----
    auto* previewRow = new wxBoxSizer(wxHORIZONTAL);

    // Left preview column
    auto* leftCol = new wxBoxSizer(wxVERTICAL);

    m_pLabelLeft = new Label(this, _L("Original Model"));
    m_pLabelLeft->SetFont(getMediumFont());
    m_pLabelLeft->SetForegroundColour(g_labelTextColor);
    leftCol->Add(m_pLabelLeft, 0, wxEXPAND);
    leftCol->AddSpacer(FromDIP(g_labelTopMargin));

    m_pPreviewLeft = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                 FromDIP(wxSize(g_leftPreviewW, g_leftPreviewH)));
    m_pPreviewLeft->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_pPreviewLeft->SetBackgroundColour(g_previewBg);
    m_pPreviewLeft->SetMinSize(FromDIP(wxSize(g_leftPreviewW, g_leftPreviewH)));
    m_pPreviewLeft->SetMaxSize(FromDIP(wxSize(g_leftPreviewW, g_leftPreviewH)));
    m_pPreviewLeft->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        paintPreview(m_pPreviewLeft, m_originalBitmap);
    });
    leftCol->Add(m_pPreviewLeft, 0, wxEXPAND);

    // Stretch spacer: pushes navRow to the bottom so that
    // left column height (label + 220 preview + nav) equals right column height (label + 263 preview)
    leftCol->AddStretchSpacer(1);

    // ---- Navigation bar (justify-between: left arrow | "Plate" + selector | right arrow) ----
    auto* navRow = new wxBoxSizer(wxHORIZONTAL);

    // Left arrow
    m_pArrowLeft = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                               FromDIP(wxSize(g_arrowSize, g_arrowSize)));
    m_pArrowLeft->SetMinSize(FromDIP(wxSize(g_arrowSize, g_arrowSize)));
    m_pArrowLeft->SetMaxSize(FromDIP(wxSize(g_arrowSize, g_arrowSize)));
    m_pArrowLeft->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxPaintDC dc(m_pArrowLeft);
        drawArrowBitmap(dc, m_pArrowLeft, getLeftArrowBitmap(FromDIP(g_arrowSize)));
    });
    m_pArrowLeft->Bind(wxEVT_LEFT_DOWN, &PlaterPreview::onLeftArrow, this);
    navRow->Add(m_pArrowLeft, 0, wxALIGN_CENTER_VERTICAL);

    navRow->AddStretchSpacer(1);

    // Middle group: "Plate" label + 12px gap + disk selector
    {
        auto* middleGroup = new wxBoxSizer(wxHORIZONTAL);

        m_pDiskLabel = new Label(this, _CTX(L_CONTEXT("Plate", "FilamentSync"), "FilamentSync"));
        m_pDiskLabel->SetFont(Label::Body_12);
        m_pDiskLabel->SetForegroundColour(g_diskLabelColor);
        middleGroup->Add(m_pDiskLabel, 0, wxALIGN_CENTER_VERTICAL);

        middleGroup->AddSpacer(FromDIP(g_navItemGap));

        m_pPlateCombo = new ComboBox(this, wxID_ANY, wxEmptyString,
                                     wxDefaultPosition, FromDIP(wxSize(g_plateSelectorW, g_plateSelectorH)), 0, nullptr,
                                     wxCB_READONLY);
        m_pPlateCombo->SetMinSize(FromDIP(wxSize(g_plateSelectorW, g_plateSelectorH)));
        m_pPlateCombo->SetMaxSize(FromDIP(wxSize(g_plateSelectorW, g_plateSelectorH)));
        m_pPlateCombo->SetCornerRadius(FromDIP(g_plateSelectorRadius));
        m_pPlateCombo->SetBorderWidth(FromDIP(g_plateSelectorBorderW));
        m_pPlateCombo->SetBorderColor(StateColor(g_platerBorderColor));
        m_pPlateCombo->SetBackgroundColor(StateColor(g_panelBg));
        m_pPlateCombo->GetDropDown().SetBorderColor(StateColor(g_platerBorderColor));
        m_pPlateCombo->GetDropDown().SetBackgroundColour(g_panelBg);
        m_pPlateCombo->Bind(wxEVT_COMBOBOX, &PlaterPreview::onPlateComboBoxChanged, this);
        middleGroup->Add(m_pPlateCombo, 0, wxALIGN_CENTER_VERTICAL);

        navRow->Add(middleGroup, 0, wxALIGN_CENTER_VERTICAL);
    }

    navRow->AddStretchSpacer(1);

    // Right arrow
    m_pArrowRight = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                FromDIP(wxSize(g_arrowSize, g_arrowSize)));
    m_pArrowRight->SetMinSize(FromDIP(wxSize(g_arrowSize, g_arrowSize)));
    m_pArrowRight->SetMaxSize(FromDIP(wxSize(g_arrowSize, g_arrowSize)));
    m_pArrowRight->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxPaintDC dc(m_pArrowRight);
        drawArrowBitmap(dc, m_pArrowRight, getRightArrowBitmap(FromDIP(g_arrowSize)));
    });
    m_pArrowRight->Bind(wxEVT_LEFT_DOWN, &PlaterPreview::onRightArrow, this);
    navRow->Add(m_pArrowRight, 0, wxALIGN_CENTER_VERTICAL);

    leftCol->Add(navRow, 0, wxEXPAND);

    previewRow->Add(leftCol, 0, wxEXPAND);
    previewRow->AddSpacer(FromDIP(g_previewGap));

    // Right preview column
    auto* rightCol = new wxBoxSizer(wxVERTICAL);

    m_pLabelRight = new Label(this, _L("Mapped Model"));
    m_pLabelRight->SetFont(getMediumFont());
    m_pLabelRight->SetForegroundColour(g_labelTextColor);
    rightCol->Add(m_pLabelRight, 0, wxEXPAND);
    rightCol->AddSpacer(FromDIP(g_labelTopMargin));

    m_pPreviewRight = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                  FromDIP(wxSize(g_rightPreviewW, g_rightPreviewH)));
    m_pPreviewRight->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_pPreviewRight->SetBackgroundColour(g_previewBg);
    m_pPreviewRight->SetMinSize(FromDIP(wxSize(g_rightPreviewW, g_rightPreviewH)));
    m_pPreviewRight->SetMaxSize(FromDIP(wxSize(g_rightPreviewW, g_rightPreviewH)));
    m_pPreviewRight->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        paintPreview(m_pPreviewRight, m_coverBitmap);
    });
    rightCol->Add(m_pPreviewRight, 0, wxEXPAND);

    previewRow->Add(rightCol, 0, wxEXPAND);
    outerSizer->Add(previewRow, 0, wxEXPAND);

    SetSizer(outerSizer);
    SetMinSize(wxSize(FromDIP(g_platerWidth), FromDIP(g_platerHeight)));
    Layout();

    updateNavigation();
}

void PlaterPreview::setOriginalPreview(const wxBitmap& thumbnail)
{
    m_originalBitmap = thumbnail;
    if (m_pPreviewLeft)
        m_pPreviewLeft->Refresh();
}

void PlaterPreview::setCoverPreview(const wxBitmap& thumbnail)
{
    m_coverBitmap = thumbnail;
    if (m_pPreviewRight)
        m_pPreviewRight->Refresh();
}

void PlaterPreview::updateCoverPreview(const wxBitmap& thumbnail)
{
    setCoverPreview(thumbnail);
}

void PlaterPreview::setCoverLabel(const wxString& label)
{
    if (!m_pLabelRight)
        return;

    m_pLabelRight->SetLabel(label);
    Layout();
}

void PlaterPreview::setCurrentPlate(unsigned int plateIndex)
{
    if (plateIndex >= m_totalPlateCount)
        return;

    m_currentPlateIndex = plateIndex;

    if (m_pPlateCombo)
        m_pPlateCombo->SetSelection(plateIndex);

    updateNavigation();
}

unsigned int PlaterPreview::getCurrentPlate() const
{
    return m_currentPlateIndex;
}

void PlaterPreview::setTotalPlateCount(unsigned int count)
{
    m_totalPlateCount = (count > 0) ? count : 1;

    if (m_currentPlateIndex >= m_totalPlateCount)
        m_currentPlateIndex = 0;

    if (m_pPlateCombo) {
        m_pPlateCombo->Clear();
        for (unsigned int i = 0; i < m_totalPlateCount; ++i)
            m_pPlateCombo->Append(wxString::Format("%02u", i + 1));
        m_pPlateCombo->SetSelection(m_currentPlateIndex);
    }

    updateNavigation();
}

void PlaterPreview::bindPlateSwitchCallback(std::function<void(unsigned int newPlateIndex)> cb)
{
    m_plateSwitchCallback = std::move(cb);
}

void PlaterPreview::onLeftArrow(wxMouseEvent&)
{
    if (m_currentPlateIndex > 0)
        navigateTo(m_currentPlateIndex - 1);
}

void PlaterPreview::onRightArrow(wxMouseEvent&)
{
    if (m_currentPlateIndex + 1 < m_totalPlateCount)
        navigateTo(m_currentPlateIndex + 1);
}

void PlaterPreview::onPlateComboBoxChanged(wxCommandEvent&)
{
    if (!m_pPlateCombo)
        return;

    int sel = m_pPlateCombo->GetSelection();
    if (sel >= 0 && sel < m_totalPlateCount)
        navigateTo(sel);
}

void PlaterPreview::navigateTo(int index)
{
    if (index < 0 || index >= m_totalPlateCount)
        return;

    setCurrentPlate(index);

    if (m_plateSwitchCallback)
        m_plateSwitchCallback(m_currentPlateIndex);
}

void PlaterPreview::updateNavigation()
{
    m_pArrowLeft->Enable(m_currentPlateIndex > 0);
    m_pArrowRight->Enable(m_currentPlateIndex + 1 < m_totalPlateCount);

    m_pArrowLeft->Refresh();
    m_pArrowRight->Refresh();
}

void PlaterPreview::paintPreview(wxWindow* win, const wxBitmap& bmp)
{
    wxAutoBufferedPaintDC dc(win);
    wxSize sz = win->GetClientSize();

    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(g_panelBg));
    dc.DrawRectangle(0, 0, sz.x, sz.y);

    wxGCDC gdc(dc);

    gdc.SetPen(*wxTRANSPARENT_PEN);
    gdc.SetBrush(wxBrush(g_previewBg));
    gdc.DrawRoundedRectangle(0, 0, sz.x, sz.y, FromDIP(g_previewRadius));

    if (bmp.IsOk()) {
        int insetW = sz.x - g_rescaleInsetMargin;
        int insetH = sz.y - g_rescaleInsetMargin;
        if (insetW >= g_minClientSize && insetH >= g_minClientSize) {
            wxImage img = bmp.ConvertToImage();
            wxSize imgSz = img.GetSize();
            if (imgSz.GetWidth() > 0 && imgSz.GetHeight() > 0) {
                double scale = std::min(
                    static_cast<double>(insetW) / imgSz.GetWidth(),
                    static_cast<double>(insetH) / imgSz.GetHeight());
                int newW = std::max(1, static_cast<int>(imgSz.GetWidth() * scale));
                int newH = std::max(1, static_cast<int>(imgSz.GetHeight() * scale));
                img.Rescale(newW, newH, wxIMAGE_QUALITY_HIGH);
                dc.DrawBitmap(wxBitmap(img), (sz.x - newW) / 2, (sz.y - newH) / 2);
            }
        }
    }
}

} // namespace GUI
} // namespace Slic3r
