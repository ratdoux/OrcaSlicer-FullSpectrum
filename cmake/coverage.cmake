# cmake/coverage.cmake — Code coverage with gcov/lcov (GCC/Clang)
#
# Provides ENABLE_COVERAGE option and a 'coverage' target.
#
# Usage:
#   include(cmake/coverage.cmake)
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON
#   cmake --build build --target tests
#   ctest --test-dir build
#   cmake --build build --target coverage
#
# On MSVC: coverage is not supported; enabling emits a warning and is a no-op.

option(ENABLE_COVERAGE "Enable code coverage instrumentation (GCC/Clang only)" OFF)

if(NOT ENABLE_COVERAGE)
    return()
endif()

# ── MSVC ────────────────────────────────────────────────────────────────────────
if(MSVC)
    message(WARNING "Code coverage (ENABLE_COVERAGE) is not supported on MSVC — ignored")
    return()
endif()

# ── GCC / Clang ─────────────────────────────────────────────────────────────────
message(STATUS "Code coverage (gcov/lcov) enabled")

# Compiler and linker flags for coverage instrumentation
add_compile_options(--coverage)
add_link_options(--coverage)

# Find required tools
find_program(LCOV_EXECUTABLE lcov)
find_program(GENHTML_EXECUTABLE genhtml)

# Auto-detect the correct gcov for the active compiler.
# GCC → system gcov; Clang → llvm-cov gcov (or gcov if present).
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    find_program(GCOV_EXECUTABLE NAMES "llvm-cov")
    if(GCOV_EXECUTABLE)
        set(GCOV_TOOL "${GCOV_EXECUTABLE} gcov")
    else()
        set(GCOV_TOOL "gcov")
    endif()
else()
    set(GCOV_TOOL "gcov")
endif()

if(NOT LCOV_EXECUTABLE)
    message(WARNING "lcov not found — 'coverage' target will not be available.")
    message(WARNING "Install with: sudo apt-get install lcov")
endif()

if(NOT GENHTML_EXECUTABLE)
    message(WARNING "genhtml not found — 'coverage' target will not be available.")
    message(WARNING "Install with: sudo apt-get install lcov")
endif()

# ── Coverage target ─────────────────────────────────────────────────────────────
if(LCOV_EXECUTABLE AND GENHTML_EXECUTABLE)
    set(COVERAGE_DIR  "${CMAKE_BINARY_DIR}/coverage")
    set(COVERAGE_INFO "${COVERAGE_DIR}/coverage.info")

    add_custom_target(coverage

        # Clean previous run
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${COVERAGE_DIR}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${COVERAGE_DIR}

        # Reset counters (in case of incremental builds)
        COMMAND ${LCOV_EXECUTABLE} --directory . --zerocounters --quiet

        # Run tests (re-run to collect fresh coverage data)
        # -E \[trace\]: skip ghost test names created by Catch2 v3 discovery
        # stdout pollution (see .github/workflows/coverage.yml "Run tests" step).
        # VERBATIM ensures the regex is properly quoted in generated Makefile
        # rules (without it, the backslashes are consumed by the shell).
        COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure -E "\\[trace\\]"
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            VERBATIM

        # Capture coverage data
        COMMAND ${LCOV_EXECUTABLE}
            --directory .
            --capture
            --output-file ${COVERAGE_INFO}.raw
            --gcov-tool ${GCOV_TOOL}
            --quiet

        # Strip system headers and third-party deps
        COMMAND ${LCOV_EXECUTABLE}
            --remove ${COVERAGE_INFO}.raw
            '/usr/*'
            '*/deps/*'
            '*/deps_src/*'
            '*/build/*'
            '*/tests/*'
            --output-file ${COVERAGE_INFO}
            --quiet

        # Generate HTML report
        COMMAND ${GENHTML_EXECUTABLE}
            ${COVERAGE_INFO}
            --output-directory ${COVERAGE_DIR}
            --title "${PROJECT_NAME} Coverage"
            --legend
            --show-details
            --quiet

        # Print summary
        COMMAND ${LCOV_EXECUTABLE}
            --list ${COVERAGE_INFO}
            --quiet

        COMMENT "Code coverage report: ${COVERAGE_DIR}/index.html"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        USES_TERMINAL
    )
endif()
