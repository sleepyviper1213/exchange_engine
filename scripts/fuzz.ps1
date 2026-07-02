<#
.SYNOPSIS
    Build, dry-run, fuzz, or replay-crash the order_book WinAFL harnesses.

.DESCRIPTION
    Wraps the WinAFL + DynamoRIO workflow so you don't have to remember the two
    gotchas: afl-fuzz must run from the directory holding winafl.dll, and the
    cold-start first execution needs a lenient timeout (-t <N>+).

    Tool locations default to the install under C:\Users\BZS_TestCode1\fuzztools
    and can be overridden with the -DynamoRIO / -WinAFL parameters or the
    ORDERBOOK_DYNAMORIO / ORDERBOOK_WINAFL environment variables.

.PARAMETER Harness
    Which harness to drive: 'parse' (binance depth parsers) or 'engine'
    (OrderBook place/cancel flow).

.PARAMETER Action
    build   - configure (fuzzing ON) and build both harnesses
    dryrun  - run once under DynamoRIO -debug to verify instrumentation
    fuzz    - start an afl-fuzz session (runs until Ctrl+C)
    replay  - run a crash file through the harness (no fuzzer); newest if -Crash omitted

.PARAMETER Crash
    Path to a crash input for -Action replay. Defaults to the newest file in the
    harness's crashes/ directory.

.PARAMETER Timeout
    Per-exec timeout in ms passed to afl-fuzz as "<Timeout>+" (lenient). Default 60000.

.PARAMETER Log
    Quiet mode: send afl-fuzz's status to <out>\fuzz.log instead of the terminal.
    Default is the afl-fuzz TUI, which redraws the dashboard in place (no scroll).

.PARAMETER Fresh
    Wipe the output directory and start a new session. Without it, an existing
    session is resumed (afl-fuzz -i -); a fresh corpus run is used only when no
    prior session is found.

.EXAMPLE
    .\fuzz.ps1 -Action build
    .\fuzz.ps1 parse -Action dryrun
    .\fuzz.ps1 parse                     # fuzz the parser
    .\fuzz.ps1 engine                    # fuzz the engine
    .\fuzz.ps1 parse -Action replay      # replay newest parser crash
#>
[CmdletBinding()]
param(
    [ValidateSet('parse', 'engine')] [string] $Harness = 'parse',
    [ValidateSet('build', 'dryrun', 'fuzz', 'replay')] [string] $Action = 'fuzz',
    [string] $Crash,
    [int]    $Timeout = 60000,
    [switch] $Log,
    [switch] $Fresh
)

$ErrorActionPreference = 'Stop'

# --- Paths -----------------------------------------------------------------
$proj = Split-Path -Parent $PSScriptRoot          # repo root (fuzz/ is under it)
$binDir = Join-Path $proj 'build\windows-mingw\bin\Release'
$outRoot = Join-Path $proj 'build\windows-mingw\fuzz-out'

# Tool roots (override via env if the install ever moves).
$drRoot = if ($env:ORDERBOOK_DYNAMORIO) { $env:ORDERBOOK_DYNAMORIO } else { 'C:\Users\BZS_TestCode1\fuzztools\DynamoRIO-Windows-11.91.20630' }
$wbin = if ($env:ORDERBOOK_WINAFL) { $env:ORDERBOOK_WINAFL } else { 'C:\Users\BZS_TestCode1\fuzztools\winafl\build64\bin\Release' }

# --- Harness -> module / corpus map ----------------------------------------
$map = @{
    parse  = @{ Exe = 'parse_depth_fuzz.exe'; Corpus = 'parse_depth' }
    engine = @{ Exe = 'engine_ops_fuzz.exe'; Corpus = 'engine_ops' }
}
$exe = $map[$Harness].Exe
$exePath = Join-Path $binDir $exe
$corpus = Join-Path $proj "fuzz\corpus\$($map[$Harness].Corpus)"
$outDir = Join-Path $outRoot $Harness

