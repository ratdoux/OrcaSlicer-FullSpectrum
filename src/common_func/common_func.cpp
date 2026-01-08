#include "common_func.hpp"
#include <boost/asio/ip/host_name.hpp>

#ifdef _WIN32
#include <windows.h>
#include <Shlobj.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#elif __APPLE__
#include <stdlib.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/network/IONetworkInterface.h>
#include <IOKit/network/IONetworkController.h>
#include <AvailabilityMacros.h>
#endif

#include <fstream>
#include <nlohmann/json.hpp>

namespace common
{
    std::string get_pc_name()
    { 
        return boost::asio::ip::host_name();
    }

    std::string getMachineId()
    {
    std::string machineId = std::string();
#ifdef _WIN32

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

    HKEY key = NULL;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        wchar_t buffer[1024];
        memset(buffer, 0, sizeof(wchar_t) * 1024);
        DWORD size = sizeof(buffer);
        bool  ok   = (RegQueryValueEx(key, L"MachineGuid", NULL, NULL, (LPBYTE) buffer, &size) == ERROR_SUCCESS);
        RegCloseKey(key);
        if (ok) {
            machineId = wstringTostring(buffer);
        }
    }

#elif __APPLE__
    FILE* fp = NULL;
    char  buffer[1024];

    memset(buffer, 0, 1024);
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOPlatformExpertDevice"));
    CFStringRef  strRef  = (CFStringRef) IORegistryEntryCreateCFProperty(service, CFSTR(kIOPlatformUUIDKey), kCFAllocatorDefault, 0);
    CFStringGetCString(strRef, buffer, 1024, kCFStringEncodingMacRoman);
    machineId = buffer;

#endif // _WIN32
    return machineId;
    }

    std::string get_flutter_version()
    {

            std::string versionFilePath = "";

#ifdef _WIN32
            PWSTR   pszPath = nullptr;
            char*   path    = new char[MAX_PATH]();
            size_t  pathLength = 0;
            HRESULT hr         = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &pszPath);
            if (SUCCEEDED(hr)) {
                wcstombs_s(&pathLength, path, MAX_PATH, pszPath, MAX_PATH);
                CoTaskMemFree(pszPath);
            }                      
            
            std::string filePath = path;
            versionFilePath      = filePath + "\\" + std::string("Snapmaker_Orca\\web\\flutter_web\\version.json");

            delete[] path;
#elif __APPLE__
            const char* home_env = getenv("HOME");
            versionFilePath      = home_env;
            versionFilePath      = versionFilePath + "/Library/Application Support/Snapmaker_Orca/web/flutter_web/version.json";
#else

#endif

            std::ifstream json_file(versionFilePath);
            if (!json_file.is_open()) {
                std::ifstream json_file(versionFilePath);                
                return "";
            }
            nlohmann::json json_data;
            json_file >> json_data;
            std::string str_version = json_data.value("version", "");
            std::string str_build_number = json_data.value("build_number", "");

            std::string flutter_version = std::string("flutter_version: ") + str_version + std::string("  ") + std::string("build_number: ") +
                              str_build_number;
           
            return flutter_version;
    }


    std::string getLocalArea() 
    { 
        std::string localArea = "";
        std::string cfgfile = "";
#ifdef _WIN32

        PWSTR   pszPath = nullptr;
        char*   path    = new char[MAX_PATH]();
        size_t  pathLength = 0;
        HRESULT hr         = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &pszPath);
        if (SUCCEEDED(hr)) {
            wcstombs_s(&pathLength, path, MAX_PATH, pszPath, MAX_PATH);
            CoTaskMemFree(pszPath);
        } 

        std::string filePath = path;
        cfgfile              = filePath + "\\" + std::string("Snapmaker_Orca\\Snapmaker_Orca.conf");
        delete[] path;

#elif __APPLE__
        const char* home_env = getenv("HOME");
        cfgfile              = home_env;
        cfgfile              = versionFilePath + "/Library/Application Support/Snapmaker_Orca/Snapmaker_Orca.conf";
#else

#endif
        std::ifstream json_file(cfgfile);
        if (!json_file.is_open()) {
            std::ifstream json_file(cfgfile);
            return "";
        }

        nlohmann::json json_data;
        json_file >> json_data;

        auto dataObj    = json_data.value("app", nlohmann::json::object());
        localArea = dataObj.value("region", "");

        return localArea;
    }

    std::string getLanguage() 
    {
        std::string localLanguage = "";

        std::string cfgfile       = "";
#ifdef _WIN32

        PWSTR   pszPath = nullptr;
        char*   path    = new char[MAX_PATH]();
        size_t  pathLength = 0;
        HRESULT hr         = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &pszPath);
        if (SUCCEEDED(hr)) {
            wcstombs_s(&pathLength, path, MAX_PATH, pszPath, MAX_PATH);
            CoTaskMemFree(pszPath);
        } 

        std::string filePath = path;
        cfgfile              = filePath + "\\" + std::string("Snapmaker_Orca\\Snapmaker_Orca.conf");
        delete[] path;

#elif __APPLE__
        const char* home_env = getenv("HOME");
        cfgfile              = home_env;
        cfgfile              = versionFilePath + "/Library/Application Support/Snapmaker_Orca/Snapmaker_Orca.conf";
#else

#endif
        std::ifstream json_file(cfgfile);
        if (!json_file.is_open()) {
            std::ifstream json_file(cfgfile);
            return "";
        }

        nlohmann::json json_data;
        json_file >> json_data;

       auto dataObj  = json_data.value("app", nlohmann::json::object());
       localLanguage = dataObj.value("language", "");

       return localLanguage;
    }
}