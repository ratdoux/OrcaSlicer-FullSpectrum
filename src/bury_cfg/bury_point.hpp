#ifndef _BURY_POINT_hpp_
#define _BURY_POINT_hpp_
#include <string>
#include <iostream>
#include <atomic>

#ifdef _WIN32
    #ifdef BURY_EXPORTS
        #define BURY_API __declspec(dllexport)
    #else
        #define BURY_API __declspec(dllimport)
    #endif
#else
    #define BURY_API  extern
#endif

#define BURY_POINT "bury_point"

#define BP_START_SOFT "bury_point_start_soft"
#define BP_SOFT_WORKS_TIME "bury_point_soft_works_time"

#define BP_DEIVCE_CONNECT "bury_point_device_connect"
#define BP_CONNECT_DEVICE_ID "bury_point_device_id"
#define BP_CONNECT_NET_TYPE "bury_point_net_type"

#define BP_LOGIN "bury_point_login"
#define BP_LOGIN_USER_ID "bury_point_user_id"
#define BP_LOGIN_HTTP_CODE "bury_point_http_code"

#define BP_VIDEO_START "bury_point_video_start"
#define BP_VIDEO_STATUS "bury_point_video_status"

#define BP_DEVICE_ERROR "bury_point_device_error"
#define BP_DEVICE_ERROR_STATUS "bury_point_error_status"

#define BP_UPLOAD "bury_point_upload"

#define BP_UPLOAD_AND_PRINT "bury_point_upload_and_print"

#define BP_COLOR_PAINTING "bury_point_color_painting"

#define BP_VIDEO_ABNORMAL "bury_point_video_abnormal"

#define BP_SLICE_DURATION "bury_point_slice_duration"
#define BP_SLICE_DURATION_TIME "bury_point_slice_duration_time"

#define BP_WEB_VIEW "bury_point_webview"

#define BP_LOCAL_SERVER "bury_point_local_server"
#define BP_LOCAL_SERVER_STATUS "bury_point_local_server_status"
#define BP_LOCAL_SERVER_ERROR_CODE "bury_point_local_server_error_code"

#define BP_LOCAL_SERVER "bury_point_local_server"
#define BP_LOCAL_SERVER_ERR_CODE "bury_point_local_server_err_code"


//device connect error info
#define DEVICE_CONNECT_ERR "error_device_connect"
#define DEVICE_SUBSCRIBE_ERR "error_device_subscribe"
#define DEVICE_PBLISH_ERR "error_device_publish"
#define DEVICE_SET_ENGINE_ERR "error_device_set_engine"


//webview bury point

    BURY_API bool get_sentry_flags();
    BURY_API void set_sentry_flags(bool flags);

    BURY_API bool               get_privacy_policy();
    BURY_API void               set_privacy_policy(bool isAgree);
    BURY_API std::string get_timestamp_seconds();
    BURY_API long long   get_time_timestamp();
    BURY_API std::string get_works_time(const long long& timestamp);

#endif