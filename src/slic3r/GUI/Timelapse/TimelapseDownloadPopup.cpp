#include "TimelapseDownloadPopup.hpp"

#include <wx/sizer.h>
#include <wx/display.h>
#include <wx/panel.h>
#include <wx/toplevel.h>
#include <wx/gdicmn.h>
#include <wx/statbmp.h>
#include <wx/dcclient.h>
#include <wx/wupdlock.h>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"

namespace Slic3r {
namespace GUI {

// ===========================================================================
// Design color palette (from Figma)
// ===========================================================================
namespace {
// Light mode (Figma)
constexpr wxUint32 COL_BG_LIGHT           = 0xFFFFFF;
constexpr wxUint32 COL_TITLE_TEXT_LIGHT    = 0x242424;
constexpr wxUint32 COL_FILE_NAME_LIGHT     = 0x333333;
constexpr wxUint32 COL_STATUS_TEXT_LIGHT   = 0x8F8F8F;
constexpr wxUint32 COL_DIVIDER_LIGHT       = 0xD9D9D9;

// Dark mode (Figma)
constexpr wxUint32 COL_BG_DARK            = 0x2D2D31;
constexpr wxUint32 COL_TITLE_TEXT_DARK     = 0xE0E0E0;
constexpr wxUint32 COL_FILE_NAME_DARK      = 0xE0E0E0;
constexpr wxUint32 COL_STATUS_TEXT_DARK    = 0x909090;
constexpr wxUint32 COL_DIVIDER_DARK        = 0x3E3E45;

// Shared across modes (unchanged in dark mode per Figma)
constexpr wxUint32 COL_PROGRESS_FILL = 0x009988;
constexpr wxUint32 COL_PROGRESS_TRACK= 0xD9D9D9;

inline wxColour make_color(wxUint32 rgb) {
    return wxColour((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// Dynamic color accessors (switches on dark_mode())
inline wxColour bg_color()         { return wxGetApp().dark_mode() ? make_color(COL_BG_DARK)         : make_color(COL_BG_LIGHT); }
inline wxColour title_text_color() { return wxGetApp().dark_mode() ? make_color(COL_TITLE_TEXT_DARK)  : make_color(COL_TITLE_TEXT_LIGHT); }
inline wxColour file_name_color()  { return wxGetApp().dark_mode() ? make_color(COL_FILE_NAME_DARK)   : make_color(COL_FILE_NAME_LIGHT); }
inline wxColour status_text_color(){ return wxGetApp().dark_mode() ? make_color(COL_STATUS_TEXT_DARK) : make_color(COL_STATUS_TEXT_LIGHT); }
inline wxColour divider_color()    { return wxGetApp().dark_mode() ? make_color(COL_DIVIDER_DARK)     : make_color(COL_DIVIDER_LIGHT); }
} // namespace

// ===========================================================================
// Progress bar — track + proportional fill.
// ===========================================================================
namespace {
class ProgressBar : public wxPanel
{
public:
    ProgressBar(wxWindow* parent, int width, int height)
        : wxPanel(parent, wxID_ANY), m_percent(0), m_visible(true)
    {
        SetBackgroundColour(make_color(COL_PROGRESS_TRACK));
        SetMinSize(wxSize(width, height));
        SetMaxSize(wxSize(width, height));
        Bind(wxEVT_PAINT, &ProgressBar::on_paint, this);
    }

    void set_percent(int p) {
        if (p < 0) {
            p = 0;
        }
        if (p > 100) {
            p = 100;
        }
        if (m_percent != p) {
            m_percent = p;
            Refresh();
        }
    }

    void set_visible(bool v) {
        // Only controls painting — widget stays in sizer to preserve layout slot.
        if (m_visible != v) {
            m_visible = v;
            Refresh();
        }
    }

private:
    void on_paint(wxPaintEvent&) {
        wxPaintDC dc(this);
        wxRect r = GetClientSize();
        if (!m_visible) {
            // Hidden: fill with parent background so the track fully disappears
            // on completed/failed/cancelled rows (only icon + text shown).
            wxColour bg = GetParent() ? GetParent()->GetBackgroundColour() : *wxWHITE;
            dc.SetBrush(wxBrush(bg));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(r);
            return;
        }
        dc.SetBrush(make_color(COL_PROGRESS_TRACK));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(r);
        if (m_percent <= 0) {
            return;
        }
        int fill_w = (r.width * m_percent) / 100;
        if (fill_w < 1) {
            return;
        }
        dc.SetBrush(make_color(COL_PROGRESS_FILL));
        dc.DrawRectangle(wxRect(0, 0, fill_w, r.height));
    }

    int  m_percent;
    bool m_visible;
};
} // namespace

// ===========================================================================
// TimelapseTaskRow implementation
// ===========================================================================
TimelapseTaskRow::TimelapseTaskRow(wxWindow* parent, const wxString& file_name)
    : wxPanel(parent, wxID_ANY)
    , m_state(State::Pending)
    , m_percent(0)
    , m_cancel_enabled(true)
    , m_status_text(_L("Waiting for device to upload"))
{
    SetBackgroundColour(bg_color());
    SetMinSize(wxSize(FromDIP(ROW_WIDTH), FromDIP(ROW_HEIGHT)));
    SetMaxSize(wxSize(FromDIP(ROW_WIDTH), FromDIP(ROW_HEIGHT)));

    auto* root_sizer = new wxBoxSizer(wxHORIZONTAL);

    // Left column: name + progress + status_row
    auto* left_sizer = new wxBoxSizer(wxVERTICAL);

    m_name_label = new Label(this, file_name, wxST_ELLIPSIZE_MIDDLE);
    m_name_label->SetFont(::Label::Body_13);
    m_name_label->SetForegroundColour(file_name_color());
    m_name_label->SetMinSize(wxSize(FromDIP(PROGRESS_WIDTH), -1));
    m_name_label->SetMaxSize(wxSize(FromDIP(PROGRESS_WIDTH), -1));
    left_sizer->Add(m_name_label, 0, wxEXPAND | wxBOTTOM, FromDIP(4));

    m_progress_track = new ProgressBar(this, FromDIP(PROGRESS_WIDTH), FromDIP(PROGRESS_HEIGHT));
    left_sizer->Add(m_progress_track, 0, wxEXPAND | wxBOTTOM, FromDIP(4));

    // Status row: [icon] [text]
    m_status_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_status_icon = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap);
    m_status_icon->SetMinSize(wxSize(FromDIP(STATUS_ICON_SIZE), FromDIP(STATUS_ICON_SIZE)));
    m_status_icon->SetMaxSize(wxSize(FromDIP(STATUS_ICON_SIZE), FromDIP(STATUS_ICON_SIZE)));
    m_status_icon->Hide();
    m_status_sizer->Add(m_status_icon, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

    m_status_label = new Label(this, m_status_text);
    m_status_label->SetFont(::Label::Body_13);
    m_status_label->SetForegroundColour(status_text_color());
    m_status_sizer->Add(m_status_label, 1, wxALIGN_CENTER_VERTICAL);

    left_sizer->Add(m_status_sizer, 0, wxEXPAND);

    root_sizer->Add(left_sizer, 1, wxALIGN_CENTER_VERTICAL);

    root_sizer->AddSpacer(FromDIP(23));

    // Cancel button — SVG bitmap button.
    // timelapse_cancel_active (orange) when enabled, timelapse_cancel_disabled (gray) when finished.
    wxBitmap cancel_bmp = create_scaled_bitmap("timelapse_cancel_active", this, CANCEL_SIZE);
    m_cancel_btn = new wxStaticBitmap(this, wxID_ANY, cancel_bmp);
    m_cancel_btn->SetMinSize(wxSize(FromDIP(CANCEL_SIZE), FromDIP(CANCEL_SIZE)));
    m_cancel_btn->SetMaxSize(wxSize(FromDIP(CANCEL_SIZE), FromDIP(CANCEL_SIZE)));
    m_cancel_btn->SetCursor(wxCURSOR_HAND);
    m_cancel_btn->Bind(wxEVT_LEFT_DOWN, &TimelapseTaskRow::on_cancel_down, this);
    root_sizer->Add(m_cancel_btn, 0, wxALIGN_CENTER_VERTICAL);

    // Outer padding (Figma: 8px 20px 8px 14px — top right bottom left)
    wxBoxSizer* outer = new wxBoxSizer(wxHORIZONTAL);
    outer->AddSpacer(FromDIP(14));
    outer->Add(root_sizer, 1, wxEXPAND);
    outer->AddSpacer(FromDIP(20));

    SetSizer(outer);
    Layout();

    SetDoubleBuffered(true);

    set_state(State::Pending);
}

TimelapseTaskRow::~TimelapseTaskRow() = default;

void TimelapseTaskRow::set_state(State s)
{
    if (m_state == s) {
        return;
    }
    m_state = s;

    // Status icon SVG
    const char* icon_name = nullptr;
    switch (s) {
        case State::Completed: icon_name = "timelapse_success"; break;
        case State::Failed:    icon_name = "timelapse_error";   break;
        default:               icon_name = nullptr;              break;
    }
    if (icon_name) {
        m_status_icon->SetBitmap(create_scaled_bitmap(icon_name, this, STATUS_ICON_SIZE));
        m_status_icon->Show();
    } else {
        m_status_icon->Hide();
    }

    if (auto* p = dynamic_cast<ProgressBar*>(m_progress_track)) {
        p->set_visible(s == State::Downloading);
    }

    switch (s) {
        case State::Pending:
            m_status_text = _L("Waiting for device to upload");
            m_percent = 0;
            m_cancel_enabled = true;
            m_cancel_btn->SetBitmap(create_scaled_bitmap("timelapse_cancel_active", this, CANCEL_SIZE));
            m_cancel_btn->SetCursor(wxCURSOR_HAND);
            break;
        case State::Downloading:
            if (m_status_text.empty() || m_status_text == _L("Waiting for device to upload")) {
                m_status_text = wxString::Format(_L("%d%%"), m_percent);
            }
            m_cancel_enabled = true;
            m_cancel_btn->SetBitmap(create_scaled_bitmap("timelapse_cancel_active", this, CANCEL_SIZE));
            m_cancel_btn->SetCursor(wxCURSOR_HAND);
            break;
        case State::Completed:
            m_status_text = _L("Downloaded");
            m_percent = 100;
            disable_cancel();
            break;
        case State::Failed:
            disable_cancel();
            break;
        case State::Cancelled:
            m_status_text = _L("Download Cancelled");
            disable_cancel();
            break;
    }

    if (s == State::Failed) {
        m_status_text = _L("Download Failed");
    }

    if (auto* p = dynamic_cast<ProgressBar*>(m_progress_track)) {
        p->set_percent(m_percent);
    }

    m_status_label->SetLabel(m_status_text);
    Layout();
    Refresh();
}

void TimelapseTaskRow::set_progress(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    m_percent = percent;
    if (auto* p = dynamic_cast<ProgressBar*>(m_progress_track)) {
        p->set_percent(percent);
    }
    m_status_text = wxString::Format(_L("%d%%"), percent);
    m_status_label->SetLabel(m_status_text);
    m_status_label->Refresh();
}

void TimelapseTaskRow::set_status_text(const wxString& text)
{
    m_status_text = text;
    m_status_label->SetLabel(text);
    Layout();
    Refresh();
}

void TimelapseTaskRow::set_cancel_callback(std::function<void()> cb)
{
    m_cancel_cb = std::move(cb);
}

void TimelapseTaskRow::set_dismiss_callback(std::function<void()> cb)
{
    m_dismiss_cb = std::move(cb);
}

void TimelapseTaskRow::disable_cancel()
{
    m_cancel_enabled = false;
    m_cancel_btn->SetBitmap(create_scaled_bitmap(
        wxGetApp().dark_mode() ? "timelapse_cancel_disabled_dark" : "timelapse_cancel_disabled",
        this, CANCEL_SIZE));
    m_cancel_btn->SetCursor(wxCURSOR_HAND);
}

void TimelapseTaskRow::refresh_dark_mode()
{
    SetBackgroundColour(bg_color());
    m_name_label->SetForegroundColour(file_name_color());
    m_status_label->SetForegroundColour(status_text_color());
    // Progress track: its on_paint reads parent bg for the hidden state, so
    // force a repaint to pick up the new theme.
    if (m_progress_track) {
        m_progress_track->Refresh();
    }
    // Re-apply cancel button bitmap for current state
    if (m_cancel_enabled) {
        m_cancel_btn->SetBitmap(create_scaled_bitmap("timelapse_cancel_active", this, CANCEL_SIZE));
    } else {
        m_cancel_btn->SetBitmap(create_scaled_bitmap(
            wxGetApp().dark_mode() ? "timelapse_cancel_disabled_dark" : "timelapse_cancel_disabled",
            this, CANCEL_SIZE));
    }
    // Refresh each child explicitly — wx Refresh() doesn't recurse into children.
    m_name_label->Refresh();
    m_status_label->Refresh();
    m_cancel_btn->Refresh();
    if (m_status_icon) {
        m_status_icon->Refresh();
    }
    Refresh();
}

void TimelapseTaskRow::on_cancel_down(wxMouseEvent&)
{
    if (m_cancel_enabled) {
        // Active download — trigger cancel (no confirmation)
        disable_cancel();
        if (m_cancel_cb) {
            wxGetApp().CallAfter([this]() {
                if (m_cancel_cb) {
                    m_cancel_cb();
                }
            });
        }
    } else {
        // Finished state — dismiss row from list (UI only)
        if (m_dismiss_cb) {
            wxGetApp().CallAfter([this]() {
                if (m_dismiss_cb) {
                    m_dismiss_cb();
                }
            });
        }
    }
}

// ===========================================================================
// TimelapseDownloadPopup implementation
// ===========================================================================
TimelapseDownloadPopup::TimelapseDownloadPopup(wxWindow* parent)
#ifdef __WXMSW__
    : DPIDialog(parent, wxID_ANY, wxEmptyString, wxDefaultPosition,
                wxDefaultSize,
                wxSTAY_ON_TOP | wxBORDER_NONE)
#else
    : DPIDialog(parent, wxID_ANY, wxEmptyString, wxDefaultPosition,
                wxDefaultSize,
                wxSTAY_ON_TOP | wxBORDER_SIMPLE)
#endif
    , m_content_panel(nullptr)
    , m_title_bar(nullptr)
    , m_title_divider(nullptr)
    , m_title_label(nullptr)
    , m_collapse_btn(nullptr)
    , m_task_panel(nullptr)
    , m_task_sizer(nullptr)
    , m_collapsed(false)
    , m_all_complete(false)
    , m_task_count(0)
    , m_was_dark_mode(wxGetApp().dark_mode())
{
    // Dialog background = border color; the inset content panel (bg_color)
    // creates a 1px border ring so the popup is visually separated from the
    // (white) device page behind it — wxBORDER_SIMPLE renders nothing on macOS.
    SetBackgroundColour(divider_color());
    m_content_panel = new wxPanel(this, wxID_ANY);
    m_content_panel->SetBackgroundColour(bg_color());

    // Flicker-free for the whole popup subtree (title bar, scrolled list, rows).
    SetDoubleBuffered(true);

#ifdef __WXMSW__
    // The 1px ring is this dialog's background showing around m_content_panel.
    // Under WS_EX_COMPOSITED + Freeze/Thaw the erase pass is intermittently
    // skipped, leaving the ring showing stale content. Fill it on every paint
    // so the border is deterministic.
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxPaintDC dc(this);
        dc.SetBackground(wxBrush(divider_color()));
        dc.Clear();
    });
#endif

    // ---- Title bar (375×40) ----
    m_title_bar = new wxPanel(m_content_panel, wxID_ANY);
    m_title_bar->SetBackgroundColour(bg_color());
    m_title_bar->SetMinSize(wxSize(FromDIP(DIALOG_WIDTH), FromDIP(TITLE_BAR_HEIGHT)));
    m_title_bar->SetMaxSize(wxSize(FromDIP(DIALOG_WIDTH), FromDIP(TITLE_BAR_HEIGHT)));

    wxBoxSizer* title_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_title_label = new Label(m_title_bar, _L("Download Lists"));
    m_title_label->SetFont(::Label::Head_13);
    m_title_label->SetForegroundColour(title_text_color());
    title_sizer->Add(m_title_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(14));

    title_sizer->AddStretchSpacer(1);

    // Collapse button — SVG bitmap (down arrow when expanded, up arrow when collapsed)
    wxBitmap collapse_bmp = create_scaled_bitmap(
        wxGetApp().dark_mode() ? "timelapse_collapse_down_dark" : "timelapse_collapse_down",
        m_title_bar, 16);
    m_collapse_btn = new wxStaticBitmap(m_title_bar, wxID_ANY, collapse_bmp);
    m_collapse_btn->SetMinSize(wxSize(FromDIP(16), FromDIP(16)));
    m_collapse_btn->SetMaxSize(wxSize(FromDIP(16), FromDIP(16)));
    m_collapse_btn->SetCursor(wxCURSOR_HAND);
    m_collapse_btn->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { toggle_collapse(); });
    title_sizer->Add(m_collapse_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(22));

