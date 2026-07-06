#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r
{

enum class FilamentColorMode
{
    Segment = 0, //Single colors or side by side segments
    Gradient = 1
};

std::string NormalizeFilamentHexColor(const std::string& color);
std::string NormalizeFilamentHexColor(const std::string& color, const std::string& fallbackColor);
std::vector<std::string> SplitFilamentMultiColors(const std::string& value);
std::string JoinFilamentMultiColors(const std::vector<std::string>& colors);
std::string GetFilamentMatchName(const std::string& name);
FilamentColorMode FilamentColorModeFromConfig(int modeValue);
int FilamentColorModeToConfig(FilamentColorMode mode);

struct FilamentColor
{
    std::vector<std::string> colors;
    FilamentColorMode mode { FilamentColorMode::Segment };

    bool Empty() const;
    FilamentColorMode NormalizedMode() const;
    bool IsGradient() const;
    std::string PrimaryColor(const std::string& fallbackColor = "#26A69A") const;
    std::string ToMultiColorsString() const;
    bool Matches(const FilamentColor& other) const;

    static FilamentColor FromColors(const std::vector<std::string>& colors, FilamentColorMode mode,
                                    const std::string& fallbackColor = "#26A69A");
    static FilamentColor FromMultiColors(const std::string& multiColors, FilamentColorMode mode,
                                         const std::string& fallbackColor = "#26A69A");
};

struct FilamentColorItem
{
    std::unordered_map<std::string, std::string> colorNames;
    std::string sku;
    FilamentColor colorData;
};

struct FilamentColorInfo
{
    std::string filamentId;
    std::string filamentName;
    std::string type;
    std::vector<FilamentColorItem> colors;
};

class FilamentColorLibrary
{
public:
    static FilamentColorLibrary& Instance();

    bool EnsureLoaded();
    void Reload();

    bool FindFilamentById(const std::string& filamentId, FilamentColorInfo& outFilament);
    bool FindFilamentByName(const std::string& filamentName, FilamentColorInfo& outFilament);

private:
    bool LoadIndex();
    bool FindFilamentByIndex(size_t index, FilamentColorInfo& outFilament) const;
    void Clear();

private:
    bool _loaded { false };

    std::vector<FilamentColorInfo> _filamentInfoVec;
    std::unordered_map<std::string, size_t> _filamentIndexByIdMap;   // filament_id to index in _filamentInfoVec
    std::unordered_map<std::string, size_t> _filamentIndexByNameMap; // normalized filament name to index in _filamentInfoVec
};

} // namespace Slic3r