switch ($Action) {

    'build' {
        $env:VCPKG_ROOT = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg'
        cmake --preset windows-mingw -DORDERBOOK_ENABLE_FUZZING=ON
        cmake --build (Join-Path $proj 'build\windows-mingw') --config Release `
            --target parse_depth_fuzz engine_ops_fuzz
        Write-Host "[+] Built harnesses in $binDir" -ForegroundColor Green
    }

    'dryrun' {
        if (-not (Test-Path $exePath)) { throw "$exe not built. Run: .\fuzz.ps1 -Action build" }
        $drrun = Join-Path $drRoot 'bin64\drrun.exe'
        $winafl = Join-Path $wbin 'winafl.dll'
        $seed = Get-ChildItem $corpus -File | Select-Object -First 1
        $logDir = Join-Path $outDir 'dryrun-log'
        New-Item -ItemType Directory -Force -Path $logDir | Out-Null
        Write-Host "[*] DynamoRIO dry-run of $exe on $($seed.Name)  (log -> $logDir)" -ForegroundColor Cyan
        & $drrun -c $winafl -debug -logdir $logDir `
            -target_module $exe -target_method winafl_target -fuzz_iterations 5 -nargs 1 `
            -coverage_module liborder_book.dll -coverage_module $exe `
            -- $exePath $seed.FullName
    }

    'fuzz' {
        if (-not (Test-Path $exePath)) { throw "$exe not built. Run: .\fuzz.ps1 -Action build" }

        # A Ctrl+C'd WinAFL run can leave the target process alive, holding a handle
        # on the out dir and making the next afl-fuzz abort ("cleanup failed"). Reap
        # any stale instance of this harness first.
        $procName = [IO.Path]::GetFileNameWithoutExtension($exe)
        $stale = Get-Process -Name $procName -ErrorAction SilentlyContinue
        if ($stale) {
            Write-Host "[*] Stopping $($stale.Count) stale $exe process(es)" -ForegroundColor DarkYellow
            $stale | Stop-Process -Force
        }

        if ($Fresh -and (Test-Path $outDir)) { Remove-Item -Recurse -Force $outDir }
        New-Item -ItemType Directory -Force -Path $outDir | Out-Null

        # Resume an existing session (afl-fuzz -i -) rather than reusing/wiping it,
        # unless -Fresh was asked for or there is no prior session to resume.
        $resume = (-not $Fresh) -and (Test-Path (Join-Path $outDir 'fuzzer_stats'))
        $inArg = if ($resume) { '-' } else { $corpus }
        if ($resume) { Write-Host "[*] Resuming existing session in $outDir  (use -Fresh to start over)" -ForegroundColor DarkCyan }

        $afl = Join-Path $wbin 'afl-fuzz.exe'
        $drBin = Join-Path $drRoot 'bin64'
        # afl-fuzz must run from the winafl.dll directory to find its client DLL.
        Push-Location $wbin
        try {
            if ($Log) {
                # Quiet: full plain status to a file; the terminal stays clean.
                $env:AFL_NO_UI = '1'
                $logFile = Join-Path $outDir 'fuzz.log'
                Write-Host "[*] Fuzzing $exe -> $logFile" -ForegroundColor Cyan
                Write-Host "    Follow live:  Get-Content '$logFile' -Wait -Tail 40   (Ctrl+C here stops the fuzzer)"
                & $afl -i $inArg -o $outDir -D $drBin -t "$Timeout+" `
                    -- -coverage_module liborder_book.dll -coverage_module $exe `
                       -fuzz_iterations 1000 -target_module $exe -target_method winafl_target -nargs 1 `
                    -- $exePath '@@' *> $logFile
            }
            else {
                # Default: afl-fuzz's TUI redraws the dashboard in place (no scroll spam).
                Remove-Item Env:\AFL_NO_UI -ErrorAction SilentlyContinue
                Write-Host "[*] Fuzzing $exe  (Ctrl+C to stop; crashes -> $outDir\crashes)" -ForegroundColor Cyan
                & $afl -i $inArg -o $outDir -D $drBin -t "$Timeout+" `
                    -- -coverage_module liborder_book.dll -coverage_module $exe `
                       -fuzz_iterations 1000 -target_module $exe -target_method winafl_target -nargs 1 `
                    -- $exePath '@@'
            }
        } finally { Pop-Location }
    }

    'replay' {
        if (-not $Crash) {
            $Crash = (Get-ChildItem (Join-Path $outDir 'crashes') -File -ErrorAction SilentlyContinue |
                Where-Object Name -like 'id*' | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
            if (-not $Crash) { throw "No crashes found under $outDir\crashes." }
        }
        Write-Host "[*] Replaying $Crash" -ForegroundColor Cyan
        & $exePath $Crash
        $code = $LASTEXITCODE
        $note = if ($code -eq -1073741819) { ' (0xC0000005 access violation)' }
                elseif ($code -eq -1073740791) { ' (0xC0000409 abort / invariant)' }
                else { '' }
        Write-Host "[+] exit=$code$note" -ForegroundColor Yellow
    }
}
