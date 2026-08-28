<#
.SYNOPSIS
    Build, run the suite and report gcov coverage for a MinGW build tree.

.DESCRIPTION
    Windows only, and GCC only. Coverage on Windows means the MinGW toolchain:
    --coverage is a GCC/Clang flag, cmake/Coverage.cmake returns early under
    MSVC, so a windows-msvc tree with ORDER_BOOK_ENABLE_COVERAGE=ON builds
    without instrumentation and reports nothing at all rather than reporting
    zero. For the Clang source-based path on macOS use scripts/coverage.sh.

    Everything needed is read from the tree's CMakeCache.txt, so a hand
    configured build directory works as well as a preset.

    See docs/coverage.md for how to read the output.

.PARAMETER BuildDir
    Build tree to measure. Defaults to build\<preset> when -Preset is given,
    otherwise .\build if it is configured.

.PARAMETER Preset
    Configure preset name. Routes the build and test through
    `cmake --build --preset` and `ctest --preset` instead of driving the tree
    directly.

.PARAMETER Config
    Debug or RelWithDebInfo. Required for a multi-config tree, ignored for a
    single-config one. Release and the sanitizer configurations carry no
    instrumentation by design.

.PARAMETER ReportOnly
    Skip the build and the test run; report from counters already present.

.PARAMETER NoHtml
    Write the summaries and the Cobertura XML, but skip the HTML detail pages.

.PARAMETER OutDir
    Report directory. Defaults to <BuildDir>\coverage\<Config>.

.EXAMPLE
    .\scripts\coverage.ps1 -Preset windows-mingw-coverage

.EXAMPLE
    .\scripts\coverage.ps1 -BuildDir build\windows-mingw-coverage -ReportOnly
#>
[CmdletBinding()]
param(
    [Alias('B')][string]$BuildDir,
    [Alias('p')][string]$Preset,
    [Alias('c')][ValidateSet('Debug', 'RelWithDebInfo')][string]$Config,
    [switch]$ReportOnly,
    [switch]$NoHtml,
    [Alias('o')][string]$OutDir
)

$ErrorActionPreference = 'Stop'

function Write-Step($message) {
    Write-Host "==> $message" -ForegroundColor Cyan
}

function Stop-WithMessage($message) {
    Write-Host "coverage: $message" -ForegroundColor Red
    exit 1
}

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

# ------------------------------------------------------------------ build tree

if (-not $BuildDir) {
    if ($Preset) {
        $BuildDir = Join-Path $RepoRoot "build\$Preset"
    }
    elseif (Test-Path (Join-Path $RepoRoot 'build\CMakeCache.txt')) {
        $BuildDir = Join-Path $RepoRoot 'build'
    }
    else {
        Stop-WithMessage 'no build tree given; pass -BuildDir <dir> or -Preset <name>'
    }
}

if (-not (Test-Path $BuildDir)) { Stop-WithMessage "no such directory: $BuildDir" }
$BuildDir = (Resolve-Path $BuildDir).Path

$cachePath = Join-Path $BuildDir 'CMakeCache.txt'
if (-not (Test-Path $cachePath)) {
    Stop-WithMessage "$BuildDir is not a configured build tree (no CMakeCache.txt)"
}

$cache = Get-Content $cachePath
function Get-CacheEntry($name) {
    $line = $cache | Where-Object { $_ -match "^$([regex]::Escape($name)):[^=]*=" } | Select-Object -First 1
    if ($null -eq $line) { return '' }
    return $line -replace "^$([regex]::Escape($name)):[^=]*=", ''
}

$cxx           = Get-CacheEntry 'CMAKE_CXX_COMPILER'
$configTypes   = Get-CacheEntry 'CMAKE_CONFIGURATION_TYPES'
$buildType     = Get-CacheEntry 'CMAKE_BUILD_TYPE'
$coverageOn    = Get-CacheEntry 'ORDER_BOOK_ENABLE_COVERAGE'
$unityOn       = Get-CacheEntry 'ORDER_BOOK_ENABLE_UNITY_BUILD'

