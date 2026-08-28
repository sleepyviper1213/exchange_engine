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

.PARAMETER ForceRun
    Rebuild and rerun the suite even when recent counters are already on disk.

.PARAMETER MaxAgeMinutes
    Reuse existing counters when the newest is younger than this, instead of
    spending minutes reproducing them. Default 5. Zero disables the reuse.

.PARAMETER NoHtml
    Write the summaries and the Cobertura XML, but skip the HTML detail pages.

.PARAMETER UseCTest
    Run the suite through ctest instead of invoking order_test directly. Much
    slower here: gtest_discover_tests registers one ctest test per gtest case,
    so ctest pays a process launch and ten DLL loads per case.

.PARAMETER TestArgs
    Extra arguments forwarded to order_test, e.g. --gtest_filter=OrderBook.*.
    Ignored with -UseCTest.

.PARAMETER OutDir
    Report directory. Defaults to <BuildDir>\coverage\<Config>.

.EXAMPLE
    .\scripts\coverage.ps1 -Preset windows-mingw-coverage

.EXAMPLE
    .\scripts\coverage.ps1 -BuildDir build\windows-mingw-coverage -ReportOnly
#>
# PositionalBinding=$false so a bare argument is not silently bound to the first
# parameter — without it `--help` lands in -BuildDir and reports "no such
# directory". Everything must be named, and strays reach $Remaining.
[CmdletBinding(PositionalBinding = $false)]
param(
    [Alias('B')][string]$BuildDir,
    [Alias('p')][string]$Preset,
    [Alias('c')][ValidateSet('Debug', 'RelWithDebInfo')][string]$Config,
    [switch]$ReportOnly,
    [switch]$NoHtml,
    [switch]$UseCTest,
    [switch]$ForceRun,
    [int]$MaxAgeMinutes = 5,
    [string[]]$TestArgs = @(),
    [Alias('o')][string]$OutDir,
    # Aliased explicitly: PowerShell's usual -h prefix match for -Help does not
    # apply once a ValueFromRemainingArguments parameter exists.
    [Alias('h')][switch]$Help,
    # PowerShell cannot bind a literal --help, so unrecognised arguments are
    # collected here: --help and /? print usage, anything else is a typo worth
    # reporting rather than ignoring.
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Remaining = @()
)

$ErrorActionPreference = 'Stop'

function Write-Step($message) {
    Write-Host "==> $message" -ForegroundColor Cyan
}

function Stop-WithMessage($message) {
    Write-Host "coverage: $message" -ForegroundColor Red
    exit 1
}

function Show-Usage {
    @'
Build, run the suite and report gcov coverage for a MinGW build tree.

Windows and GCC only. Coverage on Windows means the MinGW toolchain; for the
Clang source-based path on macOS use scripts/coverage.sh. Everything needed is
read from the tree's CMakeCache.txt, so a hand-configured build directory works
as well as a preset.

Usage:
  .\scripts\coverage.ps1 -Preset windows-mingw-coverage
  .\scripts\coverage.ps1 -BuildDir build\windows-mingw-coverage -ReportOnly
  .\scripts\coverage.ps1 -BuildDir build -TestArgs '--gtest_filter=OrderBook.*'

Options:
  -BuildDir DIR     Build tree to measure. Default: build\<preset> with
                    -Preset, otherwise .\build if it is configured.  (-B)
  -Preset NAME      Configure preset. Routes the build and test through
                    cmake --build --preset and ctest --preset.        (-p)
  -Config CFG       Debug or RelWithDebInfo. Required for a multi-config
                    tree, ignored for a single-config one.            (-c)
  -ReportOnly       Skip build and test; report from existing counters.
  -ForceRun         Rebuild and rerun even when recent counters exist.
  -MaxAgeMinutes N  Reuse counters newer than N minutes instead of spending
                    minutes reproducing them. Default 5; 0 disables reuse.
  -UseCTest         Run through ctest instead of invoking order_test
                    directly. Much slower: gtest_discover_tests registers one
                    ctest test per gtest case, so ctest pays a process launch
                    and a full set of DLL loads per case.
  -TestArgs ARGS    Forwarded to order_test. Quote anything starting with a
                    dash: -TestArgs '--gtest_filter=OrderBook.*'
  -NoHtml           Summary and machine-readable output only.
  -OutDir DIR       Report directory. Default <BuildDir>\coverage\<Config>. (-o)
  -Help             This text. --help and /? work too.

Get-Help .\scripts\coverage.ps1 -Full  gives the full parameter documentation.
See docs/coverage.md for how to read the output.
'@ | Write-Host
}