    m_title_bar->SetSizer(title_sizer);

    // ---- Task panel (scrolled window) ----
    m_task_panel = new wxScrolledWindow(m_content_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_task_panel->SetBackgroundColour(bg_color());
    m_task_panel->SetDoubleBuffered(true);
    m_task_panel->SetScrollRate(0, FromDIP(SCROLL_RATE));
    m_task_panel->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_NEVER);
    m_task_sizer = new wxBoxSizer(wxVERTICAL);
    m_task_panel->SetSizer(m_task_sizer);

    // ---- Main layout ----
    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_title_bar, 0, wxEXPAND);

    // Title divider (Figma: border-bottom: 1px solid #D9D9D9)
    m_title_divider = new wxPanel(m_content_panel, wxID_ANY);
    m_title_divider->SetBackgroundColour(divider_color());
    m_title_divider->SetMinSize(wxSize(FromDIP(345), FromDIP(1)));
    m_title_divider->SetMaxSize(wxSize(FromDIP(345), FromDIP(1)));
    main_sizer->Add(m_title_divider, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(14));

    main_sizer->Add(m_task_panel, 0, wxEXPAND);
    m_content_panel->SetSizer(main_sizer);

    // Outer sizer: 1px margin on all sides exposes the divider_color dialog
    // background as a border ring around the content panel.
    wxBoxSizer* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(m_content_panel, 1, wxEXPAND | wxALL, FromDIP(BORDER_WIDTH));
    SetSizer(outer);

