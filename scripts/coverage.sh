#!/usr/bin/env bash
#
# Build, run and report code coverage for one build tree.
#
# Two backends, picked from the compiler in the tree's CMakeCache:
#   GCC   -> gcov counters, report via gcovr
#   Clang -> source-based instrumentation, report via llvm-profdata + llvm-cov
#
# The llvm path passes every module .dylib/.so as an -object. Without that,
# llvm-cov reports only the headers instantiated into order_test itself and no
# .cpp at all, because each module here is a SHARED library carrying its own
# coverage mapping.
#
# Usage:
#   scripts/coverage.sh -B build                       # a tree you configured yourself
#   scripts/coverage.sh -p windows-mingw-coverage      # a configure preset
#   scripts/coverage.sh -B build --report-only         # reuse existing counters
#
# See docs/coverage.md for how to read the output.

set -euo pipefail

# `pwd -W` is Git Bash's native form. gcovr and the LLVM tools are Windows
# binaries there and do not understand an MSYS /f/... path; on macOS and Linux
# the flag does not exist and plain pwd is already right.
abs_path() { (cd "$1" && { pwd -W 2>/dev/null || pwd; }); }

REPO_ROOT=$(abs_path "$(dirname "$0")/..")

BUILD_DIR=""
PRESET=""
CONFIG=""
BACKEND="auto"
OUT_DIR=""
DO_RUN=1
DO_HTML=1
IGNORE_RE='(/test/|/benchmark/|vcpkg_installed|/_deps/|/usr/|/Xcode|/Cellar/|/MinGW|/mingw64/)'

die() { printf 'coverage: %s\n' "$*" >&2; exit 1; }
note() { printf '\033[1m==> %s\033[0m\n' "$*"; }

usage() {
    sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'
    cat <<'EOF'

Options:
  -B, --build-dir DIR   Build tree to measure. Default: build/<preset> with -p,
                        otherwise ./build if it exists.
  -p, --preset NAME     Configure preset. Enables `cmake --build --preset` and
                        `ctest --preset` instead of driving the tree directly.
  -c, --config CFG      Debug or RelWithDebInfo. Required for a multi-config
                        tree; ignored for a single-config one.
      --backend B       auto (default), gcov, or llvm.
      --report-only     Skip build and test; report from existing counters.
      --no-html         Summary and machine-readable output only.
  -o, --out DIR         Report directory. Default <build>/coverage[/<config>].
      --ignore-regex R  Override the llvm-cov filename ignore pattern.
  -h, --help            This text.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -B|--build-dir)    BUILD_DIR="$2"; shift 2 ;;
        -p|--preset)       PRESET="$2"; shift 2 ;;
        -c|--config)       CONFIG="$2"; shift 2 ;;
        --backend)         BACKEND="$2"; shift 2 ;;
        --report-only)     DO_RUN=0; shift ;;
        --no-html)         DO_HTML=0; shift ;;
        -o|--out)          OUT_DIR="$2"; shift 2 ;;
        --ignore-regex)    IGNORE_RE="$2"; shift 2 ;;
        -h|--help)         usage; exit 0 ;;
        *)                 die "unknown argument '$1' (try --help)" ;;
    esac
done

# ---------------------------------------------------------------- build tree

if [ -z "$BUILD_DIR" ]; then
    if [ -n "$PRESET" ]; then
        BUILD_DIR="$REPO_ROOT/build/$PRESET"
    elif [ -d "$REPO_ROOT/build" ] && [ -f "$REPO_ROOT/build/CMakeCache.txt" ]; then
        BUILD_DIR="$REPO_ROOT/build"
    else
        die "no build tree given; pass -B <dir> or -p <preset>"
    fi
fi

[ -d "$BUILD_DIR" ] || die "no such directory: $BUILD_DIR"
BUILD_DIR=$(abs_path "$BUILD_DIR")

CACHE="$BUILD_DIR/CMakeCache.txt"
[ -f "$CACHE" ] || die "$BUILD_DIR is not a configured build tree (no CMakeCache.txt)"

cache_get() { sed -n "s|^$1:[^=]*=||p" "$CACHE" | head -1; }

CXX=$(cache_get CMAKE_CXX_COMPILER)
CONFIG_TYPES=$(cache_get CMAKE_CONFIGURATION_TYPES)
BUILD_TYPE=$(cache_get CMAKE_BUILD_TYPE)
COVERAGE_ON=$(cache_get ORDER_BOOK_ENABLE_COVERAGE)
UNITY_ON=$(cache_get ORDER_BOOK_ENABLE_UNITY_BUILD)

