#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <wx/panel.h>

#include "FilamentData.hpp"
#include "FilamentColorMapBox.hpp"

class Label;

namespace Slic3r
{
namespace GUI
{

class MachineFilamentPicker;

class FilamentColorMapBoxGroup : public wxPanel
{
public:
    FilamentColorMapBoxGroup(wxWindow* parent,
                             const std::vector<FilamentData>& designDataList,
                             const std::vector<FilamentData>& machineDataList);

    std::vector<FilamentData> getCurFilamentList() const;

    void setGroupBoxEnable(bool bEnable, FilamentColorMapBox::ButtonType type);
    void showMachineFilamentPicker(int boxIndex);
    void updateBoxBelowData(int boxIndex, const FilamentData& machineData, bool bTriggerCallback = true);
    int  getBoxCount() const;
    int  getVisibleBoxCount() const;
    void setVisibleCount(int count);

    // Returns true if the number of visible rows exceeds maxRows.
    bool exceedsRowCount(int maxRows) const;
    // Returns the total height (in physical pixels) the group would have
    // if only `rows` rows of cards were displayed.
    int  getHeightForRowCount(int rows) const;

    void bindMappingChangedCallback(std::function<void()> cb);

    // Dismiss the currently-open machine filament picker popup, if any.
    // Returns true if a popup was dismissed.
    bool dismissOpenPicker();
    bool hasOpenPicker() const;

    static int GetGridCols();

    bool Layout() override;

private:
    void onPaint(wxPaintEvent& event);
    void updateBoxFilament(int boxIndex, const FilamentData& machineData, bool bTriggerCallback = true);

    std::vector<std::unique_ptr<FilamentColorMapBox>> m_boxList;
    std::vector<FilamentData> m_designDataList;
    std::vector<FilamentData> m_machineDataList;

    MachineFilamentPicker* m_pPicker = nullptr;

    Label* m_pLabelDesign  = nullptr;
    Label* m_pLabelMachine = nullptr;

    std::function<void()> m_mappingChangedCallback = nullptr;
};

} // namespace GUI
} // namespace Slic3r
