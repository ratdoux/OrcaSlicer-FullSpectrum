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
#include <stdlib.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
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
#include <mutex>
#include "common_func/common_func.hpp"
#include <iostream>

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


#ifdef _WIN32
// C-style wrapper function for sentry_init to allow use of __try/__except
// This function must be C-style because __try/__except cannot be used in functions
// that require C++ object unwinding (functions with C++ objects that need destructors)
extern "C" {
    static int safe_sentry_init(sentry_options_t* options) {
        int result = -1;
        __try {
            result = sentry_init(options);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            // Exception occurred during sentry_init
            result = -1;
        }
        return result;
    }
}
#endif

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

void initSentryEx()
{
    sentry_options_t* options = sentry_options_new();
    std::string       dsn = std::string("https://282935326eecb9758e7f84a2ad3ae0ab@o4508125599563776.ingest.us.sentry.io/4510425163956224");
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
        // Use extended path length support for Windows (up to 32767 characters)
        const DWORD MAX_PATH_EXTENDED = 32767;
        wchar_t exeDir[MAX_PATH_EXTENDED];
        DWORD pathLen = ::GetModuleFileNameW(nullptr, exeDir, MAX_PATH_EXTENDED);
        
        // GetModuleFileNameW returns 0 on error, or the number of characters written (excluding null terminator)
        // If return value equals buffer size, the path was truncated
        if (pathLen == 0) {
            // Failed to get module path, use fallback
            DWORD lastError = GetLastError();
            std::cout<< "Failed to get module file name, error: " << lastError;
            handlerDir = "";
            dataBaseDir = "";
        } else if (pathLen >= MAX_PATH_EXTENDED) {
            // Path was truncated, which shouldn't happen with MAX_PATH_EXTENDED
            std::cout<< "Module file path too long or truncated, length: " << pathLen;
            handlerDir = "";
            dataBaseDir = "";
        } else {
            // Ensure null termination (GetModuleFileNameW should do this, but be safe)
            exeDir[pathLen] = L'\0';
            
            std::wstring wsExeDir(exeDir, pathLen);
            size_t nPos = wsExeDir.find_last_of(L'\\');
            
            if (nPos == std::wstring::npos) {
                // No backslash found, use current directory as fallback
                std::cout<< "No backslash found in executable path, using current directory";
                nPos = 0;
            }
            
            // Ensure nPos + 1 doesn't exceed string length
            if (nPos + 1 > wsExeDir.length()) {
                std::cout<< "Invalid path position, using full path";
                nPos = wsExeDir.length();
            }
            
            std::wstring wsDmpDir = wsExeDir.substr(0, nPos + 1);
            std::wstring desDir   = wsDmpDir + L"crashpad_handler.exe";
            wsDmpDir += L"dump";

            auto wstringTostring = [](const std::wstring& wTmpStr) -> std::string {
                if (wTmpStr.empty())
                    return std::string();
                    
                int len = WideCharToMultiByte(CP_UTF8, 0, wTmpStr.c_str(), -1, nullptr, 0, nullptr, nullptr);
                if (len <= 0) {
                    std::cout<< "WideCharToMultiByte failed, error: " << GetLastError();
                    return std::string();
                }

                // Allocate buffer with size len (includes null terminator)
                std::string desStr;
                desStr.resize(len - 1); // Reserve space excluding null terminator
                int result = WideCharToMultiByte(CP_UTF8, 0, wTmpStr.c_str(), -1, &desStr[0], len, nullptr, nullptr);
                if (result == 0 || result != len) {
                    std::cout<< "WideCharToMultiByte conversion failed, error: " << GetLastError();
                    return std::string();
                }
                
                // Remove null terminator if present (safely check before accessing)
                if (!desStr.empty() && desStr.back() == '\0')
                    desStr.pop_back();

                return desStr;
            };

            handlerDir = wstringTostring(desDir);
        }

        // Get LocalAppData folder path
        PWSTR   pszPath = nullptr;
        char*   path    = nullptr;
        size_t  pathLength = 0;
        HRESULT hr      = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &pszPath);

        if (SUCCEEDED(hr) && pszPath != nullptr) {
            // Calculate required buffer size first
            size_t wcsLen = wcslen(pszPath);
            if (wcsLen > 0 && wcsLen < SIZE_MAX / 3) { // Check for overflow
                // Allocate buffer with extra space for safety
                size_t requiredSize = wcsLen * 3 + 1; // UTF-8 can be up to 3 bytes per wchar
                path = new (std::nothrow) char[requiredSize]();
                
                if (path != nullptr) {
                    errno_t err = wcstombs_s(&pathLength, path, requiredSize, pszPath, _TRUNCATE);
                    if (err != 0) {
                        std::cout<< "wcstombs_s failed, error: " << err;
                        delete[] path;
                        path = nullptr;
                    }
                } else {
                    std::cout<< "Failed to allocate memory for path conversion";
                }
            } else if (wcsLen == 0) {
                std::cout<< "SHGetKnownFolderPath returned empty path";
            } else {
                std::cout<< "Path length overflow detected: " << wcsLen;
            }
            // Always free the path returned by SHGetKnownFolderPath
            CoTaskMemFree(pszPath);
            pszPath = nullptr;
        } else {
            std::cout<< "SHGetKnownFolderPath failed, hr: " << std::hex << hr;
            // Ensure pszPath is freed even on failure (though it should be nullptr)
            if (pszPath != nullptr) {
                CoTaskMemFree(pszPath);
                pszPath = nullptr;
            }
        }

        if (path != nullptr) {
            std::string filePath = path;
            std::string appName  = "\\" + std::string("Snapmaker_Orca\\");
            dataBaseDir          = filePath + appName;
            delete[] path;
            path = nullptr;
        } else {
            // Fallback: use temp directory
            char tempPath[MAX_PATH];
            if (GetTempPathA(MAX_PATH, tempPath) != 0) {
                dataBaseDir = std::string(tempPath) + "Snapmaker_Orca\\";
                std::cout<< "Using temp directory as fallback for Sentry data: " << dataBaseDir;
            } else {
                dataBaseDir = "";
                std::cout<< "Failed to get temp path, Sentry data directory will be empty";
            }
        }
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

        //sentry_options_set_environment(options, "develop");
        sentry_options_set_environment(options, "Release");

        sentry_options_set_auto_session_tracking(options, 0);
        sentry_options_set_symbolize_stacktraces(options, 1);
        sentry_options_set_on_crash(options, on_crash_callback, NULL);

        sentry_options_set_sample_rate(options, 1.0);
        sentry_options_set_traces_sample_rate(options, 1.0);

        sentry_options_set_enable_logs(options, 1);
        sentry_options_set_before_send_log(options, before_send_log, NULL);
        sentry_options_set_logs_with_attributes(options, true);

        // Set release version for symbolication
        sentry_options_set_release(options, Snapmaker_VERSION);
        bool init_success = false;
        