if ($Help -or ($Remaining | Where-Object { $_ -in '--help', '-help', '/?', '/h' })) {
    Show-Usage
    exit 0
}

if ($Remaining.Count -gt 0) {
    Show-Usage
    Write-Host ''
    Stop-WithMessage "unknown argument(s): $($Remaining -join ' ')"
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

# The suite takes minutes. If it already ran recently the counters on disk are
# the ones a rerun would produce, so go straight to the report. Freshness is the
# newest .gcda's mtime — gcov writes them at process exit, so that timestamp is
# when the suite finished. -ForceRun, or -MaxAgeMinutes 0, always reruns.
if (-not $ReportOnly -and -not $ForceRun -and $MaxAgeMinutes -gt 0) {
    $newest = Get-ChildItem -Path $BuildDir -Filter '*.gcda' -Recurse -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($newest) {
        $ageMinutes = ((Get-Date) - $newest.LastWriteTime).TotalMinutes
        if ($ageMinutes -lt $MaxAgeMinutes) {
            Write-Step ("counters are {0:N1} min old (under {1}); reusing them" -f $ageMinutes, $MaxAgeMinutes)
            Write-Step '  -ForceRun to rebuild and rerun the suite anyway'
            $ReportOnly = $true
        }
    }
}

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

    # order_test is run directly rather than through ctest. gtest_discover_tests
    # registers one ctest test per gtest case, so ctest spawns a process per case
    # and each one pays Windows process creation plus the load of ten project
    # DLLs and openssl/simdjson/spdlog/fmt — before any test body runs. One
    # process covers the same code in a fraction of the time, and for the Clang
    # backend it also collapses hundreds of .profraw files into one.
    #
    # -UseCTest keeps the old path. Note that ctest's random scheduling is what
    # catches order-dependent tests, so that property lives with the ctest
    # presets in CMakePresets.json, not here; a coverage run only cares that
    # every test executed.
    Write-Step 'running the suite'
    if ($UseCTest) {
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
    else {
        $candidates = @(Get-ChildItem -Path $BuildDir -Filter 'order_test.exe' -Recurse -ErrorAction SilentlyContinue)
        if ($Config) {
            $matched = @($candidates | Where-Object { $_.FullName -like "*\$Config\*" })
            if ($matched.Count -gt 0) { $candidates = $matched }
        }
        if ($candidates.Count -eq 0) {
            Stop-WithMessage "order_test.exe not found under $BuildDir — was the build target built?"
        }
        $exe = $candidates[0]

        # gtest_discover_tests runs each case with WORKING_DIRECTORY set to the
        # test target's binary dir; match that so a test using a relative path
        # behaves the same here as under ctest.
        $workDir = Join-Path $BuildDir 'test'
        if (-not (Test-Path $workDir)) { $workDir = $exe.DirectoryName }

        Write-Step "  $($exe.FullName)"
        Push-Location $workDir
        try { & $exe.FullName @TestArgs } finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) {
            Write-Host "coverage: order_test reported failures (exit $LASTEXITCODE); reporting coverage anyway" -ForegroundColor Yellow
        }
    }
}

# --------------------------------------------------------------- gcov / gcovr

if (-not (Get-Command gcovr -ErrorAction SilentlyContinue)) {
    Stop-WithMessage 'gcovr not found (pip install gcovr)'
}

# gcov must come from the toolchain that wrote the .gcno files.
#
# Forward slashes are mandatory, not cosmetic: gcovr splits --gcov-executable
# with shlex in POSIX mode (formats/gcov/read.py, shlex.split), so a Windows
# path arrives as C:UsersTruongNKAppData...gcov.exe with every backslash eaten
# as an escape, and CreateProcess fails with WinError 2 inside a traceback that
# blames the file rather than the quoting. CMakeCache.txt stores the compiler
# with forward slashes already; Join-Path is what normalises them away.
$gcovExe = (Join-Path (Split-Path $cxx -Parent) 'gcov.exe') -replace '\\', '/'
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
    '--gcov-ignore-parse-errors', 'negative_hits.warn_once_per_file'
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
