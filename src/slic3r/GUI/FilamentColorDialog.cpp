#include "FilamentColorDialog.hpp"

#include "FilamentColorUtils.hpp"
#include "GUI_App.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/StaticBox.hpp"
#include "wxExtensions.hpp"
#include "libslic3r/AppConfig.hpp"

#include <ColorSpaceConvert.hpp>
#include <boost/log/trivial.hpp>

#include <wx/colordlg.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/display.h>
#include <wx/panel.h>
#include <wx/region.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/wrapsizer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace Slic3r
{
namespace GUI
{
namespace
{

/**
 * @brief HSV value cached for color sorting.
 */
struct SortHsv
{
    float h { 0.0f };
    float s { 0.0f };
    float v { 0.0f };
};

/**
 * @brief Lightweight color item with cached sort data.
 */
struct ColorSortItem
{
    const FilamentColorItem* colorItem { nullptr };
    std::vector<std::string> colors;
    std::vector<SortHsv> hsvColors;
    int typeOrder { 0 };
    std::string enName;
    std::string zhName;
};

/**
 * @brief Gets the current language code used by the filament color data.
 */
std::string GetLanguageCode()
{
    if (wxGetApp().app_config == nullptr)
        return "en";

    const std::string language = wxGetApp().app_config->get("language");
    if (language.empty())
        return "en";
    if (language == "zh-cn")
        return "zh_CN";
    return language;
}

/**
 * @brief Converts an UTF-8 string to wxString.
 */
wxString FromStdString(const std::string& value)
{
    return wxString::FromUTF8(value.c_str());
}

/**
 * @brief Gets a localized color name with English fallback.
 */
std::string GetLocalizedColorName(const FilamentColorItem& colorItem, const std::string& languageCode,
                                  const std::vector<std::string>& colors)
{
    std::unordered_map<std::string, std::string>::const_iterator localizedName =
        colorItem.colorNames.find(languageCode);
    if (localizedName != colorItem.colorNames.end() && !localizedName->second.empty())
        return localizedName->second;

    std::unordered_map<std::string, std::string>::const_iterator englishName = colorItem.colorNames.find("en");
    if (englishName != colorItem.colorNames.end() && !englishName->second.empty())
        return englishName->second;

    if (colors.size() > 1)
        return _L("Multiple Color").ToStdString(wxConvUTF8);
    return colors.empty() ? std::string() : colors.front();
}

/**
 * @brief Gets normalized colors already loaded by the filament color library.
 */
std::vector<std::string> ColorListFromLibrary(const FilamentColorItem& colorItem)
{
    return colorItem.colorData.colors;
}

/**
 * @brief Builds color data from a built-in color.
 */
bool MakeFilamentColor(const FilamentColorItem& colorItem, FilamentColor& colorData)
{
    if (colorItem.colorData.colors.empty())
    {
        BOOST_LOG_TRIVIAL(error) << "Invalid official filament color without valid colors: " << colorItem.sku;
        return false;
    }

    colorData = FilamentColor::FromColors(colorItem.colorData.colors, colorItem.colorData.mode);
    return true;
}

/**
 * @brief Finds an official color item matching the saved colors and display mode.
 */
std::vector<FilamentColorItem>::const_iterator FindColorBySavedColors(const FilamentColorInfo& filament,
                                                                      const FilamentColor& colorData)
{
    if (colorData.colors.empty())
        return filament.colors.end();

    const FilamentColor normalizedColor = FilamentColor::FromColors(colorData.colors, colorData.mode);
    return std::find_if(filament.colors.begin(), filament.colors.end(),
                        [&normalizedColor](const FilamentColorItem& item)
                        {
                            return item.colorData.Matches(normalizedColor);
                        });
}

/**
 * @brief Gets the preview name for the current selected color data.
 */
std::string GetSelectionDisplayName(const FilamentColorInfo& filament, const FilamentColor& colorData,
                                    const std::string& selectedSku, const std::string& languageCode)
{
    if (!selectedSku.empty())
    {
        std::vector<FilamentColorItem>::const_iterator colorIt =
            std::find_if(filament.colors.begin(), filament.colors.end(),
                         [&selectedSku](const FilamentColorItem& colorItem)
                         {
                             return colorItem.sku == selectedSku;
                         });
        if (colorIt != filament.colors.end())
        {
            const std::vector<std::string> officialColors = ColorListFromLibrary(*colorIt);
            const std::string name = GetLocalizedColorName(*colorIt, languageCode, officialColors);
            if (!name.empty())
                return name;
        }
    }

    if (colorData.colors.size() > 1)
        return _L("Multiple Color").ToStdString(wxConvUTF8);
    return colorData.PrimaryColor("#26A69A");
}

/**
 * @brief Converts a color to HSV for sorting.
 */
SortHsv MakeSortHsvFromHex(const std::string& color)
{
    wxColour wxColor(color);
    if (!wxColor.IsOk())
        return SortHsv();

    SortHsv hsv;
    RGB2HSV(static_cast<float>(wxColor.Red()) / 255.0f, static_cast<float>(wxColor.Green()) / 255.0f,
            static_cast<float>(wxColor.Blue()) / 255.0f, &hsv.h, &hsv.s, &hsv.v);
    if (hsv.h < 0.0f)
        hsv.h += 360.0f;
    return hsv;
}

/**
 * @brief Gets the display type order used by the color grid.
 */
int GetColorTypeOrder(const std::vector<std::string>& colors, FilamentColorMode mode)
{
    if (colors.size() <= 1)
        return 0;
    return mode == FilamentColorMode::Gradient ? 2 : 1;
}

/**
 * @brief Compares two HSV values.
 */
int CompareHsv(const SortHsv& left, const SortHsv& right)
{
    if (left.h != right.h)
        return left.h < right.h ? -1 : 1;
    if (left.s != right.s)
        return left.s < right.s ? -1 : 1;
    if (left.v != right.v)
        return left.v < right.v ? -1 : 1;
    return 0;
}

/**
 * @brief Gets a language-specific name used as the final sort tie breaker.
 */
std::string GetColorNameForSort(const FilamentColorItem& colorItem, const std::string& languageCode)
{
    std::unordered_map<std::string, std::string>::const_iterator name = colorItem.colorNames.find(languageCode);
    return name != colorItem.colorNames.end() ? name->second : std::string();
}

/**
 * @brief Builds a lightweight, precomputed sort key.
 */
ColorSortItem MakeColorSortItem(const FilamentColorItem& colorItem)
{
    ColorSortItem item;
    item.colorItem = &colorItem;
    item.colors = ColorListFromLibrary(colorItem);
    item.typeOrder = GetColorTypeOrder(item.colors, colorItem.colorData.NormalizedMode());
    item.hsvColors.reserve(item.colors.size());
    for (const std::string& hexColor : item.colors)
        item.hsvColors.emplace_back(MakeSortHsvFromHex(hexColor));
    item.enName = GetColorNameForSort(colorItem, "en");
    item.zhName = GetColorNameForSort(colorItem, "zh_CN");
    return item;
}

/**
 * @brief Compares two precomputed sort keys.
 */
bool ColorSortItemLess(const ColorSortItem& left, const ColorSortItem& right)
{
    if (left.colorItem == nullptr)
        return false;
    if (right.colorItem == nullptr)
        return true;

    if (left.typeOrder != right.typeOrder)
        return left.typeOrder < right.typeOrder;

    const size_t color_count = std::min(left.hsvColors.size(), right.hsvColors.size());
    for (size_t index = 0; index < color_count; ++index)
    {
        const int hsv_compare = CompareHsv(left.hsvColors[index], right.hsvColors[index]);
        if (hsv_compare != 0)
            return hsv_compare < 0;
    }

    if (left.colors.size() != right.colors.size())
        return left.colors.size() < right.colors.size();
    if (left.colorItem->sku != right.colorItem->sku)
        return left.colorItem->sku < right.colorItem->sku;
    if (left.enName != right.enName)
        return left.enName < right.enName;
    return left.zhName < right.zhName;
}

/**
 * @brief Builds sorted lightweight items without copying full color records.
 */
std::vector<ColorSortItem> BuildSortedColorItems(const std::vector<FilamentColorItem>& colorItems)
{
    std::vector<ColorSortItem> items;
    items.reserve(colorItems.size());
    for (const FilamentColorItem& colorItem : colorItems)
        items.emplace_back(MakeColorSortItem(colorItem));

    std::stable_sort(items.begin(), items.end(), ColorSortItemLess);
    return items;
}

/**
 * @brief Gets the light swatch border color used by the official color dialog.
 */
wxColour FilamentColorLightBorderColor()
{
    return StateColor::darkModeColorFor(wxColour(180, 180, 180));
}

wxBitmap MakePreviewBitmap(const FilamentColor& colorData, int size);

wxFont DialogFont(wxFontWeight weight)
{
    wxFont font = Label::Body_14;
    font.SetWeight(weight);
    return font;
}

/**
 * @brief Small swatch used by the filament color dialog.
 */
class FilamentColorSwatch : public wxPanel
{
public:
    FilamentColorSwatch(wxWindow* parent, const FilamentColorItem& colorItem,
                        const std::string& languageCode, const wxSize& size)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
        , _colorItem(&colorItem)
        , _languageCode(languageCode)
    {
        SetMinSize(size);
        SetMaxSize(size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        const std::vector<std::string> colors = ColorListFromLibrary(colorItem);
        const std::string tooltip = GetLocalizedColorName(colorItem, languageCode, colors);
        if (!tooltip.empty())
            SetToolTip(FromStdString(tooltip));
        Bind(wxEVT_PAINT, &FilamentColorSwatch::OnPaint, this);
    }

    /**
     * @brief Updates the selected visual state.
     */
    void SetSelected(bool selected)
    {
        if (_selected == selected)
            return;
        _selected = selected;
        Refresh();
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        dc.SetBackground(wxBrush(StateColor::darkModeColorFor(wxColour("#FFFFFF"))));
        dc.Clear();

        if (_colorItem == nullptr)
            return;

        const int selectedBorderWidth = std::max(FromDIP(1), 2);
        const int selectedGap = FromDIP(1);
        const int fillInset = selectedBorderWidth + selectedGap;
        const int fillWidth = std::max(1, size.GetWidth() - fillInset * 2);
        const int fillHeight = std::max(1, size.GetHeight() - fillInset * 2);
        const wxRect fillRect(fillInset, fillInset, fillWidth, fillHeight);
        FilamentColor colorData;
        if (!MakeFilamentColor(*_colorItem, colorData))
            return;

        const int bitmapWidth = fillRect.GetWidth();
        const int bitmapHeight = fillRect.GetHeight();
        const wxColour borderColor = FilamentColorLightBorderColor();
        wxBitmap* bitmap = FilamentColorUtils::GetFilamentColorIcon(colorData.colors, colorData.NormalizedMode(), "",
                                                                    bitmapWidth, bitmapHeight, borderColor);
        if (bitmap != nullptr)
            dc.DrawBitmap(*bitmap, fillRect.GetLeft(), fillRect.GetTop(), true);

        if (_selected)
        {
            const int middleHeight = std::max(0, size.GetHeight() - selectedBorderWidth * 2);
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour("#019687")));
            dc.DrawRectangle(0, 0, size.GetWidth(), selectedBorderWidth);
            dc.DrawRectangle(0, selectedBorderWidth, selectedBorderWidth, middleHeight);
            dc.DrawRectangle(size.GetWidth() - selectedBorderWidth, selectedBorderWidth,
                             selectedBorderWidth, middleHeight);
            dc.DrawRectangle(0, size.GetHeight() - selectedBorderWidth, size.GetWidth(), selectedBorderWidth);
        }
    }

private:
    const FilamentColorItem* _colorItem { nullptr };
    std::string _languageCode;
    bool _selected { false };
};

/**
 * @brief Tertiary button used for the custom color entry.
 */
class MoreColorPanel : public wxPanel
{
public:
    MoreColorPanel(wxWindow* parent, const wxSize& size)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
        , _icon(this, "filament_color_more", 16)
    {
        SetMinSize(size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &MoreColorPanel::OnPaint, this);
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        dc.SetBackground(wxBrush(StateColor::darkModeColorFor(wxColour("#F0F0F0"))));
        dc.Clear();

        dc.SetFont(DialogFont(wxFONTWEIGHT_MEDIUM));
        dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#242424")));
        const wxString label = _L("Other Colors");
        wxCoord textWidth = 0;
        wxCoord textHeight = 0;
        dc.GetTextExtent(label, &textWidth, &textHeight);
        const int iconWidth = _icon.bmp().IsOk() ? _icon.GetBmpWidth() : FromDIP(16);
        const int iconHeight = _icon.bmp().IsOk() ? _icon.GetBmpHeight() : FromDIP(16);
        const int gap = FromDIP(4);
        const int contentWidth = iconWidth + gap + static_cast<int>(textWidth);
        const int iconX = std::max(0, (size.GetWidth() - contentWidth) / 2);
        const int iconY = std::max(0, (size.GetHeight() - iconHeight) / 2);
        const int textX = iconX + iconWidth + gap;
        const int textY = std::max(0, (size.GetHeight() - static_cast<int>(textHeight)) / 2);

        if (_icon.bmp().IsOk())
        {
            dc.DrawBitmap(_icon.bmp(), iconX, iconY, true);
        }
        else
        {
            dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#8A8A8A")), FromDIP(2)));
            dc.DrawLine(iconX + iconWidth / 2, iconY + FromDIP(4),
                        iconX + iconWidth / 2, iconY + iconHeight - FromDIP(4));
            dc.DrawLine(iconX + FromDIP(4), iconY + iconHeight / 2,
                        iconX + iconWidth - FromDIP(4), iconY + iconHeight / 2);
        }
        dc.DrawText(label, textX, textY);
    }

private:
    ScalableBitmap _icon;
};

wxBitmap MakePreviewBitmap(const FilamentColor& colorData, int size)
{
    size = std::max(1, size);
    wxBitmap output(size, size);
    wxMemoryDC dc;
    dc.SelectObject(output);
    dc.SetBackground(wxBrush(StateColor::darkModeColorFor(wxColour("#FFFFFF"))));
    dc.Clear();

    const wxColour borderColor = FilamentColorLightBorderColor();
    const FilamentColorMode colorMode = colorData.NormalizedMode();
    wxBitmap* bitmap = FilamentColorUtils::GetFilamentColorIcon(colorData.colors, colorMode, "",
                                                                size, size, borderColor);
    if (bitmap != nullptr)
        dc.DrawBitmap(*bitmap, 0, 0, true);

    dc.SelectObject(wxNullBitmap);
    return output;
}

wxRegion MakeRoundedRegion(const wxSize& size, int radius)
{
    const int width = std::max(1, size.GetWidth());
    const int height = std::max(1, size.GetHeight());
    radius = std::max(0, std::min(radius, std::min(width, height) / 2));
    if (radius == 0)
        return wxRegion(0, 0, width, height);

    constexpr double pi = 3.14159265358979323846;
    const int segments = 8;
    std::vector<wxPoint> points;
    points.reserve((segments + 1) * 4);

    auto appendArc = [&points, radius, segments, pi](int centerX, int centerY, double start, double end)
    {
        for (int index = 0; index <= segments; ++index)
        {
            const double ratio = static_cast<double>(index) / static_cast<double>(segments);
            const double angle = (start + (end - start) * ratio) * pi / 180.0;
            const int x = centerX + static_cast<int>(std::round(std::cos(angle) * radius));
            const int y = centerY + static_cast<int>(std::round(std::sin(angle) * radius));
            points.emplace_back(x, y);
        }
    };

    appendArc(width - radius - 1, radius, -90.0, 0.0);
    appendArc(width - radius - 1, height - radius - 1, 0.0, 90.0);
    appendArc(radius, height - radius - 1, 90.0, 180.0);
    appendArc(radius, radius, 180.0, 270.0);
    return wxRegion(static_cast<int>(points.size()), points.data());
}

bool IsValidWindowRect(const wxRect& rect)
{
    return rect.GetWidth() > 0 && rect.GetHeight() > 0;
}

wxRect WindowClientScreenRect(const wxWindow* window)
{
    if (window == nullptr)
        return wxRect();

    const wxSize size = window->GetClientSize();
    if (size.GetWidth() <= 0 || size.GetHeight() <= 0)
        return wxRect();

    return wxRect(window->ClientToScreen(wxPoint(0, 0)), size);
}

} // namespace

FilamentColorDialog::FilamentColorDialog(wxWindow* parent, const FilamentColorInfo& filament,
                                         const FilamentColor& currentColor)
    : DPIDialog(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxFRAME_SHAPED)
    , _filament(filament)
    , _languageCode(GetLanguageCode())
{
    const FilamentColor normalizedCurrent =
        FilamentColor::FromColors(currentColor.colors, currentColor.mode, "#26A69A");
    std::vector<FilamentColorItem>::const_iterator currentColorIt =
        FindColorBySavedColors(_filament, normalizedCurrent);

    if (currentColorIt != _filament.colors.end() && MakeFilamentColor(*currentColorIt, _selection))
    {
        _selectedSku = currentColorIt->sku;
        _highlightSku = currentColorIt->sku;
    }
    else
    {
        _selectedSku.clear();
        _highlightSku.clear();
        _selection = normalizedCurrent;
    }

    BuildUi();
    UpdatePreview();
    UpdateSwatchSelection();
}

void FilamentColorDialog::BuildUi()
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));

    wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);
    const int dialogWidth = FromDIP(380);
    const int contentMargin = FromDIP(16);
    const int buttonMargin = FromDIP(20);
    const int contentWidth = dialogWidth - contentMargin * 2;

    StaticBox* card = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    card->SetBackgroundColorNormal(wxColour("#FFFFFF"));
    card->SetCornerRadius(FromDIP(8));
    wxBoxSizer* cardSizer = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer* currentRow = new wxBoxSizer(wxHORIZONTAL);
    _previewBitmap = new wxStaticBitmap(card, wxID_ANY, wxBitmap(FromDIP(60), FromDIP(60)));
    BindDragWindow(_previewBitmap);
    currentRow->Add(_previewBitmap, 0, wxALIGN_TOP | wxRIGHT, FromDIP(12));

    wxBoxSizer* infoSizer = new wxBoxSizer(wxVERTICAL);
    _nameLabel = new wxStaticText(card, wxID_ANY, wxEmptyString);
    _nameLabel->SetFont(DialogFont(wxFONTWEIGHT_SEMIBOLD));
    _nameLabel->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
    _nameLabel->SetMinSize(wxSize(-1, FromDIP(20)));
    BindDragWindow(_nameLabel);
    _skuLabel = new wxStaticText(card, wxID_ANY, wxEmptyString);
    _skuLabel->SetFont(DialogFont(wxFONTWEIGHT_MEDIUM));
    _skuLabel->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#4A4A4A")));
    _skuLabel->SetMinSize(wxSize(-1, FromDIP(20)));
    BindDragWindow(_skuLabel);
    infoSizer->Add(_nameLabel, 0, wxBOTTOM, FromDIP(6));
    infoSizer->Add(_skuLabel, 0);
    currentRow->Add(infoSizer, 1, wxALIGN_TOP);
    cardSizer->AddSpacer(FromDIP(28));
    cardSizer->Add(currentRow, 0, wxEXPAND | wxLEFT | wxRIGHT, contentMargin);

    wxStaticText* officialLabel = new wxStaticText(card, wxID_ANY, _L("Official Filaments"));
    officialLabel->SetFont(DialogFont(wxFONTWEIGHT_MEDIUM));
    officialLabel->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
    officialLabel->SetMinSize(wxSize(-1, FromDIP(20)));
    BindDragWindow(officialLabel);
    cardSizer->AddSpacer(FromDIP(20));
    cardSizer->Add(officialLabel, 0, wxLEFT | wxRIGHT, contentMargin);

    wxPanel* swatchPanel = new wxPanel(card, wxID_ANY);
    swatchPanel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    const int swatchColumns = 10;
    const int swatchColorSize = FromDIP(24);
    const int swatchFrameInset = FromDIP(3);
    const wxSize swatchSize(swatchColorSize + swatchFrameInset * 2, swatchColorSize + swatchFrameInset * 2);
    const int swatchHGap = FromDIP(5);
    const int swatchVGap = 0;
    wxGridSizer* swatchSizer = new wxGridSizer(0, swatchColumns, swatchVGap, swatchHGap);
    const std::vector<ColorSortItem> sortedItems = BuildSortedColorItems(_filament.colors);
    for (const ColorSortItem& item : sortedItems)
    {
        if (item.colorItem == nullptr)
            continue;

        const FilamentColorItem* colorItem = item.colorItem;
        FilamentColorSwatch* swatch = new FilamentColorSwatch(swatchPanel, *colorItem, _languageCode, swatchSize);
        swatch->Bind(wxEVT_LEFT_UP, [this, colorItem](wxMouseEvent&)
        {
            if (colorItem != nullptr)
                SelectFilamentColor(*colorItem);
        });
        swatchSizer->Add(swatch, 0);
        _swatchBySku.emplace_back(swatch, colorItem->sku);
    }
    const size_t swatchCount = _swatchBySku.size();
    const size_t swatchColumnCount = static_cast<size_t>(swatchColumns);
    int swatchRows = 0;
    if (swatchCount > 0)
        swatchRows = static_cast<int>((swatchCount + swatchColumnCount - 1) / swatchColumnCount);
    const int swatchVisibleRows = std::max(2, swatchRows);
    const int swatchPanelHeight = swatchVisibleRows * swatchSize.GetHeight() + (swatchVisibleRows - 1) * swatchVGap;
    swatchPanel->SetMinSize(wxSize(contentWidth, swatchPanelHeight));
    swatchPanel->SetSizer(swatchSizer);
    cardSizer->AddSpacer(FromDIP(20));
    cardSizer->Add(swatchPanel, 0, wxLEFT | wxRIGHT, contentMargin);

    MoreColorPanel* moreButton = new MoreColorPanel(card, wxSize(contentWidth, FromDIP(40)));
    moreButton->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&)
    {
        OpenMoreColorDialog();
    });
    cardSizer->AddSpacer(FromDIP(20));
    cardSizer->Add(moreButton, 0, wxLEFT | wxRIGHT, contentMargin);

    cardSizer->AddSpacer(FromDIP(16));
    wxPanel* divider = new wxPanel(card, wxID_ANY);
    divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F0F0F0")));
    divider->SetMinSize(wxSize(dialogWidth, FromDIP(1)));
    BindDragWindow(divider);
    cardSizer->Add(divider, 0, wxEXPAND);

    wxPanel* buttonPanel = new wxPanel(card, wxID_ANY);
    buttonPanel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    buttonPanel->SetMinSize(wxSize(dialogWidth, FromDIP(61)));
    BindDragWindow(buttonPanel);
    wxBoxSizer* buttonPanelSizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer* buttons = new wxBoxSizer(wxHORIZONTAL);
    Button* cancel = new Button(buttonPanel, _L("Cancel"), wxEmptyString, wxBORDER_NONE, 0, wxID_CANCEL);
    const wxSize buttonSize(FromDIP(166), FromDIP(38));
    cancel->SetMinSize(buttonSize);
    cancel->SetSize(buttonSize);
    cancel->SetCornerRadius(FromDIP(4));
    cancel->SetBorderWidth(FromDIP(1));
    cancel->SetBackgroundColorNormal(wxColour("#FFFFFF"));
    cancel->SetBorderColorNormal(wxColour("#D1D5DC"));
    cancel->SetTextColorNormal(wxColour("#242424"));
    cancel->SetFont(DialogFont(wxFONTWEIGHT_MEDIUM));
    Button* ok = new Button(buttonPanel, _L("OK"), wxEmptyString, wxBORDER_NONE, 0, wxID_OK);
    ok->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    ok->SetMinSize(buttonSize);
    ok->SetSize(buttonSize);
    ok->SetCornerRadius(FromDIP(4));
    ok->SetBackgroundColorNormal(wxColour("#019687"));
    ok->SetBorderColorNormal(wxColour("#019687"));
    ok->SetTextColorNormal(wxColour("#FFFFFF"));
    ok->SetFont(DialogFont(wxFONTWEIGHT_MEDIUM));

    buttons->Add(cancel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    buttons->Add(ok, 0, wxALIGN_CENTER_VERTICAL);
    buttonPanelSizer->AddSpacer(FromDIP(12));
    buttonPanelSizer->Add(buttons, 0, wxLEFT | wxRIGHT, buttonMargin);
    buttonPanel->SetSizer(buttonPanelSizer);
    cardSizer->Add(buttonPanel, 0, wxEXPAND);

    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        EndModal(wxID_CANCEL);
    });

    ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        EndModal(wxID_OK);
    });

    card->SetSizer(cardSizer);
    BindDragWindow(card);
    BindDragWindow(this);
    root->Add(card, 0, wxEXPAND);
    SetSizer(root);
    root->SetSizeHints(this);
    SetClientSize(wxSize(dialogWidth, root->CalcMin().GetHeight()));
    SetMinClientSize(wxSize(dialogWidth, root->CalcMin().GetHeight()));
    UpdateRoundedShape();
    Layout();
    PlaceNearFilamentPanel();
}

