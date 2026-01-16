#ifndef slic3r_GUI_App_hpp_
#define slic3r_GUI_App_hpp_

#include <memory>
#include <string>
#include "ImGuiWrapper.hpp"
#include "ConfigWizard.hpp"
#include "OpenGLManager.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/UserNotification.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/GUI/WebViewDialog.hpp"
#include "slic3r/GUI/WebUserLoginDialog.hpp"
#include "slic3r/GUI/WebSMUserLoginDialog.hpp"
#include "slic3r/GUI/WebDeviceDialog.hpp"
#include "slic3r/GUI/WebPreprintDialog.hpp"
#include "slic3r/GUI/BindDialog.hpp"
#include "slic3r/GUI/HMS.hpp"
#include "slic3r/GUI/Jobs/UpgradeNetworkJob.hpp"
#include "slic3r/GUI/HttpServer.hpp"
#include "../Utils/PrintHost.hpp"
#include "slic3r/GUI/SSWCP.hpp"
#include "slic3r/Utils/Bonjour.hpp"
#include "slic3r/GUI/PrinterWebView.hpp"
#include <wx/app.h>
#include <wx/colour.h>
#include <wx/font.h>
#include <wx/string.h>
#include <wx/snglinst.h>
#include <wx/msgdlg.h>

#include <mutex>
#include <stack>

//#define BBL_HAS_FIRST_PAGE          1
#define STUDIO_INACTIVE_TIMEOUT     15*60*1000
#define LOG_FILES_MAX_NUM           30
#define TIMEOUT_CONNECT             15
#define TIMEOUT_RESPONSE            15

#define BE_UNACTED_ON               0x00200001
#define SHOW_BACKGROUND_BITMAP_PIXEL_THRESHOLD 80
#ifndef _MSW_DARK_MODE
    #define _MSW_DARK_MODE            1
#endif // _MSW_DARK_MODE

class wxMenuItem;
class wxMenuBar;
class wxTopLevelWindow;
class wxDataViewCtrl;
class wxBookCtrlBase;
// BBS
class Notebook;
struct wxLanguageInfo;


namespace Slic3r {

class AppConfig;
class PresetBundle;
class PresetUpdater;
class ModelObject;
class Model;
class UserManager;
class DeviceManager;
class NetworkAgent;
class TaskManager;

namespace GUI{

class RemovableDriveManager;
class OtherInstanceMessageHandler;
class MainFrame;
class Sidebar;
class ObjectSettings;
class ObjectList;
class ObjectLayers;
class Plater;
class ParamsPanel;
class NotificationManager;
class Downloader;
struct GUI_InitParams;
class ParamsDialog;
class HMSQuery;
class ModelMallDialog;
class PingCodeBindDialog;
class NetworkErrorDialog;


enum FileType
{
    FT_STEP,
    FT_STL,
    FT_OBJ,
    FT_AMF,
    FT_3MF,
    FT_GCODE_3MF,
    FT_GCODE,
    FT_MODEL,
    FT_ZIP,
    FT_PROJECT,
    FT_GALLERY,

    FT_INI,
    FT_SVG,

    FT_TEX,

    FT_SL1,

    FT_SIZE,
};

extern wxString file_wildcards(FileType file_type, const std::string &custom_extension = std::string{});

enum ConfigMenuIDs {
    //ConfigMenuWizard,
    //ConfigMenuSnapshots,
    //ConfigMenuTakeSnapshot,
    //ConfigMenuUpdate,
    //ConfigMenuDesktopIntegration,
    ConfigMenuPreferences,
    ConfigMenuPrinter,
    //ConfigMenuModeSimple,
    //ConfigMenuModeAdvanced,
    //ConfigMenuLanguage,
    //ConfigMenuFlashFirmware,
    ConfigMenuCnt,
};

enum Snapmaker_OrcaMenuIDs {
  Snapmaker_OrcaMenuAbout,
  Snapmaker_OrcaMenuPreferences,
};

enum CameraMenuIDs {
    wxID_CAMERA_PERSPECTIVE,
    wxID_CAMERA_ORTHOGONAL,
    wxID_CAMERA_COUNT,
};


class Tab;
class ConfigWizard;
class GizmoObjectManipulation;

static wxString dots("...", wxConvUTF8);

// Does our wxWidgets version support markup?
#if wxUSE_MARKUP && wxCHECK_VERSION(3, 1, 1)
    #define SUPPORTS_MARKUP
#endif


#define  VERSION_LEN    4
class VersionInfo
{
public:
    std::string version_str;
    std::string version_name;
    std::string description;
    std::string url;
    bool        force_upgrade{ false };
    int      ver_items[VERSION_LEN];  // AA.BB.CC.DD
    VersionInfo() {
        for (int i = 0; i < VERSION_LEN; i++) {
            ver_items[i] = 0;
        }
        force_upgrade = false;
        version_str = "";
    }

