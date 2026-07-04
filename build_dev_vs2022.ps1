# Fast Windows developer build for Snapmaker_Orca.
#
# Typical usage:
#   .\build_dev_vs2022.ps1
#   .\build_dev_vs2022.ps1 -Target app
#   .\build_dev_vs2022.ps1 -Target core
#   .\build_dev_vs2022.ps1 -Config Debug -BuildDeps
#   .\build_dev_vs2022.ps1 -Reconfigure
#   .\build_dev_vs2022.ps1 -DryRun

[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",

    [string]$Target = "auto",

    [string]$BuildDir = "",

    [string]$DepsPrefix = "",

    [ValidateSet("Visual Studio 17 2022", "Ninja", "Ninja Multi-Config")]
    [string]$Generator = "Visual Studio 17 2022",

    [string]$Platform = "x64",

    [int]$Jobs = 0,

    [switch]$BuildDeps,
    [switch]$Reconfigure,
    [switch]$Clean,
    [switch]$Tests,
    [switch]$Tools,
    [switch]$Install,
    [switch]$RunTests,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $RepoRoot

function Resolve-RepoPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $Path))
}

function Test-DepsPrefix {
    param([Parameter(Mandatory = $true)][string]$Path)

    return (
        (Test-Path $Path) -and
        (Test-Path (Join-Path $Path "include")) -and
        (Test-Path (Join-Path $Path "lib"))
    )
}

function Find-DepsPrefix {
    param([Parameter(Mandatory = $true)][string]$BuildConfig)

    $candidates = @()

    if ($BuildConfig -eq "Debug") {
        $candidates += "deps/build-dbg/OrcaSlicer_dep/usr/local"
    } elseif ($BuildConfig -eq "RelWithDebInfo") {
        $candidates += "deps/build-dbginfo/OrcaSlicer_dep/usr/local"
    }

    $candidates += @(
        "deps/build/OrcaSlicer_dep/usr/local",
        "deps/build/destdir/usr/local"
    )

    foreach ($candidate in $candidates) {
        $fullPath = Resolve-RepoPath $candidate
        if (Test-DepsPrefix $fullPath) {
            return $fullPath
        }
    }

    return $null
}

function Invoke-CommandLine {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    Write-Host ""
    Write-Host "> $FilePath $($Arguments -join ' ')"
    if ($DryRun) {
        Write-Host "(dry run)"
        return
    }

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $LASTEXITCODE"
    }
}

