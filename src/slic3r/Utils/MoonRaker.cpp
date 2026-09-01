// Implementation of Moonraker printer host communication
#include "MoonRaker.hpp"
#include "MQTT.hpp"

#include <algorithm>
#include <sstream>
#include <exception>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/nowide/convert.hpp>
#include <curl/curl.h>

#include <wx/progdlg.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/format.hpp"
#include "Http.hpp"
#include "libslic3r/AppConfig.hpp"
#include "Bonjour.hpp"
#include "slic3r/GUI/BonjourDialog.hpp"
#include "slic3r/GUI/WebPreprintDialog.hpp"
#include "slic3r/GUI/SSWCP.hpp"

#include "sentry_wrapper/SentryWrapper.hpp"
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
#endif

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;


namespace Slic3r {

namespace {

#ifdef WIN32
// Helper function to extract host and port from URL
std::string get_host_from_url(const std::string& url_in)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
        
    wcp_loger.add_log("getting machine info from URL: " + url_in, false, "", "Moonraker_Mqtt", "info");
    
    std::string url = url_in;
    // Add http:// if there is no scheme
    size_t double_slash = url.find("//");
    if (double_slash == std::string::npos) {
        url = "http://" + url;        
        wcp_loger.add_log("added default protocol prefix, updated URL: " + url, false, "", "Moonraker_Mqtt", "info");
    }
    
    std::string out  = url;
    CURLU*      hurl = curl_url();
    if (hurl) {
        // Parse the input URL
        CURLUcode rc = curl_url_set(hurl, CURLUPART_URL, url.c_str(), 0);
        if (rc == CURLUE_OK) {
            // Extract host and port
            char* host;
            rc = curl_url_get(hurl, CURLUPART_HOST, &host, 0);
            if (rc == CURLUE_OK) {
                char* port;
                rc = curl_url_get(hurl, CURLUPART_PORT, &port, 0);
                if (rc == CURLUE_OK && port != nullptr) {
                    out = std::string(host) + ":" + port;                    
                    wcp_loger.add_log("extracted host and port: " + out, false, "", "Moonraker_Mqtt", "info");
                    curl_free(port);
                } else {
                    out = host;                    
                    wcp_loger.add_log("extracted host (no port found): " + out, false, "", "Moonraker_Mqtt", "info");
                    curl_free(host);
                }
            } else {
                BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to get host from URL: " << url;
                wcp_loger.add_log("failed to get host from URL: " + url, false, "", "Moonraker_Mqtt", "error");
            }
                
        } else {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to parse URL: " << url;
            wcp_loger.add_log("failed to parse URL: " + url, false, "", "Moonraker_Mqtt", "error");
        }
            
        curl_url_cleanup(hurl);
    } else {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to allocate curl_url handle";
        wcp_loger.add_log("failed to allocate curl_url handle", false, "", "Moonraker_Mqtt", "error");
    }
              
    return out;
}

// Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
std::string substitute_host(const std::string& orig_addr, std::string sub_addr)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();    
    
    // put ipv6 into [] brackets
    if (sub_addr.find(':') != std::string::npos && sub_addr.at(0) != '[') {
        sub_addr = "[" + sub_addr + "]";
        BOOST_LOG_TRIVIAL(debug) << "[Moonraker_Mqtt] detected IPv6 address, wrapping in brackets: " << sub_addr;
        wcp_loger.add_log("detected IPv6 address, wrapping in brackets: " + sub_addr, false, "", "Moonraker_Mqtt", "info");
    }

#if 0
    //URI = scheme ":"["//"[userinfo "@"] host [":" port]] path["?" query]["#" fragment]
    std::string final_addr = orig_addr;
    //  http
    size_t double_dash = orig_addr.find("//");
    size_t host_start = (double_dash == std::string::npos ? 0 : double_dash + 2);
    // userinfo
    size_t at = orig_addr.find("@");
    host_start = (at != std::string::npos && at > host_start ? at + 1 : host_start);
    // end of host, could be port(:), subpath(/) (could be query(?) or fragment(#)?)
    // or it will be ']' if address is ipv6 )
    size_t potencial_host_end = orig_addr.find_first_of(":/", host_start); 
    // if there are more ':' it must be ipv6
    if (potencial_host_end != std::string::npos && orig_addr[potencial_host_end] == ':' && orig_addr.rfind(':') != potencial_host_end) {
        size_t ipv6_end = orig_addr.find(']', host_start);
        // DK: Uncomment and replace orig_addr.length() if we want to allow subpath after ipv6 without [] parentheses.
        potencial_host_end = (ipv6_end != std::string::npos ? ipv6_end + 1 : orig_addr.length()); //orig_addr.find('/', host_start));
    }
    size_t host_end = (potencial_host_end != std::string::npos ? potencial_host_end : orig_addr.length());
    // now host_start and host_end should mark where to put resolved addr
    // check host_start. if its nonsense, lets just use original addr (or  resolved addr?)
    if (host_start >= orig_addr.length()) {
        return final_addr;
    }
    final_addr.replace(host_start, host_end - host_start, sub_addr);
    return final_addr;
#else
    // Using the new CURL API for handling URL. https://everything.curl.dev/libcurl/url
    // If anything fails, return the input unchanged.
    std::string out  = orig_addr;
    CURLU*      hurl = curl_url();
    if (hurl) {
        // Parse the input URL.
        CURLUcode rc = curl_url_set(hurl, CURLUPART_URL, orig_addr.c_str(), 0);
        if (rc == CURLUE_OK) {
            // Replace the address.
            rc = curl_url_set(hurl, CURLUPART_HOST, sub_addr.c_str(), 0);
            if (rc == CURLUE_OK) {
                // Extract a string fromt the CURL URL handle.
                char* url;
                rc = curl_url_get(hurl, CURLUPART_URL, &url, 0);
                if (rc == CURLUE_OK) {
                    out = url;                    
                    curl_free(url);
                } else {
                    BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to extract URL after host replacement";
                    wcp_loger.add_log("failed to extract URL after host replacement", false, "", "Moonraker_Mqtt", "error");
                }
            } else {
                BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] host replacement failed: " << sub_addr << ", URL: " << orig_addr;
                wcp_loger.add_log("host replacement failed: " + sub_addr, false, "", "Moonraker_Mqtt", "error");
            }
                
        } else {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to parse original URL: " << orig_addr;
            wcp_loger.add_log("failed to parse original URL: " + orig_addr, false, "", "Moonraker_Mqtt", "error");
        }
            
        curl_url_cleanup(hurl);
    } else{
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] assign curl_url failed";
        wcp_loger.add_log("assign curl_url failed", false, "", "Moonraker_Mqtt", "error");
    }
        
    return out;
#endif
}
#endif // WIN32

// Helper function to URL encode a string
std::string escape_string(const std::string& unescaped)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();    

    std::string ret_val;
    CURL*       curl = curl_easy_init();
    if (curl) {
        char* decoded = curl_easy_escape(curl, unescaped.c_str(), unescaped.size());
        if (decoded) {
            ret_val = std::string(decoded);
            BOOST_LOG_TRIVIAL(debug) << "[Moonraker_Mqtt] URL encoded successfully";
            wcp_loger.add_log("URL encoded successfully", false, "", "Moonraker_Mqtt", "info");
            curl_free(decoded);
        } else {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] URL encoding failed";
            wcp_loger.add_log("URL encoding failed", false, "", "Moonraker_Mqtt", "error");
        }
        curl_easy_cleanup(curl);
    } else {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] CURL initialization failed";
        wcp_loger.add_log("CURL initialization failed", false, "", "Moonraker_Mqtt", "error");
    }
    return ret_val;
}

// Helper function to URL encode each element of a filesystem path
std::string escape_path_by_element(const boost::filesystem::path& path)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    
    std::string             ret_val = escape_string(path.filename().string());
    boost::filesystem::path parent(path.parent_path());
    while (!parent.empty() &&
           parent.string() != "/") // "/" check is for case "/file.gcode" was inserted. Then boost takes "/" as parent_path.
    {
        ret_val = escape_string(parent.filename().string()) + "/" + ret_val;
        parent  = parent.parent_path();
    }
    
    return ret_val;
}
} // namespace

Moonraker::Moonraker(DynamicPrintConfig* config)
    : m_host(config->opt_string("print_host"))
    , m_apikey(config->opt_string("printhost_apikey"))
    , m_cafile(config->opt_string("printhost_cafile"))
    , m_ssl_revoke_best_effort(config->opt_bool("printhost_ssl_ignore_revoke"))
    , time_sync_manager_(std::make_shared<TimeSyncManager>())
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
}

// Return the name of this print host type
const char* Moonraker::get_name() const { return "Moonraker"; }

#ifdef WIN32
bool Moonraker::test_with_resolved_ip(wxString& msg) const
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    
    // Since the request is performed synchronously here,
    // it is ok to refer to `msg` from within the closure
    const char* name = get_name();
    bool        res  = true;
    // Msg contains ip string.
    auto url = substitute_host(make_url("api/version"), GUI::into_u8(msg));
    msg.Clear();

    wcp_loger.add_log(name + std::string(": fetching version info, URL: ") + url, false, "", "Moonraker_Mqtt", "info");

    std::string host = get_host_from_url(m_host);
    auto        http = Http::get(url); // std::move(url));
    // "Host" header is necessary here. We have resolved IP address and subsituted it into "url" variable.
    // And when creating Http object above, libcurl automatically includes "Host" header from address it got.
    // Thus "Host" is set to the resolved IP instead of host filled by user. We need to change it back.
    // Not changing the host would work on the most cases (where there is 1 service on 1 hostname) but would break when f.e. reverse proxy
    // is used (issue #9734). Also when allow_ip_resolve = 0, this is not needed, but it should not break anything if it stays.
    // https://www.rfc-editor.org/rfc/rfc7230#section-5.4
    http.header("Host", host);
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] " << boost::format("%1%: failed to fetch version info, URL: %2%, error: %3%, HTTP status: %4%, response body: `%5%`") % name % url %
                                            error % status % body;
            wcp_loger.add_log(std::string(name) + ": failed to fetch version info, URL: " + url + ", error: " + error + ", HTTP status: " + std::to_string(status) +
                                  ", response body: " + body,
                              false, "", "Moonraker_Mqtt", "error");
            res = false;
            msg = format_error(body, error, status);
        })
        .on_complete([&, this](std::string body, unsigned) {
            
            wcp_loger.add_log(std::string(name) + ": fetched version info successfully: " + body, false, "", "Moonraker_Mqtt", "info");

            {
                std::stringstream ss(body);
                pt::ptree         ptree;
                pt::read_json(ss, ptree);

                if (!ptree.get_optional<std::string>("api")) {
                    BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] API field not found in response";
                    wcp_loger.add_log("API field not found in response", false, "", "Moonraker_Mqtt", "error");
                    res = false;
                    return;
                }

                const auto text = ptree.get_optional<std::string>("text");
                res             = validate_version_text(text);
                if (!res) {
                    BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] version text validation failed";
                    wcp_loger.add_log("version text validation failed", false, "", "Moonraker_Mqtt", "error");
                    msg = GUI::format_wxstr(_L("Mismatched type of print host: %s"), (text ? *text : name));
                } else {
                    BOOST_LOG_TRIVIAL(debug) << "[Moonraker_Mqtt] version validation succeeded";
                    wcp_loger.add_log("version validation succeeded", false, "", "Moonraker_Mqtt", "info");
                }
            } 
        })
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .perform_sync();
    
    wcp_loger.add_log("IP test completed, result: " + std::string(res ? "success" : "failed"), false, "", "Moonraker_Mqtt", "info");
    return res;
}
#endif // WIN32

bool Moonraker::get_machine_info(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets, json& response) {
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    
    bool res = true;
    auto url = make_url("printer/objects/query");
    auto http = Http::post(std::move(url));

    for (const auto pair : targets) {
        std::string value = "";
        for (size_t i = 0; i < pair.second.size(); ++i) {
            if (i != 0) {
                value += ",";
            }
            value += pair.second[i];
        }
        http.form_add(pair.first, value);        
        wcp_loger.add_log("adding query parameter: " + pair.first + " = " + value, false, "", "Moonraker_Mqtt", "info");
    }

    http.on_error([&](std::string body, std::string error, unsigned status) {

            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to get machine info, error: " << error << ", HTTP status: " << status;
            wcp_loger.add_log("failed to get machine info, error: " + error + ", HTTP status: " + std::to_string(status), false, "", "Moonraker_Mqtt", "error");
            res = false;
            try{
                response = json::parse(body);
            } catch (std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] analysis error: " << e.what();
            }
        })
        .on_complete([&](std::string body, unsigned) {
        
            wcp_loger.add_log("got machine info successfully", false, "", "Moonraker_Mqtt", "info");
            try {
                response = json::parse(body);
            } catch (std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] analysis machine response: " << e.what();
            }
        })
        .perform_sync();

    return res;
}

