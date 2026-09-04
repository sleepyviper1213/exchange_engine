<#
.SYNOPSIS
    The pre-merge gate: static analysis, then the suite with coverage.

.DESCRIPTION
    Two build trees, run in sequence. They cannot be one workflow preset -
    cmake-presets(7) requires that every non-configure step in a workflow name
    the same configurePreset as its single leading configure step, so a
    workflow spans exactly one build tree. Chaining two is this script's whole
    job.

    The split is not only a schema workaround. Analysis compiles and never runs
    anything; coverage runs the suite and needs its counters. Welding them into
    one tree meant clang-tidy's guards in test/CMakeLists.txt stripped both
    unity batching and the precompiled header from the tree that also had to
    run the tests - the slowest configuration in the project, for no gain.

    MinGW on both counts. clang-tidy and cppcheck have no MSVC integration
    here, and cmake/Coverage.cmake returns early under MSVC, so a windows-msvc
    tree with coverage on builds uninstrumented and reports nothing at all
    rather than reporting zero.

    CI does not use this script: two GitHub jobs cost the max of the two trees
    where this costs the sum. See .github/workflows/ci.yml.

.PARAMETER Analysis
    Analysis configure preset. Default: windows-mingw-analysis.

.PARAMETER Coverage
    Coverage configure preset. Default: windows-mingw-coverage.

.PARAMETER Config
    Debug or RelWithDebInfo for the coverage tree. Default: Debug. The analysis
    tree is Debug-only by construction - both analysers read the same sources
    in every configuration.

.PARAMETER NoAnalysis
    Skip the analysis tree.

.PARAMETER NoCoverage
    Skip the coverage tree and its report.

.EXAMPLE
    .\scripts\premerge.ps1

.EXAMPLE
    .\scripts\premerge.ps1 -NoCoverage
#>
param(
    [Alias('a')][string]$Analysis = 'windows-mingw-analysis',
    [Alias('c')][string]$Coverage = 'windows-mingw-coverage',
    [ValidateSet('Debug', 'RelWithDebInfo')][string]$Config = 'Debug',
    [switch]$NoAnalysis,
    [switch]$NoCoverage
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot

function Note($m) { Write-Host "==> $m" -ForegroundColor White }

function Invoke-Checked($exe, $arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "premerge: $exe $($arguments -join ' ') exited $LASTEXITCODE"
    }
}

try {
    if (-not $NoAnalysis) {
        Note "Static analysis: $Analysis"
        Invoke-Checked cmake @('--workflow', '--preset', $Analysis)
    }

    if (-not $NoCoverage) {
        $workflow = "$Coverage-$($Config.ToLowerInvariant())"
        Note "Tests and coverage: $Coverage / $Config"
        Invoke-Checked cmake @('--workflow', '--preset', $workflow)
        # -ReportOnly reads the counters the run above just wrote, so the suite
        # is neither rebuilt nor re-executed.
        & "$PSScriptRoot\coverage.ps1" -BuildDir "build\$Coverage" -Config $Config -ReportOnly
        if ($LASTEXITCODE -ne 0) { throw "premerge: coverage report exited $LASTEXITCODE" }
    }

    Note 'Pre-merge gate passed'
}
finally {
    Pop-Location
}