if ($coverageOn -ne 'ON') {
    Stop-WithMessage @"
ORDER_BOOK_ENABLE_COVERAGE is '$(if ($coverageOn) { $coverageOn } else { 'unset' })' in $BuildDir.
   Reconfigure with -D ORDER_BOOK_ENABLE_COVERAGE=ON, or use a coverage preset.
"@
}

if ($unityOn -eq 'ON') {
    Stop-WithMessage 'ORDER_BOOK_ENABLE_UNITY_BUILD is ON; unity batching makes coverage misleading.'
}

# Two build trees that both install into one VCPKG_INSTALLED_DIR reconcile it
# against each other on every configure: each removes what the other added, takes
# the filesystem lock, and blocks the other's build. Sharing the directory saves
# nothing here — the packages come back from the vcpkg binary cache in seconds —
# so each tree owns its own, and this catches a tree still wired the old way.
$manifestInstall = Get-CacheEntry 'VCPKG_MANIFEST_INSTALL'
$installedDir    = Get-CacheEntry 'VCPKG_INSTALLED_DIR'
if ($manifestInstall -eq 'ON' -and $installedDir -and
    (Split-Path $installedDir -Leaf) -ne (Split-Path $BuildDir -Leaf)) {
    Write-Host @"
coverage: this tree installs into
     $installedDir
   which is named for a different build tree. Both will run vcpkg install
   against it and undo each other's work on every configure. Reconfigure so
   this tree gets its own:
     cmake --preset <this-preset>
   A cached VCPKG_INSTALLED_DIR only changes on a full configure, not on the
   CMake re-run a build triggers.
"@ -ForegroundColor Yellow
}

# -------------------------------------------------------------------- compiler

# CMake settled this at configure time. The binary name proves nothing: MinGW
# installs the driver as c++.exe and its --version banner names neither gcc
# nor GCC.
$compilerId = ''
$compilerRecord = Get-ChildItem -Path (Join-Path $BuildDir 'CMakeFiles') `
    -Filter 'CMakeCXXCompiler.cmake' -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($compilerRecord) {
    $idLine = Select-String -Path $compilerRecord.FullName `
        -Pattern 'set\(CMAKE_CXX_COMPILER_ID "(.*)"\)' | Select-Object -First 1
    if ($idLine) { $compilerId = $idLine.Matches[0].Groups[1].Value }
}

switch ($compilerId) {
    'GNU' { break }
    'MSVC' {
        Stop-WithMessage @"
this tree is MSVC. --coverage is a GCC/Clang flag and cmake/Coverage.cmake
   returns early under MSVC, so the build carries no instrumentation and there
   is nothing to report. Use a MinGW coverage preset instead.
"@
    }
    { $_ -in 'Clang', 'AppleClang' } {
        Stop-WithMessage @"
this tree is Clang, which produces source-based profiles rather than gcov
   counters. Use scripts/coverage.sh, which drives llvm-profdata and llvm-cov.
"@
    }
    default {
        Stop-WithMessage "cannot determine the compiler for $BuildDir (id '$compilerId')"
    }
}

# ---------------------------------------------------------------- config name

if ($configTypes) {
    if (-not $Config) { $Config = 'Debug' }
    if ($configTypes -split ';' -notcontains $Config) {
        Stop-WithMessage "config '$Config' is not one of: $configTypes"
    }
}
else {
    $Config = $buildType
    if ($Config -and @('Debug', 'RelWithDebInfo') -notcontains $Config) {
        Stop-WithMessage @"
config '$Config' carries no instrumentation — cmake/Coverage.cmake gates the
   flags to Debug and RelWithDebInfo, so this would report zero counters.
"@
    }
}

