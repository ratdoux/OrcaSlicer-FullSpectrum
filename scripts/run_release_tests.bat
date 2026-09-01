@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0.."

set "MODE=%~1"
if "%MODE%"=="" set "MODE=release"

if /i "%MODE%"=="release" (
    set "BUILD_DIR=build_test_release"
    set "CONFIG=Release"
    set "EXTRA_FLAGS=-DORCA_SKIP_ENC_CHECK=ON"
    set "DESC=Release (VS2022, no sanitizer)"
    goto :clean
)
if /i "%MODE%"=="asan" (
    set "BUILD_DIR=build_test_asan"
    set "CONFIG=Debug"
    set "EXTRA_FLAGS=-DENABLE_ASAN=ON -DORCA_SKIP_ENC_CHECK=ON"
    set "DESC=Debug + ASan (VS2022)"
    goto :clean
)

echo Unknown mode: %MODE%
echo Usage: run_release_tests.bat [release^|asan]
echo   release  - Release build, no sanitizer (default)
echo   asan     - Debug + AddressSanitizer
pause
exit /b 1

:clean
echo.
echo ============================================================
echo Mode: %DESC%
echo ============================================================
if exist %BUILD_DIR% (
    echo Cleaning %BUILD_DIR%...
    rmdir /s /q %BUILD_DIR%
)

echo.
echo === Configuring ===
cmake -S . -B %BUILD_DIR% -G "Visual Studio 17 2022" -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=%CONFIG% %EXTRA_FLAGS% -DCMAKE_PREFIX_PATH="%CD%/deps/build/OrcaSlicer_dep/usr/local"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: CMake configure failed
    pause
    exit /b 1
)

echo.
echo === Building tests ===
cmake --build %BUILD_DIR% --target tests --config %CONFIG% --parallel
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Build failed
    pause
    exit /b 1
)

echo.
echo === Running tests ===
ctest --test-dir %BUILD_DIR% -C %CONFIG% --output-on-failure

echo.
echo Done.
pause