bool Moonraker::send_gcodes(const std::vector<std::string>& codes, std::string& extraInfo)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();    
    wcp_loger.add_log("Starting to send G-code, count: " + std::to_string(codes.size()), false, "", "Moonraker_Mqtt", "info");
    
    bool res = true;
    std::string param = "?script=";
    for (const auto code : codes) {
        param += "\n";
        param += code;        
        wcp_loger.add_log("adding G-code: " + code, false, "", "Moonraker_Mqtt", "info");
    }
    auto url = make_url("printer/gcode/script");
    auto http = Http::post(std::move(url));

    http.form_add("script", param)
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send G-code, error: " << error << ", HTTP status: " << status;
            wcp_loger.add_log("failed to send G-code, error: " + error + ", HTTP status: " + std::to_string(status), false, "", "Moonraker_Mqtt", "error");
            res = false;    
        })
        .on_complete([&](std::string body, unsigned){            
            wcp_loger.add_log("G-code sent successfully", false, "", "Moonraker_Mqtt", "info");
            res = true;
        })
        .perform_sync();

    return res;
}

bool Moonraker::test(wxString& msg) const
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();    
    wcp_loger.add_log("Starting connection test", false, "", "Moonraker_Mqtt", "info");    
    
    // Since the request is performed synchronously here,
    // it is ok to refer to `msg` from within the closure
    const char* name = get_name();

    bool res = true;
    auto url = make_url("printer/info");
    
    wcp_loger.add_log(name + std::string(": fetching version info, URL: ") + url, false, "", "Moonraker_Mqtt", "info");
    // Here we do not have to add custom "Host" header - the url contains host filled by user and libCurl will set the header by itself.
    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] " << boost::format("%1%: failed to fetch version info: %2%, HTTP status: %3%, response body: `%4%`") % name % error % status %
                                            body;
            wcp_loger.add_log(name + std::string(": failed to fetch version info: ") + error + ", HTTP status: " + std::to_string(status) + ", response body: " + body, false, "", "Moonraker_Mqtt", "error");
            res = false;
            msg = format_error(body, error, status);
        })
        .on_complete([&, this](std::string body, unsigned) {            
            wcp_loger.add_log(name + std::string(": fetched version info successfully: ") + body, false, "", "Moonraker_Mqtt", "info");

        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .on_ip_resolve([&](std::string address) {
            // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
            // Remember resolved address to be reused at successive REST API call.            
            wcp_loger.add_log("IP resolved successfully: " + address, false, "", "Moonraker_Mqtt", "info");
            msg = GUI::from_u8(address);
        })
#endif // WIN32
        .perform_sync();
    
    wcp_loger.add_log("Connection test completed, result: " + std::string((res ? "success" : "failed")), false, "", "Moonraker_Mqtt", "info");
    return res;
}

// Return success message for connection test
wxString Moonraker::get_test_ok_msg() const { return _(L("Connection to Moonraker works correctly.")); }

// Return formatted error message for failed connection test
wxString Moonraker::get_test_failed_msg(wxString& msg) const
{
    return GUI::format_wxstr("%s: %s\n\n%s", _L("Could not connect to Moonraker"), msg,
                             _L("Note: Moonraker version at least 1.1.0 is required."));
}

// Upload a file to the printer
bool Moonraker::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();    
    wcp_loger.add_log("Starting file upload, source: " + upload_data.source_path.string() + ", destination: " + upload_data.upload_path.string(), false, "", "Moonraker_Mqtt", "info");
    
    if (upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
        
        wcp_loger.add_log("Start print after upload required, showing preprint dialog", false, "", "Moonraker_Mqtt", "info");
        // Create promise and future for thread synchronization
        std::promise<bool> promise;
        auto               future = promise.get_future();
        
        wxTheApp->CallAfter([this, &promise, upload_data, &wcp_loger]() {
            GUI::WebPreprintDialog dialog;

            dialog.set_gcode_file_name(upload_data.source_path.string());
            dialog.set_display_file_name(upload_data.upload_path.string());
            bool res = dialog.run();
            
            wcp_loger.add_log("Preprint dialog result: " + std::string((res ? "confirmed" : "cancelled")), false, "", "Moonraker_Mqtt", "info");            
            promise.set_value(res);
        });
        
        bool flag = future.get();

        if (!flag) {

            wcp_loger.add_log("User cancelled print, aborting upload", false, "", "Moonraker_Mqtt", "info");
            Http::Progress progress(0, 0, 0, 0, "");
            bool           cancel = true;
            prorgess_fn(progress, cancel);
            return false;
        }
    }

#ifndef WIN32    
    wcp_loger.add_log("Uploading via hostname (non-Windows)", false, "", "Moonraker_Mqtt", "info");
    return upload_inner_with_host(std::move(upload_data), prorgess_fn, error_fn, info_fn);
#else    
    wcp_loger.add_log("Windows platform, starting IP resolution flow", false, "", "Moonraker_Mqtt", "info");
    std::string host = get_host_from_url(m_host);

    // decide what to do based on m_host - resolve hostname or upload to ip
    std::vector<boost::asio::ip::address> resolved_addr;
    boost::system::error_code             ec;
    boost::asio::ip::address              host_ip = boost::asio::ip::make_address(host, ec);
    if (!ec) {        
        wcp_loger.add_log("Host is already an IP address: " + host_ip.to_string(), false, "", "Moonraker_Mqtt", "info");
        resolved_addr.push_back(host_ip);
    } else if (GUI::get_app_config()->get_bool("allow_ip_resolve") && boost::algorithm::ends_with(host, ".local")) {
        
        wcp_loger.add_log("Resolving .local hostname: " + host, false, "", "Moonraker_Mqtt", "info");
        Bonjour("Moonraker")
            .set_hostname(host)
            .set_retries(5) // number of rounds of queries send
            .set_timeout(1) // after each timeout, if there is any answer, the resolving will stop
            .on_resolve([&ra = resolved_addr, &wcp_loger](const std::vector<BonjourReply>& replies) {
                for (const auto& rpl : replies) {
                    boost::asio::ip::address ip(rpl.ip);
                    ra.emplace_back(ip);                    
                    wcp_loger.add_log("Resolved IP address: " + rpl.ip.to_string(), false, "", "Moonraker_Mqtt", "info");
                }
            })
            .resolve_sync();
    }

    // Handle different resolution scenarios
    if (resolved_addr.empty()) {
        // no resolved addresses - try system resolving        
        wcp_loger.add_log("Failed to resolve hostname " + m_host + " to IP, using system resolver for upload", false, "", "Moonraker_Mqtt", "error");
        return upload_inner_with_host(std::move(upload_data), prorgess_fn, error_fn, info_fn);
    } else if (resolved_addr.size() == 1) {
        // one address resolved - upload there        
        wcp_loger.add_log("Resolved single address, starting upload: " + resolved_addr.front().to_string(), false, "", "Moonraker_Mqtt", "info");
        return upload_inner_with_resolved_ip(std::move(upload_data), prorgess_fn, error_fn, info_fn, resolved_addr.front());
    } else if (resolved_addr.size() == 2 && resolved_addr[0].is_v4() != resolved_addr[1].is_v4()) {
        // there are just 2 addresses and 1 is ip_v4 and other is ip_v6
        // try sending to both. (Then if both fail, show both error msg after second try)        
        wcp_loger.add_log("Resolved IPv4 and IPv6 addresses, trying upload sequentially", false, "", "Moonraker_Mqtt", "info");
        wxString error_message;
        if (!upload_inner_with_resolved_ip(
                std::move(upload_data), prorgess_fn,
                [&msg = error_message, resolved_addr, &wcp_loger](wxString error) {                     
                    wcp_loger.add_log("First IP upload failed: " + resolved_addr.front().to_string(), false, "", "Moonraker_Mqtt", "warning");
                    msg = GUI::format_wxstr("%1%: %2%", resolved_addr.front(), error); 
                },
                info_fn, resolved_addr.front()) &&
            !upload_inner_with_resolved_ip(
                std::move(upload_data), prorgess_fn,
                [&msg = error_message, resolved_addr, &wcp_loger](wxString error) {                    
                    wcp_loger.add_log("Second IP upload failed: " + resolved_addr.back().to_string(), false, "", "Moonraker_Mqtt", "warning");
                    msg += GUI::format_wxstr("\n%1%: %2%", resolved_addr.back(), error);
                },
                info_fn, resolved_addr.back())) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] upload failed on all IP addresses";
            wcp_loger.add_log("upload failed on all IP addresses", false, "", "Moonraker_Mqtt", "error");
            error_fn(error_message);
            return false;
        }        
        wcp_loger.add_log("Upload succeeded on at least one IP address", false, "", "Moonraker_Mqtt", "info");
        return true;
    } else {

        // There are multiple addresses - user needs to choose which to use.        
        wcp_loger.add_log("Resolved multiple addresses (" + std::to_string(resolved_addr.size()) + "), user selection required", false, "", "Moonraker_Mqtt", "info");
        size_t       selected_index = resolved_addr.size();
        IPListDialog dialog(nullptr, boost::nowide::widen(m_host), resolved_addr, selected_index);
        if (dialog.ShowModal() == wxID_OK && selected_index < resolved_addr.size()) {            
            wcp_loger.add_log("User selected IP address: " + resolved_addr[selected_index].to_string(), false, "", "Moonraker_Mqtt", "info");
            return upload_inner_with_resolved_ip(std::move(upload_data), prorgess_fn, error_fn, info_fn, resolved_addr[selected_index]);
        } else {            
            wcp_loger.add_log("User cancelled IP selection", false, "", "Moonraker_Mqtt", "info");
        }
    }
    return false;
#endif // WIN32
}