if (-not $OutDir) {
    $OutDir = Join-Path $BuildDir 'coverage'
    if ($Config) { $OutDir = Join-Path $OutDir $Config }
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

Write-Step "tree     $BuildDir"
Write-Step "compiler $compilerId   $cxx"
Write-Step "config   $Config"
Write-Step "reports  $OutDir"

# ------------------------------------------------------------- build, then run

if (-not $ReportOnly) {
    Write-Step 'clearing stale .gcda counters'
    Get-ChildItem -Path $BuildDir -Filter '*.gcda' -Recurse -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue

    Write-Step 'building'
    if ($Preset) {
        cmake --build --preset "$Preset-$($Config.ToLower())"
    }
    elseif ($configTypes) {
        cmake --build $BuildDir --config $Config
    }
    else {
        cmake --build $BuildDir
    }
    if ($LASTEXITCODE -ne 0) { Stop-WithMessage "the build failed (exit $LASTEXITCODE)" }

    Write-Step 'running the suite'
    if ($Preset) {
        ctest --preset "$Preset-$($Config.ToLower())"
    }
    elseif ($configTypes) {
        ctest --test-dir $BuildDir --build-config $Config --output-on-failure
    }
    else {
        ctest --test-dir $BuildDir --output-on-failure
    }
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'coverage: ctest reported failures; reporting coverage anyway' -ForegroundColor Yellow
    }
}

# --------------------------------------------------------------- gcov / gcovr

if (-not (Get-Command gcovr -ErrorAction SilentlyContinue)) {
    Stop-WithMessage 'gcovr not found (pip install gcovr)'
}

# gcov must come from the toolchain that wrote the .gcno files.
$gcovExe = Join-Path (Split-Path $cxx -Parent) 'gcov.exe'
if (-not (Test-Path $gcovExe)) { $gcovExe = 'gcov' }

$gcdaCount = @(Get-ChildItem -Path $BuildDir -Filter '*.gcda' -Recurse -ErrorAction SilentlyContinue).Count
Write-Step "reading $gcdaCount .gcda counter files"
if ($gcdaCount -eq 0) {
    Stop-WithMessage @"
no .gcda counters in $BuildDir — the suite has not been run against this
   build. Drop -ReportOnly, or run ctest first.
"@
}

# --filter is the load-bearing argument: without it the report is dominated by
# libstdc++ and Boost headers and src/ disappears. See docs/coverage.md.
$common = @(
    '--root', $RepoRoot
    '--filter', 'src/'
    '--exclude', '.*\.test\.cpp'
    '--exclude', '.*\.fixture\.hpp'
    '--gcov-executable', $gcovExe
    '--exclude-unreachable-branches'
    '--exclude-throw-branches'
)

Write-Step 'gcovr: line summary'
gcovr @common --print-summary --txt (Join-Path $OutDir 'lines.txt') $BuildDir
if ($LASTEXITCODE -ne 0) { Stop-WithMessage "gcovr failed (exit $LASTEXITCODE)" }

Write-Step 'gcovr: branch summary'
gcovr @common --txt-metric branch --txt (Join-Path $OutDir 'branches.txt') $BuildDir

Write-Step 'gcovr: cobertura xml'
gcovr @common --xml-pretty --output (Join-Path $OutDir 'cobertura.xml') $BuildDir

if (-not $NoHtml) {
    Write-Step 'gcovr: html detail'
    $htmlDir = Join-Path $OutDir 'html'
    New-Item -ItemType Directory -Force -Path $htmlDir | Out-Null
    gcovr @common --html-details (Join-Path $htmlDir 'index.html') $BuildDir
}

Write-Step 'done'
Write-Host ''
Get-ChildItem $OutDir | ForEach-Object { Write-Host "  $($_.FullName)" }
if (-not $NoHtml) {
    Write-Host ''
    Write-Host "Open $(Join-Path $OutDir 'html\index.html')"
}
