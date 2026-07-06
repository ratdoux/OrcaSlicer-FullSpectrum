#ifndef slic3r_GUI_NetworkTestDialog_hpp_
#define slic3r_GUI_NetworkTestDialog_hpp_

#include <wx/wx.h>
#include <boost/thread.hpp>

#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"
#include <slic3r/GUI/Widgets/Button.hpp>
#include <wx/artprov.h>
#include <wx/xrc/xmlres.h>
#include <wx/button.h>
#include <wx/string.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/icon.h>
#include <wx/gdicmn.h>
#include <wx/font.h>
#include <wx/colour.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/grid.h>
#include <wx/dialog.h>
#include <wx/srchctrl.h>
#include <wx/stattext.h>
#include <iomanip>
#include <sys/timeb.h>
#include <time.h>
#include <vector>
#include <algorithm>
#include <memory>

namespace Slic3r { 
namespace GUI {

enum TestJob {
	TEST_BING_JOB = 0,
	TEST_ORCA_JOB = 1,
	TEST_PING_JOB,
	TEST_LAN_MQTT_JOB,
	TEST_CLOUD_MQTT_JOB,
	TEST_LOGIN_API_JOB,
	TEST_UPLOAD_API_JOB,
	TEST_JOB_MAX
};

class NetworkTestDialog : public DPIDialog
{
protected:
	std::shared_ptr<NetworkTestDialog> self_ptr;
	std::weak_ptr<NetworkTestDialog> weak_this;
	Button* btn_start;
	Button* btn_start_sequence;
	Button* btn_download_log;
	Button* btn_clear_log;
	wxStaticText* text_basic_info;
	wxStaticText* text_version_title;
	wxStaticText* text_version_val;
	wxStaticText* txt_sys_info_title;
	wxStaticText* txt_sys_info_value;
	wxStaticText* txt_dns_info_title;
	wxStaticText* txt_dns_info_value;
	Button*     btn_link;
	wxStaticText* text_link_title;
	wxStaticText* text_link_val;
	Button*     btn_bing;
	wxStaticText* text_bing_title;
	wxStaticText* text_bing_val;
	Button*     btn_lan_mqtt;
	wxStaticText* text_lan_mqtt_title;
	wxStaticText* text_lan_mqtt_val;
	Button*     btn_cloud_mqtt;
	wxStaticText* text_cloud_mqtt_title;
	wxStaticText* text_cloud_mqtt_val;
	Button*     btn_login_api;
	wxStaticText* text_login_api_title;
	wxStaticText* text_login_api_val;
	Button*     btn_upload_api;
	wxStaticText* text_upload_api_title;
	wxStaticText* text_upload_api_val;
	wxStaticText* text_ping_title;
	wxStaticText* text_ping_value;
	wxStaticText* text_result;
	wxTextCtrl* txt_log;

	wxBoxSizer* create_top_sizer(wxWindow* parent);
	wxBoxSizer* create_info_sizer(wxWindow* parent);
	wxBoxSizer* create_content_sizer(wxWindow* parent);
	wxBoxSizer* create_result_sizer(wxWindow* parent);

	boost::thread* test_job[TEST_JOB_MAX];
	boost::thread* m_sequence_job { nullptr };
	std::atomic<bool> m_in_testing[TEST_JOB_MAX];
	bool           m_download_cancel = false;
	std::atomic<bool> m_closing{false};

	void init_bind();

public:
	NetworkTestDialog(wxWindow* parent, wxWindowID id = wxID_ANY, const wxString& title = wxEmptyString, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxSize(605, 375), long style = wxDEFAULT_DIALOG_STYLE);

	~NetworkTestDialog();

	void on_dpi_changed(const wxRect& suggested_rect);

	void set_default();
	wxString get_studio_version();
	wxString get_os_info();
	wxString get_dns_info();

	void start_all_job();
	void start_all_job_sequence();
	void start_test_bing_thread();
	void start_test_github_thread();
	void start_test_ping_thread();
	void start_test_lan_mqtt_thread();
	void start_test_cloud_mqtt_thread();
	void start_test_login_api_thread();
	void start_test_upload_api_thread();

	void start_test_url(TestJob job, wxString name, wxString url);
	void start_test_telnet(TestJob job, wxString name, wxString server, int port);
	void start_test_ping(wxString server, TestJob job);

	void on_close(wxCloseEvent& event);

	void update_status(int job_id, wxString info);

	wxString get_cloud_server_address();

	// Print a section header with separator lines to both UI and file log.
	void log_section_header(const wxString& title);

private:
	void cleanup_threads();
};

} // namespace GUI
} // namespace Slic3r

#endif