#ifdef _WIN32
        // Use C-style wrapper function to safely call sentry_init with SEH exception handling
        int result = safe_sentry_init(options);
        std::cout << "sentry_init returned: " << result << std::endl;
        if (result == 0) {
            init_success = true;
            std::cout << "sentry_init succeeded, init_success set to true" << std::endl;
        } else {
            std::cout << "Error: sentry_init failed or exception occurred, Sentry initialization failed (result=" << result << ")" << std::endl;
            // Exception occurred or sentry_init returned error, sentry_init was not successfully called
            // so we need to free options
            sentry_options_free(options);
            set_sentry_flags(false);
            return; // Exit early if initialization fails
        }
#else
        int result = sentry_init(options);
        std::cout << "sentry_init returned: " << result << std::endl;
        if (result == 0) {
            init_success = true;
            std::cout << "sentry_init succeeded, init_success set to true" << std::endl;
        } else {
            std::cout << "Warning: sentry_init returned non-zero: " << result << std::endl;
        }
#endif

        // Start session and set tags only if initialization succeeded
        if (init_success) {
            std::cout << "Starting Sentry session and setting flags..." << std::endl;
            sentry_start_session();
            set_sentry_flags(true);            
            
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
        } else {            
            set_sentry_flags(false);            
        }
    }
}