void FilamentColorDialog::SelectFilamentColor(const FilamentColorItem& colorItem)
{
    FilamentColor colorData;
    if (!MakeFilamentColor(colorItem, colorData))
        return;

    _selection = colorData;
    _selectedSku = colorItem.sku;
    _highlightSku = colorItem.sku;
    UpdatePreview();
    UpdateSwatchSelection();
}

void FilamentColorDialog::SelectCustomColor(const std::string& color)
{
    const std::string normalized = FilamentColorUtils::NormalizeHexColor(color, "#26A69A");
    _selection = FilamentColor::FromMultiColors(normalized, FilamentColorMode::Segment, normalized);
    _selectedSku.clear();
    _highlightSku.clear();
    UpdatePreview();
    UpdateSwatchSelection();
}

void FilamentColorDialog::UpdatePreview()
{
    if (_previewBitmap == nullptr || _nameLabel == nullptr || _skuLabel == nullptr)
        return;

    _previewBitmap->SetBitmap(MakePreviewBitmap(_selection, FromDIP(60)));

    const wxString name = FromStdString(GetSelectionDisplayName(_filament, _selection, _selectedSku, _languageCode));
    _nameLabel->SetLabel(name);
    _skuLabel->SetLabel(_selectedSku.empty() ? wxString(wxEmptyString) : FromStdString("sku " + _selectedSku));
    _skuLabel->Show(!_selectedSku.empty());
    Layout();
}

