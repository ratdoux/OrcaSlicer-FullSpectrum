/**
 * @file SentryWrapper.cpp
 * @brief Sentry crash reporting wrapper implementation for cross-platform support.
 * 
 * This implementation provides a unified API for Sentry integration.
 * When SLIC3R_SENTRY is not defined, all functions become no-ops.
 */

#include "SentryWrapper.hpp"

#ifdef SLIC3R_SENTRY
#include "sentry.h"
#endif

#ifdef _WIN32
#include <Windows.h>
#include <shlobj.h>
#endif

#ifdef __APPLE__
#include <unistd.h>
#include <mach-o/dyld.h>
#include <libgen.h>
#include <string.h>
#endif

#include <cstdlib>
#include <atomic>
#include <random>
#include "common_func/common_func.hpp"

namespace Slic3r {

#ifdef SLIC3R_SENTRY

#define SENTRY_EVENT_TRACE "trace"
#define SENTRY_EVENT_DEBUG "info"
#define SENTRY_EVENT_INFO "debug"
#define SENTRY_EVENT_WARNING "warning"
#define SENTRY_EVENT_ERROR "error"
#define SENTRY_EVENT_FATAL "fatal"

#define MACHINE_MODULE "Moonraker_Mqtt"

#define SENTRY_KEY_LEVEL "level"



static sentry_value_t on_crash_callback(const sentry_ucontext_t* uctx, sentry_value_t event, void* closure)
{
    (void) uctx;
    (void) closure;

    // tell the backend to retain the event
    return event;
}