[ "$COVERAGE_ON" = "ON" ] || die \
"ORDER_BOOK_ENABLE_COVERAGE is '${COVERAGE_ON:-unset}' in $BUILD_DIR.
   Reconfigure with -D ORDER_BOOK_ENABLE_COVERAGE=ON, or use a coverage preset."

[ "$UNITY_ON" != "ON" ] || die \
"ORDER_BOOK_ENABLE_UNITY_BUILD is ON; unity batching makes coverage misleading."

# Two build trees that both install into one VCPKG_INSTALLED_DIR reconcile it
# against each other on every configure: each removes what the other added, takes
# the filesystem lock, and blocks the other's build. Sharing the directory saves
# nothing here — the packages come back from the vcpkg binary cache in seconds —
# so each tree owns its own, and this catches a tree still wired the old way.
MANIFEST_INSTALL=$(cache_get VCPKG_MANIFEST_INSTALL)
INSTALLED_DIR=$(cache_get VCPKG_INSTALLED_DIR)
if [ "$MANIFEST_INSTALL" = "ON" ] && [ -n "$INSTALLED_DIR" ] &&
   [ "$(basename "$INSTALLED_DIR")" != "$(basename "$BUILD_DIR")" ]; then
    printf 'coverage: this tree installs into\n     %s\n' "$INSTALLED_DIR" >&2
    printf '   which is named for a different build tree. Both will run vcpkg install\n' >&2
    printf '   against it and undo each other'"'"'s work on every configure. Reconfigure so\n' >&2
    printf '   this tree gets its own: cmake --preset <this-preset>\n' >&2
fi

# --------------------------------------------------------------- config name

if [ -n "$CONFIG_TYPES" ]; then
    [ -n "$CONFIG" ] || CONFIG=Debug
    case ";$CONFIG_TYPES;" in
        *";$CONFIG;"*) ;;
        *) die "config '$CONFIG' is not one of: $CONFIG_TYPES" ;;
    esac
else
    CONFIG="${BUILD_TYPE:-}"
fi

case "$CONFIG" in
    Debug|RelWithDebInfo|"") ;;
    *) die \
"config '$CONFIG' carries no instrumentation — cmake/Coverage.cmake gates the
   flags to Debug and RelWithDebInfo, so this would report zero counters." ;;
esac

# ------------------------------------------------------------------- backend

# CMake already determined this at configure time and wrote it down. The binary
# name settles nothing — MinGW installs the driver as c++.exe and so does
# AppleClang, and MinGW's --version banner names neither gcc nor GCC.
COMPILER_ID=""
compiler_record=$(find "$BUILD_DIR/CMakeFiles" -name CMakeCXXCompiler.cmake 2>/dev/null | head -1 || true)
if [ -n "$compiler_record" ]; then
    COMPILER_ID=$(grep -m1 "set(CMAKE_CXX_COMPILER_ID" "$compiler_record" | cut -d'"' -f2)
fi

case "$COMPILER_ID" in
    Clang|AppleClang) DETECTED=llvm ;;
    GNU)              DETECTED=gcov ;;
    *)
        # No compiler record — fall back to asking the driver. GCC's banner is
        # only identifiable by the FSF copyright line, so match the whole output.
        version=$("$CXX" --version 2>/dev/null || true)
        case "$version" in
            *clang*|*Clang*)                DETECTED=llvm ;;
            *"Free Software Foundation"*)   DETECTED=gcov ;;
            *)                              DETECTED="" ;;
        esac
        ;;
esac
if [ "$BACKEND" = auto ]; then
    BACKEND="$DETECTED"
fi
[ -n "$BACKEND" ] || die "cannot infer a backend from '$CXX'; pass --backend"

if [ -z "$OUT_DIR" ]; then
    OUT_DIR="$BUILD_DIR/coverage${CONFIG:+/$CONFIG}"
fi
mkdir -p "$OUT_DIR"

note "tree     $BUILD_DIR"
note "compiler ${COMPILER_ID:-unknown}   $CXX"
note "backend  $BACKEND${CONFIG:+   config $CONFIG}"
note "reports  $OUT_DIR"

# ------------------------------------------------------- locate the binaries

# Ninja Multi-Config appends the configuration to output directories, so prefer
# artefacts under a matching /<Config>/ component when the tree has any.
select_for_config() {
    if [ -n "$CONFIG" ]; then
        matched=$(printf '%s\n' "$1" | grep "/$CONFIG/" || true)
        [ -n "$matched" ] && { printf '%s\n' "$matched"; return; }
    fi
    printf '%s\n' "$1"
}