void FilamentColorDialog::UpdateSwatchSelection()
{
    for (const std::pair<wxWindow*, std::string>& item : _swatchBySku)
    {
        FilamentColorSwatch* swatch = dynamic_cast<FilamentColorSwatch*>(item.first);
        if (swatch != nullptr)
            swatch->SetSelected(!_highlightSku.empty() && item.second == _highlightSku);
    }
}

void FilamentColorDialog::on_dpi_changed(const wxRect&)
{
    Fit();
    UpdateRoundedShape();
    PlaceNearFilamentPanel();
}

void FilamentColorDialog::UpdateRoundedShape()
{
    const wxSize size = GetSize();
    if (size.GetWidth() <= 0 || size.GetHeight() <= 0)
        return;

    SetShape(MakeRoundedRegion(size, FromDIP(8)));
}

void FilamentColorDialog::BindDragWindow(wxWindow* window)
{
    if (window == nullptr)
        return;

    window->Bind(wxEVT_LEFT_DOWN, &FilamentColorDialog::StartDrag, this);
    window->Bind(wxEVT_MOTION, &FilamentColorDialog::DragDialog, this);
    window->Bind(wxEVT_LEFT_UP, &FilamentColorDialog::EndDrag, this);
}

void FilamentColorDialog::StartDrag(wxMouseEvent& event)
{
    _dragPending = true;
    _isDragging = false;
    _dragStartMouse = wxGetMousePosition();
    _dragStartPosition = GetScreenPosition();
    event.Skip();
}