void exitSentryEx()
{
    if (get_sentry_flags()) {
        sentry_close();
        set_sentry_flags(false);
    }
}
void sentryReportLogEx(SENTRY_LOG_LEVEL   logLevel,
                       const std::string& logContent,
                       const std::string& funcModule,
                       const std::string& logTagKey,
                       const std::string& logTagValue,
                       const std::string& logTraceId)
{
    // Check if Sentry is initialized before using it
    if (!get_sentry_flags()) {
        return;
    }
    
    if (!get_privacy_policy()) {
        return;
    }

    sentry_level_t sentry_msg_level;
    sentry_value_t tags = sentry_value_new_object();

    if (!funcModule.empty()) {
        sentry_value_t attr = sentry_value_new_attribute(sentry_value_new_string(funcModule.c_str()), NULL);
        sentry_value_set_by_key(tags, "function_module", attr);
    }

    if (!logTraceId.empty()) {
        sentry_value_t attr = sentry_value_new_attribute(sentry_value_new_string(logTraceId.c_str()), NULL);
        sentry_value_set_by_key(tags, "snapmaker_trace_id", attr);
    }

    if (!logTagKey.empty()) {
        sentry_value_t attr = sentry_value_new_attribute(sentry_value_new_string(logTagValue.c_str()), NULL);
        sentry_value_set_by_key(tags, logTagKey.c_str(), attr);
    }

    sentry_value_t attr = sentry_value_new_attribute(sentry_value_new_string(Snapmaker_VERSION), NULL);
    sentry_value_set_by_key(tags, "snapmaker_version", attr);

    std::string flutterVersion = common::get_flutter_version();
    if (!flutterVersion.empty()) {
        sentry_value_t attr = sentry_value_new_attribute(sentry_value_new_string(flutterVersion.c_str()), NULL);
        sentry_value_set_by_key(tags, "flutter_version", attr);
    }
    std::string pcName = common::get_pc_name();
    if (!pcName.empty()) {
        sentry_value_t attr = sentry_value_new_attribute(sentry_value_new_string(pcName.c_str()), NULL);
        sentry_value_set_by_key(tags, "pc_name", attr);
    }
    static std::string machineID = "";
    if (machineID.empty())
        machineID = common::getMachineId();

    if (!machineID.empty()) {
        sentry_value_t attr = sentry_value_new_attribute(sentry_value_new_string(machineID.c_str()), NULL);
        sentry_value_set_by_key(tags, "machine_id", attr);
    }

    static std::string currentLanguage = "";
    if (currentLanguage.empty())
        currentLanguage = common::getLanguage();

    if (!currentLanguage.empty()) {
        sentry_value_t attr = sentry_value_new_attribute(sentry_value_new_string(currentLanguage.c_str()), NULL);
        sentry_value_set_by_key(tags, "current_language", attr);
    }

    static std::string localArea = "";
    if (localArea.empty())
        localArea = common::getLocalArea();

    if (!localArea.empty()) 
    {
        sentry_value_t attr = sentry_value_new_attribute(sentry_value_new_string(localArea.c_str()), NULL);
        sentry_value_set_by_key(tags, "local_area", attr);
    }
    switch (logLevel) {
    case SENTRY_LOG_TRACE: {
        sentry_msg_level    = SENTRY_LEVEL_TRACE;
        sentry_value_t attr = sentry_value_new_attribute(sentry_value_new_string("snapmaker_bury_point"), NULL);
        sentry_value_set_by_key(tags, BURY_POINT, attr);
        sentry_log_trace(logContent.c_str(), tags);
    } break;
    case SENTRY_LOG_DEBUG: {
        sentry_msg_level = SENTRY_LEVEL_DEBUG;
        sentry_log_debug(logContent.c_str(), tags);
    } break;
    case SENTRY_LOG_INFO: {
        sentry_msg_level = SENTRY_LEVEL_INFO;
        sentry_log_info(logContent.c_str(), tags);
    } break;
    case SENTRY_LOG_WARNING: {
        sentry_msg_level = SENTRY_LEVEL_WARNING;
        sentry_log_warn(logContent.c_str(), tags);
    } break;
    case SENTRY_LOG_ERROR:
    {
        sentry_msg_level = SENTRY_LEVEL_ERROR;
        sentry_log_error(logContent.c_str(), tags);
    }
        break;
    case SENTRY_LOG_FATAL: 
    {
        sentry_msg_level = SENTRY_LEVEL_FATAL;
        sentry_log_fatal(logContent.c_str(), tags);
    }
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

