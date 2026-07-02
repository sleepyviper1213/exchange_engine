# Fuzzing

Coverage-guided fuzz harnesses for the parser and matching engine. They build
only when `ORDERBOOK_ENABLE_FUZZING=ON`.

| Harness            | Target                                   | Input                                        |
|--------------------|------------------------------------------|----------------------------------------------|
| `parse_depth_fuzz` | The three `binance::parse_*` entry points | Raw bytes → JSON `string_view`               |
| `engine_ops_fuzz`  | `OrderBook` identified flow (place/cancel) | Byte stream decoded into an operation script |

Two coverage engines are wired up behind one harness source:

- **Linux/macOS — [AFL++](https://github.com/AFLplusplus/AFLplusplus)**: source
  instrumentation via `afl-clang-fast`, fork-server persistent mode.
- **Windows — [WinAFL](https://github.com/googleprojectzero/winafl)**: DynamoRIO
  dynamic binary instrumentation, no recompiler needed. The harness exports
  `winafl_target(const char* path)`, which WinAFL hooks and loops.

`fuzz/afl_harness.hpp` selects the right entry point per build (`FUZZ_MAIN`). Built
with a plain compiler the same binary reads one testcase (a file argument, else
stdin) and runs once — the crash-replay path for a debugger or an ASan build.

## Wrapper scripts

Prefer these over the raw commands below — they encode the tool paths and gotchas:

- **Windows:** `fuzz/fuzz.ps1` — `.\fuzz.ps1 -Action build`, then
  `.\fuzz.ps1 parse` / `.\fuzz.ps1 engine` (also `-Action dryrun` / `replay`).
- **Linux/macOS:** `fuzz/fuzz.sh` — `./fuzz.sh build`, then
  `./fuzz.sh fuzz parse` / `./fuzz.sh fuzz engine` (also `./fuzz.sh replay parse`).

The sections below document the underlying commands each wrapper runs.

---

# Windows / WinAFL

## Installed tooling (this machine)

WinAFL ships no prebuilt binaries, so it was built from source against a prebuilt
DynamoRIO. Everything lives under `C:\Users\BZS_TestCode1\fuzztools`:

| Component      | Path                                                                        |
|----------------|-----------------------------------------------------------------------------|
| DynamoRIO      | `fuzztools\DynamoRIO-Windows-11.91.20630\bin64\drrun.exe`                    |
| `afl-fuzz.exe` | `fuzztools\winafl\build64\bin\Release\afl-fuzz.exe`                          |
| `winafl.dll`   | `fuzztools\winafl\build64\bin\Release\winafl.dll`                            |

Rebuild WinAFL after a `git pull`:

```powershell
cmake -S fuzztools\winafl -B fuzztools\winafl\build64 -G "Visual Studio 17 2022" -A x64 `
  "-DDynamoRIO_DIR=fuzztools\DynamoRIO-Windows-11.91.20630\cmake"
cmake --build fuzztools\winafl\build64 --config Release --target afl-fuzz winafl
```

## Build the harnesses (MinGW)

```powershell
cmake --preset windows-mingw -DORDERBOOK_ENABLE_FUZZING=ON
cmake --build build/windows-mingw --config Release --target parse_depth_fuzz engine_ops_fuzz
```

The harness `.exe` and the code under test (`liborder_book.dll`) land in
`build\windows-mingw\bin\Release`. WinAFL instruments the PE directly, so no
special compiler is required — but run `afl-fuzz` from that directory (or with it
on `PATH`) so the harness's DLLs resolve.

## Dry-run under DynamoRIO (always do this first)

Confirms the target function is hooked and coverage is recorded, without the fuzzer:

```powershell
$dr = "C:\Users\BZS_TestCode1\fuzztools\DynamoRIO-Windows-11.91.20630"
$winafl = "C:\Users\BZS_TestCode1\fuzztools\winafl\build64\bin\Release\winafl.dll"
& "$dr\bin64\drrun.exe" -c $winafl -debug `
  -target_module parse_depth_fuzz.exe -target_method winafl_target -fuzz_iterations 5 -nargs 1 `
  -coverage_module liborder_book.dll -coverage_module parse_depth_fuzz.exe `
  -- parse_depth_fuzz.exe ..\..\..\..\fuzz\corpus\parse_depth\snapshot.json
```

Expect `pre_fuzz_handler`/`post_fuzz_handler` printed once per iteration and
"Everything appears to be running normally."

## Fuzz

```powershell
$dr = "C:\Users\BZS_TestCode1\fuzztools\DynamoRIO-Windows-11.91.20630"
$afl = "C:\Users\BZS_TestCode1\fuzztools\winafl\build64\bin\Release\afl-fuzz.exe"
& $afl -i ..\..\..\..\fuzz\corpus\parse_depth -o ..\..\fuzz-out\parse -D "$dr\bin64" -t 20000 `
  -- -coverage_module liborder_book.dll -coverage_module parse_depth_fuzz.exe `
     -fuzz_iterations 1000 -target_module parse_depth_fuzz.exe -target_method winafl_target -nargs 1 `
  -- parse_depth_fuzz.exe `@`@
```

For `engine_ops_fuzz`, swap the module/method names and use the `engine_ops`
corpus. `-t` (timeout, ms) is mandatory under WinAFL. `@@` is replaced with the
testcase path each run.

## Platform support

- **Linux** — AFL++, best throughput.
- **macOS** — AFL++ works; `fork()` is slower and ASan is fiddly.
- **Windows** — WinAFL + DynamoRIO (installed above). Native, no WSL needed.

## Build

```sh
CC=afl-clang-fast CXX=afl-clang-fast++ \
  cmake -S . -B build/fuzz -DORDERBOOK_ENABLE_FUZZING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/fuzz --target parse_depth_fuzz engine_ops_fuzz
```

Add sanitizers by exporting before configuring (AFL++ reads these at compile time):

```sh
export AFL_USE_ASAN=1   # AddressSanitizer
export AFL_USE_UBSAN=1  # UndefinedBehaviorSanitizer — catches the parser int overflow
```

## Run

```sh
# Parser
afl-fuzz -i fuzz/corpus/parse_depth -o build/fuzz/out/parse \
  -x fuzz/dictionaries/binance_depth.dict -- build/fuzz/bin/parse_depth_fuzz

# Engine
afl-fuzz -i fuzz/corpus/engine_ops -o build/fuzz/out/engine \
  -- build/fuzz/bin/engine_ops_fuzz
```

## Triage a crash

The harness in stdin-fallback mode replays a saved crash for debugging:

```sh
build/fuzz/bin/parse_depth_fuzz < build/fuzz/out/parse/crashes/id:000000*
```

## Invariants checked by `engine_ops_fuzz`

- The book is never left crossed (`best_bid < best_ask`) after a mutation.

Extend with an L2-replay harness (`set_level`/`add_order`/`delete_order`) as a
*separate* binary — those paths rest liquidity without matching and can cross the
book by design, so they must not share the identified-flow invariants above.