    SetMinSize(wxSize(FromDIP(DIALOG_WIDTH + BORDER_WIDTH * 2), FromDIP(DIALOG_HEIGHT + BORDER_WIDTH * 2)));
    SetMaxSize(wxSize(FromDIP(DIALOG_WIDTH + BORDER_WIDTH * 2), FromDIP(DIALOG_HEIGHT + BORDER_WIDTH * 2)));
    Fit();
    Layout();

    position_bottom_right();

    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
        if (m_close_callback) {
            m_close_callback();
        }
    });

    m_position_timer = new wxTimer(this);
    Bind(wxEVT_TIMER, &TimelapseDownloadPopup::on_timer, this);
    m_position_timer->Start(200);

    wxGetApp().UpdateDlgDarkUI(this);
}

TimelapseDownloadPopup::~TimelapseDownloadPopup() = default;

int TimelapseDownloadPopup::add_tasks(const std::vector<TaskInfo>& tasks)
{
    // Freeze the entire popup while batch-creating rows + reordering + resizing.
    // Without this, creating 30+ rows triggers a repaint per row → heavy flicker.
    // RAII: Freeze on construct, Thaw on destruct (function return).
    wxWindowUpdateLocker lock(this);

    int row_offset = static_cast<int>(m_rows.size());
    m_task_count += static_cast<int>(tasks.size());

    for (size_t i = 0; i < tasks.size(); ++i) {
        int global_idx = row_offset + static_cast<int>(i);
        auto* row = new TimelapseTaskRow(m_task_panel,
            wxString::FromUTF8(tasks[i].file_name.c_str()));
        row->set_queue_position(global_idx + 1);
        row->set_dismiss_callback([this, global_idx]() { dismiss_row(global_idx); });
        TaskRowInfo info;
        info.row = row;
        info.state = 0;
        info.visible = true;
        m_rows.push_back(std::move(info));
    }

    // Refresh title with total visible count (across all batches)
    int visible = 0;
    for (const auto& r : m_rows) {
        if (r.visible) { ++visible; }
    }
    m_title_label->SetLabel(
        wxString::Format(_L("Download Lists (%d)"), visible));

    reorder_rows();
    update_layout_size();
    return row_offset;
}

