#include <catch_main.hpp>

// resources_dir() bootstrap for ALL test suites lives in tests/catch_main.hpp
// (TestFrameworkBootstrap listener). A static object here previously tried
// the same thing, but assigning g_resources_dir before its own constructor
// runs (unspecified cross-TU static-init order) wiped the value. The
// listener runs after all static init and wins.

// The nanoSVG C implementation (NANOSVG_IMPLEMENTATION) used to be defined
// inline here. It now lives in the shared tests/nanosvg_impl.cpp, compiled
// into the test_nanosvg_impl OBJECT library that every test executable
// links, so that all headless test binaries (not just this one) can resolve
// libslic3r's nanoSVG references without duplicate definitions.