    void parse_version_str(std::string str) {
        version_str = str;
        std::vector<std::string> items;
        boost::split(items, str, boost::is_any_of("."));
        if (items.size() == VERSION_LEN) {
            try {
                for (int i = 0; i < VERSION_LEN; i++) {
                    ver_items[i] = stoi(items[i]);
                }
            }
            catch (...) {
                ;
            }
        }
    }
    static std::string convert_full_version(std::string short_version);
    static std::string convert_short_version(std::string full_version);
    static std::string get_full_version() {
        return convert_full_version(SLIC3R_VERSION);
    }

    /* return > 0, need update */
    int compare(std::string ver_str) {
        if (version_str.empty()) return -1;

        int      ver_target[VERSION_LEN];
        std::vector<std::string> items;
        boost::split(items, ver_str, boost::is_any_of("."));
        if (items.size() == VERSION_LEN) {
            try {
                for (int i = 0; i < VERSION_LEN; i++) {
                    ver_target[i] = stoi(items[i]);
                    if (ver_target[i] < ver_items[i]) {
                        return 1;
                    }
                    else if (ver_target[i] == ver_items[i]) {
                        continue;
                    }
                    else {
                        return -1;
                    }
                }
            }
            catch (...) {
                return -1;
            }
        }
        return -1;
    }
};

class GUI_App : public wxApp
{
public:

    //BBS: remove GCodeViewer as seperate APP logic
    enum class EAppMode : unsigned char
    {
        Editor,
        GCodeViewer
    };

    static std::atomic<bool> m_app_alive; // 标记应用是否存活

private:
    bool            m_initialized { false };
    bool            m_post_initialized { false };
    bool            m_app_conf_exists{ false };
    EAppMode        m_app_mode{ EAppMode::Editor };
    bool            m_is_recreating_gui{ false };
#ifdef __linux__
    bool            m_opengl_initialized{ false };
#endif
#if defined(__WINDOWS__)
    bool            m_is_arm64{false};
#endif

   
//#ifdef _WIN32
    wxColour        m_color_label_modified;
    wxColour        m_color_label_sys;
    wxColour        m_color_label_default;
    wxColour        m_color_window_default;
    wxColour        m_color_highlight_label_default;
    wxColour        m_color_hovered_btn_label;
    wxColour        m_color_default_btn_label;
    wxColour        m_color_highlight_default;
    wxColour        m_color_selected_btn_bg;
    bool            m_force_colors_update { false };
//#endif

    wxFont		    m_small_font;
    wxFont		    m_bold_font;
	wxFont			m_normal_font;
	wxFont			m_code_font;
    wxFont		    m_link_font;

    int             m_em_unit; // width of a "m"-symbol in pixels for current system font
                               // Note: for 100% Scale m_em_unit = 10 -> it's a good enough coefficient for a size setting of controls

    std::unique_ptr<wxLocale> 	  m_wxLocale;
    // System language, from locales, owned by wxWidgets.
    const wxLanguageInfo		 *m_language_info_system = nullptr;
    // Best translation language, provided by Windows or OSX, owned by wxWidgets.
    const wxLanguageInfo		 *m_language_info_best   = nullptr;

    OpenGLManager m_opengl_mgr;
    std::unique_ptr<RemovableDriveManager> m_removable_drive_manager;

    std::unique_ptr<ImGuiWrapper> m_imgui;
    std::unique_ptr<PrintHostJobQueue> m_printhost_job_queue;
	std::unique_ptr <OtherInstanceMessageHandler> m_other_instance_message_handler;
    std::unique_ptr <wxSingleInstanceChecker> m_single_instance_checker;
    std::string m_instance_hash_string;
	size_t m_instance_hash_int;

    std::unique_ptr<Downloader> m_downloader;

