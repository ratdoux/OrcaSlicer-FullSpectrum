#ifndef CATCH_MAIN
#define CATCH_MAIN

// Catch2 v3 — main() is provided by Catch2::Catch2WithMain (linked via
// test_common → Catch2::Catch2WithMain in tests/CMakeLists.txt).
//
// This header exists as a convenience include for all test entry points.
// The custom VerboseConsoleReporter has been removed because Catch2 v3
// makes ConsoleReporter final; the default console reporter is used instead.
//
// If richer output is needed, use Catch2 v3 listener hooks or pass
// --verbosity high on the command line.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "libslic3r/Utils.hpp"

// Test-runtime bootstrap, injected into every test TU via the -include of
// this header. Two separate mechanisms with opposite timing requirements:
//
// 1. Log level — must be raised BEFORE static initialisers run. Catch2's
//    catch_discover_tests() treats every stdout line of its discovery run
//    (`--list-tests --verbosity quiet` in v3) as a test name, so the
//    "[trace] Initializing StaticPrintConfigs" line emitted during
//    StaticPrintConfigs static init registers a bogus, always-failing test.
//    A prioritised constructor (101) runs before unprioritised C++ static
//    init, which is the ordering we need on clang/GCC. MSVC has no
//    prioritised constructors: it falls back to a plain static object, whose
//    ordering vs. the PrintConfig TU is unspecified — the discovery-pollution
//    race may therefore still exist on MSVC (untested; CI only runs Linux).
//    This is safe to do early: set_logging_level() only writes a trivially
//    constructible severity variable and the boost::log core singleton.
//
// 2. resources_dir() — must be set AFTER static init. g_resources_dir is a
//    non-trivial std::string in libslic3r; assigning it from a prioritised
//    constructor happens before its own constructor runs (formally UB; in
//    practice the value is wiped — verified in lldb: set succeeds at
//    dyld-init time, value is "" again by test time). A Catch2 event
//    listener's testRunStarting() runs after all static init but before any
//    test, which is exactly the right window. Note this ACTIVATES
//    resources_dir()-dependent lazy loaders (info/nozzle_info.json,
//    info/filament_temp_type fallbacks in Print.cpp) for EVERY suite; tests
//    used to see the empty-dir hardcoded fallbacks outside fff_print.
#if defined(__clang__) || defined(__GNUC__)
    #define TEST_FRAMEWORK_EARLY_INIT __attribute__((constructor(101)))
#else
    #define TEST_FRAMEWORK_EARLY_INIT
#endif

namespace {
TEST_FRAMEWORK_EARLY_INIT
void test_framework_early_init()
{
    Slic3r::set_logging_level(2); // warning and above: keep stdout clean for Catch2
}

#if !defined(__clang__) && !defined(__GNUC__)
// MSVC fallback: no prioritised constructors exist, so a plain static object
// is the best we can do. Ordering vs. the StaticPrintConfigs trace is
// unspecified — see the comment above.
struct TestFrameworkEarlyInitFallback {
    TestFrameworkEarlyInitFallback() { test_framework_early_init(); }
};
static TestFrameworkEarlyInitFallback g_test_framework_early_init_fallback;
#endif

struct TestFrameworkBootstrap : Catch::EventListenerBase {
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const&) override
    {
        Slic3r::set_resources_dir(TEST_RESOURCES_DIR);
    }
};
} // namespace

CATCH_REGISTER_LISTENER(TestFrameworkBootstrap)

namespace Catch {
using Matchers::Equals;
} // namespace Catch

namespace Catch::Matchers {
template<typename Target, typename Epsilon>
inline WithinRelMatcher WithinRel(Target target, Epsilon eps)
{
    return WithinRel(static_cast<double>(target), static_cast<double>(eps));
}
} // namespace Catch::Matchers

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinULP;
using Catch::Approx;

#endif // CATCH_MAIN