#ifdef WIN32
// Upload file using resolved IP address
bool Moonraker::upload_inner_with_resolved_ip(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn, const boost::asio::ip::address& resolved_addr) const
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();    
    wcp_loger.add_log("Uploading via resolved IP address: " + resolved_addr.to_string(), false, "", "Moonraker_Mqtt", "info");
    info_fn(L"resolve", boost::nowide::widen(resolved_addr.to_string()));

    // If test fails, test_msg_or_host_ip contains the error message.
    // Otherwise on Windows it contains the resolved IP address of the host.
    // Test_msg already contains resolved ip and will be cleared on start of test().
    wxString test_msg_or_host_ip = GUI::from_u8(resolved_addr.to_string());
    if (/* !test_with_resolved_ip(test_msg_or_host_ip)*/ false) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] IP test failed";
        wcp_loger.add_log("IP test failed", false, "", "Moonraker_Mqtt", "error");
        error_fn(std::move(test_msg_or_host_ip));
        return false;
    }

    const char* name               = get_name();
    const auto  upload_filename    = upload_data.upload_path.filename();
    const auto  upload_parent_path = upload_data.upload_path.parent_path();
    std::string url                = substitute_host(make_url("server/files/upload"), resolved_addr.to_string());
    bool        result             = true;

    info_fn(L"resolve", boost::nowide::widen(url));    
    wcp_loger.add_log(name + std::string(": uploading file ") + upload_data.source_path.string() + " to " + url + ", filename: " + upload_filename.string() + ", path: " + upload_parent_path.string() + ", print: " + (upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "yes" : "no"), false, "", "Moonraker_Mqtt", "info");

    std::string host = get_host_from_url(m_host);
    auto        http = Http::post(url); // std::move(url));
    // "Host" header is necessary here. We have resolved IP address and subsituted it into "url" variable.
    // And when creating Http object above, libcurl automatically includes "Host" header from address it got.
    // Thus "Host" is set to the resolved IP instead of host filled by user. We need to change it back.
    // Not changing the host would work on the most cases (where there is 1 service on 1 hostname) but would break when f.e. reverse proxy
    // is used (issue #9734). https://www.rfc-editor.org/rfc/rfc7230#section-5.4
    http.header("Host", host);
    set_auth(http);
    http.form_add("print", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
        .form_add("path", upload_parent_path.string()) // XXX: slashes on windows ???
        .form_add_file("file", upload_data.source_path.string(), upload_filename.string())

        .on_complete([&](std::string body, unsigned status) {            
            wcp_loger.add_log(name + std::string(": file uploaded successfully: HTTP ") + std::to_string(status) + ": " + body, false, "", "Moonraker_Mqtt", "info");
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] " << boost::format("%1%: failed to upload file to %2%: %3%, HTTP %4%, response body: `%5%`") % name % url % error %
                                            status % body;
            wcp_loger.add_log(name + std::string(": failed to upload file to ") + url + ": " + error + ", HTTP " + std::to_string(status) + ", response body: `" + body + "`", false, "", "Moonraker_Mqtt", "error");
            // fallback to port 8080
            wcp_loger.add_log("Retrying upload on port 8080", false, "", "Moonraker_Mqtt", "info");
            url = substitute_host(make_url_8080("server/files/upload"), GUI::into_u8(test_msg_or_host_ip));

            auto http_8080 = Http::post(std::move(url));
            http_8080.header("host", host);
            set_auth(http_8080);

            http_8080.form_add("print", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
                .form_add("path", upload_parent_path.string()) // XXX: slashes on windows ???
                .form_add_file("file", upload_data.source_path.string(), upload_filename.string())
                .on_complete([&](std::string body, unsigned status) {                    
                    wcp_loger.add_log(name + std::string(": file uploaded successfully on port 8080: HTTP ") + std::to_string(status) + ": " + body, false, "", "Moonraker_Mqtt", "info");
                })
                .on_error([&](std::string body, std::string error, unsigned status) {
                    BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] " << boost::format("%1%: port 8080 upload failed: %2%, HTTP %3%, response body: `%4%`") % name % error %
                                                    status % body;
                    wcp_loger.add_log(name + std::string(": port 8080 upload failed: ") + error + ", HTTP " + std::to_string(status) + ", response body: `" + body + "`", false, "", "Moonraker_Mqtt", "error");
                    error_fn(format_error(body, error, status));
                    result = false;
                })
                .on_progress([&](Http::Progress progress, bool& cancel) {
                    prorgess_fn(std::move(progress), cancel);
                    if (cancel) {
                        // Upload was canceled                        
                        wcp_loger.add_log(name + std::string(": port 8080 upload cancelled"), false, "", "Moonraker_Mqtt", "info");
                        result = false;
                    }
                })
#ifdef WIN32
                .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
                .perform_sync();
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                // Upload was canceled                
                wcp_loger.add_log(name + std::string(": upload cancelled"), false, "", "Moonraker_Mqtt", "info");
                result = false;
            }
        })
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .perform_sync();
    
    wcp_loger.add_log("IP address upload completed, result: " + std::string((result ? "success" : "failed")), false, "", "Moonraker_Mqtt", "info");
    return result;
}
#endif // WIN32

// Upload file using hostname
bool Moonraker::upload_inner_with_host(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();    
    wcp_loger.add_log("Uploading file via hostname", false, "", "Moonraker_Mqtt", "info");
    const char* name = get_name();
    const auto upload_filename    = upload_data.upload_path.filename();
    const auto upload_parent_path = upload_data.upload_path.parent_path();
    wxString    test_msg_or_host_ip = "";   
    std::string url;
    bool        res = true;

#ifdef WIN32
    // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
    if (m_host.find("https://") == 0 || test_msg_or_host_ip.empty() || !GUI::get_app_config()->get_bool("allow_ip_resolve"))
#endif // _WIN32
    {
        // If https is entered we assume signed ceritificate is being used
        // IP resolving will not happen - it could resolve into address not being specified in cert        
        wcp_loger.add_log("Using HTTPS or IP resolve disabled, uploading via hostname directly", false, "", "Moonraker_Mqtt", "info");
        url = make_url("server/files/upload");
    }
#ifdef WIN32
    else {
        // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
        // Curl uses easy_getinfo to get ip address of last successful transaction.
        // If it got the address use it instead of the stored in "host" variable.
        // This new address returns in "test_msg_or_host_ip" variable.
        // Solves troubles of uploades failing with name address.
        // in original address (m_host) replace host for resolved ip
        
        wcp_loger.add_log("Using IP resolve workaround", false, "", "Moonraker_Mqtt", "info");
        info_fn(L"resolve", test_msg_or_host_ip);
        url = substitute_host(make_url("server/files/upload"), GUI::into_u8(test_msg_or_host_ip));        
        wcp_loger.add_log("Upload URL after IP resolve: " + url, false, "", "Moonraker_Mqtt", "info");
    }
#endif // _WIN32
    
    wcp_loger.add_log(name + std::string(": uploading file ") + upload_data.source_path.string() + " to " + url + ", filename: " + upload_filename.string() + ", path: " + upload_parent_path.string() + ", print: " + (upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "yes" : "no"), false, "", "Moonraker_Mqtt", "info");
    auto http = Http::post(std::move(url));

#ifdef WIN32
    // "Host" header is necessary here. In the workaround above (two mDNS..) we have got IP address from test connection and subsituted it
    // into "url" variable. And when creating Http object above, libcurl automatically includes "Host" header from address it got. Thus "Host"
    // is set to the resolved IP instead of host filled by user. We need to change it back. Not changing the host would work on the most cases
    // (where there is 1 service on 1 hostname) but would break when f.e. reverse proxy is used (issue #9734). Also when allow_ip_resolve =
    // 0, this is not needed, but it should not break anything if it stays. https://www.rfc-editor.org/rfc/rfc7230#section-5.4
    std::string host = get_host_from_url(m_host);
    http.header("Host", host);    
    wcp_loger.add_log("Setting Host header: " + host, false, "", "Moonraker_Mqtt", "info");
#endif // _WIN32
    set_auth(http);
    http.form_add("print", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
        .form_add("path", upload_parent_path.string()) // XXX: slashes on windows ???
        .form_add_file("file", upload_data.source_path.string(), upload_filename.string())
        .on_complete([&](std::string body, unsigned status) {            
            wcp_loger.add_log(name + std::string(": file uploaded successfully: HTTP ") + std::to_string(status) + ": " + body, false, "", "Moonraker_Mqtt", "info");
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] " << boost::format("%1%: failed to upload file: %2%, HTTP %3%, response body: `%4%`") % name % error % status %
                                            body;
            wcp_loger.add_log(name + std::string(": failed to upload file: ") + error + ", HTTP " + std::to_string(status) + ", response body: `" + body + "`", false, "", "Moonraker_Mqtt", "error");
            // fallback to port 8080

            wcp_loger.add_log("Retrying upload on port 8080", false, "", "Moonraker_Mqtt", "info");
            url = make_url_8080("server/files/upload");            
            auto http_8080 = Http::post(std::move(url));
            set_auth(http_8080);

            http_8080.form_add("print", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
                .form_add("path", upload_parent_path.string()) // XXX: slashes on windows ???
                .form_add_file("file", upload_data.source_path.string(), upload_filename.string())
                .on_complete([&](std::string body, unsigned status) {                    
                    wcp_loger.add_log(name + std::string(": file uploaded successfully on port 8080: HTTP ") + std::to_string(status) + ": " + body, false, "", "Moonraker_Mqtt", "info");
                })
                .on_error([&](std::string body, std::string error, unsigned status) {
                    BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] " << boost::format("%1%: port 8080 upload failed: %2%, HTTP %3%, response body: `%4%`") % name % error %
                                                    status % body;
                    wcp_loger.add_log(name + std::string(": port 8080 upload failed: ") + error + ", HTTP " + std::to_string(status) + ", response body: `" + body + "`", false, "", "Moonraker_Mqtt", "error");
                    error_fn(format_error(body, error, status));
                    res = false;
                })
                .on_progress([&](Http::Progress progress, bool& cancel) {
                    prorgess_fn(std::move(progress), cancel);
                    if (cancel) {
                        // Upload was canceled                        
                        wcp_loger.add_log(name + std::string(": port 8080 upload cancelled"), false, "", "Moonraker_Mqtt", "info");
                        res = false;
                    }
                })
#ifdef WIN32
                .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
                .perform_sync();
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                // Upload was canceled                
                wcp_loger.add_log(name + std::string(": upload cancelled"), false, "", "Moonraker_Mqtt", "info");
                res = false;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();
    
    wcp_loger.add_log("Hostname upload completed, result: " + std::string((res ? "success" : "failed")), false, "", "Moonraker_Mqtt", "info");
    return res;
}

// Validate version text to confirm printer type
bool Moonraker::validate_version_text(const boost::optional<std::string>& version_text) const
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    bool result = version_text ? boost::starts_with(*version_text, "Moonraker") : true;    
    wcp_loger.add_log("Version text validation: " + (version_text ? *version_text : "empty") + ", result: " + (result ? "passed" : "failed"), false, "", "Moonraker_Mqtt", "info");
    return result;
}

// Set authentication headers for HTTP requests
void Moonraker::set_auth(Http& http) const
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();    
    wcp_loger.add_log("Setting auth header, API key length: " + std::to_string(m_apikey.length()), false, "", "Moonraker_Mqtt", "info");
    http.header("X-Api-Key", m_apikey);

    if (!m_cafile.empty()) {        
        wcp_loger.add_log("Setting CA file: " + m_cafile, false, "", "Moonraker_Mqtt", "info");
        http.ca_file(m_cafile);
    }
}

std::string Moonraker::make_url_8080(const std::string& path) const
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();    
    wcp_loger.add_log("Building port 8080 URL, path: " + path, false, "", "Moonraker_Mqtt", "info");
    size_t pos = m_host.find(":");
    std::string target_host = m_host;
    if (pos != std::string::npos && pos != std::string("http:").length() - 1 && std::string("https:").length() - 1) {
        target_host = target_host.substr(0, pos);        
        wcp_loger.add_log("Removed port number, target host: " + target_host, false, "", "Moonraker_Mqtt", "info");
    }
    
    std::string result;
    if (target_host.find("http://") == 0 || target_host.find("https://") == 0) {
        if (target_host.back() == '/') {
            target_host = target_host.substr(0, target_host.length() - 1);
            result = (boost::format("%1%:8080/%2%") % target_host % path).str();
        } else {
            result = (boost::format("%1%:8080/%2%") % target_host % path).str();
        }
    } else {
        result = (boost::format("http://%1%:8080/%2%") % target_host % path).str();
    }
        
    wcp_loger.add_log("Port 8080 URL built: " + result, false, "", "Moonraker_Mqtt", "info");
    return result;
}

// Construct full URL for API endpoint
std::string Moonraker::make_url(const std::string& path) const
{
    if (m_host.find("http://") == 0 || m_host.find("https://") == 0) {
        if (m_host.back() == '/') {
            return (boost::format("%1%%2%") % m_host % path).str();
        } else {
            return (boost::format("%1%/%2%") % m_host % path).str();
        }
    } else {
        if (m_host.find(":1884") != std::string::npos) {
            std::string http_host = m_host.substr(0, m_host.find(":1884"));
            return (boost::format("http://%1%/%2%") % http_host % path).str();
        }
        if (m_host.find(":8883") != std::string::npos) {
            std::string http_host = m_host.substr(0, m_host.find(":8883"));
            return (boost::format("http://%1%/%2%") % http_host % path).str();
        }
        return (boost::format("http://%1%/%2%") % m_host % path).str();
    }
}

// Basic connect implementation
bool Moonraker::connect(wxString& msg, const nlohmann::json& params) {
    return test(msg);
}

// Moonraker_mqtt

std::shared_ptr<MqttClient> Moonraker_Mqtt::m_mqtt_client = nullptr;
std::shared_ptr<MqttClient> Moonraker_Mqtt::m_mqtt_client_tls = nullptr;
TimeoutMap<int64_t, Moonraker_Mqtt::RequestCallback> Moonraker_Mqtt::m_request_cb_map;
std::string Moonraker_Mqtt::m_response_topic = "/response";
std::string Moonraker_Mqtt::m_status_topic = "/status";
std::string Moonraker_Mqtt::m_notification_topic = "/notification";
std::string Moonraker_Mqtt::m_request_topic = "/request";
std::string Moonraker_Mqtt::m_sn = "";
std::mutex Moonraker_Mqtt::m_sn_mtx;
std::string Moonraker_Mqtt::m_auth_topic = "/config/response";
std::string Moonraker_Mqtt::m_auth_req_topic = "/config/request";
nlohmann::json Moonraker_Mqtt::m_auth_info = nlohmann::json::object();
std::unordered_map<std::string, std::function<void(const nlohmann::json&)>> Moonraker_Mqtt::m_status_cbs;
std::unordered_map<std::string, std::function<void(const nlohmann::json&)>> Moonraker_Mqtt::m_notification_cbs;



