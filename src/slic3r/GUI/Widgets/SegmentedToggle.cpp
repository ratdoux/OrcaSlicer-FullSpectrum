#include "SegmentedToggle.hpp"

#include <wx/sizer.h>

#include "Button.hpp"
#include "StaticBox.hpp"
#include "../GUI_App.hpp"

namespace
{

// --- Visual style (Figma specs) ---
constexpr int g_containerHeight  = 28;  // DIP — container height
constexpr int g_containerRadius  = 4;   // DIP
constexpr int g_buttonMinWidth   = 80;  // DIP
constexpr int g_buttonMinHeight  = 28;  // DIP
constexpr int g_buttonPaddingW   = 6;   // DIP
constexpr int g_buttonPaddingH   = 2;   // DIP
constexpr int g_buttonRadius     = 4;   // DIP
constexpr int g_buttonGap        = 4;   // DIP — gap between buttons
constexpr int g_buttonMarginV    = 0;   // DIP — vertical button margin (container already sized for button)
constexpr int g_outerMargin      = 6;   // DIP — (block 40 - container 28) / 2

// Unselected: transparent bg (container's #F8F7F7 shows through), #4A4A4A text
// Selected:   #009688 bg, #FEFEFE text
constexpr const char* g_containerBg    = "#F8F7F7";
constexpr const char* g_unselectedFg   = "#4A4A4A";
constexpr const char* g_selectedBg     = "#009688";
constexpr const char* g_selectedFg     = "#FEFEFE";

} // namespace

namespace Slic3r
{
namespace GUI
{

SegmentedToggle::SegmentedToggle(wxWindow* parent,
                                 const std::vector<wxString>& options,
                                 int selectedIndex)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    , m_selectedIndex(selectedIndex)
    , m_unselectedBg(StateColor())
    , m_unselectedFg(StateColor(std::pair(wxColour(g_unselectedFg), (int)StateColor::Normal)))
    , m_selectedBg(StateColor(std::pair(wxColour(g_selectedBg), (int)StateColor::Normal)))
    , m_selectedFg(StateColor(std::pair(wxColour(g_selectedFg), (int)StateColor::Normal)))
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));

    auto* outerSizer = new wxBoxSizer(wxVERTICAL);

    m_pContainer = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_pContainer->SetCornerRadius(FromDIP(g_containerRadius));
    m_pContainer->SetBorderWidth(0);
    m_pContainer->SetMinSize(wxSize(-1, FromDIP(g_containerHeight)));
    m_pContainer->SetBackgroundColor(
        StateColor(std::pair(wxColour(g_containerBg), (int)StateColor::Normal)));

    auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);

    for (int i = 0; i < (int)options.size(); ++i) {
        if (i > 0)
            btnSizer->AddSpacer(FromDIP(g_buttonGap));

        auto* btn = new Button(m_pContainer, options[i]);
        btn->SetMinSize(wxSize(FromDIP(g_buttonMinWidth), FromDIP(g_buttonMinHeight)));
        btn->SetPaddingSize(wxSize(FromDIP(g_buttonPaddingW), FromDIP(g_buttonPaddingH)));
        btn->SetCornerRadius(FromDIP(g_buttonRadius));
        btn->SetBorderWidth(0);
        btn->SetFont(Label::Body_12);

        if (i == m_selectedIndex) {
            btn->SetBackgroundColor(m_selectedBg);
            btn->SetTextColor(m_selectedFg);
            btn->SetCanFocus(false);
        } else {
            btn->SetBackgroundColor(m_unselectedBg);
            btn->SetTextColor(m_unselectedFg);
        }

        btn->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) {
            onButtonClicked(i);
        });

        m_buttons.push_back(btn);
        btnSizer->Add(btn, 1, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(g_buttonMarginV));
    }

    m_pContainer->SetSizer(btnSizer);

    outerSizer->Add(m_pContainer, 1, wxEXPAND | wxALL, FromDIP(g_outerMargin));
    SetSizer(outerSizer);
    Layout();
}

void SegmentedToggle::setSelected(int index)
{
    if (index < 0 || index >= (int)m_buttons.size() || index == m_selectedIndex)
        return;

    // Deselect previous
    m_buttons[m_selectedIndex]->SetBackgroundColor(m_unselectedBg);
    m_buttons[m_selectedIndex]->SetTextColor(m_unselectedFg);
    m_buttons[m_selectedIndex]->SetCanFocus(true);

    // Select new
    m_selectedIndex = index;
    m_buttons[m_selectedIndex]->SetBackgroundColor(m_selectedBg);
    m_buttons[m_selectedIndex]->SetTextColor(m_selectedFg);
    m_buttons[m_selectedIndex]->SetCanFocus(false);
}

int SegmentedToggle::getSelected() const
{
    return m_selectedIndex;
}

void SegmentedToggle::bindSelectionCallback(SelectionCallback cb)
{
    m_selectionCallback = std::move(cb);
}

void SegmentedToggle::onButtonClicked(int index)
{
    if (index == m_selectedIndex)
        return;

    setSelected(index);

    if (m_selectionCallback)
        m_selectionCallback(m_selectedIndex);
}

} // namespace GUI
} // namespace Slic3r