    //BBS
    bool m_is_closing {false};
    Slic3r::DeviceManager* m_device_manager { nullptr };
    Slic3r::UserManager* m_user_manager { nullptr };
    Slic3r::TaskManager* m_task_manager { nullptr };
    NetworkAgent* m_agent { nullptr };
    std::vector<std::string> need_delete_presets;   // store setting ids of preset
    std::vector<bool> m_create_preset_blocked { false, false, false, false, false, false }; // excceed limit
    bool m_networking_compatible { false };
    bool m_networking_need_update { false };
    bool m_networking_cancel_update { false };
    std::shared_ptr<UpgradeNetworkJob> m_upgrade_network_job;

    // login widget
    ZUserLogin*     login_dlg { nullptr };
    SMUserLogin*    sm_login_dlg{ nullptr };


public:
    // device dialog
    WebDeviceDialog* web_device_dialog{ nullptr };
    WebPreprintDialog* web_preprint_dialog{ nullptr };

private:
    VersionInfo version_info;
    VersionInfo privacy_version_info;
    static std::string version_display;
    HMSQuery    *hms_query { nullptr };

    boost::thread    m_sync_update_thread;
    std::shared_ptr<int> m_user_sync_token;
    bool             m_is_dark_mode{ false };
    bool             m_adding_script_handler { false };
    bool             m_side_popup_status{false};
    bool             m_show_http_errpr_msgdlg{false};
    wxString         m_info_dialog_content;
    HttpServer       m_http_server;

public:
    HttpServer       m_page_http_server;
    
private:
    bool             m_show_gcode_window{true};
    boost::thread    m_check_network_thread;

    std::shared_ptr<PrintHost> m_connected_host = nullptr;
    std::mutex                 m_cnt_hst_mtx;
    DynamicPrintConfig              m_host_config;
    std::mutex                 m_host_cfg_mtx;

    wxTimer* m_machine_find_timer = nullptr;
    std::shared_ptr<Bonjour> m_machine_find_engine = nullptr;
    const int                m_machine_find_id     = 10086;

  public:
    DynamicPrintConfig*             get_host_config() {
        DynamicPrintConfig* res = nullptr;
        m_host_cfg_mtx.lock();
        res = &m_host_config;
        m_host_cfg_mtx.unlock();

        return res;
    }

    void import_presets();

    void import_flutter_web();

    void reset_machine_find_engine() { m_machine_find_engine = nullptr; }

    void                       set_host_config(const DynamicPrintConfig& config)
    {
        m_host_cfg_mtx.lock();
        m_host_config = config;
        m_host_cfg_mtx.unlock();
    }
    void      get_connect_host(std::shared_ptr<PrintHost>& output);
    void                       set_connect_host(const std::shared_ptr<PrintHost>& intput);
    wxDialog* get_web_device_dialog() { return web_device_dialog; }
    void                       set_web_preprint_dialog(WebPreprintDialog* obj) { web_preprint_dialog = obj; }
    wxDialog*                  get_web_preprint_dialog() { return web_preprint_dialog; }
      //try again when subscription fails
    void            on_start_subscribe_again(std::string dev_id);
    void            check_filaments_in_blacklist(std::string tag_supplier, std::string tag_material, bool& in_blacklist, std::string& action, std::string& info);
    std::string     get_local_models_path();
    bool            OnInit() override;
    int             OnExit() override;
    bool            initialized() const { return m_initialized; }
    inline bool     is_enable_multi_machine() { return this->app_config&& this->app_config->get("enable_multi_machine") == "true"; }

    std::map<std::string, bool> test_url_state;

    //BBS: remove GCodeViewer as seperate APP logic
    explicit GUI_App();
    //explicit GUI_App(EAppMode mode = EAppMode::Editor);
    ~GUI_App() override;

    void                   machine_find();
    void show_message_box(std::string msg) { wxMessageBox(msg); }
    EAppMode get_app_mode() const { return m_app_mode; }
    Slic3r::DeviceManager* getDeviceManager() { return m_device_manager; }
    Slic3r::TaskManager*   getTaskManager() { return m_task_manager; }
    HMSQuery* get_hms_query() { return hms_query; }
    NetworkAgent* getAgent() { return m_agent; }
    bool is_editor() const { return m_app_mode == EAppMode::Editor; }
    bool is_gcode_viewer() const { return m_app_mode == EAppMode::GCodeViewer; }
    bool is_recreating_gui() const { return m_is_recreating_gui; }
    std::string logo_name() const { return is_editor() ? "Snapmaker_Orca" : "Snapmaker_Orca-gcodeviewer"; }
    
