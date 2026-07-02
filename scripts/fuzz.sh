#!/usr/bin/env bash
# Build, fuzz, or replay-crash the order_book harnesses with AFL++ (Linux/macOS).
#
# The Windows counterpart is fuzz.ps1 (WinAFL + DynamoRIO). This path uses AFL++
# source instrumentation (afl-clang-fast) and the harnesses' persistent-mode loop,
# which is faster and supports sanitizers.
#
# Usage:
#   ./fuzz.sh build [parse|engine|all]   # configure (fuzzing ON) + build; default all
#   ./fuzz.sh fuzz  [parse|engine]       # start/resume a session (Ctrl+C to stop)
#   ./fuzz.sh fuzz  [parse|engine] --fresh   # wipe the out dir and start over
#   ./fuzz.sh replay [parse|engine] [crash-file]   # run a crash through the harness
#
# An existing session is resumed by default (afl-fuzz -i -); pass --fresh to wipe it.
#
# Environment:
#   AFL_CC / AFL_CXX   compiler wrappers (default afl-clang-fast / afl-clang-fast++)
#   VCPKG_ROOT         if set, its toolchain file is used to find fmt/simdjson/...
#   AFL_USE_ASAN=1     export before `build` for AddressSanitizer
#   AFL_USE_UBSAN=1    export before `build` for UBSan (catches the parser int overflow)
set -euo pipefail

proj="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$proj/build/fuzz"
bin="$build/bin"

# harness -> exe + corpus subdir + optional dictionary
harness_exe()    { [[ "$1" == engine ]] && echo engine_ops_fuzz || echo parse_depth_fuzz; }
harness_corpus() { [[ "$1" == engine ]] && echo "$proj/fuzz/corpus/engine_ops" || echo "$proj/fuzz/corpus/parse_depth"; }
harness_dict()   { [[ "$1" == engine ]] && echo "" || echo "$proj/fuzz/dictionaries/binance_depth.dict"; }

# Pull the --fresh flag out of the positional args wherever it appears.
fresh=0
argv=()
for a in "$@"; do
  if [[ "$a" == "--fresh" ]]; then fresh=1; else argv+=("$a"); fi
done
set -- ${argv[@]+"${argv[@]}"}   # empty-array-safe under set -u (macOS bash 3.2)

action="${1:-fuzz}"
target="${2:-}"

do_build() {
  local which="${1:-all}"
  local cc="${AFL_CC:-afl-clang-fast}" cxx="${AFL_CXX:-afl-clang-fast++}"
  command -v "$cc"  >/dev/null || { echo "error: $cc not on PATH (install AFL++)"; exit 1; }
  local args=(-S "$proj" -B "$build" -DORDERBOOK_ENABLE_FUZZING=ON -DCMAKE_BUILD_TYPE=Release
              -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx")
  [[ -n "${VCPKG_ROOT:-}" ]] && args+=(-DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake")
  cmake "${args[@]}"
  if [[ "$which" == all ]]; then
    cmake --build "$build" --target parse_depth_fuzz engine_ops_fuzz
  else
    cmake --build "$build" --target "$(harness_exe "$which")"
  fi
  echo "[+] Built in $bin"
}

do_fuzz() {
  [[ -n "$target" ]] || { echo "error: specify parse or engine"; exit 1; }
  local name; name="$(harness_exe "$target")"
  local exe="$bin/$name"
  [[ -x "$exe" ]] || { echo "error: $exe not built. Run: ./fuzz.sh build $target"; exit 1; }
  local out="$build/out/$target"

  # A Ctrl+C'd run can leave the target alive, holding the out dir; reap it. Match
  # on the full binary path (comm is truncated to 15 chars, so -x is unreliable).
  if command -v pkill >/dev/null && pgrep -f "$exe" >/dev/null 2>&1; then
    echo "[*] Stopping stale $name process(es)"
    pkill -f "$exe" || true
  fi

  if (( fresh )) && [[ -d "$out" ]]; then rm -rf "$out"; fi
  mkdir -p "$out"

  # Resume an existing session (afl-fuzz -i -) rather than reusing/wiping it,
  # unless --fresh was asked for or there is no prior session to resume.
  local in
  if (( ! fresh )) && [[ -f "$out/default/fuzzer_stats" ]]; then
    in='-'; echo "[*] Resuming existing session in $out  (use --fresh to start over)"
  else
    in="$(harness_corpus "$target")"
  fi

  local dict; dict="$(harness_dict "$target")"
  local xopt=(); [[ -n "$dict" ]] && xopt=(-x "$dict")
  echo "[*] Fuzzing $name  (Ctrl+C to stop; crashes -> $out/default/crashes)"
  afl-fuzz -i "$in" -o "$out" ${xopt[@]+"${xopt[@]}"} -- "$exe"
}

do_replay() {
  [[ -n "$target" ]] || { echo "error: specify parse or engine"; exit 1; }
  local exe; exe="$bin/$(harness_exe "$target")"
  local crash="${3:-}"
  if [[ -z "$crash" ]]; then
    crash="$(ls -t "$build/out/$target"/*/crashes/id* 2>/dev/null | head -n1 || true)"
    [[ -n "$crash" ]] || { echo "error: no crashes under $build/out/$target"; exit 1; }
  fi
  echo "[*] Replaying $crash"
  "$exe" < "$crash"; echo "[+] exit=$?"
}

case "$action" in
  build)  do_build "${2:-all}" ;;
  fuzz)   do_fuzz ;;
  replay) do_replay "$@" ;;
  *) echo "usage: ./fuzz.sh {build|fuzz|replay} [parse|engine] [crash-file]"; exit 1 ;;
esac
