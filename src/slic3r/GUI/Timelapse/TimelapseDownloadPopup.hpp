#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>

#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include <wx/dialog.h>
#include <wx/timer.h>
#include <wx/scrolwin.h>
#include <wx/panel.h>
#include <wx/statbmp.h>

namespace Slic3r {

class BBLStatusBarSend;

namespace GUI {

class TimelapseTaskRow;

class TimelapseDownloadPopup : public DPIDialog
{
public:
    struct TaskInfo
    {
        std::string file_name;
        std::string file_url;
        std::string sn;
        std::string date_index;
    };

    TimelapseDownloadPopup(wxWindow* parent);
    ~TimelapseDownloadPopup();

    // Append tasks. Returns the global row offset where this batch starts
    // (caller uses offset + per-batch idx to address rows in a shared popup).
    int add_tasks(const std::vector<TaskInfo>& tasks);

    void set_task_progress(int index, int percent,
                           size_t downloaded, size_t total);
    void set_task_status(int index, const wxString& text);
    void mark_task_complete(int index, const std::string& file_path);
    void mark_task_error(int index, const std::string& error);
    void mark_task_cancelled(int index);

    void mark_all_complete(int completed, int failed,
                           const std::string& save_path,
                           const std::string& latest_file);

    void set_task_cancel_callback(int index, std::function<void()> cb);
    void set_close_callback(std::function<void()> cb);

    void Close();

    void refresh_dark_mode(); // re-apply colors & bitmaps after theme switch

    wxWindow* GetPopupParent() { return DPIDialog::GetParent(); }

private:
    void position_bottom_right();
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void on_timer(wxTimerEvent&);
    void toggle_collapse();
    void update_layout_size();
    void reorder_rows();
    void destroy_dividers();
    void set_row_state(int index, int state);
    void dismiss_row(int index);

    struct TaskRowInfo
    {
        TimelapseTaskRow* row;
        int               state; // 0=pending, 1=downloading, 2=complete, 3=error, 4=cancelled
        bool              visible;
    };

    wxPanel*           m_content_panel;  // inset content panel (bg_color) inside the border
    wxPanel*           m_title_bar;
    wxPanel*           m_title_divider;
    Label*             m_title_label;
    wxStaticBitmap*    m_collapse_btn;
    wxScrolledWindow*  m_task_panel;
    wxBoxSizer*        m_task_sizer;

    std::vector<TaskRowInfo> m_rows;
    bool m_collapsed;
    bool m_all_complete;
    int  m_task_count;

    std::string m_open_target;

    std::function<void()> m_close_callback;

    wxTimer* m_position_timer;
    bool     m_was_dark_mode;  // for detecting theme switch in on_timer

    // Design constants (match Figma spec, in raw px — converted via FromDIP)
    static constexpr int BORDER_WIDTH       = 1;   // outer border ring (divider_color)
    static constexpr int TITLE_BAR_HEIGHT   = 40;
    static constexpr int TASK_ROW_HEIGHT    = 58;
    static constexpr int DIALOG_WIDTH       = 375;
    static constexpr int DIALOG_HEIGHT      = 195;
    static constexpr int CONTENT_HEIGHT     = 155;
    static constexpr int MAX_VISIBLE_ROWS   = 3;
    static constexpr int DEFAULT_VISIBLE    = 3;
    static constexpr int SCROLL_RATE        = 10;
    static constexpr int BOTTOM_MARGIN      = 20;
};

// Custom task row matching the Figma design.
// State machine: Pending → Downloading → (Completed | Failed | Cancelled)
class TimelapseTaskRow : public wxPanel
{
public:
    enum class State
    {
        Pending,
        Downloading,
        Completed,
        Failed,
        Cancelled
    };

    TimelapseTaskRow(wxWindow* parent, const wxString& file_name);
    ~TimelapseTaskRow();

    void set_state(State s);
    void set_progress(int percent);
    void set_status_text(const wxString& text);
    void set_cancel_callback(std::function<void()> cb);
    void set_dismiss_callback(std::function<void()> cb);
    void disable_cancel(); // visual gray-out + switch click behavior to dismiss
    void set_queue_position(int pos) { m_queue_position = pos; }
    void refresh_dark_mode(); // re-apply colors & bitmaps after theme switch

    // Figma design sizes (raw px — converted via FromDIP at use sites)
    static constexpr int ROW_WIDTH        = 375;
    static constexpr int ROW_HEIGHT       = 58;
    static constexpr int PROGRESS_WIDTH   = 299;
    static constexpr int PROGRESS_HEIGHT  = 4;
    static constexpr int CANCEL_SIZE      = 19;
    static constexpr int STATUS_ICON_SIZE = 16;

private:
    void on_cancel_down(wxMouseEvent&);

    Label*           m_name_label;
    wxPanel*         m_progress_track;
    wxStaticBitmap*  m_status_icon;
    Label*           m_status_label;
    wxStaticBitmap*  m_cancel_btn;
    wxSizer*         m_status_sizer;

    State     m_state;
    int       m_percent;
    int       m_queue_position = 1;
    bool      m_cancel_enabled;
    wxString  m_status_text;

    std::function<void()> m_cancel_cb;
    std::function<void()> m_dismiss_cb;
};

}} // namespace Slic3r::GUI

