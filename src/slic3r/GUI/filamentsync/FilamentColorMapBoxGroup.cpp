#include "FilamentColorMapBoxGroup.hpp"

#include <algorithm>

#include <wx/dcclient.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/dcmemory.h>
#include <wx/sizer.h>

#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "MachineFilamentPicker.hpp"

namespace
{

// ============================================================
// Container layout
// ============================================================
constexpr int g_containerPadding   = 16; // padding inside container
constexpr int g_containerRadius    = 4;  // border-radius
constexpr int g_containerBorderW   = 1;  // border width
constexpr int g_labelGap           = 20; // gap between labels and cards
constexpr int g_cardGap            = 20; // gap between cards (Figma: gap-[20px])

// Label vertical positioning: align with card top-bar text (y=7)
constexpr int g_labelDesignTopMargin = 6;  // align "Source Filament" with top bar text
constexpr int g_labelVerticalGap     = 24; // gap between "Source Filament" and "Printer Filament"

// ============================================================
// Colours
// ============================================================
const wxColour g_containerBg(0xFF, 0xFF, 0xFF);
const wxColour g_dialogBg(0xF8, 0xF7, 0xF7);
const wxColour g_containerBorder(0xF0, 0xF0, 0xF0);
const wxColour g_labelTextColor(0x24, 0x24, 0x24);
constexpr const char* g_defaultCardColor = "#CCCCCC";

// ============================================================
// Default below (placeholder)
// ============================================================
Slic3r::GUI::FilamentData makeDefaultBelow(unsigned int index)
{
    Slic3r::GUI::FilamentData d;
    d.m_index   = index;
    d.m_name    = "NONE";
    d.m_type    = "NONE";
    std::vector<std::string> colors = { g_defaultCardColor };
    d.m_color   = Slic3r::FilamentColor::FromColors(colors, Slic3r::FilamentColorMode::Segment);
    return d;
}

} // namespace

namespace Slic3r
{
namespace GUI
{

int FilamentColorMapBoxGroup::GetGridCols()
{
    const wxString lang = wxGetApp().app_config->get_language_code();
    if (lang.StartsWith("zh") || lang.StartsWith("ja") || lang.StartsWith("ko"))
        return 5;
    return 4;
}

FilamentColorMapBoxGroup::FilamentColorMapBoxGroup(wxWindow* parent,
                                                   const std::vector<FilamentData>& designDataList,
                                                   const std::vector<FilamentData>& machineDataList)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
              wxFULL_REPAINT_ON_RESIZE)
    , m_designDataList(designDataList)
    , m_machineDataList(machineDataList)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(g_containerBg);
    Bind(wxEVT_PAINT, &FilamentColorMapBoxGroup::onPaint, this);

    // ---- Outer horizontal sizer (after padding) ----
    auto* rowSizer = new wxBoxSizer(wxHORIZONTAL);

    // ---- Left label column ----
    auto* labelSizer = new wxBoxSizer(wxVERTICAL);

    m_pLabelDesign = new Label(this, _L("Source Filament"));
    m_pLabelDesign->SetFont(Label::Body_14);
    m_pLabelDesign->SetForegroundColour(g_labelTextColor);
    m_pLabelDesign->SetBackgroundStyle(wxBG_STYLE_TRANSPARENT);
    m_pLabelDesign->SetBackgroundColour(g_containerBg);
    labelSizer->AddSpacer(FromDIP(g_labelDesignTopMargin));
    labelSizer->Add(m_pLabelDesign, 0, wxEXPAND);
    labelSizer->AddSpacer(FromDIP(g_labelVerticalGap));

    m_pLabelMachine = new Label(this, _L("Printer Filament"));
    m_pLabelMachine->SetFont(Label::Body_14);
    m_pLabelMachine->SetForegroundColour(g_labelTextColor);
    m_pLabelMachine->SetBackgroundStyle(wxBG_STYLE_TRANSPARENT);
    m_pLabelMachine->SetBackgroundColour(g_containerBg);
    labelSizer->Add(m_pLabelMachine, 0, wxEXPAND);

    // Constrain both labels to the same width so the label column has a
    // predictable size regardless of which translation ends up longer.
    {
        int w1 = m_pLabelDesign->GetTextExtent(m_pLabelDesign->GetLabel()).GetWidth();
        int w2 = m_pLabelMachine->GetTextExtent(m_pLabelMachine->GetLabel()).GetWidth();
        int maxW = std::max(w1, w2);
        m_pLabelDesign->SetMinSize(wxSize(maxW, -1));
        m_pLabelMachine->SetMinSize(wxSize(maxW, -1));
    }
    labelSizer->AddStretchSpacer(1);

    rowSizer->Add(labelSizer, 0, wxEXPAND | wxRIGHT, FromDIP(g_labelGap));

    // ---- Right card grid — 5 columns, auto rows ----
    auto* cardGridSizer = new wxFlexGridSizer(0, GetGridCols(), FromDIP(g_cardGap), FromDIP(g_cardGap));

    int boxIndex = 0;
    for (const auto& designData : m_designDataList) {
        FilamentData initialBelow = makeDefaultBelow(designData.m_index);

        auto box = std::make_unique<FilamentColorMapBox>(this, designData, initialBelow);
        box->bindButton([this, boxIndex](const FilamentData&) {
            showMachineFilamentPicker(boxIndex);
        }, FilamentColorMapBox::ButtonType::Below);

        cardGridSizer->Add(box.get(), 0, wxALIGN_TOP);
        m_boxList.push_back(std::move(box));
        ++boxIndex;
    }