void TimelapseDownloadPopup::set_task_progress(int index, int percent,
                                                size_t downloaded, size_t total)
{
    if (index < 0 || index >= static_cast<int>(m_rows.size())) {
        return;
    }
    TaskRowInfo& info = m_rows[index];
    if (info.state == 2 || info.state == 3 || info.state == 4) {
        return;
    }
    if (info.state != 1) {
        set_row_state(index, 1);
    }
    info.row->set_progress(percent);
}

void TimelapseDownloadPopup::set_task_status(int index, const wxString& text)
{
    if (index < 0 || index >= static_cast<int>(m_rows.size())) {
        return;
    }
    m_rows[index].row->set_status_text(text);
}

void TimelapseDownloadPopup::mark_task_complete(int index, const std::string&)
{
    if (index < 0 || index >= static_cast<int>(m_rows.size())) {
        return;
    }
    set_row_state(index, 2);
}

void TimelapseDownloadPopup::mark_task_error(int index, const std::string& error)
{
    if (index < 0 || index >= static_cast<int>(m_rows.size())) {
        return;
    }
    set_row_state(index, 3);
    m_rows[index].row->set_status_text(error.empty()
        ? _L("Download Failed")
        : wxString::FromUTF8(error.c_str()));
}

void TimelapseDownloadPopup::mark_task_cancelled(int index)
{
    if (index < 0 || index >= static_cast<int>(m_rows.size())) {
        return;
    }
    set_row_state(index, 4);
}