    // SoftFever
    bool show_gcode_window() const { return m_show_gcode_window; }
    void toggle_show_gcode_window();

    bool show_3d_navigator() const { return app_config->get_bool("show_3d_navigator"); }
    void toggle_show_3d_navigator() const { app_config->set_bool("show_3d_navigator", !show_3d_navigator()); }

    bool show_outline() const { return app_config->get_bool("show_outline"); }
    void toggle_show_outline() const { app_config->set_bool("show_outline", !show_outline()); }

    wxString get_inf_dialog_contect () {return m_info_dialog_content;};

    std::vector<std::string> split_str(std::string src, std::string separator);
    // To be called after the GUI is fully built up.
    // Process command line parameters cached in this->init_params,
    // load configs, STLs etc.
    void            post_init();
    void            shutdown(bool isRecreate = false);
    // If formatted for github, plaintext with OpenGL extensions enclosed into <details>.
    // Otherwise HTML formatted for the system info dialog.
    static std::string get_gl_info(bool for_github);
    wxGLContext*    init_glcontext(wxGLCanvas& canvas);
    bool            init_opengl();

    void            init_download_path();
#if wxUSE_WEBVIEW_EDGE
    void            init_webview_runtime();
#endif
    static unsigned get_colour_approx_luma(const wxColour& colour);
    static bool     dark_mode();
    const wxColour  get_label_default_clr_system();
    const wxColour  get_label_default_clr_modified();
    void            init_label_colours();
    void            update_label_colours_from_appconfig();
    void            update_publish_status();
    bool            has_model_mall();
    void            update_label_colours();
    // update color mode for window
    void            UpdateDarkUI(wxWindow *window, bool highlited = false, bool just_font = false);
    void            UpdateDarkUIWin(wxWindow* win);
    void            Update_dark_mode_flag();
    // update color mode for whole dialog including all children
    void            UpdateDlgDarkUI(wxDialog* dlg);
    void            UpdateFrameDarkUI(wxFrame* dlg);
    // update color mode for DataViewControl
    void            UpdateDVCDarkUI(wxDataViewCtrl* dvc, bool highlited = false);
    // update color mode for panel including all static texts controls
    void            UpdateAllStaticTextDarkUI(wxWindow* parent);
    void            init_fonts();
	void            update_fonts(const MainFrame *main_frame = nullptr);
    void            set_label_clr_modified(const wxColour& clr);
    void            set_label_clr_sys(const wxColour& clr);
    //update side popup status
    bool            get_side_menu_popup_status();
    void            set_side_menu_popup_status(bool status);
    void            link_to_network_check();
    void            link_to_lan_only_wiki();

    const wxColour& get_label_clr_modified() { return m_color_label_modified; }
    const wxColour& get_label_clr_sys()     { return m_color_label_sys; }
    const wxColour& get_label_clr_default() { return m_color_label_default; }
    const wxColour& get_window_default_clr(){ return m_color_window_default; }

    // BBS
//#ifdef _WIN32
    const wxColour& get_label_highlight_clr()   { return m_color_highlight_label_default; }
    const wxColour& get_highlight_default_clr() { return m_color_highlight_default; }
    const wxColour& get_color_hovered_btn_label() { return m_color_hovered_btn_label; }
    const wxColour& get_color_selected_btn_bg() { return m_color_selected_btn_bg; }
    void            force_colors_update();
#ifdef _MSW_DARK_MODE
    void            force_menu_update();
#endif //_MSW_DARK_MODE
//#endif

    const wxFont&   small_font()            { return m_small_font; }
    const wxFont&   bold_font()             { return m_bold_font; }
    const wxFont&   normal_font()           { return m_normal_font; }
    const wxFont&   code_font()             { return m_code_font; }
    const wxFont&   link_font()             { return m_link_font; }
    int             em_unit() const         { return m_em_unit; }
    bool            tabs_as_menu() const;
    wxSize          get_min_size() const;
    float           toolbar_icon_scale(const bool is_limited = false) const;
    void            set_auto_toolbar_icon_scale(float scale) const;
    void            check_printer_presets();

    void            recreate_GUI(const wxString& message);
    void            system_info();
    void            keyboard_shortcuts();
    void            load_project(wxWindow *parent, wxString& input_file) const;
    void            import_model(wxWindow *parent, wxArrayString& input_files) const;
    void            import_zip(wxWindow* parent, wxString& input_file) const;
    void            load_gcode(wxWindow* parent, wxString& input_file) const;