Moonraker_Mqtt::Moonraker_Mqtt(DynamicPrintConfig* config, bool change_engine) : Moonraker(config) {
    std::string host_info = config->option<ConfigOptionString>("print_host")->value;
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    if (change_engine) {
        std::string local_ip = "";
        try {
            #ifdef _WIN32
                SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
                if (sock == INVALID_SOCKET) {
                    throw std::runtime_error("Failed to create socket");
                }
            #else
                int sock = socket(AF_INET, SOCK_DGRAM, 0);
                if (sock < 0) {
                    throw std::runtime_error("Failed to create socket");
                }
            #endif
            
            // connect google server
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(53);
            inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr);
            
            if (::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                #ifdef _WIN32
                    closesocket(sock);
                #else
                    close(sock);
                #endif
                throw std::runtime_error("Failed to connect");
            }
            
            // get local addr
            struct sockaddr_in local_addr;
            socklen_t len = sizeof(local_addr);
            if (getsockname(sock, (struct sockaddr*)&local_addr, &len) < 0) {
                #ifdef _WIN32
                    closesocket(sock);
                #else
                    close(sock);
                #endif
                throw std::runtime_error("Failed to get local address");
            }
                        
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &local_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
            local_ip = ip_str;
            
            #ifdef _WIN32
                closesocket(sock);
            #else
                close(sock);
            #endif

        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Error getting local IP: " << e.what();
            local_ip = "0.0.0.0"; 
        }
        try {
            m_mqtt_client.reset(new MqttClient("mqtt://" + host_info, local_ip, "", "", true));
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to create MQTT client: " << e.what();
            m_mqtt_client.reset();
        }
        m_mqtt_client_tls.reset();
        BOOST_LOG_TRIVIAL(error) << "local ip" << local_ip;
        wcp_loger.add_log("local IP: " + local_ip, false, "", "Moonraker_Mqtt", "error");
    }
    
}

// set auth info
void Moonraker_Mqtt::set_auth_info(const nlohmann::json& info) {
    m_auth_info = info;
}

nlohmann::json Moonraker_Mqtt::get_auth_info() {
    json authinfo;
    authinfo["user"] = m_user_name;
    authinfo["password"] = m_password;
    authinfo["ca"]       = m_ca;
    authinfo["cert"]     = m_cert;
    authinfo["key"]      = m_key;
    authinfo["clientId"] = m_client_id;
    authinfo["port"]     = m_port;

    return authinfo;
}

bool Moonraker_Mqtt::set_engine(const std::shared_ptr<MqttClient>& engine, std::string& msg)
{    

    if (!engine) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] engine pointer is null, cannot set";
        msg = "engine is null";
        return false;
    }

    BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] checking new engine connection status...";
    if (!engine->CheckConnected()) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] new engine connection check failed";
        msg = "engine connection failed";
        return false;
    }
    BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] new engine connection status OK";

    if (m_mqtt_client_tls) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] checking old engine connection status...";
        if (m_mqtt_client_tls->CheckConnected()) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] old engine still connected, disconnecting...";
            std::string dis_msg = "success";
            bool disconnect_result = m_mqtt_client_tls->Disconnect(dis_msg);
            if (disconnect_result) {
                BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] old engine disconnected successfully: " << dis_msg;
            } else {
                BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] old engine disconnect failed: " << dis_msg;
            }
        } else {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] old engine already disconnected";
        }
    } else {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] no old engine to disconnect";
    }

    BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] setting new engine pointer";
    m_mqtt_client_tls = engine;

    BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] setting message callback";
    std::weak_ptr<TimeSyncManager> weak_tsm = time_sync_manager_;
    m_mqtt_client_tls->SetMessageCallback([this, weak_tsm](const std::string& topic, const std::string& payload) {
        if (weak_tsm.expired()) {
            BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] object destroyed, ignoring MQTTS message";
            return;
        }
        this->on_mqtt_tls_message_arrived(topic, payload);
    });
    
    if (time_sync_manager_) {
        time_sync_manager_->reset();
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] time sync state reset";
    }

    BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] engine setup completed";
    msg = "success";
    return true;
}

// Ask for TLS info
bool Moonraker_Mqtt::ask_for_tls_info(const nlohmann::json& cn_params)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    if (!m_mqtt_client) {
        return false;
    }

    if(m_mqtt_client->CheckConnected()) {
        std::string dc_msg = "";
        m_mqtt_client->Disconnect(dc_msg);
    }

    m_sn_mtx.lock();
    m_sn = "";
    m_sn_mtx.unlock();

    std::string connection_msg = "";
    bool is_connect = m_mqtt_client->Connect(connection_msg);

    if(!is_connect || !cn_params.count("code") || cn_params["code"].get<std::string>() == "") {
        return false;
    }

    std::string auth_code  = cn_params["code"].get<std::string>();

    std::string sub_msg             = "success";
    bool response_subscribed = m_mqtt_client->Subscribe(auth_code + m_auth_topic, 1, sub_msg);
    if (!response_subscribed) {
        return false;
    }
    std::weak_ptr<TimeSyncManager> weak_tsm_mqtt = time_sync_manager_;
    m_mqtt_client->SetMessageCallback([this, weak_tsm_mqtt](const std::string& topic, const std::string& payload) {
        if (weak_tsm_mqtt.expired()) {
            BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] object destroyed, ignoring MQTT message";
            return;
        }
        this->on_mqtt_message_arrived(topic, payload);
    });

    json body;
    body["jsonrpc"] = "2.0";
    body["method"] = "server.request_key";
    json params;
    std::string clientid = "";    
    clientid = m_mqtt_client->get_client_id();   
    if(clientid == "0.0.0.0") {
        return false;
    }

    params["clientid"] = clientid;
    body["params"] = params;

    int64_t seq_id = m_seq_generator.generate_seq_id();
   
    std::promise<bool> auth_promise;
    std::future<bool> auth_future = auth_promise.get_future();

    auto callback = [this, &auth_promise](const nlohmann::json& res) {
        {
            json result = res;
            std::string state = result["state"].get<std::string>();
            if (state != "success") {
                auth_promise.set_value(false);
                return;
            }

            // sn
            m_sn_mtx.lock();
            m_sn = result["sn"].get<std::string>();
            m_sn_mtx.unlock();

            
            m_client_id = result["clientid"].get<std::string>();
            m_user_name = "";
            m_password  = "";
            m_ca = result["ca"].get<std::string>();
            m_cert = result["cert"].get<std::string>();
            m_key = result["key"].get<std::string>();
            m_port = result["port"].get<int>();

            auth_promise.set_value(true);
        }
    };

    auto timeout_callback = [this, &auth_promise]() {        
        auth_promise.set_value(false);
    };

    if (!add_response_target(seq_id, callback, timeout_callback, false, std::chrono::seconds(60))) {
            return false;
    }
    body["id"] = seq_id;
    
    if (time_sync_manager_) {
        time_sync_manager_->addTimeFields(body);
    }

    std::string pub_msg = "";
    if(!m_mqtt_client->Publish(auth_code + m_auth_req_topic, body.dump(), 1, pub_msg)){
        return false;
    }
    
    auto status = auth_future.wait_for(std::chrono::seconds(70));
    if(status == std::future_status::timeout){
        return false;
    }

    return auth_future.get();
}

// Connect to MQTT broker
bool Moonraker_Mqtt::connect(wxString& msg, const nlohmann::json& params) {
    auto& wcp_loger = GUI::WCP_Logger::getInstance();    
    wcp_loger.add_log("start connect mqtt", false, "", "Moonraker_Mqtt", "info");

    
    if(!params.count("ca") || !params.count("cert") 
    || !params.count("key") || !params.count("port") || !params.count("clientId") || !params.count("sn")){
        BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] less TLS data and try to get approve info";
        wcp_loger.add_log("less TLS data and try to get approve info", false, "", "Moonraker_Mqtt", "info");
        bool flag = ask_for_tls_info(params);
        if (!flag) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to get TLS auth info";
            wcp_loger.add_log("failed to get TLS auth info", false, "", "Moonraker_Mqtt", "error");
            return false;
        }
    }else{
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] using provided TLS parameters";
        wcp_loger.add_log("using provided TLS parameters", false, "", "Moonraker_Mqtt", "info");
        m_user_name = params.count("user")     ? params["user"].get<std::string>() : "";
        m_password = params.count("password") ? params["password"].get<std::string>() : "";
        m_ca = params["ca"].get<std::string>();
        m_cert = params["cert"].get<std::string>();
        m_key = params["key"].get<std::string>();
        m_port = params["port"].get<int>();
        m_client_id = params["clientId"].get<std::string>();

        m_sn_mtx.lock();
        m_sn = params["sn"].get<std::string>();
        m_sn_mtx.unlock();
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] setting SN: " << m_sn;
        wcp_loger.add_log("setting SN: " + m_sn, false, "", "Moonraker_Mqtt", "info");

        if (m_ca == "" || m_cert == "" || m_key == "") {
            bool flag = ask_for_tls_info(params);
            if (!flag) {
                BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to get TLS auth info";
                wcp_loger.add_log("failed to get TLS auth info", false, "", "Moonraker_Mqtt", "error");
                return false;
            }
        }
    }

    // Validate certificate format
    if (!m_ca.empty()) {
        if (m_ca.find("-----BEGIN CERTIFICATE-----") == std::string::npos) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] invalid CA certificate format";
            wcp_loger.add_log("invalid CA certificate format", false, "", "Moonraker_Mqtt", "error");
            return false;
        }
        BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] CA certificate format validation passed";
        wcp_loger.add_log("CA certificate format validation passed", false, "", "Moonraker_Mqtt", "info");
    }

    if (!m_cert.empty()) {
        if (m_cert.find("-----BEGIN CERTIFICATE-----") == std::string::npos) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] invalid client certificate format";
            wcp_loger.add_log("invalid client certificate format", false, "", "Moonraker_Mqtt", "error");
            return false;
        }
        BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] client certificate format validation passed";
        wcp_loger.add_log("client certificate format validation passed", false, "", "Moonraker_Mqtt", "info");
    }

    if (!m_key.empty()) {
        if (m_key.find("-----BEGIN PRIVATE KEY-----") == std::string::npos 
            && m_key.find("-----BEGIN RSA PRIVATE KEY-----") == std::string::npos) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] invalid private key format";
            wcp_loger.add_log("invalid private key format", false, "", "Moonraker_Mqtt", "error");
            return false;
        }
        BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] private key format validation passed";
        wcp_loger.add_log("private key format validation passed", false, "", "Moonraker_Mqtt", "info");
    }

    // Check port number
    if (m_port <= 0 || m_port > 65535) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] invalid port number: " << m_port;
        wcp_loger.add_log("invalid port number: " + std::to_string(m_port), false, "", "Moonraker_Mqtt", "error");
        return false;
    }

    // Log connection parameters (without sensitive content)
    BOOST_LOG_TRIVIAL(debug) << "[Moonraker_Mqtt] MQTTS connection parameters:"
        << "\n - host: " << m_host
        << "\n - port: " << m_port
        << "\n - client ID: " << m_client_id
        << "\n - CA cert present: " << (!m_ca.empty() ? "yes" : "no")
        << "\n - client cert present: " << (!m_cert.empty() ? "yes" : "no")
        << "\n - private key present: " << (!m_key.empty() ? "yes" : "no");
    wcp_loger.add_log("MQTTS connection parameters: " + m_host + ":" + std::to_string(m_port) + ", client ID: " + m_client_id + ", CA cert present: " + (!m_ca.empty() ? "yes" : "no") + ", client cert present: " + (!m_cert.empty() ? "yes" : "no") + ", private key present: " + (!m_key.empty() ? "yes" : "no"), false, "", "Moonraker_Mqtt", "info");

    // Ensure old connection is disconnected before creating a new one
    if (m_mqtt_client) {
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] disconnecting old MQTT client";
        wcp_loger.add_log("disconnecting old MQTT client", false, "", "Moonraker_Mqtt", "info");
        std::string dc_msg = "success";
        m_mqtt_client->Disconnect(dc_msg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        m_mqtt_client.reset();
    }

    if (m_mqtt_client_tls) {
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] disconnecting old MQTTS client";
        wcp_loger.add_log("disconnecting old MQTTS client", false, "", "Moonraker_Mqtt", "info");
        std::string dc_msg = "success";
        m_mqtt_client_tls->Disconnect(dc_msg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        m_mqtt_client_tls.reset();
    }

    // Create new MQTTS connection
    std::string host_ip = m_host;
    size_t      pos     = host_ip.find(":");
    if (pos != std::string::npos) {
        host_ip = host_ip.substr(0, pos);
        BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] extracted host IP: " << host_ip;
        wcp_loger.add_log("extracted host IP: " + host_ip, false, "", "Moonraker_Mqtt", "info");
    }
    
    std::string mqtts_url = "mqtts://" + host_ip + ":" + std::to_string(m_port);
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] creating MQTTS client, URL: " << mqtts_url << ", client ID: " << m_client_id;
    wcp_loger.add_log("creating MQTTS client, URL: " + mqtts_url + ", client ID: " + m_client_id, false, "", "Moonraker_Mqtt", "info");
    try {
        m_mqtt_client_tls.reset(new MqttClient(mqtts_url, m_client_id, m_ca, m_cert, m_key));
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to create MQTTS client: " << e.what();
        wcp_loger.add_log("failed to create MQTTS client: " + std::string(e.what()), false, "", "Moonraker_Mqtt", "error");
        m_mqtt_client_tls.reset();
        return false;
    }

    if (!m_mqtt_client_tls) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to create MQTT client";
        wcp_loger.add_log("failed to create MQTT client", false, "", "Moonraker_Mqtt", "error");
        return false;
    }

    std::string connection_msg = "";
    bool is_connect = m_mqtt_client_tls->Connect(connection_msg);
    msg                        = connection_msg;
    if (!is_connect) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] MQTT connection failed";
        wcp_loger.add_log("MQTT connection failed", false, "", "Moonraker_Mqtt", "error");
        return false;
    }
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] MQTTS connected successfully";
    wcp_loger.add_log("MQTTS connected successfully", false, "", "Moonraker_Mqtt", "info");

    // Reset time sync state
    if (time_sync_manager_) {
        time_sync_manager_->reset();
    }

    m_sn_mtx.lock();
    std::string tmp_sn = m_sn;
    m_sn_mtx.unlock();

    if(tmp_sn == ""){
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] SN is empty, cannot subscribe to topics";
        wcp_loger.add_log("SN is empty, cannot subscribe to topics", false, "", "Moonraker_Mqtt", "error");
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::string no_sub_msg              = "success";
    bool notification_subscribed = m_mqtt_client_tls->Subscribe(tmp_sn + m_notification_topic, 1, no_sub_msg);

    std::string res_sub_msg         = "success";
    bool response_subscribed = m_mqtt_client_tls->Subscribe(tmp_sn + m_response_topic, 1, res_sub_msg);
    
    BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] topic subscription result - notification topic: " << (notification_subscribed ? "success" : "failed")
                           << ", response topic: " << (response_subscribed ? "success" : "failed");
    wcp_loger.add_log("topic subscription result - notification topic: " + std::string((notification_subscribed ? "success" : "failed")) + ", response topic: " + (response_subscribed ? "success" : "failed"), false, "", "Moonraker_Mqtt", "info");
    std::weak_ptr<TimeSyncManager> weak_tsm = time_sync_manager_;
    m_mqtt_client_tls->SetMessageCallback([this, weak_tsm](const std::string& topic, const std::string& payload) {
        if (weak_tsm.expired()) {
            BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] object destroyed, ignoring MQTTS message";
            return;
        }
        this->on_mqtt_tls_message_arrived(topic, payload);
    });

    bool result = is_connect && notification_subscribed && response_subscribed;
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] MQTT connection flow completed, result: " << (result ? "success" : "failed");
    wcp_loger.add_log("MQTT connection flow completed, result: " + std::string((result ? "success" : "failed")), false, "", "Moonraker_Mqtt", "info");
    return result;
}

