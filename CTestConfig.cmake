# CTestConfig.cmake — Dashboard and reporting configuration
#
# Enables ctest XML output for CI dashboards.
# Usage:
#   ctest --test-dir build --output-junit build/test-results.xml
#   ctest --test-dir build -D Experimental

set(CTEST_PROJECT_NAME "Snapmaker_Orca")
set(CTEST_NIGHTLY_START_TIME "03:00:00 UTC")

# Output settings
set(CTEST_OUTPUT_ON_FAILURE ON)

# Coverage settings (used with ENABLE_COVERAGE=ON)
set(CTEST_COVERAGE_COMMAND "gcov")
set(CTEST_COVERAGE_EXTRA_FLAGS "--preserve-paths --relative-only")
