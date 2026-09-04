#!/usr/bin/env bash
#
# The pre-merge gate: static analysis, then the suite with coverage.
#
# Two build trees, run in sequence. They cannot be one workflow preset -
# cmake-presets(7) requires that every non-configure step in a workflow name
# the same configurePreset as its single leading configure step, so a workflow
# spans exactly one build tree. Chaining two is this script's whole job.
#
# The split is not only a schema workaround. Analysis compiles and never runs
# anything; coverage runs the suite and needs its counters. Welding them into
# one tree meant clang-tidy's guards in test/CMakeLists.txt stripped both unity
# batching and the precompiled header from the tree that also had to run the
# tests - the slowest configuration in the project, for no gain.
#
# Usage:
#   scripts/premerge.sh                        # defaults for this platform
#   scripts/premerge.sh -a linux-gcc-analysis  # analyse with GCC instead
#   scripts/premerge.sh --no-coverage          # analysis only
#
# CI does not use this script: two GitHub jobs cost the max of the two trees
# where this costs the sum. @see .github/workflows/ci.yml

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO_ROOT"

die() { printf 'premerge: %s\n' "$*" >&2; exit 1; }
note() { printf '\033[1m==> %s\033[0m\n' "$*"; }

# Clang is the better analysis host: clang-tidy re-parses the compiler's own
# command line, so on a Clang tree none of the GCC reconciliation in
# cmake/StaticAnalyzers.cmake is needed. MinGW is the only choice on Windows.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) ANALYSIS=windows-mingw-analysis; COVERAGE=windows-mingw-coverage ;;
    Linux)                ANALYSIS=linux-clang-analysis;   COVERAGE=linux-gcc-coverage ;;
    Darwin)               ANALYSIS="";                     COVERAGE=macos-arm64-llvm-coverage ;;
    *)                    ANALYSIS="";                     COVERAGE="" ;;
esac

CONFIG=Debug
DO_ANALYSIS=1
DO_COVERAGE=1

usage() { sed -n '3,23p' "$0" | sed 's/^# \{0,1\}//'; cat <<'EOF'

Options:
  -a, --analysis PRESET   Analysis configure preset. Default: per platform.
  -c, --coverage PRESET   Coverage configure preset. Default: per platform.
      --config CFG        Debug or RelWithDebInfo for the coverage tree.
                          Default: Debug.
      --no-analysis       Skip the analysis tree.
      --no-coverage       Skip the coverage tree and its report.
  -h, --help              This text.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -a|--analysis) ANALYSIS=${2:-}; shift 2 ;;
        -c|--coverage) COVERAGE=${2:-}; shift 2 ;;
        --config)      CONFIG=${2:-};   shift 2 ;;
        --no-analysis) DO_ANALYSIS=0;   shift ;;
        --no-coverage) DO_COVERAGE=0;   shift ;;
        -h|--help)     usage; exit 0 ;;
        *)             die "unknown argument '$1' (see --help)" ;;
    esac
done

if [ "$DO_ANALYSIS" = 1 ]; then
    [ -n "$ANALYSIS" ] || die "no analysis preset for $(uname -s); pass -a, or --no-analysis"
    note "Static analysis: $ANALYSIS"
    cmake --workflow --preset "$ANALYSIS"
fi

if [ "$DO_COVERAGE" = 1 ]; then
    [ -n "$COVERAGE" ] || die "no coverage preset for $(uname -s); pass -c, or --no-coverage"
    lower=$(printf '%s' "$CONFIG" | tr '[:upper:]' '[:lower:]')
    note "Tests and coverage: $COVERAGE / $CONFIG"
    cmake --workflow --preset "${COVERAGE}-${lower}"
    # --report-only reads the counters the run above just wrote, so the suite is
    # neither rebuilt nor re-executed.
    scripts/coverage.sh -B "build/${COVERAGE}" -c "$CONFIG" --report-only
fi

note "Pre-merge gate passed"
