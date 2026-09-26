# slic3rutils tests require libslic3r_gui (contains Http.cpp).
# Skip entirely when GUI is disabled.
if (NOT SLIC3R_GUI)
    return()
endif()

get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests_main.cpp
    test_gradient_preview.cpp
    )

# Match the wxWidgets headers and compile flags selected by the application.
get_directory_property(_gui_wx_includes DIRECTORY "${CMAKE_SOURCE_DIR}/src" DEFINITION wxWidgets_INCLUDE_DIRS)
get_directory_property(_gui_wx_definitions DIRECTORY "${CMAKE_SOURCE_DIR}/src" DEFINITION wxWidgets_DEFINITIONS)
get_directory_property(_gui_wx_flags DIRECTORY "${CMAKE_SOURCE_DIR}/src" DEFINITION wxWidgets_CXX_FLAGS)
target_include_directories(${_TEST_NAME}_tests SYSTEM PRIVATE ${_gui_wx_includes})
target_compile_definitions(${_TEST_NAME}_tests PRIVATE ${_gui_wx_definitions} wxDEBUG_LEVEL=0)
separate_arguments(_gui_wx_flags NATIVE_COMMAND "${_gui_wx_flags}")
target_compile_options(${_TEST_NAME}_tests PRIVATE ${_gui_wx_flags})

find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(libnoise REQUIRED)
find_package(Qhull 7.2 QUIET)
if(UNIX AND NOT APPLE)
    target_link_options(${_TEST_NAME}_tests PRIVATE "-Wl,--no-as-needed")
endif()
if(TARGET libslic3r_gui)
    target_link_libraries(${_TEST_NAME}_tests PRIVATE libslic3r_gui ${TEST_LINK_LIBS})
else()
    target_link_libraries(${_TEST_NAME}_tests PRIVATE ${TEST_LINK_LIBS})
endif()
if (MSVC)
    target_link_libraries(${_TEST_NAME}_tests PRIVATE Setupapi.lib)
endif ()
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

if (WIN32)
    if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
        Snapmaker_Orca_copy_dlls(COPY_DLLS "Debug" "d" output_dlls_Debug)
    elseif("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo")
        Snapmaker_Orca_copy_dlls(COPY_DLLS "RelWithDebInfo" "" output_dlls_Release)
    else()
        Snapmaker_Orca_copy_dlls(COPY_DLLS "Release" "" output_dlls_Release)
    endif()
endif()

catch_discover_tests(${_TEST_NAME}_tests TEST_PREFIX "${_TEST_NAME}: " EXTRA_ARGS ${CATCH_EXTRA_ARGS})
# add_test(${_TEST_NAME}_tests ${_TEST_NAME}_tests "${CATCH_EXTRA_ARGS} exclude:[NotWorking]")
