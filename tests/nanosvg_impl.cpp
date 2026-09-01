// Provides the nanoSVG C implementation for the headless test executables.
//
// In the production build the nanoSVG implementation (`nsvgParseFromFile`,
// `nsvgParse`, `nsvgDelete`, the rasterizer, ...) is compiled inside
// src/slic3r/GUI/BitmapCache.cpp, which is a *GUI* translation unit. The test
// CI builds with -DSLIC3R_GUI=OFF, so that TU is never compiled and libslic3r's
// references to the nanoSVG symbols would be left undefined at link time
// (e.g. undefined reference to `nsvgParseFromFile`).
//
// Rather than moving the implementation into production sources, we provide it
// here at the test-link level: tests/CMakeLists.txt compiles this file into the
// `test_nanosvg_impl` static library and links it into every test executable via
// TEST_LINK_LIBS. This mirrors the former inline defines that lived in
// tests/fff_print/fff_print_tests.cpp (now consolidated here to avoid duplicate
// definitions across test binaries).

#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"

#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"