function Get-GitChangedFiles {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        return @()
    }

    if (-not (Test-Path (Join-Path $RepoRoot ".git"))) {
        return @()
    }

    $paths = @()
    $oldErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $paths += & git -C $RepoRoot diff --name-only 2>$null
        $paths += & git -C $RepoRoot diff --cached --name-only 2>$null
        $paths += & git -C $RepoRoot ls-files --others --exclude-standard 2>$null
    } finally {
        $ErrorActionPreference = $oldErrorActionPreference
    }

    return @(
        $paths |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            ForEach-Object { $_.Replace("\", "/") } |
            Sort-Object -Unique
    )
}

function Resolve-TargetAlias {
    param([Parameter(Mandatory = $true)][string]$RequestedTarget)

    switch -Regex ($RequestedTarget.ToLowerInvariant()) {
        "^app$" { return "Snapmaker_Orca_app_gui" }
        "^core$" { return "libslic3r" }
        "^gui$" { return "libslic3r_gui" }
        "^tests?$" { return "ALL_BUILD" }
        default { return $RequestedTarget }
    }
}

function Resolve-TestTarget {
    param([Parameter(Mandatory = $true)][string[]]$ChangedFiles)

    $testTargets = @()

    foreach ($path in $ChangedFiles) {
        if ($path -match "^tests/libslic3r/") {
            $testTargets += "libslic3r_tests"
        } elseif ($path -match "^tests/fff_print/") {
            $testTargets += "fff_print_tests"
        } elseif ($path -match "^tests/sla_print/") {
            $testTargets += "sla_print_tests"
        } elseif ($path -match "^tests/slic3rutils/") {
            $testTargets += "slic3rutils_tests"
        } elseif ($path -match "^tests/libnest2d/") {
            $testTargets += "libnest2d_tests"
        } elseif ($path -match "^tests/example/") {
            $testTargets += "example_tests"
        }
    }

    $testTargets = @($testTargets | Sort-Object -Unique)
    if ($testTargets.Count -eq 1) {
        return $testTargets[0]
    }

    if ($testTargets.Count -gt 1) {
        return "ALL_BUILD"
    }

    return "ALL_BUILD"
}

function Resolve-AutoTarget {
    $changedFiles = @(Get-GitChangedFiles)
    $buildFiles = @(
        $changedFiles |
            Where-Object {
                ($_ -notmatch "^doc/") -and
                ($_ -notmatch "^docs/") -and
                ($_ -notmatch "^SoftFever_doc/") -and
                ($_ -notmatch "\.md$") -and
                ($_ -notmatch "^README") -and
                ($_ -notmatch "^AGENTS\.md$") -and
                ($_ -notmatch "^CLAUDE\.md$")
            }
    )

    $requiresReconfigure = @(
        $buildFiles |
            Where-Object {
                ($_ -match "(^|/)CMakeLists\.txt$") -or
                ($_ -match "\.cmake$") -or
                ($_ -match "^cmake/")
            }
    ).Count -gt 0

    if ($changedFiles.Count -eq 0) {
        return [pscustomobject]@{
            Target = "Snapmaker_Orca_app_gui"
            EnableTests = $false
            Reconfigure = $false
            Reason = "no git changes found; building the app target"
            ChangedFiles = $changedFiles
        }
    }

    if ($buildFiles.Count -eq 0) {
        return [pscustomobject]@{
            Target = "none"
            EnableTests = $false
            Reconfigure = $false
            Reason = "only docs or metadata changed"
            ChangedFiles = $changedFiles
        }
    }

    if (@($buildFiles | Where-Object { $_ -match "^tests/" }).Count -gt 0) {
        return [pscustomobject]@{
            Target = (Resolve-TestTarget $buildFiles)
            EnableTests = $true
            Reconfigure = $true
            Reason = "test changes detected"
            ChangedFiles = $changedFiles
        }
    }

    $appPatterns = @(
        "^src/slic3r/",
        "^src/Snapmaker_Orca",
        "^src/dev-utils/",
        "^src/mqtt/",
        "^resources/",
        "^localization/",
        "^deps/",
        "^deps_src/",
        "(^|/)CMakeLists\.txt$",
        "\.cmake$",
        "^cmake/"
    )

    foreach ($pattern in $appPatterns) {
        if (@($buildFiles | Where-Object { $_ -match $pattern }).Count -gt 0) {
            return [pscustomobject]@{
                Target = "Snapmaker_Orca_app_gui"
                EnableTests = $false
                Reconfigure = $requiresReconfigure
                Reason = "app, GUI, resources, dependency, or build-system changes detected"
                ChangedFiles = $changedFiles
            }
        }
    }

    if (@($buildFiles | Where-Object { $_ -match "^src/libslic3r/" }).Count -gt 0) {
        return [pscustomobject]@{
            Target = "libslic3r"
            EnableTests = $false
            Reconfigure = $requiresReconfigure
            Reason = "core libslic3r changes detected"
            ChangedFiles = $changedFiles
        }
    }

    return [pscustomobject]@{
        Target = "Snapmaker_Orca_app_gui"
        EnableTests = $false
        Reconfigure = $requiresReconfigure
        Reason = "unclassified build changes detected"
        ChangedFiles = $changedFiles
    }
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "cmake was not found in PATH. Install CMake or run from a shell where CMake is available."
}

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = "build-dev-$($Config.ToLowerInvariant())"
}

$BuildPath = Resolve-RepoPath $BuildDir