void TimelapseDownloadPopup::mark_all_complete(int completed, int failed,
                                                  const std::string& save_path,
                                                  const std::string& latest_file)
{
    m_all_complete = true;
    m_open_target = latest_file.empty() ? save_path : latest_file;
    if (!m_open_target.empty()) {
        m_close_callback = [this]() {
            desktop_open_any_folderEx(m_open_target);
            Close();
        };
    }
}

void TimelapseDownloadPopup::set_task_cancel_callback(int index, std::function<void()> cb)
{
    if (index < 0 || index >= static_cast<int>(m_rows.size())) {
        return;
    }
    m_rows[index].row->set_cancel_callback([this, cb = std::move(cb), index]() mutable {
        if (index >= 0 && index < static_cast<int>(m_rows.size())) {
            m_rows[index].row->disable_cancel();
        }
        wxGetApp().CallAfter([cb = std::move(cb)]() { cb(); });
    });
}

void TimelapseDownloadPopup::set_close_callback(std::function<void()> cb)
{
    m_close_callback = std::move(cb);
}

void TimelapseDownloadPopup::set_row_state(int index, int new_state)
{
    if (index < 0 || index >= static_cast<int>(m_rows.size())) {
        return;
    }
    TaskRowInfo& info = m_rows[index];
    if (info.state == new_state) {
        return;
    }
    info.state = new_state;

    using S = TimelapseTaskRow::State;
    S s;
    switch (new_state) {
        case 0: s = S::Pending; break;
        case 1: s = S::Downloading; break;
        case 2: s = S::Completed; break;
        case 3: s = S::Failed; break;
        case 4: s = S::Cancelled; break;
        default: return;
    }
    info.row->set_state(s);

    reorder_rows();
}

