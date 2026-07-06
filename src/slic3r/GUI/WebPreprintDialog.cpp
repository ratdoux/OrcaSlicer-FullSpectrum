#include "WebPreprintDialog.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "SSWCP.hpp"
#include <wx/sizer.h>
#include <slic3r/GUI/Widgets/WebView.hpp>
#include "NotificationManager.hpp"
#include "sentry_wrapper/SentryWrapper.hpp"

namespace Slic3r { namespace GUI {

BEGIN_EVENT_TABLE(WebPreprintDialog, wxDialog)
    EVT_CLOSE(WebPreprintDialog::OnClose)
END_EVENT_TABLE()

WebPreprintDialog::WebPreprintDialog()
    : wxDialog((wxWindow*)(wxGetApp().mainframe), wxID_ANY, _L("Print preset"))
{
    m_prePrint_url = wxString::FromUTF8(LOCALHOST_URL + std::to_string(wxGetApp().get_page_http_port()) +
                     "/web/flutter_web/index.html?path=4");

    m_preSend_url = wxString::FromUTF8(LOCALHOST_URL + std::to_string(wxGetApp().get_page_http_port()) +
                     "/web/flutter_web/index.html?path=5");
    SetBackgroundColour(*wxWHITE);

    // Create the webview

    // 语言判断
    wxString target_url = wxGetApp().get_international_url(m_prePrint_url);

    m_browser = WebView::CreateWebView(this, target_url);
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }
    //m_browser->Hide();

    // Connect the webview events
    Bind(wxEVT_WEBVIEW_NAVIGATING, &WebPreprintDialog::OnNavigationRequest, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_NAVIGATED, &WebPreprintDialog::OnNavigationComplete, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_LOADED, &WebPreprintDialog::OnDocumentLoaded, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_ERROR, &WebPreprintDialog::OnError, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &WebPreprintDialog::OnScriptMessage, this, m_browser->GetId());

    // Set dialog size
    SetMinSize(FromDIP(wxSize(714, 750)));
    SetSize(FromDIP(wxSize(714, 750)));

    // Create sizer and add webview
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    SetSizer(sizer);

    // Center dialog
    CenterOnParent();

    wxGetApp().UpdateDlgDarkUI(this);

    auto ptr = wxGetApp().get_web_device_dialog();
    if (ptr) {
        delete ptr;
    }

