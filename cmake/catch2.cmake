# cmake/catch2.cmake — FetchContent configuration for Catch2 v3
#
# This module downloads Catch2 v3.x at configure time and makes the following
# targets available:
#   Catch2::Catch2          — header-only library (no main)
#   Catch2::Catch2WithMain  — header-only library with main()
#
# It also provides the catch_discover_tests() CMake function for CTest
# integration, replacing the old vendored cmake/modules/Catch2/ scripts.
#
# Usage (from tests/CMakeLists.txt):
#   include(../cmake/catch2.cmake)
#   target_link_libraries(test_common INTERFACE Catch2::Catch2)
#
# Offline / air-gapped builds
# ---------------------------
# The clone happens once at configure time (only when BUILD_TESTS=ON) and is
# cached under <build-dir>/_deps/catch2-*. Fresh build directories need
# network access unless one of these built-in FetchContent overrides is used:
#   - Point FetchContent at a local checkout instead of downloading:
#       cmake -DFETCHCONTENT_SOURCE_DIR_CATCH2=/path/to/Catch2 ...
#   - Reuse previously downloaded content and skip the git update entirely:
#       cmake -DFETCHCONTENT_FULLY_DISCONNECTED=ON ...

include(FetchContent)

# Prevent Catch2 from building its own tests, examples, and benchmarks
set(CATCH_BUILD_TESTING    OFF CACHE INTERNAL "")
set(CATCH_BUILD_EXAMPLES   OFF CACHE INTERNAL "")
set(CATCH_BUILD_EXTRA_TESTS OFF CACHE INTERNAL "")
set(CATCH_INSTALL_DOCS     OFF CACHE INTERNAL "")
set(CATCH_INSTALL_HELPERS  OFF CACHE INTERNAL "")

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    # Pinned by full commit hash, not the (mutable) tag: tags can be deleted or
    # retargeted upstream, hashes cannot. This is the exact commit of the
    # v3.7.1 release the test suites were verified against. To bump the
    # version, resolve the new commit with:
    #   git ls-remote https://github.com/catchorg/Catch2.git "refs/tags/vX.Y.Z^{}"
    GIT_TAG        fa43b77429ba76c462b1898d6cd2f2d7a9416b14   # v3.7.1
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)

# FetchContent_MakeAvailable is CMake 3.14+; use manual populate for 3.13 compat
FetchContent_GetProperties(Catch2)
if(NOT Catch2_POPULATED)
    FetchContent_Populate(Catch2)
    add_subdirectory(
        ${catch2_SOURCE_DIR} ${catch2_BINARY_DIR}
        EXCLUDE_FROM_ALL
    )
endif()

include(${catch2_SOURCE_DIR}/extras/Catch.cmake)

if(WIN32 AND TARGET Catch2WithMain)
    target_compile_definitions(Catch2WithMain PRIVATE DO_NOT_USE_WMAIN)
endif()

message(STATUS "Catch2 v3: using FetchContent (${catch2_SOURCE_DIR})")