void FilamentColorDialog::DragDialog(wxMouseEvent& event)
{
    if (!_dragPending && !_isDragging)
    {
        event.Skip();
        return;
    }

    if (!event.LeftIsDown())
    {
        _dragPending = false;
        _isDragging = false;
        if (HasCapture())
            ReleaseMouse();
        event.Skip();
        return;
    }

    const wxPoint currentMouse = wxGetMousePosition();
    if (!_isDragging)
    {
        const int threshold = FromDIP(3);
        const int distanceX = std::abs(currentMouse.x - _dragStartMouse.x);
        const int distanceY = std::abs(currentMouse.y - _dragStartMouse.y);
        if (distanceX < threshold && distanceY < threshold)
        {
            event.Skip();
            return;
        }

        _isDragging = true;
        if (!HasCapture())
            CaptureMouse();
    }

    if (!_isDragging)
    {
        event.Skip();
        return;
    }

    Move(wxPoint(_dragStartPosition.x + currentMouse.x - _dragStartMouse.x,
                 _dragStartPosition.y + currentMouse.y - _dragStartMouse.y));
}

void FilamentColorDialog::EndDrag(wxMouseEvent& event)
{
    if (_dragPending || _isDragging)
    {
        _dragPending = false;
        _isDragging = false;
        if (HasCapture())
            ReleaseMouse();
    }
    event.Skip();
}

