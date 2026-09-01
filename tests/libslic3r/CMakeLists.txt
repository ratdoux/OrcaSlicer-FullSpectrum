get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

add_executable(${_TEST_NAME}_tests 
	${_TEST_NAME}_tests.cpp
	test_3mf.cpp
	test_aabbindirect.cpp
	test_clipper_offset.cpp
	test_clipper_utils.cpp
	test_config.cpp
	test_custom_gcode.cpp
	test_elephant_foot_compensation.cpp
	test_geometry.cpp
	test_full_spectrum_ks_pair_residual.cpp
	test_mixed_filament.cpp
	test_obj_image_map.cpp
	test_mixed_filament_color_golden.cpp
	test_triangle_selector.cpp
	test_local_z_order_optimizer.cpp
	test_placeholder_parser.cpp
	test_polygon.cpp
	test_profile_load_util.cpp
	test_mutable_polygon.cpp
	test_mutable_priority_queue.cpp
	test_stl.cpp
	test_meshboolean.cpp
	test_marchingsquares.cpp
	test_timeutils.cpp
	test_voronoi.cpp
    test_optimizers.cpp
    test_png_io.cpp
	test_sswcp_protocol.cpp
    test_indexed_triangle_set.cpp
    ../libnest2d/printer_parts.cpp
	)

if (TARGET OpenVDB::openvdb)
    target_sources(${_TEST_NAME}_tests PRIVATE test_hollowing.cpp)
    # libslic3r links OpenVDB PRIVATE, so its symbols don't propagate to test
    # executables. test_hollowing.cpp references OpenVDB directly and must link
    # it explicitly here.
    target_link_libraries(${_TEST_NAME}_tests PRIVATE OpenVDB::openvdb)
endif()
    
find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(libnoise REQUIRED)
find_package(Qhull 7.2 QUIET)
if(UNIX AND NOT APPLE)
    target_link_options(${_TEST_NAME}_tests PRIVATE "-Wl,--no-as-needed")
endif()
target_link_libraries(${_TEST_NAME}_tests PRIVATE ${TEST_LINK_LIBS})
if (WIN32)
    target_link_libraries(${_TEST_NAME}_tests PRIVATE bcrypt.lib)
endif()
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
# add_test(${_TEST_NAME}_tests ${_TEST_NAME}_tests ${CATCH_EXTRA_ARGS})