void TimelapseDownloadPopup::destroy_dividers()
{
    std::vector<wxWindow*> to_destroy;
    for (wxWindow* child : m_task_panel->GetChildren()) {
        if (dynamic_cast<TimelapseTaskRow*>(child) == nullptr) {
            to_destroy.push_back(child);
        }
    }
    for (wxWindow* child : to_destroy) {
        child->Destroy();
    }
}

void TimelapseDownloadPopup::dismiss_row(int index)
{
    if (index < 0 || index >= static_cast<int>(m_rows.size())) {
        return;
    }
    TaskRowInfo& info = m_rows[index];
    if (!info.visible) {
        return;
    }
    info.visible = false;
    info.row->Hide();
    reorder_rows();
    update_layout_size();

    // Auto-close when no visible rows remain (user has dismissed everything).
    int visible_count = 0;
    for (const auto& r : m_rows) {
        if (r.visible) {
            ++visible_count;
        }
    }
    m_title_label->SetLabel(
        wxString::Format(_L("Download Lists (%d)"), visible_count));
    if (visible_count == 0) {
        wxGetApp().CallAfter([this]() {
            if (!IsBeingDeleted()) {
                Close();
            }
        });
    }
}

void TimelapseDownloadPopup::reorder_rows()
{
    // Freeze/Thaw: reorder detaches all rows and re-creates dividers, which
    // without freezing repaints on every detach/add — visible as flicker.
    m_task_panel->Freeze();

    // Priority: downloading(1) > pending(0) > finished(2,3,4)
    // Only include visible rows.
    std::vector<int> order;
    order.reserve(m_rows.size());
    for (int p = 1; p >= 0; --p) {
        for (size_t i = 0; i < m_rows.size(); ++i) {
            if (!m_rows[i].visible) {
                continue;
            }
            if (m_rows[i].state == p) {
                order.push_back(static_cast<int>(i));
            }
        }
    }
    for (size_t i = 0; i < m_rows.size(); ++i) {
        if (!m_rows[i].visible) {
            continue;
        }
        if (m_rows[i].state >= 2) {
            order.push_back(static_cast<int>(i));
        }
    }

    destroy_dividers();
    while (m_task_sizer->GetItemCount() > 0) {
        m_task_sizer->Detach(0);
    }
    for (size_t i = 0; i < order.size(); ++i) {
        m_task_sizer->Add(m_rows[order[i]].row, 0, wxEXPAND);
        if (i + 1 < order.size()) {
            auto* divider = new wxPanel(m_task_panel, wxID_ANY);
            divider->SetBackgroundColour(divider_color());
            divider->SetMinSize(wxSize(FromDIP(345), FromDIP(1)));
            divider->SetMaxSize(wxSize(FromDIP(345), FromDIP(1)));
            m_task_sizer->Add(divider, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(14));
        }
    }
    m_task_panel->Layout();
    Layout();
    m_task_panel->Thaw();
}