    rowSizer->Add(cardGridSizer, 0, wxALIGN_TOP);

    // ---- Padding around the row ----
    auto* outerSizer = new wxBoxSizer(wxVERTICAL);
    outerSizer->Add(rowSizer, 1, wxEXPAND | wxALL, FromDIP(g_containerPadding));
    SetSizer(outerSizer);
    Layout();
}

bool FilamentColorMapBoxGroup::Layout()
{
    bool ret = wxPanel::Layout();

    wxSize minSize = GetSizer()->CalcMin();
    SetMinSize(minSize);

    return ret;
}

void FilamentColorMapBoxGroup::onPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.Clear();

    wxSize sz = GetClientSize();
    int    w  = sz.x;
    int    h  = sz.y;

    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(g_dialogBg));
    dc.DrawRectangle(0, 0, w, h);

    wxGCDC gdc(dc);
    int    radius = FromDIP(g_containerRadius);

    gdc.SetPen(wxPen(g_containerBorder, FromDIP(g_containerBorderW)));
    gdc.SetBrush(wxBrush(g_containerBg));
    gdc.DrawRoundedRectangle(0, 0, w, h, radius);
}

std::vector<FilamentData> FilamentColorMapBoxGroup::getCurFilamentList() const
{
    std::vector<FilamentData> result;
    for (const auto& box : m_boxList) {
        result.push_back(box->getBelowData());
    }
    return result;
}

void FilamentColorMapBoxGroup::setGroupBoxEnable(bool bEnable, FilamentColorMapBox::ButtonType type)
{
    for (auto& box : m_boxList)
        box->setEnable(bEnable, type);
    Layout();
}

void FilamentColorMapBoxGroup::showMachineFilamentPicker(int boxIndex)
{
    if (boxIndex < 0 || boxIndex >= static_cast<int>(m_boxList.size()))
        return;

    if (m_pPicker) {
        m_pPicker->Destroy();
        m_pPicker = nullptr;
    }

    unsigned int curMachineIndex = m_boxList[boxIndex]->getBelowData().m_index;

    auto* picker = new MachineFilamentPicker(this, m_machineDataList, curMachineIndex);

    picker->bindSelectionCallback([this, boxIndex](const FilamentData& data) {
        updateBoxFilament(boxIndex, data);
        m_pPicker = nullptr;
    });

    picker->bindOnDismissCallback([this]() {
        m_pPicker = nullptr;
    });

    wxPoint screenPos = m_boxList[boxIndex]->GetScreenPosition();
    screenPos.y += m_boxList[boxIndex]->GetSize().y;

    m_pPicker = picker;
    picker->popupAt(screenPos);
}

void FilamentColorMapBoxGroup::updateBoxBelowData(int boxIndex, const FilamentData& machineData, bool bTriggerCallback /* = true */)
{
    updateBoxFilament(boxIndex, machineData, bTriggerCallback);
}

int FilamentColorMapBoxGroup::getBoxCount() const
{
    return m_boxList.size();
}

int FilamentColorMapBoxGroup::getVisibleBoxCount() const
{
    int visibleCount = 0;
    for (const auto& box : m_boxList) {
        if (box->IsShown())
            ++visibleCount;
    }
    return visibleCount;
}

bool FilamentColorMapBoxGroup::exceedsRowCount(int maxRows) const
{
    int visibleCount = getVisibleBoxCount();
    if (visibleCount == 0)
        return false;
    int totalRows = (visibleCount + GetGridCols() - 1) / GetGridCols();
    return totalRows > maxRows;
}

int FilamentColorMapBoxGroup::getHeightForRowCount(int rows) const
{
    if (rows <= 0 || m_boxList.empty())
        return 0;

    int cardH = m_boxList[0]->GetMinSize().y; // already DPI-scaled
    int vGap  = FromDIP(g_cardGap);
    int gridH = rows * cardH + std::max(0, rows - 1) * vGap;
    int pad   = FromDIP(g_containerPadding);
    return gridH + 2 * pad;
}

void FilamentColorMapBoxGroup::bindMappingChangedCallback(std::function<void()> cb)
{
    m_mappingChangedCallback = std::move(cb);
}

bool FilamentColorMapBoxGroup::dismissOpenPicker()
{
    if (m_pPicker) {
        m_pPicker->Dismiss();
        return true;
    }
    return false;
}

bool FilamentColorMapBoxGroup::hasOpenPicker() const
{
    return m_pPicker != nullptr;
}

void FilamentColorMapBoxGroup::updateBoxFilament(int boxIndex, const FilamentData& machineData, bool bTriggerCallback /* = true */)
{
    if (boxIndex < 0 || boxIndex >= m_boxList.size())
        return;

    m_boxList[boxIndex]->updateBelowData(machineData);

    if (m_mappingChangedCallback && bTriggerCallback)
        m_mappingChangedCallback();
}

void FilamentColorMapBoxGroup::setVisibleCount(int count)
{
    for (auto& box : m_boxList)
        box->Show(true);

    for (size_t i = count; i < m_boxList.size(); ++i)
        m_boxList[i]->Show(false);

    Layout();
    if (GetParent())
        GetParent()->Layout();
}

} // namespace GUI
} // namespace Slic3r
