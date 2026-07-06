#include "FilamentColorLibrary.hpp"
#include "Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace Slic3r
{
namespace
{

std::string TrimCopy(const std::string& value)
{
    std::string::const_iterator begin = std::find_if_not(value.begin(), value.end(),
                                        [](unsigned char ch)
                                        {
                                            return std::isspace(ch);
                                        });
    std::string::const_iterator end = std::find_if_not(value.rbegin(), value.rend(),
                                      [](unsigned char ch)
                                      {
                                          return std::isspace(ch);
                                      }).base();
    return begin < end ? std::string(begin, end) : std::string();
}

bool EndsWithCaseInsensitive(const std::string& value, const std::string& suffix)
{
    if (value.empty() || suffix.empty() || value.size() < suffix.size())
        return false;

    const size_t offset = value.size() - suffix.size();
    for (size_t index = 0; index < suffix.size(); ++index)
    {
        const unsigned char left = static_cast<unsigned char>(value[offset + index]);
        const unsigned char right = static_cast<unsigned char>(suffix[index]);
        if (std::tolower(left) != std::tolower(right))
            return false;
    }

    return true;
}

std::string NormalizeHexColorNoFallback(const std::string& color)
{
    std::string value = TrimCopy(color);
    if (!value.empty() && value.front() == '#')
        value.erase(value.begin());
    if (value.size() == 8)
        value = value.substr(0, 6);
    if (value.size() != 6)
        return {};

    for (char& ch : value)
    {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (!std::isxdigit(uch))
            return {};
        ch = static_cast<char>(std::toupper(uch));
    }

    return "#" + value;
}

std::vector<std::string> NormalizeColorList(const std::vector<std::string>& colors)
{
    std::vector<std::string> normalizedColors;
    normalizedColors.reserve(colors.size());
    for (const std::string& color : colors)
    {
        const std::string normalized = NormalizeFilamentHexColor(color);
        if (!normalized.empty())
            normalizedColors.emplace_back(normalized);
    }
    return normalizedColors;
}

boost::filesystem::path FilamentsColoursPath()
{
    // Prefer the system copy, fall back to bundled resources for a fresh install.
    boost::filesystem::path path = boost::filesystem::path(Slic3r::data_dir());
    path /= "system";
    path /= "Snapmaker";
    path /= "filament";
    path /= "filaments_colours.json";
    if (boost::filesystem::exists(path))
        return path.make_preferred();

    path = boost::filesystem::path(Slic3r::resources_dir());
    path /= "profiles";
    path /= "Snapmaker";
    path /= "filament";
    path /= "filaments_colours.json";
    return path.make_preferred();
}

std::string JsonString(const nlohmann::json& j, const char* key)
{
    nlohmann::json::const_iterator it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int JsonMode(const nlohmann::json& j)
{
    nlohmann::json::const_iterator it = j.find("mode");
    if (it == j.end() || !it->is_number_integer())
        return 0;
    return it->get<int>() == 1 ? 1 : 0;
}

bool JsonBool(const nlohmann::json& j, const char* key, bool defaultValue)
{
    nlohmann::json::const_iterator it = j.find(key);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : defaultValue;
}

std::unordered_map<std::string, std::string> JsonStringMap(const nlohmann::json& j, const char* key)
{
    std::unordered_map<std::string, std::string> values;
    nlohmann::json::const_iterator objectIt = j.find(key);
    if (objectIt == j.end() || !objectIt->is_object())
        return values;

    for (nlohmann::json::const_iterator it = objectIt->begin(); it != objectIt->end(); ++it)
    {
        if (it.value().is_string())
            values.emplace(it.key(), it.value().get<std::string>());
    }
    return values;
}

std::vector<std::string> JsonStringArray(const nlohmann::json& j, const char* key)
{
    std::vector<std::string> values;
    nlohmann::json::const_iterator it = j.find(key);
    if (it == j.end() || !it->is_array())
        return values;

    for (const nlohmann::json& item : *it)
    {
        if (item.is_string())
            values.emplace_back(item.get<std::string>());
    }
    return values;
}

std::string NormalizeJsonColor(const std::string& color, bool& hasInvalidColor)
{
    const std::string normalized = NormalizeFilamentHexColor(color);
    if (!color.empty() && normalized.empty())
        hasInvalidColor = true;
    return normalized;
}

bool LoadJsonFile(const boost::filesystem::path& path, nlohmann::json& out)
{
    boost::nowide::ifstream ifs(path.string());
    if (!ifs.is_open())
    {
        BOOST_LOG_TRIVIAL(warning) << "Failed to open official filament color file: " << path.string();
        return false;
    }

    out = nlohmann::json::parse(ifs, nullptr, false);
    if (out.is_discarded())
    {
        BOOST_LOG_TRIVIAL(warning) << "Failed to parse official filament color file: " << path.string();
        return false;
    }

    return true;
}

} // namespace

std::string NormalizeFilamentHexColor(const std::string& color)
{
    return NormalizeHexColorNoFallback(color);
}

std::string NormalizeFilamentHexColor(const std::string& color, const std::string& fallbackColor)
{
    const std::string normalized = NormalizeHexColorNoFallback(color);
    if (!normalized.empty())
        return normalized;

    if (color == fallbackColor)
        return {};
    return NormalizeHexColorNoFallback(fallbackColor);
}

std::vector<std::string> SplitFilamentMultiColors(const std::string& value)
{
    std::vector<std::string> colors;
    std::stringstream stream(value);
    std::string token;

    while (std::getline(stream, token, '|'))
    {
        const std::string normalized = NormalizeFilamentHexColor(token);
        if (!normalized.empty())
            colors.emplace_back(normalized);
    }

    return colors;
}

std::string JoinFilamentMultiColors(const std::vector<std::string>& colors)
{
    std::ostringstream out;
    bool first = true;
    for (const std::string& color : colors)
    {
        const std::string normalized = NormalizeFilamentHexColor(color);
        if (normalized.empty())
            continue;
        if (!first)
            out << '|';
        out << normalized;
        first = false;
    }
    return out.str();
}

std::string GetFilamentMatchName(const std::string& name)
{
    std::string matchName = TrimCopy(name);
    const std::string nozzleSuffix = " nozzle";
    if (EndsWithCaseInsensitive(matchName, nozzleSuffix))
    {
        matchName = TrimCopy(matchName.substr(0, matchName.size() - nozzleSuffix.size()));
        const size_t nozzlePos = matchName.find_last_of(" \t\r\n");
        if (nozzlePos != std::string::npos)
            matchName = matchName.substr(0, nozzlePos);
    }

    return TrimCopy(matchName);
}

FilamentColorMode FilamentColorModeFromConfig(int modeValue)
{
    if (modeValue == static_cast<int>(FilamentColorMode::Gradient))
        return FilamentColorMode::Gradient;
    return FilamentColorMode::Segment;
}

int FilamentColorModeToConfig(FilamentColorMode mode)
{
    if (mode == FilamentColorMode::Gradient)
        return static_cast<int>(FilamentColorMode::Gradient);
    return static_cast<int>(FilamentColorMode::Segment);
}

bool FilamentColor::Empty() const
{
    return colors.empty();
}

FilamentColorMode FilamentColor::NormalizedMode() const
{
    if (colors.size() > 1 && mode == FilamentColorMode::Gradient)
        return FilamentColorMode::Gradient;
    return FilamentColorMode::Segment;
}

bool FilamentColor::IsGradient() const
{
    return NormalizedMode() == FilamentColorMode::Gradient;
}

std::string FilamentColor::PrimaryColor(const std::string& fallbackColor) const
{
    for (const std::string& color : colors)
    {
        const std::string normalized = NormalizeFilamentHexColor(color);
        if (!normalized.empty())
            return normalized;
    }
    return NormalizeFilamentHexColor(fallbackColor, "#26A69A");
}

std::string FilamentColor::ToMultiColorsString() const
{
    return JoinFilamentMultiColors(colors);
}

bool FilamentColor::Matches(const FilamentColor& other) const
{
    FilamentColor left = FromColors(colors, mode);
    FilamentColor right = FromColors(other.colors, other.mode);
    return left.colors == right.colors && left.NormalizedMode() == right.NormalizedMode();
}

FilamentColor FilamentColor::FromColors(const std::vector<std::string>& colors, FilamentColorMode mode,
                                        const std::string& fallbackColor)
{
    FilamentColor filamentColor;
    filamentColor.colors = NormalizeColorList(colors);
    if (filamentColor.colors.empty())
    {
        const std::string fallback = NormalizeFilamentHexColor(fallbackColor, "#26A69A");
        filamentColor.colors.emplace_back(fallback.empty() ? "#26A69A" : fallback);
    }

    filamentColor.mode = filamentColor.colors.size() > 1 ? mode : FilamentColorMode::Segment;
    return filamentColor;
}

FilamentColor FilamentColor::FromMultiColors(const std::string& multiColors, FilamentColorMode mode,
                                             const std::string& fallbackColor)
{
    return FromColors(SplitFilamentMultiColors(multiColors), mode, fallbackColor);
}

FilamentColorLibrary& FilamentColorLibrary::Instance()
{
    static FilamentColorLibrary library;
    return library;
}

bool FilamentColorLibrary::EnsureLoaded()
{
    if (_loaded)
        return true;

    Clear();
    if (!LoadIndex())
    {
        Clear();
        return false;
    }
    _loaded = true;
    return true;
}

void FilamentColorLibrary::Reload()
{
    Clear();
    _loaded = false;
    EnsureLoaded();
}

bool FilamentColorLibrary::FindFilamentById(const std::string& filamentId, FilamentColorInfo& outFilament)
{
    if (!EnsureLoaded())
        return false;

    std::unordered_map<std::string, size_t>::const_iterator filamentIt = _filamentIndexByIdMap.find(filamentId);
    if (filamentIt == _filamentIndexByIdMap.end())
        return false;

    return FindFilamentByIndex(filamentIt->second, outFilament);
}

bool FilamentColorLibrary::FindFilamentByName(const std::string& filamentName, FilamentColorInfo& outFilament)
{
    if (!EnsureLoaded())
        return false;

    const std::string matchName = GetFilamentMatchName(filamentName);
    if (matchName.empty())
        return false;

    std::unordered_map<std::string, size_t>::const_iterator nameIt = _filamentIndexByNameMap.find(matchName);
    return nameIt != _filamentIndexByNameMap.end() ? FindFilamentByIndex(nameIt->second, outFilament) : false;
}

bool FilamentColorLibrary::FindFilamentByIndex(size_t index, FilamentColorInfo& outFilament) const
{
    if (index >= _filamentInfoVec.size())
        return false;

    outFilament = _filamentInfoVec[index];
    return true;
}

bool FilamentColorLibrary::LoadIndex()
{
    nlohmann::json root;
    const boost::filesystem::path filePath = FilamentsColoursPath();
    if (!LoadJsonFile(filePath, root))
        return false;

    nlohmann::json::const_iterator filamentsIt = root.find("filaments");
    if (filamentsIt == root.end() || !filamentsIt->is_array())
    {
        BOOST_LOG_TRIVIAL(warning) << "Missing official filament color filaments array: " << filePath.string();
        return false;
    }

    std::vector<FilamentColorInfo> filaments;
    std::unordered_map<std::string, size_t> filamentIndexById;
    std::unordered_map<std::string, size_t> filamentIndexByName;
    std::unordered_set<std::string> skus;

    for (const nlohmann::json& filamentJson : *filamentsIt)
    {
        if (!filamentJson.is_object())
        {
            BOOST_LOG_TRIVIAL(warning) << "Skip invalid official filament item: " << filePath.string();
            continue;
        }

        if (!JsonBool(filamentJson, "enabled", true))
            continue;

        FilamentColorInfo filament;
        filament.filamentId = JsonString(filamentJson, "filament_id");
        filament.filamentName = JsonString(filamentJson, "filament_name");
        filament.type = JsonString(filamentJson, "filament_type");

        if (filament.filamentId.empty() || filament.filamentName.empty())
        {
            BOOST_LOG_TRIVIAL(warning) << "Skip official filament without id or name: " << filePath.string();
            continue;
        }

        const std::string matchName = GetFilamentMatchName(filament.filamentName);
        if (matchName.empty())
        {
            BOOST_LOG_TRIVIAL(warning) << "Skip official filament without valid name: " << filament.filamentName;
            continue;
        }

        if (filamentIndexByName.find(matchName) != filamentIndexByName.end())
        {
            BOOST_LOG_TRIVIAL(warning) << "Skip duplicate official filament name: " << filament.filamentName;
            continue;
        }

        const bool duplicateFilamentId = filamentIndexById.find(filament.filamentId) != filamentIndexById.end();
        if (duplicateFilamentId)
            BOOST_LOG_TRIVIAL(warning) << "Official filament id already exists, keep name fallback only: "
                                       << filament.filamentId;

        nlohmann::json::const_iterator colorsIt = filamentJson.find("filament_color");
        if (colorsIt == filamentJson.end() || !colorsIt->is_array())
        {
            BOOST_LOG_TRIVIAL(warning) << "Skip official filament without colors array: " << filament.filamentId;
            continue;
        }

        for (const nlohmann::json& colorJson : *colorsIt)
        {
            if (!colorJson.is_object())
            {
                BOOST_LOG_TRIVIAL(warning) << "Skip invalid color item for official filament: " << filament.filamentId;
                continue;
            }

            if (!JsonBool(colorJson, "enabled", true))
                continue;

            FilamentColorItem colorItem;
            colorItem.sku = JsonString(colorJson, "sku");
            colorItem.colorNames = JsonStringMap(colorJson, "color_name");
            colorItem.colorData.mode = FilamentColorModeFromConfig(JsonMode(colorJson));

            bool hasInvalidColor = false;
            for (const std::string& rawColor : JsonStringArray(colorJson, "filament_color"))
            {
                const std::string normalizedColor = NormalizeJsonColor(rawColor, hasInvalidColor);
                if (!normalizedColor.empty())
                    colorItem.colorData.colors.emplace_back(normalizedColor);
            }

            if (hasInvalidColor)
            {
                BOOST_LOG_TRIVIAL(warning) << "Skip color item with invalid color value: " << colorItem.sku;
                continue;
            }

            if (colorItem.sku.empty() || colorItem.colorData.colors.empty())
            {
                BOOST_LOG_TRIVIAL(warning) << "Skip incomplete color item for official filament: "
                                           << filament.filamentId;
                continue;
            }

            if ((colorItem.colorData.mode == FilamentColorMode::Gradient && colorItem.colorData.colors.size() < 2) ||
                (colorItem.colorData.mode == FilamentColorMode::Segment && colorItem.colorData.colors.size() > 2))
            {
                // Gradient requires at least 2 colors; segment supports at most 2 colors.
                BOOST_LOG_TRIVIAL(warning) << "Skip color item with invalid color count: " << colorItem.sku;
                continue;
            }

            if (skus.find(colorItem.sku) != skus.end())
            {
                BOOST_LOG_TRIVIAL(warning) << "Skip duplicate official filament color SKU: " << colorItem.sku;
                continue;
            }

            skus.emplace(colorItem.sku);
            filament.colors.emplace_back(std::move(colorItem));
        }

        if (filament.colors.empty())
        {
            BOOST_LOG_TRIVIAL(warning) << "Skip official filament without valid colors: " << filament.filamentId;
            continue;
        }

        const size_t filamentIndex = filaments.size();
        filamentIndexByName.emplace(matchName, filamentIndex);
        if (!duplicateFilamentId)
            filamentIndexById.emplace(filament.filamentId, filamentIndex);
        filaments.emplace_back(std::move(filament));
    }

    if (filaments.empty())
    {
        BOOST_LOG_TRIVIAL(warning) << "Official filament color file has no valid filaments: " << filePath.string();
        return false;
    }

    _filamentInfoVec = std::move(filaments);
    _filamentIndexByIdMap = std::move(filamentIndexById);
    _filamentIndexByNameMap = std::move(filamentIndexByName);
    return true;
}

void FilamentColorLibrary::Clear()
{
    _filamentInfoVec.clear();
    _filamentIndexByIdMap.clear();
    _filamentIndexByNameMap.clear();
}

} // namespace Slic3r
