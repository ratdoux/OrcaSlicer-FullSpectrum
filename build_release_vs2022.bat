@REM Snapmaker_Orca build script for Windows
@echo off
set "WP=%~dp0"
for %%I in ("%WP%.") do set "WP=%%~fI"

@REM Pack deps
if "%1"=="pack" (
    setlocal ENABLEDELAYEDEXPANSION 
    cd /d "%WP%\deps\build"
    for /f "tokens=2-4 delims=/ " %%a in ('date /t') do set build_date=%%c%%b%%a
    echo packing deps: OrcaSlicer_dep_win64_!build_date!_vs2022.zip

    "%WP%\tools\7z.exe" a OrcaSlicer_dep_win64_!build_date!_vs2022.zip OrcaSlicer_dep
    exit /b 0
)

@REM OpenSSL and the Windows SDK lookup require a Visual Studio developer environment.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
set "ORCA_PERL_DIR="
if exist "%SystemDrive%\Strawberry\perl\bin\perl.exe" set "ORCA_PERL_DIR=%SystemDrive%\Strawberry\perl\bin"
if not defined ORCA_PERL_DIR for /f "delims=" %%I in ('where perl.exe 2^>nul') do if not defined ORCA_PERL_DIR set "ORCA_PERL_DIR=%%~dpI"

set "ORCA_NEED_VS_ENV=0"
where nmake >nul 2>nul
if errorlevel 1 set "ORCA_NEED_VS_ENV=1"
if not defined WindowsSdkDir set "ORCA_NEED_VS_ENV=1"
if "%ORCA_NEED_VS_ENV%"=="0" goto :vs_ready

set "ORCA_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%ORCA_VSWHERE%" (
    echo Visual Studio Installer's vswhere.exe was not found.
    exit /b 1
)
for /f "usebackq tokens=*" %%I in (`"%ORCA_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ORCA_VSINSTALL=%%I"
if not defined ORCA_VSINSTALL (
    echo A Visual Studio installation with the C++ x64 toolchain was not found.
    exit /b 1
)
call "%ORCA_VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%

:vs_ready
if defined ORCA_PERL_DIR set "PATH=%ORCA_PERL_DIR%;%PATH%"
where perl.exe >nul 2>nul
if errorlevel 1 (
    echo Perl is required to build OpenSSL but was not found.
    echo Install Strawberry Perl and run this script again.
    exit /b 1
)

set debug=OFF
set debuginfo=OFF
if "%1"=="debug" set debug=ON
if "%2"=="debug" set debug=ON
if "%1"=="debuginfo" set debuginfo=ON
if "%2"=="debuginfo" set debuginfo=ON
if "%debug%"=="ON" (
    set build_type=Debug
    set build_dir=build-dbg
) else (
    if "%debuginfo%"=="ON" (
        set build_type=RelWithDebInfo
        set build_dir=build-dbginfo
    ) else (
        set build_type=Release
        set build_dir=build
    )
)
echo build type set to %build_type%

setlocal DISABLEDELAYEDEXPANSION 
cd /d "%WP%\deps"
if not exist "%build_dir%" mkdir "%build_dir%"
cd "%build_dir%"
set DEPS=%CD%/OrcaSlicer_dep
set "SIG_FLAG="
if defined ORCA_UPDATER_SIG_KEY set "SIG_FLAG=-DORCA_UPDATER_SIG_KEY=%ORCA_UPDATER_SIG_KEY%"

if "%1"=="slicer" (
    GOTO :slicer
)
echo "building deps.."

echo on
cmake ../ -G "Visual Studio 17 2022" -A x64 -DDESTDIR="%DEPS%" -DCMAKE_BUILD_TYPE=%build_type% -DDEP_DEBUG=%debug% -DORCA_INCLUDE_DEBUG_INFO=%debuginfo%
@if errorlevel 1 exit /b %errorlevel%
cmake --build . --config %build_type% --target deps -- -m
@if errorlevel 1 exit /b %errorlevel%
@echo off

if "%1"=="deps" exit /b 0

:slicer
echo "building Snapmaker Orca..."
cd /d "%WP%"
if not exist "%build_dir%" mkdir "%build_dir%"
cd "%build_dir%"

echo on
cmake .. -G "Visual Studio 17 2022" -A x64 -DBBL_RELEASE_TO_PUBLIC=1 -DORCA_TOOLS=ON %SIG_FLAG% -DCMAKE_PREFIX_PATH="%DEPS%/usr/local" -DCMAKE_INSTALL_PREFIX="./Snapmaker_Orca" -DCMAKE_BUILD_TYPE=%build_type% -DWIN10SDK_PATH="%WindowsSdkDir%Include\%WindowsSDKVersion%"
@if errorlevel 1 exit /b %errorlevel%
cmake --build . --config %build_type% --target ALL_BUILD -- -m
@if errorlevel 1 exit /b %errorlevel%
@echo off
cd ..
call scripts/run_gettext.bat
if errorlevel 1 exit /b %errorlevel%
cd %build_dir%
cmake --build . --target install --config %build_type%
if errorlevel 1 exit /b %errorlevel%
