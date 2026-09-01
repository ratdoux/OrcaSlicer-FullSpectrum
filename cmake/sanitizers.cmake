# cmake/sanitizers.cmake — AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan)
#
# Provides options ENABLE_ASAN and ENABLE_UBSAN that add the appropriate
# compiler and linker flags for GCC, Clang, and MSVC.
#
# Both options default to OFF and are strictly opt-in. Including this
# module therefore has no effect on any build unless the caller passes
# -DENABLE_ASAN=ON / -DENABLE_UBSAN=ON. This keeps release/production
# and normal Debug builds byte-for-byte unaffected.
#
# Usage:
#   include(cmake/sanitizers.cmake)
#   cmake -S . -B build -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
#
# On GCC / Clang, ASan and UBSan can coexist: both -fsanitize=address
# and -fsanitize=undefined are passed.
#
# On MSVC, only ASan is supported (/fsanitize=address). UBSan is not
# available; enabling UBSAN on MSVC emits a warning and is a no-op.
#
# Sanitizer builds always add debug info (-g / /Zi) alongside -fno-omit-frame-pointer
# so that ASan/UBSan reports carry symbolized stack traces even in Release builds.

option(ENABLE_ASAN  "Enable AddressSanitizer (ASan) — detect memory errors" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer (UBSan) — detect undefined behavior" OFF)

include(CheckCXXCompilerFlag)

# Note on global flag application: sanitizer instrumentation is added via
# add_compile_options / add_link_options (directory-wide) rather than per-target
# target_*() calls. This is deliberate: every translation unit and the final
# executable must be instrumented for ASan/UBSan to be effective, so a
# directory-global application is the correct intent here (not the uncontrolled
# "global CMake" anti-pattern, which concerns flags that should be scoped but
# are not). These options are only active when ENABLE_ASAN/ENABLE_UBSAN is ON,
# which is opt-in and never set for release/production builds.

# ── ASan ──────────────────────────────────────────────────────────────────────
if(ENABLE_ASAN)
    # ASan is available on MSVC starting with Visual Studio 2019 16.9
    # https://devblogs.microsoft.com/cppblog/address-sanitizer-for-msvc-now-generally-available/
    if(MSVC)
        add_compile_options(/fsanitize=address /Zi)
        add_compile_definitions(_DISABLE_STRING_ANNOTATION=1 _DISABLE_VECTOR_ANNOTATION=1)
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address)

        # GCC needs explicit -lasan for static linking in some configurations
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            add_link_options(-lasan)
        endif()
    endif()

    message(STATUS "AddressSanitizer (ASan) enabled")
endif()

# ── UBSan ─────────────────────────────────────────────────────────────────────
if(ENABLE_UBSAN)
    if(MSVC)
        message(WARNING "UndefinedBehaviorSanitizer (UBSan) is not supported on MSVC — ignored")
    else()
        add_compile_options(
            -fsanitize=undefined
            -fsanitize=signed-integer-overflow
            -fsanitize-recover=undefined
            -g
        )
        add_link_options(-fsanitize=undefined)

        # -fsanitize=implicit-conversion requires GCC 7+ but full support varies;
        # probe at configure-time so the build doesn't break on older toolchains.
        check_cxx_compiler_flag(-fsanitize=implicit-conversion HAS_IMPLICIT_CONVERSION)
        if(HAS_IMPLICIT_CONVERSION)
            add_compile_options(-fsanitize=implicit-conversion)
        else()
            message(STATUS "UBSan: -fsanitize=implicit-conversion not supported — skipped")
        endif()

        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            add_link_options(-lubsan)
        endif()

        message(STATUS "UndefinedBehaviorSanitizer (UBSan) enabled")
    endif()
endif()

# ── LeakSanitizer note ────────────────────────────────────────────────────────
# ASan includes LeakSanitizer (LSan) by default on Linux x86_64.
# To disable LSan at runtime: ASAN_OPTIONS=detect_leaks=0 ./your_binary
# To run with extra UBSan checks at runtime: UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./your_binary