// Disconnect from MQTT broker
bool Moonraker_Mqtt::disconnect(wxString& msg, const nlohmann::json& params) {
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] starting MQTT disconnect";
    wcp_loger.add_log("starting MQTT disconnect", false, "", "Moonraker_Mqtt", "info");
    if (!m_mqtt_client_tls) {
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] MQTTS client does not exist, nothing to disconnect";
        wcp_loger.add_log("MQTTS client does not exist, nothing to disconnect", false, "", "Moonraker_Mqtt", "info");
        return false;
    }

    std::string dc_msg = "success";
    bool flag = m_mqtt_client_tls->Disconnect(dc_msg);
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] MQTTS disconnect result: " << (flag ? "success" : "failed");
    wcp_loger.add_log("MQTTS disconnect result: " + std::string((flag ? "success" : "failed")), false, "", "Moonraker_Mqtt", "info");

    // Release time_sync_manager_ first so weak_ptr in callbacks expires immediately,
    // ensuring no callback thread accesses a destroyed object
    time_sync_manager_.reset();

    if (flag) {
        m_status_cbs.clear();
        m_notification_cbs.clear();
    }

    m_sn_mtx.lock();
    m_sn = "";
    m_sn_mtx.unlock();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] SN reset";
    wcp_loger.add_log("SN reset", false, "", "Moonraker_Mqtt", "info");
    
    // Wait for MQTT client cleanup to complete, avoiding memory access issues
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    m_mqtt_client_tls.reset();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] MQTTS client reset";
    wcp_loger.add_log("MQTTS client reset", false, "", "Moonraker_Mqtt", "info");    
    return flag;
}

// Subscribe to printer status updates
void Moonraker_Mqtt::async_subscribe_machine_info(const std::string& hash, std::function<void(const nlohmann::json&)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] starting machine status subscription";
    wcp_loger.add_log("starting machine status subscription", false, "", "Moonraker_Mqtt", "info");

    if (m_status_cbs.empty()) {
        std::string main_layer = "+";

        m_sn_mtx.lock();
        main_layer = m_sn;
        m_sn_mtx.unlock();

        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] using SN topic: " << main_layer;
        wcp_loger.add_log("using SN topic: " + main_layer, false, "", "Moonraker_Mqtt", "info");
        std::string sub_msg    = "success";
        bool        res_status = m_mqtt_client_tls ? m_mqtt_client_tls->Subscribe(main_layer + m_status_topic, 1, sub_msg) : false;
        bool res_notification  = m_mqtt_client_tls ? m_mqtt_client_tls->Subscribe(main_layer + m_notification_topic, 1, sub_msg) : false;

        if (!res_status || !res_notification) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to subscribe to status topic";
            wcp_loger.add_log("failed to subscribe to status topic", false, "", "Moonraker_Mqtt", "error");
            callback(json::value_t::null);
            return;
        }

        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] successfully subscribed to status topic: " << main_layer + m_status_topic;
        wcp_loger.add_log("successfully subscribed to status topic: " + main_layer + m_status_topic, false, "", "Moonraker_Mqtt", "info");
    }

    m_status_cbs.insert({hash, callback});
    m_notification_cbs.insert({hash, callback});
    callback(json::object());

    
}

// start print job
void Moonraker_Mqtt::async_start_print_job(const std::string& filename, std::function<void(const nlohmann::json&)> cb)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting print job, filename: " << filename;
    wcp_loger.add_log("Starting print job, filename: " + filename, false, "", "Moonraker_Mqtt", "info");
    std::string method = "printer.print.start";
    json        params = json::object();
    params["filename"] = filename;

    if (!send_to_request(method, params, true, cb,
                         [cb, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] start print job timed out";
                             wcp_loger.add_log("start print job timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             cb(res);
                         }) &&
        cb) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send start print request";
        wcp_loger.add_log("failed to send start print request", false, "", "Moonraker_Mqtt", "error");
        cb(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_pause_print_job(std::function<void(const nlohmann::json&)> cb) {
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting pause print job";
    wcp_loger.add_log("Starting pause print job", false, "", "Moonraker_Mqtt", "info");
    std::string method =  "printer.print.pause";
    json        params  = json::object();

    if (!send_to_request(method, params, true, cb,
                         [cb, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] pause print job timed out";
                             wcp_loger.add_log("pause print job timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             cb(res);
                         }) &&
        cb) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send pause print request";
        wcp_loger.add_log("failed to send pause print request", false, "", "Moonraker_Mqtt", "error");
        cb(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_resume_print_job(std::function<void(const nlohmann::json&)> cb) {
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting resume print job";
    wcp_loger.add_log("Starting resume print job", false, "", "Moonraker_Mqtt", "info");
    std::string method = "printer.print.resume";
    json        params = json::object();

    if (!send_to_request(method, params, true, cb,
                         [cb, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] resume print job timed out";
                             wcp_loger.add_log("resume print job timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             cb(res);
                         }) &&
        cb) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send resume print request";
        wcp_loger.add_log("failed to send resume print request", false, "", "Moonraker_Mqtt", "error");
        cb(json::value_t::null);
    }
}

void Moonraker_Mqtt::test_async_wcp_mqtt_moonraker(const nlohmann::json& mqtt_request_params, std::function<void(const nlohmann::json&)> cb) {
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting MQTT Moonraker async request test";
    wcp_loger.add_log("Starting MQTT Moonraker async request test", false, "", "Moonraker_Mqtt", "info");
    int64_t id = -1;

    if (!mqtt_request_params.count("id") || !mqtt_request_params[id].is_number()) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] test request missing valid ID";
        wcp_loger.add_log("test request missing valid ID", false, "", "Moonraker_Mqtt", "error");
        cb(json::value_t::null);
        return;
    }

    id = mqtt_request_params["id"].get<int64_t>();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] test request ID: " << id;
    wcp_loger.add_log("test request ID: " + std::to_string(id), false, "", "Moonraker_Mqtt", "info");

    if (id == -1) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] invalid request ID";
        wcp_loger.add_log("invalid request ID", false, "", "Moonraker_Mqtt", "error");
        cb(json::value_t::null);
        return;
    }

    if (!add_response_target(id, cb, [cb, &wcp_loger]() {
            BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] test request timed out";
            wcp_loger.add_log("test request timed out", false, "", "Moonraker_Mqtt", "warning");
            json res;
            res["error"] = "timeout";
            cb(res);
        })){
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to add response target";
        wcp_loger.add_log("failed to add response target", false, "", "Moonraker_Mqtt", "error");
        cb(json::value_t::null);
    }

    if (m_mqtt_client_tls) {
        std::string main_layer = "+";

        if (wait_for_sn()) {
            m_sn_mtx.lock();
            main_layer = m_sn;
            m_sn_mtx.unlock();

            if (main_layer == "+" || main_layer == "") {
                BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] invalid SN, removing response target";
                wcp_loger.add_log("invalid SN, removing response target", false, "", "Moonraker_Mqtt", "error");
                delete_response_target(id);
                cb(json::value_t::null);
                return;
            }

            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] publishing test request to topic: " << main_layer + m_request_topic;
            wcp_loger.add_log("publishing test request to topic: " + main_layer + m_request_topic, false, "", "Moonraker_Mqtt", "info");
            std::string pub_msg = "success";
            bool res = m_mqtt_client_tls->Publish(main_layer + m_request_topic, mqtt_request_params.dump(), 1, pub_msg);
            if (!res) {
                BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to publish test request";
                wcp_loger.add_log("failed to publish test request", false, "", "Moonraker_Mqtt", "error");
                delete_response_target(id);
            } else {
                BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] test request published successfully";
                wcp_loger.add_log("test request published successfully", false, "", "Moonraker_Mqtt", "info");
            }
            return;
        }
    }
}

void Moonraker_Mqtt::async_cancel_print_job(std::function<void(const nlohmann::json&)> cb)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting cancel print job";
    wcp_loger.add_log("Starting cancel print job", false, "", "Moonraker_Mqtt", "info");
    std::string method = "printer.print.cancel";
    json        params = json::object();

    if (!send_to_request(method, params, true, cb,
                         [cb, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] cancel print job timed out";
                             wcp_loger.add_log("cancel print job timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             cb(res);
                         }) &&
        cb) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send cancel print request";
        wcp_loger.add_log("failed to send cancel print request", false, "", "Moonraker_Mqtt", "error");
        cb(json::value_t::null);
    }
}