find_test_binary() {
    found=$(find "$BUILD_DIR" -type f \( -name order_test -o -name order_test.exe \) 2>/dev/null || true)
    [ -n "$found" ] || return 1
    select_for_config "$found" | head -1
}

# Only this project's own modules carry coverage mapping, and vcpkg's shared
# libraries are COPIED in beside them — so filtering on the vcpkg_installed path
# misses libcrypto, libfmtd, libsimdjson and friends entirely. Match module
# names taken from src/ instead, so the list cannot drift. strategy is an
# INTERFACE target and yields nothing, which is correct.
find_module_libraries() {
    for module_dir in "$REPO_ROOT"/src/*/; do
        m=$(basename "$module_dir")
        found=$(find "$BUILD_DIR" -type f -name "lib$m.dylib" -o \
                     -type f -name "lib$m.so" -o -type f -name "lib$m.so.*" -o \
                     -type f -name "lib$m.dll" -o -type f -name "$m.dll" \
                2>/dev/null || true)
        [ -n "$found" ] || continue
        select_for_config "$found" | head -1
    done
}

# ----------------------------------------------------------- build, then run

if [ "$DO_RUN" -eq 1 ]; then
    if [ "$BACKEND" = gcov ]; then
        note "clearing stale .gcda counters"
        find "$BUILD_DIR" -name '*.gcda' -delete 2>/dev/null || true
    else
        PROFRAW_DIR="$OUT_DIR/profraw"
        note "clearing stale .profraw files"
        rm -rf "$PROFRAW_DIR"
        mkdir -p "$PROFRAW_DIR"
        # %m keeps online-merged profiles in a bounded pool of files. Each gtest
        # case is its own ctest process here, so %p would leave hundreds behind.
        export LLVM_PROFILE_FILE="$PROFRAW_DIR/order_test-%m.profraw"
    fi

    note "building"
    if [ -n "$PRESET" ]; then
        cmake --build --preset "$PRESET-$(printf '%s' "$CONFIG" | tr '[:upper:]' '[:lower:]')"
    elif [ -n "$CONFIG_TYPES" ]; then
        cmake --build "$BUILD_DIR" --config "$CONFIG"
    else
        cmake --build "$BUILD_DIR"
    fi

    note "running the suite"
    if [ -n "$PRESET" ]; then
        ctest --preset "$PRESET-$(printf '%s' "$CONFIG" | tr '[:upper:]' '[:lower:]')" || \
            note "ctest reported failures; reporting coverage anyway"
    elif [ -n "$CONFIG_TYPES" ]; then
        ctest --test-dir "$BUILD_DIR" --build-config "$CONFIG" --output-on-failure || \
            note "ctest reported failures; reporting coverage anyway"
    else
        ctest --test-dir "$BUILD_DIR" --output-on-failure || \
            note "ctest reported failures; reporting coverage anyway"
    fi
fi

# ------------------------------------------------------------- gcov / gcovr

report_gcov() {
    command -v gcovr >/dev/null 2>&1 || die "gcovr not found (pip install gcovr)"

    # gcov must come from the toolchain that wrote the .gcno files.
    gcov_exe=$(dirname "$CXX")/gcov
    [ -x "$gcov_exe" ] || [ -x "$gcov_exe.exe" ] || gcov_exe=gcov

    # Counted rather than piped through head: pipefail turns find's SIGPIPE into
    # a pipeline failure, which would report "no counters" when there are some.
    gcda_count=$(find "$BUILD_DIR" -name '*.gcda' 2>/dev/null | wc -l | tr -d ' ')
    note "reading $gcda_count .gcda counter files"
    [ "$gcda_count" -gt 0 ] || die \
"no .gcda counters in $BUILD_DIR — the suite has not been run against this
   build. Drop --report-only, or run ctest first."

    # --filter is the load-bearing argument: without it the report is dominated
    # by libstdc++ and Boost headers and src/ disappears. See docs/coverage.md.
    set -- --root "$REPO_ROOT" --filter src/ \
           --exclude '.*\.test\.cpp' --exclude '.*\.fixture\.hpp' \
           --gcov-executable "$gcov_exe" \
           --exclude-unreachable-branches --exclude-throw-branches

    note "gcovr: line summary"
    gcovr "$@" --print-summary --txt "$OUT_DIR/lines.txt" "$BUILD_DIR"

    # --txt-metric arrived in gcovr 6; older builds spell it -b.
    note "gcovr: branch summary"
    if gcovr --help 2>&1 | grep -q -- --txt-metric; then
        gcovr "$@" --txt-metric branch --txt "$OUT_DIR/branches.txt" "$BUILD_DIR"
    else
        gcovr "$@" -b --txt "$OUT_DIR/branches.txt" "$BUILD_DIR"
    fi

    note "gcovr: cobertura xml"
    gcovr "$@" --xml-pretty --output "$OUT_DIR/cobertura.xml" "$BUILD_DIR"

    if [ "$DO_HTML" -eq 1 ]; then
        note "gcovr: html detail"
        mkdir -p "$OUT_DIR/html"
        gcovr "$@" --html-details "$OUT_DIR/html/index.html" "$BUILD_DIR"
    fi
}

# --------------------------------------------------------------- llvm-cov

llvm_tool() {
    tool="$1"
    candidate="$(dirname "$CXX")/$tool"
    if [ -x "$candidate" ]; then printf '%s\n' "$candidate"; return; fi
    if command -v xcrun >/dev/null 2>&1 && xcrun --find "$tool" >/dev/null 2>&1; then
        xcrun --find "$tool"; return
    fi
    command -v "$tool" 2>/dev/null || die \
"$tool not found. It must match the clang that built this tree — look beside
   $CXX, or install the matching LLVM."
}

report_llvm() {
    profdata_exe=$(llvm_tool llvm-profdata)
    cov_exe=$(llvm_tool llvm-cov)

    PROFRAW_DIR="${PROFRAW_DIR:-$OUT_DIR/profraw}"
    raws=$(find "$PROFRAW_DIR" -name '*.profraw' 2>/dev/null || true)
    if [ -z "$raws" ]; then
        # A run driven outside this script leaves them in the working directory.
        raws=$(find "$BUILD_DIR" -name '*.profraw' 2>/dev/null || true)
    fi
    [ -n "$raws" ] || die \
"no .profraw files found. The suite must run with LLVM_PROFILE_FILE set —
   drop --report-only and let this script drive it."

    note "merging $(printf '%s\n' "$raws" | wc -l | tr -d ' ') raw profiles"
    printf '%s\n' "$raws" > "$OUT_DIR/profraw.list"
    "$profdata_exe" merge -sparse -f "$OUT_DIR/profraw.list" -o "$OUT_DIR/coverage.profdata"

    test_bin=$(find_test_binary) || die "order_test not found under $BUILD_DIR"
    note "primary object $test_bin"

    # Every module is a SHARED library with its own coverage mapping; omitting
    # them is what makes a report contain headers only.
    set -- "$test_bin"
    libs=$(find_module_libraries)
    lib_count=0
    if [ -n "$libs" ]; then
        while IFS= read -r lib; do
            [ -n "$lib" ] || continue
            set -- "$@" -object "$lib"
            lib_count=$((lib_count + 1))
        done <<EOF
$libs
EOF
    fi
    note "additional objects: $lib_count module libraries"
    [ "$lib_count" -gt 0 ] || note \
        "WARNING: no module libraries found — the report will cover headers only"

    common="-instr-profile=$OUT_DIR/coverage.profdata -ignore-filename-regex=$IGNORE_RE"

    note "llvm-cov: per-file summary"
    # shellcheck disable=SC2086
    "$cov_exe" report "$@" $common > "$OUT_DIR/summary.txt"

    note "llvm-cov: lcov export"
    # shellcheck disable=SC2086
    "$cov_exe" export "$@" $common -format=lcov > "$OUT_DIR/coverage.lcov"

    if [ "$DO_HTML" -eq 1 ]; then
        note "llvm-cov: html detail"
        # shellcheck disable=SC2086
        "$cov_exe" show "$@" $common \
            -format=html -output-dir="$OUT_DIR/html" \
            -show-branches=count -show-regions -show-line-counts-or-regions \
            -show-instantiation-summary
    fi

    tail -1 "$OUT_DIR/summary.txt"
}

case "$BACKEND" in
    gcov) report_gcov ;;
    llvm) report_llvm ;;
    *)    die "unknown backend '$BACKEND'" ;;
esac

note "done"
printf '\n'
ls -1 "$OUT_DIR" | sed 's|^|  '"$OUT_DIR"'/|'
[ "$DO_HTML" -eq 1 ] && printf '\nOpen %s/html/index.html\n' "$OUT_DIR"
exit 0