void TimelapseDownloadPopup::position_bottom_right()
{
    wxWindow* p = GetPopupParent();
    if (p == nullptr) {
        return;
    }
    wxRect parentRect = p->GetScreenRect();
    wxSize mySize = GetSize();
    int x = parentRect.GetRight() - mySize.GetWidth() - FromDIP(24);
    int y = parentRect.GetBottom() - mySize.GetHeight() - FromDIP(BOTTOM_MARGIN);
    SetPosition(wxPoint(x, y));
}

void TimelapseDownloadPopup::on_dpi_changed(const wxRect&) { update_layout_size(); }

void TimelapseDownloadPopup::on_timer(wxTimerEvent&)
{
    wxWindow* p = GetPopupParent();
    if (p != nullptr) {
        wxTopLevelWindow* tlw = dynamic_cast<wxTopLevelWindow*>(p);
        if (tlw != nullptr && tlw->IsIconized()) {
            if (IsShown()) { Hide(); }
            return;
        }
    }
    if (wxWindow::FindFocus() == nullptr) {
        if (IsShown()) { Hide(); }
        return;
    }
    if (!IsShown()) { Show(); }
    // Detect dark mode switch and refresh theme
    bool cur_dark = wxGetApp().dark_mode();
    if (m_was_dark_mode != cur_dark) {
        m_was_dark_mode = cur_dark;
        refresh_dark_mode();
    }
    position_bottom_right();
}

void TimelapseDownloadPopup::toggle_collapse()
{
    m_collapsed = !m_collapsed;
    const char* icon_down = wxGetApp().dark_mode() ? "timelapse_collapse_down_dark" : "timelapse_collapse_down";
    const char* icon_up   = wxGetApp().dark_mode() ? "timelapse_collapse_up_dark"   : "timelapse_collapse_up";
    const char* icon = m_collapsed ? icon_up : icon_down;
    m_collapse_btn->SetBitmap(create_scaled_bitmap(icon, m_title_bar, 16));
    update_layout_size();
}

