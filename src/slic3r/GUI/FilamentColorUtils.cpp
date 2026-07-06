#include "FilamentColorUtils.hpp"

#include "MixedFilamentBadge.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r
{
namespace GUI
{
namespace FilamentColorUtils
{
namespace
{

std::vector<std::string> NormalizedColorsOrFallback(const std::vector<std::string>& colors,
                                                    const std::string& fallback_color)
{
    std::vector<std::string> normalized;
    normalized.reserve(colors.size());
    for (const std::string& color : colors)
    {
        const std::string normalized_color = NormalizeHexColor(color);
        if (!normalized_color.empty())
            normalized.emplace_back(normalized_color);
    }

    if (normalized.empty())
    {
        const std::string fallback = NormalizeHexColor(fallback_color, "#26A69A");
        normalized.emplace_back(fallback.empty() ? "#26A69A" : fallback);
    }

    return normalized;
}

void AddFallbackToNormalizedColors(std::vector<std::string>& colors, const std::string& fallbackColor)
{
    if (!colors.empty())
        return;

    const std::string fallback = NormalizeHexColor(fallbackColor, "#26A69A");
    colors.emplace_back(fallback.empty() ? "#26A69A" : fallback);
}

wxColour WxColorFromHex(const std::string& color)
{
    wxColour parsed(color);
    return parsed.IsOk() ? parsed : wxColour("#26A69A");
}

wxBitmap* GetFilamentColorIconFromNormalized(const std::vector<std::string>& normalizedColors, FilamentColorMode mode,
                                             const std::string& label, int iconWidth, int iconHeight,
                                             const wxColour& lightBorderColor)
{
    std::vector<wxColour> wxColors;
    wxColors.reserve(normalizedColors.size());
    for (const std::string& color : normalizedColors)
        wxColors.emplace_back(WxColorFromHex(color));

    const bool isGradient = normalizedColors.size() > 1 && mode == FilamentColorMode::Gradient;
    return get_color_block_bitmap_cached(wxColors, isGradient, iconWidth, iconHeight, wxString::FromUTF8(label.c_str()),
                                         lightBorderColor);
}

} // namespace

std::string NormalizeHexColor(const std::string& color)
{
    return NormalizeFilamentHexColor(color);
}

std::string NormalizeHexColor(const std::string& color, const std::string& fallback_color)
{
    return NormalizeFilamentHexColor(color, fallback_color);
}

std::string StripHashForPreprint(const std::string& color)
{
    std::string normalized = NormalizeHexColor(color);
    if (!normalized.empty() && normalized.front() == '#')
        normalized.erase(normalized.begin());
    return normalized;
}

std::string StripHashForPreprint(const std::string& color, const std::string& fallback_color)
{
    std::string normalized = NormalizeHexColor(color, fallback_color);
    if (!normalized.empty() && normalized.front() == '#')
        normalized.erase(normalized.begin());
    return normalized;
}

std::vector<std::string> SplitMultiColors(const std::string& value)
{
    return SplitFilamentMultiColors(value);
}

std::string JoinMultiColors(const std::vector<std::string>& colors)
{
    return JoinFilamentMultiColors(colors);
}

std::string GetPrimaryColor(const std::vector<std::string>& colors, const std::string& fallback_color)
{
    const FilamentColor color = FilamentColor::FromColors(colors, FilamentColorMode::Segment, fallback_color);
    return color.PrimaryColor(fallback_color);
}

std::string GetFilamentMatchName(const std::string& name)
{
    return Slic3r::GetFilamentMatchName(name);
}

FilamentColor GetFilamentColorFromConfig(const DynamicPrintConfig* config, size_t colorIndex, const std::string& fallbackColor)
{
    std::vector<std::string> colors;
    FilamentColorMode mode = FilamentColorMode::Segment;

    if (config != nullptr && config->has("filament_multi_colors"))
    {
        const ConfigOptionStrings* option = config->option<ConfigOptionStrings>("filament_multi_colors");
        if (option != nullptr && option->values.size() > colorIndex)
            colors = SplitMultiColors(option->values[colorIndex]);
    }

    if (config != nullptr && config->has("filament_colour_mode"))
    {
        const ConfigOptionInts* option = config->option<ConfigOptionInts>("filament_colour_mode");
        if (option != nullptr && option->values.size() > colorIndex)
            mode = FilamentColorModeFromConfig(option->values[colorIndex]);
    }

    return FilamentColor::FromColors(colors, mode, fallbackColor);
}

nlohmann::json BuildPreprintColorMultiItem(const std::string& multiColors, FilamentColorMode mode,
                                           const std::string& fallbackColor)
{
    const FilamentColor color = FilamentColor::FromMultiColors(multiColors, mode, fallbackColor);

    nlohmann::json out_colors = nlohmann::json::array();
    for (const std::string& item : color.colors)
        out_colors.push_back(item.substr(1));

    nlohmann::json item;
    item["mode"] = FilamentColorModeToConfig(color.NormalizedMode());
    item["nums"] = color.colors.size();
    item["colors"] = out_colors;
    return item;
}

wxBitmap* GetFilamentColorIcon(const std::vector<std::string>& colors, FilamentColorMode mode, const std::string& label,
                               int iconWidth, int iconHeight, const wxColour& lightBorderColor)
{
    const std::vector<std::string> normalized = NormalizedColorsOrFallback(colors, "#26A69A");
    return GetFilamentColorIconFromNormalized(normalized, mode, label, iconWidth, iconHeight, lightBorderColor);
}

wxBitmap* GetFilamentColorIcon(const std::string& multiColors, FilamentColorMode mode, const std::string& fallbackColor,
                               const std::string& label, int iconWidth, int iconHeight, const wxColour& lightBorderColor)
{
    std::vector<std::string> normalized = SplitMultiColors(multiColors);
    AddFallbackToNormalizedColors(normalized, fallbackColor);
    return GetFilamentColorIconFromNormalized(normalized, mode, label, iconWidth, iconHeight, lightBorderColor);
}

} // namespace FilamentColorUtils
} // namespace GUI
} // namespace Slic3r
