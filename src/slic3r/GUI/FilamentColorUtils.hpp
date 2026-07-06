#pragma once

#include "libslic3r/FilamentColorLibrary.hpp"

#include <nlohmann/json.hpp>
#include <wx/bitmap.h>
#include <wx/colour.h>

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r
{

class DynamicPrintConfig;

namespace GUI
{
namespace FilamentColorUtils
{

std::string NormalizeHexColor(const std::string& color);
std::string NormalizeHexColor(const std::string& color, const std::string& fallback_color);

std::string StripHashForPreprint(const std::string& color);
std::string StripHashForPreprint(const std::string& color, const std::string& fallback_color);

std::vector<std::string> SplitMultiColors(const std::string& value);
std::string JoinMultiColors(const std::vector<std::string>& colors);

std::string GetPrimaryColor(const std::vector<std::string>& colors, const std::string& fallback_color);

std::string GetFilamentMatchName(const std::string& name);

FilamentColor GetFilamentColorFromConfig(const DynamicPrintConfig* config, size_t colorIndex, const std::string& fallbackColor);

nlohmann::json BuildPreprintColorMultiItem(const std::string& multiColors, FilamentColorMode mode,
                                           const std::string& fallbackColor);

wxBitmap* GetFilamentColorIcon(const std::vector<std::string>& colors, FilamentColorMode mode, const std::string& label,
                               int iconWidth, int iconHeight, const wxColour& lightBorderColor = wxNullColour);

wxBitmap* GetFilamentColorIcon(const std::string& multiColors, FilamentColorMode mode, const std::string& fallbackColor,
                               const std::string& label, int iconWidth, int iconHeight,
                               const wxColour& lightBorderColor = wxNullColour);

} // namespace FilamentColorUtils
} // namespace GUI
} // namespace Slic3r