void TimelapseDownloadPopup::update_layout_size()
{
    int total_rows = 0;
    for (const auto& r : m_rows) {
        if (r.visible) {
            ++total_rows;
        }
    }

    if (m_collapsed) {
        m_task_panel->Hide();
        m_title_divider->Hide();
        wxSize target(FromDIP(DIALOG_WIDTH + BORDER_WIDTH * 2), FromDIP(TITLE_BAR_HEIGHT + BORDER_WIDTH * 2));
        SetMinSize(target);
        SetMaxSize(target);
        SetSize(target);
        Layout();
        position_bottom_right();
        return;
    }

    m_task_panel->Show();
    m_title_divider->Show();

    int rows_to_show = total_rows;
    bool needs_scroll = false;
    if (rows_to_show > MAX_VISIBLE_ROWS) {
        rows_to_show = MAX_VISIBLE_ROWS;
        needs_scroll = true;
    }

    // Panel height: each row contributes TASK_ROW_HEIGHT; dividers are 1px between rows.
    int panel_h;
    if (rows_to_show == 0) {
        panel_h = FromDIP(CONTENT_HEIGHT);
    } else {
        int dividers = rows_to_show - 1;
        panel_h = rows_to_show * FromDIP(TASK_ROW_HEIGHT) + dividers * FromDIP(1);
    }

    m_task_panel->SetMinSize(wxSize(FromDIP(DIALOG_WIDTH), panel_h));
    m_task_panel->SetMaxSize(wxSize(FromDIP(DIALOG_WIDTH), panel_h));

    if (needs_scroll) {
        int total_dividers = total_rows - 1;
        int virtual_h = total_rows * FromDIP(TASK_ROW_HEIGHT) + total_dividers * FromDIP(1);
        m_task_panel->SetVirtualSize(FromDIP(DIALOG_WIDTH), virtual_h);
        m_task_panel->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_DEFAULT);
    } else {
        m_task_panel->SetVirtualSize(FromDIP(DIALOG_WIDTH), panel_h);
        m_task_panel->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_NEVER);
    }

    int dialog_h = FromDIP(TITLE_BAR_HEIGHT) + FromDIP(1) /* title divider */ + panel_h + FromDIP(BORDER_WIDTH * 2) /* outer border */;
    wxSize target(FromDIP(DIALOG_WIDTH + BORDER_WIDTH * 2), dialog_h);
    SetMinSize(target);
    SetMaxSize(target);
    SetSize(target);

    Layout();
    position_bottom_right();
}

void TimelapseDownloadPopup::Close()
{
    if (m_position_timer != nullptr) {
        m_position_timer->Stop();
    }
    this->Hide();
    this->Destroy();
}

void TimelapseDownloadPopup::refresh_dark_mode()
{
    // Popup-level colors
    SetBackgroundColour(divider_color());
    m_content_panel->SetBackgroundColour(bg_color());
    m_title_bar->SetBackgroundColour(bg_color());
    m_title_label->SetForegroundColour(title_text_color());
    m_task_panel->SetBackgroundColour(bg_color());
    m_title_divider->SetBackgroundColour(divider_color());

    // Collapse button bitmap
    const char* icon_down = wxGetApp().dark_mode() ? "timelapse_collapse_down_dark" : "timelapse_collapse_down";
    const char* icon_up   = wxGetApp().dark_mode() ? "timelapse_collapse_up_dark"   : "timelapse_collapse_up";
    m_collapse_btn->SetBitmap(create_scaled_bitmap(
        m_collapsed ? icon_up : icon_down, m_title_bar, 16));

    // Row dividers (inside m_task_panel) — refresh each so the new colour shows
    for (wxWindow* child : m_task_panel->GetChildren()) {
        if (dynamic_cast<TimelapseTaskRow*>(child) == nullptr) {
            child->SetBackgroundColour(divider_color());
            child->Refresh();
        }
    }

    // Each task row
    for (auto& info : m_rows) {
        if (info.row) {
            info.row->refresh_dark_mode();
        }
    }

    // Refresh every child explicitly — wx Refresh() only repaints the window
    // itself, not its children, so SetBackgroundColour/SetForegroundColour on
    // children won't show until each is refreshed.
    m_content_panel->Refresh();
    m_title_bar->Refresh();
    m_title_label->Refresh();
    m_title_divider->Refresh();
    m_collapse_btn->Refresh();
    m_task_panel->Refresh();
    Refresh();
    Layout();
}

}} // namespace Slic3r::GUI
