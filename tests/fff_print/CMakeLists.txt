get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests 
	${_TEST_NAME}_tests.cpp
	test_data.cpp
	test_data.hpp
	test_extrusion_entity.cpp
	test_fill.cpp
	test_flow.cpp
	test_gcode.cpp
	test_gcodewriter.cpp
	test_model.cpp
	test_print.cpp
	test_printgcode.cpp
	test_printobject.cpp
	test_skirt_brim.cpp
	test_support_material.cpp
	test_bed_temperature.cpp
	test_trianglemesh.cpp
	)
find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(libnoise REQUIRED)
find_package(Qhull 7.2 QUIET)
if(UNIX AND NOT APPLE)
    target_link_options(${_TEST_NAME}_tests PRIVATE "-Wl,--no-as-needed")
endif()
target_link_libraries(${_TEST_NAME}_tests PRIVATE ${TEST_LINK_LIBS})
# libslic3r_cgal links CGAL privately, so the GMP/MPFR libraries are needed at link time.
# They are not propagated to this test target; find them under the deps prefix or the
# system paths, and only add them when found to avoid linking a NOTFOUND placeholder.
find_library(GMP_LIB  NAMES libgmp-10  gmp  PATHS "${CMAKE_PREFIX_PATH}/lib")
find_library(MPFR_LIB NAMES libmpfr-4 mpfr PATHS "${CMAKE_PREFIX_PATH}/lib")
if (GMP_LIB AND MPFR_LIB)
    target_link_libraries(${_TEST_NAME}_tests PRIVATE ${GMP_LIB} ${MPFR_LIB})
endif()
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