    wxGetApp().set_web_preprint_dialog(this);
}

WebPreprintDialog::~WebPreprintDialog()
{
    SSWCP::on_webview_delete(m_browser);

    wxGetApp().fltviews().remove_view(m_browser);

    wxGetApp().set_web_preprint_dialog(nullptr);
}

bool WebPreprintDialog::is_send_page()
{
    return m_send_page;
}

void WebPreprintDialog::set_send_page(bool flag)
{
    m_send_page = flag;
}

void WebPreprintDialog::set_swtich_to_device(bool flag)
{
    m_switch_to_device = flag;
}

void WebPreprintDialog::set_display_file_name(const std::string& filename) {
    m_display_file_name = filename;
}

void WebPreprintDialog::set_gcode_file_name(const std::string& filename)
{ m_gcode_file_name = filename; }

void WebPreprintDialog::set_finish(bool flag)
{
    m_finish = flag;
    // BBS: Don't call EndModal here to avoid conflict with sw_FinishFilamentMapping()
    // The external sw_FinishFilamentMapping() function will handle EndModal based on m_finish flag
}

void WebPreprintDialog::SafeEndModal(int returnCode)
{
    // BBS: Prevent duplicate EndModal calls which can cause crashes
    if (IsModal() && !m_modal_ended) {
        m_modal_ended = true;
        EndModal(returnCode);
    }
}

void WebPreprintDialog::reload()
{
    load_url(m_prePrint_url);
}

void WebPreprintDialog::load_url(wxString &url)
{
    wxGetApp().fltviews().add_view(m_browser, url);
    m_browser->Show();
    m_browser->LoadURL(url);
   
    Layout();
}

bool WebPreprintDialog::run()
{
    SSWCP::update_active_filename(m_gcode_file_name);
    SSWCP::update_display_filename(m_display_file_name);

    auto real_url = m_send_page ? wxGetApp().get_international_url(m_preSend_url) : wxGetApp().get_international_url(m_prePrint_url);
    if(m_send_page){
        this->SetTitle(_L("Pretreat the uploaded content"));
    }else{
        this->SetTitle(_L("Print Preprocessing"));
    }

    this->load_url(real_url);
    
    // BBS: Reset flags before showing modal
    m_finish = false;
    m_modal_ended = false;
    
    int result = this->ShowModal();
    
    // BBS: Check finish flag to determine return value
    if (result == wxID_OK || (result == wxID_CANCEL && m_finish)) {
        return m_finish;
    }
    return false;
}

void WebPreprintDialog::RunScript(const wxString &javascript)
{
    m_javascript = javascript;
    if (!m_browser) return;
    WebView::RunScript(m_browser, javascript);
}

void WebPreprintDialog::OnNavigationRequest(wxWebViewEvent &evt)
{
    evt.Skip();
}

void WebPreprintDialog::OnNavigationComplete(wxWebViewEvent &evt)
{
    m_browser->Show();
    Layout();
}

void WebPreprintDialog::OnDocumentLoaded(wxWebViewEvent &evt)
{
    evt.Skip();
}

void WebPreprintDialog::OnError(wxWebViewEvent &event)
{
    auto e = "unknown error";
    switch (event.GetInt()) {
    case wxWEBVIEW_NAV_ERR_CONNECTION: e = "wxWEBVIEW_NAV_ERR_CONNECTION"; break;
    case wxWEBVIEW_NAV_ERR_CERTIFICATE: e = "wxWEBVIEW_NAV_ERR_CERTIFICATE"; break;
    case wxWEBVIEW_NAV_ERR_AUTH: e = "wxWEBVIEW_NAV_ERR_AUTH"; break;
    case wxWEBVIEW_NAV_ERR_SECURITY: e = "wxWEBVIEW_NAV_ERR_SECURITY"; break;
    case wxWEBVIEW_NAV_ERR_NOT_FOUND: e = "wxWEBVIEW_NAV_ERR_NOT_FOUND"; break;
    case wxWEBVIEW_NAV_ERR_REQUEST: e = "wxWEBVIEW_NAV_ERR_REQUEST"; break;
    case wxWEBVIEW_NAV_ERR_USER_CANCELLED: e = "wxWEBVIEW_NAV_ERR_USER_CANCELLED"; break;
    case wxWEBVIEW_NAV_ERR_OTHER: e = "wxWEBVIEW_NAV_ERR_OTHER"; break;
    }

    BOOST_LOG_TRIVIAL(fatal) << __FUNCTION__<< boost::format(":WebPreprintDialog error loading page %1% %2% %3% %4%") % event.GetURL() % event.GetTarget() %e % event.GetString();
    
}

void WebPreprintDialog::OnScriptMessage(wxWebViewEvent &evt)
{
    // BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ": " << evt.GetString().ToUTF8().data();

    // if (wxGetApp().get_mode() == comDevelop)
    //     wxLogMessage("Script message received; value = %s, handler = %s", evt.GetString(), evt.GetMessageHandler());

    // test
    SSWCP::handle_web_message(evt.GetString().ToUTF8().data(), m_browser);

}

void WebPreprintDialog::OnClose(wxCloseEvent& evt)
{
    auto noti_manager = wxGetApp().mainframe->plater()->get_notification_manager();
    noti_manager->close_notification_of_type(NotificationType::PrintHostUpload);
    
    // BBS: Use SafeEndModal to prevent duplicate EndModal calls
    // This ensures consistency with sw_FinishFilamentMapping() and prevents crashes
    SafeEndModal(wxID_CANCEL);
    
    // If not modal or already ended, skip the event
    if (!IsModal() || m_modal_ended) {
        evt.Skip();
    }
}

}} // namespace Slic3r::GUI 