    wxString transition_tridid(int trid_id);
    void            ShowUserGuide();
    void            ShowDownNetPluginDlg();
    void            ShowUserLogin(bool show = true);
    void            ShowOnlyFilament();
    //BBS
    void            request_login(bool show_user_info = false);
    bool            check_login();
    void            get_login_info();
    bool            is_user_login();

    wxString get_international_url(const wxString& origin_url);

    // SM
    struct SMUserInfo
    {
    public:
        bool is_user_login() { return m_login; }
        void set_user_login(bool login)
        {
            m_login = login;
            notify();
        }

        std::string get_user_name() { return m_login_user_name; }
        void     set_user_name(const std::string& name) { m_login_user_name = name; }

        std::string get_user_token() { return m_login_user_token; }
        void     set_user_token(const std::string& token) { m_login_user_token = token; }

        std::string get_user_icon_url() { return m_login_user_icon_url; }
        void     set_user_icon_url(const std::string& url) { m_login_user_icon_url = url; }

        std::string get_user_id() { return m_login_user_id; }
        void        set_user_id(const std::string& id) { m_login_user_id = id; }

        std::string get_user_account() { return m_login_user_account; }
        void        set_user_account(const std::string& account) { m_login_user_account = account; }

        void clear() {
            m_login_user_name = "";
            m_login_user_token = "";
            m_login_user_icon_url = "";
            m_login_user_id       = "";
            m_login_user_account  = "";
            m_login               = false;
        }

        void notify();
    private:
        std::string m_login_user_name = "";
        std::string m_login_user_token = "";
        std::string m_login_user_icon_url = "";
        std::string m_login_user_id       = "";
        std::string m_login_user_account  = "";
        bool     m_login = false;
    };

    SMUserInfo*     sm_get_userinfo() { return &m_login_userinfo; }
    void            sm_get_login_info();
    void            sm_request_login(bool show_user_info = false);
    void            sm_ShowUserLogin(bool show  =  true);
    void            sm_request_user_logout();
  
    void            request_user_logout();
    int             request_user_unbind(std::string dev_id);
    std::string     handle_web_request(std::string cmd);
    void            request_model_download(wxString url);
    void            download_project(std::string project_id);
    void            request_project_download(std::string project_id);
    void            request_open_project(std::string project_id);
    void            request_remove_project(std::string project_id);
    void            sm_request_remove_project(std::string project_id);

    void            handle_http_error(unsigned int status, std::string body);
    void            on_http_error(wxCommandEvent &evt);
    void            enable_user_preset_folder(bool enable);

    // BBS
    bool            is_studio_active();
    void            reset_to_active();
    bool            m_studio_active = true;
    std::chrono::system_clock::time_point  last_active_point;

    void            check_web_version();
    void            check_preset_version();
    void            check_new_version_sf(bool show_tips = false, bool by_user = false);
    void            process_network_msg(std::string dev_id, std::string msg);
    void            enter_force_upgrade();
    void            set_skip_version(bool skip = true);
    void            no_new_version();
    static std::string format_display_version();
    std::string     format_IP(const std::string& ip);
    void            show_dialog(wxString msg);
    void            push_notification(wxString msg, wxString title = wxEmptyString, UserNotificationStyle style = UserNotificationStyle::UNS_NORMAL);
    void            reload_settings();
    void            remove_user_presets();
    void            sync_preset(Preset* preset);
    void            start_sync_user_preset(bool with_progress_dlg = false);
    void            stop_sync_user_preset();
    void            start_http_server();
    void            stop_http_server();

    // page loading http server
    void            start_page_http_server();
    void            stop_page_http_server();
    void            switch_staff_pick(bool on);
    bool            check_privacy_update();
    
    void            check_track_enable();

    static bool     catch_error(std::function<void()> cb, const std::string& err);

    void            persist_window_geometry(wxTopLevelWindow *window, bool default_maximized = false);
    void            update_ui_from_settings();

    bool            switch_language();
    bool            load_language(wxString language, bool initial);

    Tab*            get_tab(Preset::Type type);
    Tab*            get_plate_tab();
    Tab*            get_model_tab(bool part = false);
    Tab*            get_layer_tab();
    ConfigOptionMode get_mode();
    std::string     get_mode_str();
    void            save_mode(const /*ConfigOptionMode*/int mode) ;
    void            update_mode();
    void            update_internal_development();
    void            show_ip_address_enter_dialog(wxString title = wxEmptyString);
    void            show_ip_address_enter_dialog_handler(wxCommandEvent &evt);
    bool            show_modal_ip_address_enter_dialog(wxString title = wxEmptyString);

