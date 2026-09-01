#include <catch_main.hpp>

// The nanoSVG C implementation used to be defined inline here. It is now
// provided solely by the shared tests/nanosvg_impl.cpp OBJECT library that
// every test executable links, so headless builds (SLIC3R_GUI=OFF) resolve
// libslic3r's nanoSVG references without duplicate definitions.
