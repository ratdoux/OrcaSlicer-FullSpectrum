#pragma once

#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/MainFrame.hpp"

namespace Slic3r
{
namespace GUI
{

class SyncConfirmDialog : public MessageDialog
{
public:
    SyncConfirmDialog(wxWindow* parent, const wxString& message, long style = wxOK);
    ~SyncConfirmDialog() override = default;
};

class SyncRichConfirmDialog : public RichMessageDialog
{
public:
    SyncRichConfirmDialog(wxWindow* parent, const wxString& message, long style = wxOK);
    ~SyncRichConfirmDialog() override = default;

    void navigateToTab(MainFrame::TabPosition pos);
};

} // namespace GUI
} // namespace Slic3r