    // BBS
    //void            add_config_menu(wxMenuBar *menu);
    //void            add_config_menu(wxMenu* menu);
    bool            has_unsaved_preset_changes() const;
    bool            has_current_preset_changes() const;
    void            update_saved_preset_from_current_preset();
    std::vector<std::pair<unsigned int, std::string>> get_selected_presets() const;
    bool            check_and_save_current_preset_changes(const wxString& caption, const wxString& header, bool remember_choice = true, bool use_dont_save_insted_of_discard = false);
    void            apply_keeped_preset_modifications();
    bool            check_and_keep_current_preset_changes(const wxString& caption, const wxString& header, int action_buttons, bool* postponed_apply_of_keeped_changes = nullptr);
    bool            can_load_project();
    bool            check_print_host_queue();
    bool            checked_tab(Tab* tab);
    //BBS: add preset combox re-active logic
    void            load_current_presets(bool active_preset_combox = false, bool check_printer_presets = true);
    std::vector<std::string> &get_delete_cache_presets();
    std::vector<std::string> get_delete_cache_presets_lock();
    void            delete_preset_from_cloud(std::string setting_id);
    void            preset_deleted_from_cloud(std::string setting_id);

    wxString        filter_string(wxString str);
    wxString        current_language_code() const { return m_wxLocale->GetCanonicalName(); }
	// Translate the language code to a code, for which Prusa Research maintains translations. Defaults to "en_US".
    wxString 		current_language_code_safe() const;
    bool            is_localized() const { return m_wxLocale->GetLocale() != "English"; }

    void            open_preferences(size_t open_on_tab = 0, const std::string& highlight_option = std::string());

    virtual bool OnExceptionInMainLoop() override;
    // Calls wxLaunchDefaultBrowser if user confirms in dialog.
    bool            open_browser_with_warning_dialog(const wxString& url, int flags = 0);
#ifdef __APPLE__
    void            OSXStoreOpenFiles(const wxArrayString &files);
    // wxWidgets override to get an event on open files.
    void            MacOpenFiles(const wxArrayString &fileNames) override;
    void            MacOpenURL(const wxString& url) override;
#endif /* __APPLE */

    Sidebar&             sidebar();
    GizmoObjectManipulation *obj_manipul();
    ObjectSettings*      obj_settings();
    ObjectList*          obj_list();
    ObjectLayers*        obj_layers();
    Plater*              plater();
    const Plater*        plater() const;
    ParamsPanel*         params_panel();
    ParamsDialog*        params_dialog();
    Model&      		 model();
    NotificationManager * notification_manager();
    Downloader*          downloader();


    std::string         m_mall_model_download_url;
    std::string         m_mall_model_download_name;
    ModelMallDialog*    m_mall_publish_dialog{ nullptr };
    PingCodeBindDialog* m_ping_code_binding_dialog{ nullptr };

    NetworkErrorDialog* m_server_error_dialog { nullptr };

    void            set_download_model_url(std::string url) {m_mall_model_download_url = url;}
    void            set_download_model_name(std::string name) {m_mall_model_download_name = name;}
    std::string     get_download_model_url() {return m_mall_model_download_url;}
    std::string     get_download_model_name() {return m_mall_model_download_name;}

#if defined(__WINDOWS__)
    bool            is_running_on_arm64() { return m_is_arm64; }
#endif

    void            load_url(wxString url);
    void            open_mall_page_dialog();
    void            open_publish_page_dialog();
    void            remove_mall_system_dialog();
    void            run_script(wxString js);
    bool            is_adding_script_handler() { return m_adding_script_handler; }
    void            set_adding_script_handler(bool status) { m_adding_script_handler = status; }

    char            from_hex(char ch);
    std::string     url_encode(std::string value);
    std::string     url_decode(std::string value);

    void            popup_ping_bind_dialog();
    void            remove_ping_bind_dialog();

    // Parameters extracted from the command line to be passed to GUI after initialization.
    GUI_InitParams* init_params { nullptr };

    AppConfig*      app_config{ nullptr };
    PresetBundle*   preset_bundle{ nullptr };
    PresetUpdater*  preset_updater{ nullptr };
    MainFrame*      mainframe{ nullptr };
    Plater*         plater_{ nullptr };