if ($Clean -and (Test-Path $BuildPath)) {
    if (-not $BuildPath.StartsWith($RepoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a build directory outside the repository: $BuildPath"
    }

    Write-Host "Cleaning $BuildPath"
    Remove-Item -LiteralPath $BuildPath -Recurse -Force
}

if ($BuildDeps) {
    $depsArgs = @("deps")
    if ($Config -eq "Debug") {
        $depsArgs += "debug"
    } elseif ($Config -eq "RelWithDebInfo") {
        $depsArgs += "debuginfo"
    }

    Invoke-CommandLine -FilePath (Join-Path $RepoRoot "build_release_vs2022.bat") -Arguments $depsArgs
}

$TargetWasAuto = $Target.Equals("auto", [System.StringComparison]::OrdinalIgnoreCase)
$AutoEnableTests = $false
$AutoReconfigure = $false
$AutoReason = ""
$AutoChangedFiles = @()

if ($TargetWasAuto) {
    $autoDecision = Resolve-AutoTarget
    $Target = $autoDecision.Target
    $AutoEnableTests = $autoDecision.EnableTests
    $AutoReconfigure = $autoDecision.Reconfigure
    $AutoReason = $autoDecision.Reason
    $AutoChangedFiles = @($autoDecision.ChangedFiles)
} else {
    $Target = Resolve-TargetAlias $Target
}

if ($Target -eq "none") {
    Write-Host "Auto target: no build needed ($AutoReason)."
    Write-Host "Changed files:"
    foreach ($path in $AutoChangedFiles) {
        Write-Host "  $path"
    }
    exit 0
}

if ([string]::IsNullOrWhiteSpace($DepsPrefix)) {
    $ResolvedDepsPrefix = Find-DepsPrefix $Config
} else {
    $ResolvedDepsPrefix = Resolve-RepoPath $DepsPrefix
}

if ([string]::IsNullOrWhiteSpace($ResolvedDepsPrefix) -or -not (Test-DepsPrefix $ResolvedDepsPrefix)) {
    throw @"
Could not find a usable deps prefix.

Expected something like:
  deps/build/OrcaSlicer_dep/usr/local

Build deps first with:
  .\build_dev_vs2022.ps1 -BuildDeps

Or pass an explicit prefix:
  .\build_dev_vs2022.ps1 -DepsPrefix path\to\OrcaSlicer_dep\usr\local
"@
}

$TestsEnabled = $Tests.IsPresent -or $AutoEnableTests
$ToolsEnabled = $Tools.IsPresent -or ($Target -eq "Snapmaker_Orca_profile_validator")

$cachePath = Join-Path $BuildPath "CMakeCache.txt"
$needsConfigure = $Reconfigure -or $AutoReconfigure -or -not (Test-Path $cachePath)

if ((Test-Path $cachePath) -and $TestsEnabled) {
    $testsCacheOff = Select-String -Path $cachePath -Pattern "^BUILD_TESTS:BOOL=OFF$" -Quiet
    if ($testsCacheOff) {
        $needsConfigure = $true
    }
}

if ((Test-Path $cachePath) -and $ToolsEnabled) {
    $toolsCacheOff = Select-String -Path $cachePath -Pattern "^ORCA_TOOLS:BOOL=OFF$" -Quiet
    if ($toolsCacheOff) {
        $needsConfigure = $true
    }
}

Write-Host "Dev build settings:"
Write-Host "  Config:      $Config"
Write-Host "  Target:      $Target"
if ($TargetWasAuto) {
    Write-Host "  Auto reason: $AutoReason"
}
Write-Host "  Build dir:   $BuildPath"
Write-Host "  Deps prefix: $ResolvedDepsPrefix"
Write-Host "  Generator:   $Generator"
Write-Host "  Tests:       $TestsEnabled"
Write-Host "  Tools:       $ToolsEnabled"

if ($needsConfigure) {
    $configureArgs = @(
        "-S", $RepoRoot,
        "-B", $BuildPath,
        "-G", $Generator,
        "-DCMAKE_PREFIX_PATH=$ResolvedDepsPrefix",
        "-DCMAKE_INSTALL_PREFIX=$BuildPath/Snapmaker_Orca",
        "-DSLIC3R_PCH=ON",
        "-DSLIC3R_MSVC_COMPILE_PARALLEL=ON",
        "-DBUILD_TESTS=$TestsEnabled",
        "-DORCA_TOOLS=$ToolsEnabled"
    )

    if ($Generator -eq "Visual Studio 17 2022") {
        $configureArgs += @("-A", $Platform, "-DCMAKE_CONFIGURATION_TYPES=$Config")
    } elseif ($Generator -eq "Ninja Multi-Config") {
        $configureArgs += "-DCMAKE_CONFIGURATION_TYPES=$Config"
    } else {
        $configureArgs += "-DCMAKE_BUILD_TYPE=$Config"
    }

    if ($Config -eq "Debug") {
        $configureArgs += @("-DBBL_INTERNAL_TESTING=1")
    } else {
        $configureArgs += @("-DBBL_RELEASE_TO_PUBLIC=1", "-DBBL_INTERNAL_TESTING=0")
    }

    if ($env:WindowsSdkDir -and $env:WindowsSDKVersion) {
        $configureArgs += "-DWIN10SDK_PATH=$($env:WindowsSdkDir)Include\$($env:WindowsSDKVersion)\"
    }

    Invoke-CommandLine -FilePath "cmake" -Arguments $configureArgs
} else {
    Write-Host "CMake cache exists. Skipping configure. Use -Reconfigure to regenerate."
}

$buildArgs = @(
    "--build", $BuildPath,
    "--config", $Config,
    "--target", $Target,
    "--parallel"
)

if ($Jobs -gt 0) {
    $buildArgs += "$Jobs"
}

Invoke-CommandLine -FilePath "cmake" -Arguments $buildArgs

if ($Install) {
    Invoke-CommandLine -FilePath "cmake" -Arguments @(
        "--build", $BuildPath,
        "--config", $Config,
        "--target", "install",
        "--parallel"
    )
}

if ($RunTests) {
    if (-not $TestsEnabled) {
        Write-Warning "RunTests was requested, but this build was configured with BUILD_TESTS=OFF."
        Write-Warning "Re-run with -Tests -Reconfigure, then use -RunTests."
    } else {
        Invoke-CommandLine -FilePath "ctest" -Arguments @(
            "--test-dir", $BuildPath,
            "-C", $Config,
            "--output-on-failure"
        )
    }
}

Write-Host ""
Write-Host "Done. For the next edit, re-run the same command; it will reuse deps and the CMake cache."