void FilamentColorDialog::PlaceNearFilamentPanel()
{
    wxWindow* parent = GetParent();
    wxWindow* topWindow = parent != nullptr ? wxGetTopLevelParent(parent) : nullptr;
    const wxRect parentRect = WindowClientScreenRect(parent);
    const wxRect topRect = WindowClientScreenRect(topWindow);
    const wxSize dialogSize = GetSize();

    wxRect horizontalAnchor = parentRect;
    wxRect verticalAnchor = parentRect;
    const int maxAnchorRight = IsValidWindowRect(topRect) ? topRect.GetLeft() + topRect.GetWidth() / 2 : 0;
    const int minPanelWidth = FromDIP(220);
    const int minPanelHeight = FromDIP(80);

    for (wxWindow* window = parent; window != nullptr && window != topWindow; window = window->GetParent())
    {
        const wxRect rect = WindowClientScreenRect(window);
        if (!IsValidWindowRect(rect))
            continue;

        const bool isLeftPanel = !IsValidWindowRect(topRect) || rect.GetRight() <= maxAnchorRight;
        if (isLeftPanel && (!IsValidWindowRect(horizontalAnchor) || rect.GetRight() >= horizontalAnchor.GetRight()))
            horizontalAnchor = rect;

        const bool containsParent = IsValidWindowRect(parentRect) &&
            rect.GetTop() <= parentRect.GetTop() && rect.GetBottom() >= parentRect.GetBottom();
        if (containsParent && rect.GetWidth() >= minPanelWidth && rect.GetHeight() >= minPanelHeight)
        {
            verticalAnchor = rect;
            break;
        }
    }

    const wxPoint fallbackPosition = topWindow != nullptr ? topWindow->ClientToScreen(wxPoint(0, 0)) : wxPoint(0, 0);
    int x = IsValidWindowRect(horizontalAnchor) ? horizontalAnchor.GetRight() + FromDIP(8) : fallbackPosition.x;
    int y = IsValidWindowRect(verticalAnchor) ? verticalAnchor.GetTop() : fallbackPosition.y + FromDIP(100);

    const int displayIndex = wxDisplay::GetFromWindow(parent != nullptr ? parent : this);
    const wxDisplay display(displayIndex == wxNOT_FOUND ? 0 : static_cast<unsigned int>(displayIndex));
    const wxRect area = display.IsOk() ? display.GetClientArea() : wxRect(fallbackPosition, wxSize(1920, 1080));
    x = std::max(area.GetLeft() + FromDIP(8), std::min(x, area.GetRight() - dialogSize.GetWidth() - FromDIP(8)));
    y = std::max(area.GetTop() + FromDIP(8), std::min(y, area.GetBottom() - dialogSize.GetHeight() - FromDIP(8)));
    Move(wxPoint(x, y));
}