	PresetUpdater*  get_preset_updater() { return preset_updater; }

    Notebook*       tab_panel() const ;
    int             extruders_cnt() const;
    int             extruders_edited_cnt() const;

    // BBS
    int             filaments_cnt() const;
    PrintSequence   global_print_sequence() const;

    std::vector<Tab *>      tabs_list;
    std::vector<Tab *>      model_tabs_list;
    Tab*                    plate_tab;

	RemovableDriveManager* removable_drive_manager() { return m_removable_drive_manager.get(); }
	OtherInstanceMessageHandler* other_instance_message_handler() { return m_other_instance_message_handler.get(); }
    wxSingleInstanceChecker* single_instance_checker() {return m_single_instance_checker.get();}

	void        init_single_instance_checker(const std::string &name, const std::string &path);
	void        set_instance_hash (const size_t hash) { m_instance_hash_int = hash; m_instance_hash_string = std::to_string(hash); }
    std::string get_instance_hash_string ()           { return m_instance_hash_string; }
	size_t      get_instance_hash_int ()              { return m_instance_hash_int; }

    ImGuiWrapper* imgui() { return m_imgui.get(); }

    PrintHostJobQueue& printhost_job_queue() { return *m_printhost_job_queue.get(); }

    void            open_web_page_localized(const std::string &http_address);
    bool            may_switch_to_SLA_preset(const wxString& caption);
    bool            run_wizard(ConfigWizard::RunReason reason, ConfigWizard::StartPage start_page = ConfigWizard::SP_WELCOME);
    void            show_desktop_integration_dialog();

#if ENABLE_THUMBNAIL_GENERATOR_DEBUG
    // temporary and debug only -> extract thumbnails from selected gcode and save them as png files
    void            gcode_thumbnails_debug();
#endif // ENABLE_THUMBNAIL_GENERATOR_DEBUG

    OpenGLManager& get_opengl_manager() { return m_opengl_mgr; }
    GLShaderProgram* get_shader(const std::string& shader_name) { return m_opengl_mgr.get_shader(shader_name); }
    GLShaderProgram* get_current_shader() { return m_opengl_mgr.get_current_shader(); }

    bool is_gl_version_greater_or_equal_to(unsigned int major, unsigned int minor) const { return m_opengl_mgr.get_gl_info().is_version_greater_or_equal_to(major, minor); }
    bool is_glsl_version_greater_or_equal_to(unsigned int major, unsigned int minor) const { return m_opengl_mgr.get_gl_info().is_glsl_version_greater_or_equal_to(major, minor); }
    int  GetSingleChoiceIndex(const wxString& message, const wxString& caption, const wxArrayString& choices, int initialSelection);

    // extend is stl/3mf/gcode/step etc 
    void            associate_files(std::wstring extend);
    void            disassociate_files(std::wstring extend);
    bool            check_url_association(std::wstring url_prefix, std::wstring& reg_bin);
    void            associate_url(std::wstring url_prefix);
    void            disassociate_url(std::wstring url_prefix);

    // URL download - PrusaSlicer gets system call to open prusaslicer:// URL which should contain address of download
    void            start_download(std::string url);

    std::string     get_plugin_url(std::string name, std::string country_code);
    int             download_plugin(std::string name, std::string package_name, InstallProgressFn pro_fn = nullptr, WasCancelledFn cancel_fn = nullptr);
    int             install_plugin(std::string name, std::string package_name, InstallProgressFn pro_fn = nullptr, WasCancelledFn cancel_fn = nullptr);
    std::string     get_http_url(std::string country_code, std::string path = {});
    std::string     get_model_http_url(std::string country_code);
    bool            is_compatibility_version();
    bool            check_networking_version();
    void            cancel_networking_install();
    void            restart_networking();
    void            check_config_updates_from_updater(bool updateByuser = false) { check_updates(updateByuser); }

private:
    int             updating_bambu_networking();
    bool            on_init_inner();
    void            copy_network_if_available();
    bool            on_init_network(bool try_backup = false);
    void            init_networking_callbacks();
    void            init_app_config();
    void            remove_old_networking_plugins();
    //BBS set extra header for http request
    std::map<std::string, std::string> get_extra_header();
    void            init_http_extra_header();
    void            update_http_extra_header();
    bool            check_older_app_config(Semver current_version, bool backup);
    void            copy_older_config();
    void                               copy_web_resources();
    void            window_pos_save(wxTopLevelWindow* window, const std::string &name);
    bool            window_pos_restore(wxTopLevelWindow* window, const std::string &name, bool default_maximized = false);
    void            window_pos_sanitize(wxTopLevelWindow* window);
    void            window_pos_center(wxTopLevelWindow *window);
    bool            select_language();