// Get printer info
void Moonraker_Mqtt::async_get_printer_info(std::function<void(const nlohmann::json& response)> callback) {
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting get printer info";
    wcp_loger.add_log("Starting get printer info", false, "", "Moonraker_Mqtt", "info");
    std::string method = "printer.info";
    json        params = json::object();

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] get printer info timed out";
                             wcp_loger.add_log("get printer info timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send get printer info request";
        wcp_loger.add_log("failed to send get printer info request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

// Send G-code commands to printer
void Moonraker_Mqtt::async_send_gcodes(const std::vector<std::string>& scripts, std::function<void(const nlohmann::json&)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting to send G-code, count: " << scripts.size();
    wcp_loger.add_log("Starting to send G-code, count: " + std::to_string(scripts.size()), false, "", "Moonraker_Mqtt", "info");
    std::string method = "printer.gcode.script";

    std::string str_scripts = "";
    for (size_t i = 0; i < scripts.size(); ++i) {
        if (i != 0) {
            str_scripts += "\n";
        }
        str_scripts += scripts[i];
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] adding G-code: " << scripts[i];
        wcp_loger.add_log("adding G-code: " + scripts[i], false, "", "Moonraker_Mqtt", "info");
    }

    json params;
    params["script"] = str_scripts;

    if (!send_to_request(method, params, true, callback, [callback, &wcp_loger](){
        BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] send G-code timed out";
        wcp_loger.add_log("send G-code timed out", false, "", "Moonraker_Mqtt", "warning");
        json res;
        res["error"] = "timeout";
        callback(res);
    }) && callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send G-code request";
        wcp_loger.add_log("failed to send G-code request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

// Unsubscribe from printer status updates
void Moonraker_Mqtt::async_unsubscribe_machine_info(const std::string& hash, std::function<void(const nlohmann::json&)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting unsubscribe machine status";
    wcp_loger.add_log("Starting unsubscribe machine status", false, "", "Moonraker_Mqtt", "info");

    if (m_status_cbs.count(hash))
        m_status_cbs.erase(hash);

    if (m_notification_cbs.count(hash))
        m_notification_cbs.erase(hash);

    if (m_status_cbs.empty()) {
        std::string main_layer = "+";

        m_sn_mtx.lock();
        main_layer = m_sn;
        m_sn_mtx.unlock();

        std::string un_sub_msg = "success";
        bool        res        = m_mqtt_client_tls ? m_mqtt_client_tls->Unsubscribe(main_layer + m_status_topic, un_sub_msg) : false;

        if (!res) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to unsubscribe from status topic";
            wcp_loger.add_log("failed to unsubscribe from status topic", false, "", "Moonraker_Mqtt", "error");
            if (callback) {
                callback(json::value_t::null);
            }
            return;
        }

        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] successfully unsubscribed from status topic: " << main_layer + m_status_topic;
        wcp_loger.add_log("successfully unsubscribed from status topic: " + main_layer + m_status_topic, false, "", "Moonraker_Mqtt", "info");
        callback(json::object());
    } else {
        callback(json::object());
    }


    
}

// Set filters for printer status subscription
void Moonraker_Mqtt::async_set_machine_subscribe_filter(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& targets,
    std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting machine subscribe filter setup, target count: " << targets.size();
    wcp_loger.add_log("Starting machine subscribe filter setup, target count: " + std::to_string(targets.size()), false, "", "Moonraker_Mqtt", "info");
    std::string method = "printer.objects.subscribe";

    json params;
    params["objects"] = json::object();

    for (size_t i = 0; i < targets.size(); ++i) {
        if (targets[i].second.size() == 0) {
            params["objects"][targets[i].first] = json::value_t::null;
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] added filter (all): " << targets[i].first;
            wcp_loger.add_log("added filter (all): " + targets[i].first, false, "", "Moonraker_Mqtt", "info");
        } else {
            params["objects"][targets[i].first] = json::array();

            for (const auto& key : targets[i].second) {
                params["objects"][targets[i].first].push_back(key);
            }
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] added filter: " << targets[i].first 
                                    << ", field count: " << targets[i].second.size();
            wcp_loger.add_log("added filter: " + targets[i].first + ", field count: " + std::to_string(targets[i].second.size()), false, "", "Moonraker_Mqtt", "info");
        }
    }

    if (!send_to_request(method, params, true, callback, [callback, &wcp_loger](){
        BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] set subscribe filter timed out";
        wcp_loger.add_log("set subscribe filter timed out", false, "", "Moonraker_Mqtt", "warning");
        json res;
        res["error"] = "timeout";
        callback(res);
    }) && callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send set subscribe filter request";
        wcp_loger.add_log("failed to send set subscribe filter request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_machine_files_roots(std::function<void(const nlohmann::json& response)> callback) {
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting get file system roots";
    wcp_loger.add_log("Starting get file system roots", false, "", "Moonraker_Mqtt", "info");
    std::string method = "server.files.roots";

    json params = json::object();

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] get file system roots timed out";
                             wcp_loger.add_log("get file system roots timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send get file system roots request";
        wcp_loger.add_log("failed to send get file system roots request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_machine_files_metadata(const std::string& filename, std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting get file metadata, filename: " << filename;
    wcp_loger.add_log("Starting get file metadata, filename: " + filename, false, "", "Moonraker_Mqtt", "info");
    std::string method = "server.files.metadata";

    json params;
    params["filename"] = filename;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] get file metadata timed out";
                             wcp_loger.add_log("get file metadata timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send get file metadata request";
        wcp_loger.add_log("failed to send get file metadata request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_set_device_name(const std::string& device_name, std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting set device name, name: " << device_name;
    wcp_loger.add_log("Starting set device name, name: " + device_name, false, "", "Moonraker_Mqtt", "info");
    std::string method = "machine.set_device_name";

    json params;
    params["name"] = device_name;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] set device name timed out";
                             wcp_loger.add_log("set device name timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send set device name request";
        wcp_loger.add_log("failed to send set device name request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_control_led(const std::string& name, int white, std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting control LED, name: " << name << ", white brightness: " << white;
    wcp_loger.add_log("Starting control LED, name: " + name + ", white brightness: " + std::to_string(white), false, "", "Moonraker_Mqtt", "info");
    std::string method = "printer.control.led";

    json params;
    params["name"]  = name;
    params["white"] = white;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] control LED timed out";
                             wcp_loger.add_log("control LED timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send control LED request";
        wcp_loger.add_log("failed to send control LED request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_control_print_speed(int percentage, std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting control print speed, percentage: " << percentage;
    wcp_loger.add_log("Starting control print speed, percentage: " + std::to_string(percentage), false, "", "Moonraker_Mqtt", "info");
    std::string method = "printer.control.print_speed";

    json params;
    params["percentage"] = percentage;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] control print speed timed out";
                             wcp_loger.add_log("control print speed timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send control print speed request";
        wcp_loger.add_log("failed to send control print speed request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}


void Moonraker_Mqtt::async_bedmesh_abort_probe_mesh(std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting abort bed mesh probe";
    wcp_loger.add_log("Starting abort bed mesh probe ", false, "", "Moonraker_Mqtt", "info");
    std::string method = "printer.bed_mesh.abort_probe_mesh";

    json params = json::object();

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] abort bed mesh probe timed out";
                             wcp_loger.add_log("abort bed mesh probe timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send abort bed mesh probe request";
        wcp_loger.add_log("failed to send abort bed mesh probe request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_controlPurifier(int                                                 fan_speed,
                                           int                                                 delay_time,
                                           int                                                 work_time,
                                           std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting control air purifier, fan speed: " << fan_speed << ", delay: " << delay_time
                            << ", work time: " << work_time;
    wcp_loger.add_log("Starting control air purifier, fan speed: " + std::to_string(fan_speed) + ", delay: " + std::to_string(delay_time) +
                          ", work time: " + std::to_string(work_time),
                      false, "", "Moonraker_Mqtt", "info");
    std::string method = "printer.control.purifier";

    json params;
    if (fan_speed != -1)
        params["fan_speed"]  = fan_speed;

    if (delay_time != -1)
        params["delay_time"] = delay_time;

    if (work_time != -1)
        params["work_time"]  = work_time;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] control air purifier timed out";
                             wcp_loger.add_log("control air purifier timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send control air purifier request";
        wcp_loger.add_log("failed to send control air purifier request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_controlPurifier(const nlohmann::json& params,
                                            std::function<void(const nlohmann::json& response)> callback)
{
    if (!send_to_request("printer.control.purifier", params, true, callback,
                         [callback]() {
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        callback(json::value_t::null);
    }
}


void Moonraker_Mqtt::async_control_main_fan(int speed, std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting control main fan, speed: " << speed;
    wcp_loger.add_log("Starting control main fan, speed: " + std::to_string(speed), false, "", "Moonraker_Mqtt", "info");
    std::string method = "printer.control.main_fan";

    json params;
    params["speed"] = speed;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] control main fan timed out";
                             wcp_loger.add_log("control main fan timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send control main fan request";
        wcp_loger.add_log("failed to send control main fan request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_control_generic_fan(const std::string&                                  name,
                                               int                                                 speed,
                                               std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting control generic fan, name: " << name << ", speed: " << speed;
    wcp_loger.add_log("Starting control generic fan, name: " + name + ", speed: " + std::to_string(speed), false, "", "Moonraker_Mqtt", "info");
    std::string method = "printer.control.generic_fan";

    json params;
    params["name"]  = name;
    params["speed"] = speed;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] control generic fan timed out";
                             wcp_loger.add_log("control generic fan timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send control generic fan request";
        wcp_loger.add_log("failed to send control generic fan request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_control_bed_temp(int temp, std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting control bed temp, target temp: " << temp;
    wcp_loger.add_log("Starting control bed temp, target temp: " + std::to_string(temp), false, "", "Moonraker_Mqtt", "info");
    std::string method = "printer.control.bed_temp";

    json params;
    params["temp"] = temp;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] control bed temp timed out";
                             wcp_loger.add_log("control bed temp timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send control bed temp request";
        wcp_loger.add_log("failed to send control bed temp request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_control_extruder_temp(int temp, int index, int map, std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting control extruder temp, target temp: " << temp << ", extruder index: " << index;
    wcp_loger.add_log("Starting control extruder temp, target temp: " + std::to_string(temp) + ", extruder index: " + std::to_string(index), false, "",
                      "Moonraker_Mqtt", "info");
    std::string method = "printer.control.extruder_temp";

    json params;
    params["temp"]  = temp;

    if (index != -1)
        params["index"] = index;

    if (map != -1)
        params["map"] = map;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] control extruder temp timed out";
                             wcp_loger.add_log("control extruder temp timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send control extruder temp request";
        wcp_loger.add_log("failed to send control extruder temp request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}


void Moonraker_Mqtt::async_files_thumbnails_base64(const std::string& path, std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting get file thumbnails, path: " << path;
    wcp_loger.add_log("Starting get file thumbnails, path: " + path, false, "", "Moonraker_Mqtt", "info");
    std::string method = "server.files.thumbnails_base64";

    json params;
    params["path"] = path;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] get file thumbnails timed out";
                             wcp_loger.add_log("get file thumbnails timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send get file thumbnails request";
        wcp_loger.add_log("failed to send get file thumbnails request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}


void Moonraker_Mqtt::async_get_file_page_list(const std::string& root, int files_per_page, int page_number, std::function<void(const nlohmann::json& response)> callback){
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting get file page list";
    wcp_loger.add_log("Starting get file page list", false, "", "Moonraker_Mqtt", "info");
    std::string method = "server.files.list_page";

    json params;

    params["root"] = root;
    params["files_per_page"] = files_per_page;
    params["page_number"]    = page_number;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] get file page list timed out";
                             wcp_loger.add_log("get file page list timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] get file page list failed";
        wcp_loger.add_log("get file page list failed", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_exception_query(std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting exception query";
    wcp_loger.add_log("Starting exception query", false, "", "Moonraker_Mqtt", "info");
    std::string method = "server.exception.query";

    json params;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] exception query timed out";
                             wcp_loger.add_log("exception query timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] exception query failed";
        wcp_loger.add_log("exception query failed", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_server_client_manager_set_userinfo(const nlohmann::json& user, std::function<void(const nlohmann::json& response)> callback) {
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting set bound user info";
    wcp_loger.add_log("Starting set bound user info", false, "", "Moonraker_Mqtt", "info");
    std::string method = "server.client_manager.set_userinfo";



    if (!user.count("auther") || !user["auther"].count("id") || !user["auther"].count("nickname")) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] set bound user info failed, id or nickname missing";
        wcp_loger.add_log("set bound user info failed, id or nickname missing", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
        return;
    }

    json params = user;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] set bound user info timed out";
                             wcp_loger.add_log("set bound user info timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send set bound user info request";
        wcp_loger.add_log("failed to send set bound user info request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_machine_files_thumbnails(const std::string& filename, std::function<void(const nlohmann::json& response)> callback) {
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting get file thumbnails, filename: " << filename;
    wcp_loger.add_log("Starting get file thumbnails, filename: " + filename, false, "", "Moonraker_Mqtt", "info");
    std::string method = "server.files.thumbnails";

    json params;
    params["filename"] = filename;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] get file thumbnails timed out";
                             wcp_loger.add_log("get file thumbnails timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send get file thumbnails request";
        wcp_loger.add_log("failed to send get file thumbnails request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_machine_files_directory(const std::string& path, bool extend, std::function<void(const nlohmann::json& response)> callback) {
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting get directory content, path: " << path << ", extended: " << extend;
    wcp_loger.add_log("Starting get directory content, path: " + path + ", extended: " + std::to_string(extend), false, "", "Moonraker_Mqtt", "info");
    std::string method = "server.files.get_directory";

    json params;
    params["path"] = path;
    params["extended"] = extend;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] get directory content timed out";
                             wcp_loger.add_log("get directory content timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send get directory content request";
        wcp_loger.add_log("failed to send get directory content request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_camera_start(const std::string& domain, int interval, bool expect_pw, std::function<void(const nlohmann::json& response)> callback) {
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting start camera monitor, domain: " << domain;
    wcp_loger.add_log("Starting start camera monitor, domain: " + domain, false, "", "Moonraker_Mqtt", "info");
    std::string method = "camera.start_monitor";

    json params;
    params["domain"]     = domain;
    params["interval"] = interval;
    params["expect_pw"] = expect_pw;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] start camera monitor timed out";
                             wcp_loger.add_log("start camera monitor timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send start camera monitor request";
        wcp_loger.add_log("failed to send start camera monitor request", false, "", "Moonraker_Mqtt", "error");
        sentryReportLog(SENTRY_LOG_TRACE, "bury_point_open video cmd error", BP_VIDEO_ABNORMAL);
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_delete_machine_file(const std::string& path, std::function<void(const nlohmann::json& response)> callback) {
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting delete file, path: " << path;
    wcp_loger.add_log("Starting delete file, path: " + path, false, "", "Moonraker_Mqtt", "info");
    std::string method = "server.files.delete_file";

    json params;
    params["path"] = path;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] delete file timed out";
                             wcp_loger.add_log("delete file timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send delete file request";
        wcp_loger.add_log("failed to send delete file request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}


void Moonraker_Mqtt::async_canmera_stop(const std::string& domain, std::function<void(const nlohmann::json& response)> callback) {
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting stop camera monitor, domain: " << domain;
    wcp_loger.add_log("Starting stop camera monitor, domain: " + domain, false, "", "Moonraker_Mqtt", "info");
    std::string method = "camera.stop_monitor";

    json params;
    params["domain"] = domain;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] stop camera monitor timed out";
                             wcp_loger.add_log("stop camera monitor timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send stop camera monitor request";
        wcp_loger.add_log("failed to send stop camera monitor request", false, "", "Moonraker_Mqtt", "error");
        sentryReportLog(SENTRY_LOG_TRACE, "bury_point_stop video cmd error", BP_VIDEO_ABNORMAL);
        callback(json::value_t::null);
    }
}

// request upload timelapse file
void Moonraker_Mqtt::async_upload_camera_timelapse(const nlohmann::json& targets,
    std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting request upload timelapse file";
    wcp_loger.add_log("Starting request upload timelapse file", false, "", "Moonraker_Mqtt", "info");
    std::string method = "camera.upload_timelapse_instance";

    json params = json::object();

    params = targets;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] request upload timelapse file timed out";
                             wcp_loger.add_log("request upload timelapse file timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send request upload timelapse file";
        wcp_loger.add_log("failed to send request upload timelapse file", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

// Request to upload timelapse file (async instance path used by WAN download)
void Moonraker_Mqtt::async_upload_timelapse_instance(const nlohmann::json& targets,
    std::function<void(const nlohmann::json& response)> callback)
{
    std::string method = "camera.upload_async_timelapse_instance";
    json params = json::object();
    params = targets;

    if (!send_to_request(method, params, true, callback,
                         [callback]() {
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] Failed to send request to upload timelapse file";
        callback(json::value_t::null);
    }
}

// get timelapse list
void Moonraker_Mqtt::async_get_timelapse_instance(const nlohmann::json& targets, std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting request get timelapse file list";
    wcp_loger.add_log("Starting request get timelapse file list", false, "", "Moonraker_Mqtt", "info");
    std::string method = "camera.get_timelapse_instance";

    json params = json::object();

    params = targets;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] request get timelapse file list timed out";
                             wcp_loger.add_log("request get timelapse file list timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send request get timelapse file list";
        wcp_loger.add_log("failed to send request get timelapse file list", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

//get the mechine local storage space
void Moonraker_Mqtt::async_get_userdata_space(const nlohmann::json& targets, std::function<void(const nlohmann::json& response)> callback)
{
    auto&       wcp_loger = GUI::WCP_Logger::getInstance();
    std::string method    = "server.files.get_userdata_space";

    json params = json::object();

    params = targets;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] get uoser storage space";
                             wcp_loger.add_log("get uoser storage space timeout", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt]send the cmd to get uoser storage space fail";
        wcp_loger.add_log("send the cmd to get uoser storage space fail", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

// request delete timelapse file
void Moonraker_Mqtt::async_delete_camera_timelapse(const nlohmann::json&                               targets,
                                                   std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting request delete timelapse file";
    wcp_loger.add_log("Starting request delete timelapse file", false, "", "Moonraker_Mqtt", "info");
    std::string method = "camera.delete_timelapse_instance";

    json params = json::object();

    params = targets;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] request delete timelapse file timed out";
                             wcp_loger.add_log("request delete timelapse file timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send request delete timelapse file";
        wcp_loger.add_log("failed to send request delete timelapse file", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

// request defect detection config
void Moonraker_Mqtt::async_defect_detaction_config(const nlohmann::json& targets, std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting request defect detection config";
    wcp_loger.add_log("Starting request defect detection config", false, "", "Moonraker_Mqtt", "info");
    std::string method = "printer.defect_detection.config";

    json params = json::object();

    params = targets;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] request defect detection config timed out";
                             wcp_loger.add_log("request defect detection config timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send request defect detection config";
        wcp_loger.add_log("failed to send request defect detection config", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

// request device download cloud file and print
void Moonraker_Mqtt::async_start_cloud_print(const nlohmann::json& targets,
                                           std::function<void(const nlohmann::json& response)>                  callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting request device start cloud print";
    wcp_loger.add_log("Starting request device cloud print", false, "", "Moonraker_Mqtt", "info");
    std::string method = "server.files.start_cloud_print";

    json params = json::object();

    params = targets;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] request device cloud print timed out";
                             wcp_loger.add_log("request device cloud print timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send request device start cloud print";
        wcp_loger.add_log("failed to send request device start cloud print", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

// Request device to start local file print
static const std::string METHOD_START_LOCAL_PRINT = "server.files.start_local_print";

void Moonraker_Mqtt::async_start_local_print(const nlohmann::json& targets,
                                              std::function<void(const nlohmann::json& response)> callback)
{
    if (!send_to_request(METHOD_START_LOCAL_PRINT, targets, true, callback,
                         [callback]() {
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        callback(json::value_t::null);
    }
}

// Request device heartbeat
void Moonraker_Mqtt::async_machine_heartbeat(const nlohmann::json& targets,
                                              std::function<void(const nlohmann::json& response)> callback)
{
    if (!send_to_request("machine.heartbeat", targets, true, callback,
                         [callback]() {
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        callback(json::value_t::null);
    }
}

// request device start cloud print
void Moonraker_Mqtt::async_pull_cloud_file(const nlohmann::json& targets, std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting request device download cloud file";
    wcp_loger.add_log("Starting request device download cloud file", false, "", "Moonraker_Mqtt", "info");
    std::string method = "server.files.pull";

    json params = json::object();

    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] added cloud file download target, property count: " << targets.size();
    wcp_loger.add_log("added cloud file download target, property count: " + std::to_string(targets.size()), false, "", "Moonraker_Mqtt", "info");

    params = targets;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] request device download file timed out";
                             wcp_loger.add_log("request device download file timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send request device download file request";
        wcp_loger.add_log("failed to send request device download file request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_cancel_pull_cloud_file(std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] request device cancel upload";
    wcp_loger.add_log("request device cancel upload", false, "", "Moonraker_Mqtt", "info");
    std::string method = "server.files.cancel_pull";

    json params = json::object();

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] request device cancel upload timed out";
                             wcp_loger.add_log("request device cancel upload timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send device cancel upload request";
        wcp_loger.add_log("failed to send device cancel upload request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

// Query device information (firmware version)
// Query printer information
void Moonraker_Mqtt::async_get_device_info(std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting query firmware info";
    wcp_loger.add_log("Starting query firmware info", false, "", "Moonraker_Mqtt", "info");
    std::string method = "system.get_device_info";

    json params;

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] query firmware info timed out";
                             wcp_loger.add_log("query firmware info timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send query firmware info request";
        wcp_loger.add_log("failed to send query firmware info request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

// Query printer information
void Moonraker_Mqtt::async_get_machine_info(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& targets,
    std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting query printer info, target count: " << targets.size();
    wcp_loger.add_log("Starting query printer info, target count: " + std::to_string(targets.size()), false, "", "Moonraker_Mqtt", "info");
    std::string method = "printer.objects.query";

    json params;
    params["objects"] = json::object();

    for (size_t i = 0; i < targets.size(); ++i) {
        if (targets[i].second.size() == 0) {
            params["objects"][targets[i].first] = json::value_t::null;
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] query object (all properties): " << targets[i].first;
            wcp_loger.add_log("query object (all properties): " + targets[i].first, false, "", "Moonraker_Mqtt", "info");
        } else {
            params["objects"][targets[i].first] = json::array();

            for (const auto& key : targets[i].second) {
                params["objects"][targets[i].first].push_back(key);
            }
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] query object: " << targets[i].first 
                                    << ", property count: " << targets[i].second.size();
            wcp_loger.add_log("query object: " + targets[i].first + ", property count: " + std::to_string(targets[i].second.size()), false, "", "Moonraker_Mqtt", "info");
        }
    }

    if (!send_to_request(method, params, true, callback, [callback, &wcp_loger](){
        BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] query printer info timed out";
        wcp_loger.add_log("query printer info timed out", false, "", "Moonraker_Mqtt", "warning");
        json res;
        res["error"] = "timeout";
        callback(res);
    }) && callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send query printer info request";
        wcp_loger.add_log("failed to send query printer info request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

// Get File state
void Moonraker_Mqtt::async_server_files_get_status(std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting get system file status";
    wcp_loger.add_log("Starting get system file status info", false, "", "Moonraker_Mqtt", "info");
    std::string method = "server.files.get_status";
    json        params = json::object();

    if (!send_to_request(method, params, true, callback,
                         [callback, &wcp_loger]() {
                             BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] get system file status timed out";
                             wcp_loger.add_log("get system file status timed out", false, "", "Moonraker_Mqtt", "warning");
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send get system file status request";
        wcp_loger.add_log("failed to send get system file status request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

// Get system info of the machine
void Moonraker_Mqtt::async_get_system_info(std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting get system info";
    wcp_loger.add_log("Starting get system info", false, "", "Moonraker_Mqtt", "info");
    std::string method = "machine.system_info";
    json params = json::object();

    if (!send_to_request(method, params, true, callback, [callback, &wcp_loger](){
        BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] get system info timed out";
        wcp_loger.add_log("get system info timed out", false, "", "Moonraker_Mqtt", "warning");
        json res;
        res["error"] = "timeout";
        callback(res);
    }) && callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send get system info request";
        wcp_loger.add_log("failed to send get system info request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

// Get list of available printer objects
void Moonraker_Mqtt::async_get_machine_objects(std::function<void(const nlohmann::json& response)> callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] Starting get available printer object list";
    wcp_loger.add_log("Starting get available printer object list", false, "", "Moonraker_Mqtt", "info");
    std::string method = "printer.objects.list";
    json params = json::object();

    if (!send_to_request(method, params, true, callback, [callback, &wcp_loger](){
        BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] get printer object list timed out";
        wcp_loger.add_log("get printer object list timed out", false, "", "Moonraker_Mqtt", "warning");
        json res;
        res["error"] = "timeout";
        callback(res);
    }) && callback) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to send get printer object list request";
        wcp_loger.add_log("failed to send get printer object list request", false, "", "Moonraker_Mqtt", "error");
        callback(json::value_t::null);
    }
}

// Send request to printer via MQTT
bool Moonraker_Mqtt::send_to_request(
    const std::string& method,
    const json& params,
    bool need_response,
    std::function<void(const nlohmann::json& response)> callback,
    std::function<void()> timeout_callback)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    wcp_loger.add_log("sending request, method: " + method + ", need response: " + std::to_string(need_response), false, "", "Moonraker_Mqtt", "info");
    
    json body;
    body["jsonrpc"] = "2.0";
    body["method"] = method;

    if (!params.empty()) {
        body["params"] = params;
    }
    

    int64_t seq_id = m_seq_generator.generate_seq_id();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] generated seq id: " << seq_id;
    wcp_loger.add_log("generated seq id: " + std::to_string(seq_id), false, "", "Moonraker_Mqtt", "info");

    if (need_response) {
        if (!add_response_target(seq_id, callback, timeout_callback)) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to add response target";
            wcp_loger.add_log("failed to add response target", false, "", "Moonraker_Mqtt", "error");
            return false;
        }
        body["id"] = seq_id;
    }

    if (m_mqtt_client_tls) {
        std::string main_layer = "+";
    
        m_sn_mtx.lock();
        main_layer = m_sn;
        m_sn_mtx.unlock();

        if (main_layer == "+" || main_layer == "") {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] invalid SN, removing response target";
            wcp_loger.add_log("invalid SN, removing response target", false, "", "Moonraker_Mqtt", "error");
            delete_response_target(seq_id);
            return false;
        }

        std::string topic = main_layer + m_request_topic;
        BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] publishing to topic: " << topic;
        wcp_loger.add_log("publishing to topic: " + topic, false, "", "Moonraker_Mqtt", "info");

        // add time sync fields
        if (time_sync_manager_) {
            time_sync_manager_->addTimeFields(body);
        }

        std::string pub_msg = "success";
        bool res = m_mqtt_client_tls->Publish(topic, body.dump(), 1, pub_msg);
        if (!res) {
            BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] failed to publish request, method: " << method;
            wcp_loger.add_log("failed to publish request, method: " + method, false, "", "Moonraker_Mqtt", "error");
            delete_response_target(seq_id);
        } else {
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] request published successfully, method: " << method;
            wcp_loger.add_log("request published successfully, method: " + method, false, "", "Moonraker_Mqtt", "info");
        }
        return res;

        
    }
    BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] MQTTS client unavailable";
    wcp_loger.add_log("MQTTS client unavailable", false, "", "Moonraker_Mqtt", "error");
    return false;
}

// Register callback for response to a request
bool Moonraker_Mqtt::add_response_target(
    int64_t id,
    std::function<void(const nlohmann::json&)> callback,
    std::function<void()> timeout_callback,
    bool passthrough,
    std::chrono::milliseconds timeout)
{
    return m_request_cb_map.add(
        id,
        RequestCallback(std::move(callback), std::move(timeout_callback), passthrough),
        timeout
    );
}

// Remove registered callback
void Moonraker_Mqtt::delete_response_target(int64_t id) {
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] removing response target, id: " << id;
    wcp_loger.add_log("removing response target, id: " + std::to_string(id), false, "", "Moonraker_Mqtt", "info");
    m_request_cb_map.remove(id);
}

bool Moonraker_Mqtt::check_sn_arrived() {
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    bool result = wait_for_sn();
    BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] checking SN arrival status: " << (result ? "arrived" : "not arrived");
    wcp_loger.add_log("checking SN arrival status: " + std::string((result ? "arrived" : "not arrived")), false, "", "Moonraker_Mqtt", "info");
    return result;
}

// Get and remove callback for a request
std::pair<std::function<void(const json&)>, bool> Moonraker_Mqtt::get_request_callback(int64_t id)
{
    auto request_cb = m_request_cb_map.get_and_remove(id);
    if (!request_cb)
        return {nullptr, false};
    return {request_cb->success_cb, request_cb->passthrough};
}

// Handle incoming MQTTs messages
void Moonraker_Mqtt::on_mqtt_tls_message_arrived(const std::string& topic, const std::string& payload) {
    auto& wcp_loger = GUI::WCP_Logger::getInstance();    
    wcp_loger.add_log("received MQTTS message, topic: " + topic + ", payload length: " + std::to_string(payload.length()), false, "", "Moonraker_Mqtt", "info");
    {
        if (topic.find(m_response_topic) != std::string::npos) {
            size_t pos = topic.find("/response");

            if (pos != std::string::npos) {
                std::string sn = topic.substr(0, pos);                
                wcp_loger.add_log("extracted SN from response topic: " + sn, false, "", "Moonraker_Mqtt", "info");
                m_sn_mtx.lock();
                m_sn = sn;
                m_sn_mtx.unlock();
            }

            on_response_arrived(payload);
        } else if (topic.find(m_status_topic) != std::string::npos) {
            size_t pos = topic.find("/status");

            if (pos != std::string::npos) {
                std::string sn = topic.substr(0, pos);                
                wcp_loger.add_log("extracted SN from status topic: " + sn, false, "", "Moonraker_Mqtt", "info");

                m_sn_mtx.lock();
                m_sn = sn;
                m_sn_mtx.unlock();
            }

            on_status_arrived(payload);
        } else if (topic.find(m_notification_topic) != std::string::npos) {
            
            size_t pos = topic.find("/notification");

            if (pos != std::string::npos) {
                std::string sn = topic.substr(0, pos);                
                wcp_loger.add_log("extracted SN from notification topic: " + sn, false, "", "Moonraker_Mqtt", "info");

                m_sn_mtx.lock();
                m_sn = sn;
                m_sn_mtx.unlock();
            }           
            on_notification_arrived(payload);
        }
        else {
            BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] received message on unknown topic: " << topic;
            wcp_loger.add_log("received message on unknown topic: " + topic, false, "", "Moonraker_Mqtt", "warning");
            return;
        }
    }
}

// Handle incoming MQTT messages
void Moonraker_Mqtt::on_mqtt_message_arrived(const std::string& topic, const std::string& payload)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();    
    wcp_loger.add_log("received MQTT message, topic: " + topic + ", payload length: " + std::to_string(payload.length()), false, "", "Moonraker_Mqtt", "info");
    {
        if (topic.find(m_auth_topic) != std::string::npos) {            
            wcp_loger.add_log("handling auth response message", false, "", "Moonraker_Mqtt", "info");
            on_auth_arrived(payload);
        } else {
            BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] received MQTT message on unknown topic: " << topic;
            wcp_loger.add_log("received MQTT message on unknown topic: " + topic, false, "", "Moonraker_Mqtt", "warning");
            return;
        }
    }
}

// Handle auth messages
void Moonraker_Mqtt::on_auth_arrived(const std::string& payload) {
    json body = json::parse(payload);

    if (time_sync_manager_) {
        time_sync_manager_->updateFromResponse(body);
    }

    if (!body.count("id")) {
        return;
    }

    int64_t id = body["id"].get<int64_t>();
    auto cb = get_request_callback(id).first;
    delete_response_target(id);

    if (!cb) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] auth callback not found, id: " << id;
        return;
    }

    cb(body["result"]);
}

// Handle response messages
void Moonraker_Mqtt::on_response_arrived(const std::string& payload)
{
    json body = json::parse(payload);

    if (time_sync_manager_) {
        time_sync_manager_->updateFromResponse(body);
    }

    if (!body.count("id")) {
        return;
    }

    int64_t id = body["id"].get<int64_t>();
    auto cb_result = get_request_callback(id);
    auto cb         = cb_result.first;
    auto passthrough = cb_result.second;
    delete_response_target(id);

    if (!cb) {
        BOOST_LOG_TRIVIAL(error) << "[Moonraker_Mqtt] response callback not found, id: " << id;
        return;
    }

    // ID 20252025 is reserved for WCP testing only and is not used in production business logic.
    if (passthrough || id == 20252025) {
        cb(body);
    } else {
        json res;
        if (!body.count("result")) {
            if (body.count("error")) {
                res["error"] = body["error"];
            }
        } else {
            res["data"] = body["result"];
        }
        res["method"] = "";
        if (body.count("method")) {
            res["method"] = body["method"];
        }
        cb(res);
    }
}

// Handle status update messages
void Moonraker_Mqtt::on_status_arrived(const std::string& payload)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();    
    wcp_loger.add_log("handling status update message", false, "", "Moonraker_Mqtt", "info");
    {
        json body = json::parse(payload);

        json data;
        if (body.count("params")) {
            data["data"] = body["params"];            
            wcp_loger.add_log("status update contains params", false, "", "Moonraker_Mqtt", "info");
        } else if (body.count("result") && body["result"].count("status")) {
            data["data"] = body["result"]["status"];            
            wcp_loger.add_log("status update contains result status", false, "", "Moonraker_Mqtt", "info");
        } else {            
            wcp_loger.add_log("invalid status update message format", false, "", "Moonraker_Mqtt", "info");
            return;
        }
        
        data["method"] = "";
        if (body.count("method")){
            data["method"] = body["method"];
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] status update contains method: " << body["method"].get<std::string>();
            wcp_loger.add_log("status update contains method: " + body["method"].get<std::string>(), false, "", "Moonraker_Mqtt", "info");
        }

        if (m_status_cbs.empty()) {
            BOOST_LOG_TRIVIAL(info) << "[Moonraker_Mqtt] status callback not set";
            wcp_loger.add_log("status callback not set", false, "", "Moonraker_Mqtt", "info");
            return;
        }
        
        wcp_loger.add_log("invoking status callback", false, "", "Moonraker_Mqtt", "info");
        for (const auto& func : m_status_cbs) {
            func.second(data);
        }

    }
}

// Handle notification messages
void Moonraker_Mqtt::on_notification_arrived(const std::string& payload)
{
    auto& wcp_loger = GUI::WCP_Logger::getInstance();    
    wcp_loger.add_log("handling notification message, payload length: " + std::to_string(payload.length()), false, "", "Moonraker_Mqtt", "info");
    {
        // TODO: add msg notice
        json body = json::parse(payload);
        json data;

        if (body.count("params")) {
            data["data"] = body["params"];
            
            wcp_loger.add_log("status update contains params", false, "", "Moonraker_Mqtt", "info");
        } else if (body.count("result") && body["result"].count("status")) {
            data["data"] = body["result"]["status"];            
            wcp_loger.add_log("status update contains result status", false, "", "Moonraker_Mqtt", "info");
        } else {
            BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] invalid status update message format";
            wcp_loger.add_log("invalid status update message format", false, "", "Moonraker_Mqtt", "warning");
            return;
        }

        data["method"] = "";
        if (body.count("method")) {
            data["method"] = body["method"];            
            wcp_loger.add_log("status update contains method: " + body["method"].get<std::string>(), false, "", "Moonraker_Mqtt", "info");
        }

        if (m_notification_cbs.empty()) {
            return;
        }

        for (const auto& func : m_notification_cbs) {
            func.second(data);
        }
    }
}

bool Moonraker_Mqtt::wait_for_sn(int timeout_seconds)
{   
    auto& wcp_loger = GUI::WCP_Logger::getInstance();    
    wcp_loger.add_log("waiting for SN, timeout: " + std::to_string(timeout_seconds) + "s", false, "", "Moonraker_Mqtt", "info");
    using namespace std::chrono;
    auto start = steady_clock::now();

    while (true) {
        {
            std::lock_guard<std::mutex> lock(m_sn_mtx);
            if (!m_sn.empty()) {                
                wcp_loger.add_log("SN acquired: " + m_sn, false, "", "Moonraker_Mqtt", "info");
                return true;
            }
        }

        auto now = steady_clock::now();
        if (duration_cast<seconds>(now - start).count() >= timeout_seconds) {
            BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] wait for SN timed out";
            wcp_loger.add_log("wait for SN timed out", false, "", "Moonraker_Mqtt", "warning");
            return false;
        }

        std::this_thread::sleep_for(milliseconds(100));
    }
}

void Moonraker_Mqtt::set_connection_lost(std::function<void()> callback) {
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    BOOST_LOG_TRIVIAL(warning) << "[Moonraker_Mqtt] setting connection lost callback";
    wcp_loger.add_log("setting connection lost callback", false, "", "Moonraker_Mqtt", "info");
    if (m_mqtt_client_tls)
        m_mqtt_client_tls->SetConnectionFailureCallback(callback);
}


std::string Moonraker_Mqtt::get_sn() {
    auto& wcp_loger = GUI::WCP_Logger::getInstance();
    std::string res = "";
    m_sn_mtx.lock();
    res = m_sn;
    m_sn_mtx.unlock();
    
    wcp_loger.add_log("get SN: " + res, false, "", "Moonraker_Mqtt", "info");
    return res;
}

} // namespace Slic3r
