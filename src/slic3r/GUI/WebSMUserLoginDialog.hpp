#pragma once
#ifndef slic3r_SMWebUserLogin_HEAD_
#define slic3r_SMWebUserLogin_HEAD_

#include "wx/artprov.h"
#include "wx/cmdline.h"
#include "wx/notifmsg.h"
#include "wx/settings.h"
#include "wx/webview.h"

#if wxUSE_WEBVIEW_IE
#include "wx/msw/webview_ie.h"
#endif
#if wxUSE_WEBVIEW_EDGE
#include "wx/msw/webview_edge.h"
#endif

#include "wx/webviewarchivehandler.h"
#include "wx/webviewfshandler.h"
#include "wx/numdlg.h"
#include "wx/infobar.h"
#include "wx/filesys.h"
#include "wx/fs_arc.h"
#include "wx/fs_mem.h"
#include "wx/stdpaths.h"
#include <wx/frame.h>
#include "wx/timer.h"
#include <wx/tbarbase.h>
#include "wx/textctrl.h"

#include <functional>
#include <memory>

#include "GUI_Utils.hpp"

namespace Slic3r { namespace GUI {

class SMUserLogin : public wxDialog
{
public:
    SMUserLogin(bool isLogout = false);
    virtual ~SMUserLogin();

    void load_url(wxString &url);

    std::string w2s(wxString sSrc);

    void UpdateState();
    void OnIdle(wxIdleEvent &evt);
    // void OnClose(wxCloseEvent &evt);

    void OnNavigationRequest(wxWebViewEvent &evt);
    void OnNavigationComplete(wxWebViewEvent &evt);
    void OnDocumentLoaded(wxWebViewEvent &evt);
    void OnNewWindow(wxWebViewEvent &evt);
    void OnError(wxWebViewEvent &evt);
    void OnTitleChanged(wxWebViewEvent &evt);
    void OnFullScreenChanged(wxWebViewEvent &evt);
    void OnScriptMessage(wxWebViewEvent &evt);

    void OnScriptResponseMessage(wxCommandEvent &evt);
    void RunScript(const wxString &javascript);
    void try_complete_oauth_callback();

    bool m_networkOk;
    bool ShowErrorPage();

    bool run();

    static int web_sequence_id;
private:
    wxTimer *m_timer { nullptr };
    void     OnTimer(wxTimerEvent &event);
    void     OnCallbackPollTimer(wxTimerEvent &event);

    wxTimer *m_callback_timer { nullptr };
    bool     m_callback_handled = false;
    int      m_callback_attempts = 0;

private:

    wxString   TargetUrl     = "https://id.snapmaker.cn?from=orca";
    wxString   LogoutUrl     = "https://id.snapmaker.cn/logout?from=orca";
    wxString   m_hostUrl     = "https://id.snapmaker.cn";
    wxString   m_accountUrl  = "https://api.snapmaker.cn";
    wxString   m_userInfoUrl = "https://api.snapmaker.cn/api/common/accounts/current";
    wxString   m_home_url    = "https://www.snapmaker.cn/";
    wxWebView *m_browser;

    std::string m_AutotestToken;

#if wxUSE_WEBVIEW_IE
    wxMenuItem *m_script_object_el;
    wxMenuItem *m_script_date_el;
    wxMenuItem *m_script_array_el;
#endif
    // Last executed JavaScript snippet, for convenience.
    wxString m_javascript;
    wxString m_response_js;

    wxString m_sm_user_agent;

    DECLARE_EVENT_TABLE()
};

class SMAskUserLoginDialog : public DPIDialog
{
public:
    SMAskUserLoginDialog(wxWindow *parent);
    ~SMAskUserLoginDialog();

    void SetKeepAliveCallback(std::function<void()> fn);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override {}

private:
    std::unique_ptr<wxTimer> m_keepalive_timer;
    std::function<void()> m_keepalive_fn;
};

}} // namespace Slic3r::GUI

#endif 