    bool            config_wizard_startup();
	void            check_updates(const bool verbose);

    bool                    m_init_app_config_from_older { false };
    bool                    m_datadir_redefined { false };
    std::string             m_older_data_dir_path;
    boost::optional<Semver> m_last_config_version;
    bool                    m_config_corrupted { false };
    std::string             m_open_method;

    SMUserInfo m_login_userinfo;

public:
    std::unordered_map<void*, std::weak_ptr<SSWCP_Instance>> m_recent_file_subscribers;
    std::unordered_map<void*, std::weak_ptr<SSWCP_Instance>> m_user_login_subscribers;
    std::unordered_map<void*, std::weak_ptr<SSWCP_Instance>> m_device_card_subscribers;
    std::unordered_map<void*, std::weak_ptr<SSWCP_Instance>> m_page_state_subscribers;
    std::unordered_map<void*, std::weak_ptr<SSWCP_Instance>> m_user_update_privacy_subscribers;
    struct CachePairCompare
    {
        bool operator()(const std::pair<void*, std::weak_ptr<SSWCP_Instance>>& lhs,
                        const std::pair<void*, std::weak_ptr<SSWCP_Instance>>& rhs) const
        {
            return lhs.first <= rhs.first;
        }
    };
    std::map<std::pair<void*, std::weak_ptr<SSWCP_Instance>>, std::string, CachePairCompare>                     m_cache_subscribers;

    void recent_file_notify(const json& res);
    void user_login_notify(const json& res);
    void device_card_notify(const json& res);
    void page_state_notify_webview(wxWebView* webview, const std::string& state);
    void cache_notify(const std::string& key, const json& res);
    void user_update_privacy_notify(const bool& res);

public:
    bool sm_disconnect_current_machine(bool need_reload_printerview = true);


public:
    class Fltviews
    {
    private:
        std::unordered_map<wxWebView*, wxString> webviews;
        std::unordered_map<WebViewPanel*, wxString> webview_panels;
        std::unordered_map<PrinterWebView*, std::pair<wxString, wxString>> printerviews;
        GUI_App*                                                           app;

    public:
        Fltviews(GUI_App* app = nullptr) : app(app) {}

        void set_app(GUI_App* app) { this->app = app; }

        void remove_view(wxWebView* view) {
            if (webviews.count(view)) {
                webviews.erase(view);
            }
        }

        void add_view(wxWebView* view, const wxString& url) {
            webviews[view] = url;
        }

        void remove_panel(WebViewPanel* panel)
        {
            if (webview_panels.count(panel)) {
                webview_panels.erase(panel);
            }
        }

        void add_webview_panel(WebViewPanel* panel, const wxString& url) {
            webview_panels[panel] = url;
        }

        void add_printer_view(PrinterWebView* prview, const wxString& url, const wxString& key)
        {
            printerviews[prview] = std::make_pair(url, key);
        }

        void remove_printer_view(PrinterWebView* prview)
        {
            if (printerviews.count(prview)) {
                printerviews.erase(prview);
            }
        }

        void relead_all() {
            for (const auto& view : webviews) {
                auto ptr = view.first;
                wxString new_url = app->get_international_url(view.second);
                ptr->LoadURL(new_url);
            }

            for (const auto& panel : webview_panels) {
                auto ptr = panel.first;
                wxString new_url = app->get_international_url(panel.second);
                ptr->load_url(new_url);
            }

            for (const auto& prview : printerviews) {
                auto ptr = prview.first;
                wxString new_url = app->get_international_url(prview.second.first);
                ptr->load_url(new_url, prview.second.second);
            }
        }
    };

public:
    Fltviews&       fltviews() { return m_fltviews; }

private:
    Fltviews m_fltviews;
};

DECLARE_APP(GUI_App)
wxDECLARE_EVENT(EVT_CONNECT_LAN_MODE_PRINT, wxCommandEvent);

bool is_support_filament(int extruder_id);
} // namespace GUI
} // Slic3r

#endif // slic3r_GUI_App_hpp_
