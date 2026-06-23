#ifndef slic3r_GUI_MFDPatternSelectorAccordion_hpp_
#define slic3r_GUI_MFDPatternSelectorAccordion_hpp_

// MFDPatternSelectorAccordion: Collapsible pattern-entry section for the Mixed Filament Dialog.
//
// Responsibility:
//   Provides the UI for building a filament-cycling pattern string:
//     - A row of clickable color swatches (one per physical filament) that append
//       the filament index to the pattern text when clicked.
//     - A text input for manual pattern editing.
//     - A backspace button that deletes the last token from the pattern.
//     - Inline parse-error feedback shown below the input.
//     - A compact summary of the current pattern shown in the collapsed header.
//
// Why a separate class?
//   The parse_pattern logic, swatch-click handlers, backspace logic, and error display
//   were previously scattered throughout build_pattern_selector_ui() with lots of
//   lambda captures pointing into dialog state. Encapsulating the parser here makes
//   the parsing logic testable in isolation, and keeps all string-manipulation
//   behavior adjacent to the UI that drives it.
//
// Data flow:
//   User edits text / clicks swatch / backspace
//     --> parse_pattern() runs internally
//     --> on_pattern_changed callback (with parsed indices)  -->  dialog
//     --> on_pattern_invalid callback  -->  dialog clears preview
//   Dialog has no need to read the pattern string back except via the callbacks.

#include "Widgets/Accordion.hpp"
#include <functional>
#include <vector>
#include <string>

namespace Slic3r::GUI {

class MFDPatternSelectorAccordion : public Accordion
{
public:
    MFDPatternSelectorAccordion(
        wxWindow*                                                   parent,
        const std::vector<std::pair<std::string, std::string>>&     physical_filaments);
    ~MFDPatternSelectorAccordion() override = default;

    // Fired when the pattern parses successfully. Provides 1-based filament index sequence.
    void set_on_pattern_changed(std::function<void(const std::vector<int>&)> cb)
    {
        m_on_pattern_changed = std::move(cb);
    }

    // Fired when the pattern is empty or contains a parse error.
    void set_on_pattern_invalid(std::function<void()> cb)
    {
        m_on_pattern_invalid = std::move(cb);
    }

    wxString get_pattern_string() const;

    // Parse 'pattern_str' against 'num_filaments' available filaments.
    // Returns true and populates out_indices on success.
    // Returns false and sets out_error_msg on failure.
    static bool parse_pattern(
        const wxString& pattern_str,
        int             num_filaments,
        std::vector<int>& out_indices,
        wxString&         out_error_msg);

    // Trigger the initial validation after all UI is fully constructed.
    void trigger_initial_validation();

protected:
    void on_collapsed_changed(bool collapsed) override;

private:
    void build_ui();
    void build_filament_row(wxPanel* parent, wxBoxSizer* parent_sizer);
    void on_text_changed(wxCommandEvent& event);
    void handle_backspace();
    void update_title_preview();

    const std::vector<std::pair<std::string, std::string>>& m_physical_filaments;

    wxPanel*        m_filament_row{nullptr};
    wxTextCtrl*     m_pattern_input{nullptr};
    wxStaticText*   m_pattern_warning{nullptr};
    wxStaticText*   m_title_preview_text{nullptr};

    std::function<void(const std::vector<int>&)> m_on_pattern_changed;
    std::function<void()>                        m_on_pattern_invalid;
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_MFDPatternSelectorAccordion_hpp_