void FilamentColorDialog::OpenMoreColorDialog()
{
    wxColourData data;
    data.SetChooseFull(true);
    data.SetChooseAlpha(false);
    data.SetColour(wxColour(_selection.PrimaryColor("#26A69A")));

    std::vector<std::string> custom_colors;
    if (wxGetApp().app_config != nullptr)
        custom_colors = wxGetApp().app_config->get_custom_color_from_config();

    const int custom_count = std::min(static_cast<int>(custom_colors.size()), CUSTOM_COLOR_COUNT);
    for (int index = 0; index < custom_count; ++index)
        data.SetCustomColour(index, string_to_wxColor(custom_colors[index]));

    wxColourDialog dialog(this, &data);
    dialog.SetTitle(_L("Please choose the filament color"));
    if (dialog.ShowModal() != wxID_OK)
        return;

    const wxColourData result = dialog.GetColourData();
    if (custom_colors.size() != CUSTOM_COLOR_COUNT)
        custom_colors.resize(CUSTOM_COLOR_COUNT);
    for (int index = 0; index < CUSTOM_COLOR_COUNT; ++index)
        custom_colors[index] = color_to_string(result.GetCustomColour(index));
    if (wxGetApp().app_config != nullptr)
        wxGetApp().app_config->save_custom_color_to_config(custom_colors);

    const wxColour color = result.GetColour();
    if (color.IsOk())
        SelectCustomColor(color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
}

} // namespace GUI
} // namespace Slic3r