 static sentry_value_t before_send_log(sentry_value_t log, void* user_dataa)
{ 
     return log;
 }

static sentry_value_t before_send(sentry_value_t event, void* hint, void* data)
{
    sentry_value_t level_val = sentry_value_get_by_key(event, SENTRY_KEY_LEVEL);
    std::string    levelName  = sentry_value_as_string(level_val);

    std::string    eventLevel = sentry_value_as_string(sentry_value_get_by_key(event, SENTRY_KEY_LEVEL));

    //module name
    sentry_value_t moduleValue = sentry_value_get_by_key(event, "logger");
    std::string    moduleName  = sentry_value_as_string(moduleValue);        

    if (MACHINE_MODULE == moduleName)
    {
        srand((unsigned int) time(0));
        int random_num = rand() % 100;
        int randNumber = rand() % 100 + 1;
        if (randNumber < 85) 
        {
            sentry_value_decref(event);
            return sentry_value_new_null();
        }
        else
        {
            return event;
        }
    }

    if (!get_privacy_policy() && levelName == SENTRY_EVENT_TRACE) {
        
        sentry_value_decref(event);
        return sentry_value_new_null();
    }

    if (SENTRY_EVENT_FATAL == eventLevel ||
        SENTRY_EVENT_ERROR == eventLevel || 
        SENTRY_EVENT_TRACE == eventLevel)
    {
        return event;
    } 
    else if (SENTRY_EVENT_WARNING == eventLevel) 
    {
        srand((unsigned int) time(0));
        int random_num = rand() % 100;
        int randNumber = rand() % 100 + 1;
        if (randNumber > 5) 
        {
            sentry_value_decref(event);
            return sentry_value_new_null();
        } 
        else 
        {
            return event;
        }
    }

    //info trace debug not report
    sentry_value_decref(event);        
    return sentry_value_new_null();

}

void initSentryEx()
{
    sentry_options_t* options = sentry_options_new();
    std::string       dsn = std::string("https://c74b617c2aedc291444d3a238d23e780@o4508125599563776.ingest.us.sentry.io/4510425163956224");
    {
        sentry_options_set_dsn(options, dsn.c_str());
        std::string handlerDir  = "";
        std::string dataBaseDir = "";

#ifdef __APPLE__

        char     exe_path[PATH_MAX] = {0};
        uint32_t buf_size           = PATH_MAX;

        if (_NSGetExecutablePath(exe_path, &buf_size) != 0) {
            throw std::runtime_error("Buffer too small for executable path");
        }

        // Get the directory containing the executable, not the executable path itself
        // Use dirname() to get parent directory (need to copy string as dirname may modify it)
        char exe_path_copy[PATH_MAX];
        strncpy(exe_path_copy, exe_path, PATH_MAX);
        char* exe_dir = dirname(exe_path_copy);
        handlerDir = std::string(exe_dir) + "/crashpad_handler";

        const char* home_env = getenv("HOME");

        dataBaseDir = home_env;
        dataBaseDir = dataBaseDir + "/Library/Application Support/Snapmaker_Orca/SentryData";
#elif _WIN32
        wchar_t exeDir[MAX_PATH];
        ::GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
        std::wstring wsExeDir(exeDir);
        int          nPos     = wsExeDir.find_last_of('\\');
        std::wstring wsDmpDir = wsExeDir.substr(0, nPos + 1);
        std::wstring desDir   = wsDmpDir + L"crashpad_handler.exe";
        wsDmpDir += L"dump";

        auto wstringTostring = [](std::wstring wTmpStr) -> std::string {
            std::string resStr = std::string();
            int         len    = WideCharToMultiByte(CP_UTF8, 0, wTmpStr.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (len <= 0)
                return std::string();

            std::string desStr(len, 0);
            WideCharToMultiByte(CP_UTF8, 0, wTmpStr.c_str(), -1, &desStr[0], len, nullptr, nullptr);
            resStr = desStr;

            return resStr;
        };

        handlerDir = wstringTostring(desDir);

        wchar_t appDataPath[MAX_PATH] = {0};
        auto    hr                    = SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appDataPath);
        char*   path                  = new char[MAX_PATH];
        size_t  pathLength;
        wcstombs_s(&pathLength, path, MAX_PATH, appDataPath, MAX_PATH);
        std::string filePath = path;
        std::string appName  = "\\" + std::string("Snapmaker_Orca\\");
        dataBaseDir          = filePath + appName;
#endif

        if (!handlerDir.empty())
            sentry_options_set_handler_path(options, handlerDir.c_str());

        if (!dataBaseDir.empty())
            sentry_options_set_database_path(options, dataBaseDir.c_str());

#if defined(_DEBUG) || !defined(NDEBUG)
        sentry_options_set_debug(options, 1);
#else
        sentry_options_set_debug(options, 0);
#endif

        sentry_options_set_environment(options, "develop");
        //sentry_options_set_environment(options, "Release");

        sentry_options_set_auto_session_tracking(options, 0);
        sentry_options_set_symbolize_stacktraces(options, 1);
        sentry_options_set_on_crash(options, on_crash_callback, NULL);
        sentry_options_set_before_send(options, before_send, NULL);

        sentry_options_set_sample_rate(options, 1.0);
        sentry_options_set_traces_sample_rate(options, 1.0);

        sentry_options_set_enable_logs(options, 1);
        sentry_options_set_before_send_log(options, before_send_log, NULL);
        sentry_options_set_logs_with_attributes(options, true);

        sentry_init(options);
        sentry_start_session();

        sentry_set_tag("snapmaker_version", Snapmaker_VERSION);

        std::string flutterVersion = common::get_flutter_version();
        if (!flutterVersion.empty())
            sentry_set_tag("flutter_version", flutterVersion.c_str());

        std::string machineID = common::getMachineId();
        if (!machineID.empty())
            sentry_set_tag("machine_id", machineID.c_str());

        std::string pcName = common::get_pc_name();
        if (!pcName.empty())
            sentry_set_tag("pc_name", pcName.c_str());
    }
}

void exitSentryEx()
{ 
    sentry_close();
}
void sentryReportLogEx(SENTRY_LOG_LEVEL   logLevel,
                         const std::string& logContent,
                         const std::string& funcModule,
                         const std::string& logTagKey,
                         const std::string& logTagValue,
                         const std::string& logTraceId)
{
    if (!get_privacy_policy()) {
        return;
    }

    sentry_level_t sentry_msg_level;
    sentry_value_t tags = sentry_value_new_object();

     if (!funcModule.empty())
        sentry_value_set_by_key(tags, "function_module", sentry_value_new_string(funcModule.c_str()));

    if (!logTraceId.empty())
        sentry_value_set_by_key(tags, "snapmaker_trace_id", sentry_value_new_string(logTraceId.c_str()));

    if (!logTagKey.empty())
        sentry_value_set_by_key(tags, logTagKey.c_str(), sentry_value_new_string(logTagValue.c_str()));

    sentry_value_set_by_key(tags, "snapmaker_version", sentry_value_new_string(Snapmaker_VERSION));

    std::string flutterVersion = common::get_flutter_version();
    if (!flutterVersion.empty())
        sentry_value_set_by_key(tags, "flutter_version", sentry_value_new_string(flutterVersion.c_str()));

    std::string pcName = common::get_pc_name();
    if (!pcName.empty())
        sentry_value_set_by_key(tags, "pc_name", sentry_value_new_string(pcName.c_str()));

    std::string machineID = common::getMachineId();
    if (!machineID.empty())
        sentry_value_set_by_key(tags, "machine_id", sentry_value_new_string(machineID.c_str()));

    std::string currentLanguage = common::getLanguage();
    if (!currentLanguage.empty())
        sentry_value_set_by_key(tags, "current_language", sentry_value_new_string(currentLanguage.c_str()));

    std::string localArea = common::getLocalArea();
    if (!localArea.empty())
        sentry_value_set_by_key(tags, "local_area", sentry_value_new_string(localArea.c_str()));

    switch (logLevel) {
    case SENTRY_LOG_TRACE:
        sentry_msg_level = SENTRY_LEVEL_TRACE;
        sentry_value_set_by_key(tags, BURY_POINT, sentry_value_new_string("snapmaker_bury_point"));
        sentry_log_trace(logContent.c_str(), tags, 3);
        break;
    case SENTRY_LOG_DEBUG:
        sentry_msg_level = SENTRY_LEVEL_DEBUG;
        sentry_log_debug(logContent.c_str(), tags, 3);
        break;
    case SENTRY_LOG_INFO:
        sentry_msg_level = SENTRY_LEVEL_INFO;
        sentry_log_info(logContent.c_str(), tags, 3);
        break;
    case SENTRY_LOG_WARNING:
        sentry_msg_level = SENTRY_LEVEL_WARNING;
        sentry_log_warn(logContent.c_str(), tags, 3);
        break;
    case SENTRY_LOG_ERROR:
        sentry_msg_level = SENTRY_LEVEL_ERROR;
        sentry_log_error(logContent.c_str(), tags, 3);
        break;
    case SENTRY_LOG_FATAL:
        sentry_msg_level = SENTRY_LEVEL_FATAL;
        sentry_log_fatal(logContent.c_str(), tags, 3);
        break;
    default: return;
    }
}


#else // SLIC3R_SENTRY not defined - provide no-op implementations


#endif // SLIC3R_SENTRY

void initSentry()
{
#ifdef SLIC3R_SENTRY
    initSentryEx();
#endif
}

void exitSentry()
{
#ifdef SLIC3R_SENTRY
    exitSentryEx();
#endif
}
void sentryReportLog(SENTRY_LOG_LEVEL   logLevel,
    const std::string& logContent,
    const std::string& funcModule,
    const std::string& logTagKey,
    const std::string& logTagValue,
    const std::string& logTraceId)
{
#ifdef SLIC3R_SENTRY
    sentryReportLogEx(logLevel, logContent, funcModule, logTagKey, logTagValue, logTraceId);
#endif
}

void set_sentry_tags(const std::string& tag_key, const std::string& tag_value)
{
#ifdef SLIC3R_SENTRY
    if (!tag_key.empty())
        sentry_set_tag(tag_key.c_str(), tag_value.c_str());
#endif
}

} // namespace Slic3r

