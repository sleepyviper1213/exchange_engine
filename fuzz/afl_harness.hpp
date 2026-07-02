#pragma once

#include <cstddef>
#include <cstdint>

/// @file
/// @brief Portable fuzz-harness driver shared by the harnesses.
///
/// One macro, @c FUZZ_MAIN(run), wires a @c run(const std::uint8_t*, std::size_t)
/// callback to whichever driver the build targets:
///
/// - **Linux / AFL++** (afl-clang-fast, defines @c __AFL_COMPILER): fork-server
///   persistent loop over @c __AFL_LOOP — one process, many inputs.
/// - **Windows / WinAFL** (DynamoRIO): exports @c winafl_target, which WinAFL hooks
///   and re-invokes once per testcase, re-reading the input file (@c \@\@) each time.
/// - **Any other build**: reads one testcase (a file argument, else stdin) and runs
///   @p run once — the crash-replay path for a debugger or an ASan build.

#if defined(__AFL_COMPILER)
// ---------------------------------------------------------------------------
// Linux: AFL++ persistent mode.
// ---------------------------------------------------------------------------
__AFL_FUZZ_INIT();

template <typename Fn>
int afl_run(Fn target) {
#ifdef __AFL_HAVE_MANUAL_CONTROL
    __AFL_INIT();
#endif
    const std::uint8_t *buf = reinterpret_cast<const std::uint8_t *>(__AFL_FUZZ_TESTCASE_BUF);
    while (__AFL_LOOP(10000)) {
        target(buf, static_cast<std::size_t>(__AFL_FUZZ_TESTCASE_LEN));
    }
    return 0;
}

#define FUZZ_MAIN(run)                                                         \
    int main() { return afl_run(run); }

#else
// ---------------------------------------------------------------------------
// Windows (WinAFL) + portable crash-replay.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#define WINAFL_EXPORT __declspec(dllexport)
#else
#define WINAFL_EXPORT
#endif

namespace afl_detail {

/// @brief Read an entire file into a byte buffer (binary; no CRLF translation).
/// @param path Filesystem path to read.
/// @return The file's bytes, or an empty buffer if it could not be opened.
inline std::vector<std::uint8_t> read_file(const char *path) {
    std::vector<std::uint8_t> data;
    std::FILE *fp = std::fopen(path, "rb");
    if (fp == nullptr) return data;
    std::uint8_t chunk[4096];
    for (std::size_t n; (n = std::fread(chunk, 1, sizeof chunk, fp)) > 0;) {
        data.insert(data.end(), chunk, chunk + n);
    }
    std::fclose(fp);
    return data;
}

/// @brief Read all of stdin into a byte buffer (binary on Windows too).
inline std::vector<std::uint8_t> read_stdin() {
#if defined(_WIN32)
    // Default stdin is text mode: it mangles CRLF and stops at Ctrl-Z (0x1A),
    // corrupting binary op-scripts. Switch to raw bytes.
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    std::vector<std::uint8_t> data;
    std::uint8_t chunk[4096];
    for (std::size_t n; (n = std::fread(chunk, 1, sizeof chunk, stdin)) > 0;) {
        data.insert(data.end(), chunk, chunk + n);
    }
    return data;
}

} // namespace afl_detail

#define FUZZ_MAIN(run)                                                         \
    /* WinAFL (DynamoRIO) hooks this exported symbol and loops it, rewriting */ \
    /* the @@ file between iterations; -target_method winafl_target -nargs 1. */ \
    extern "C" WINAFL_EXPORT int winafl_target(const char *path) {             \
        const auto data = afl_detail::read_file(path);                         \
        run(data.data(), data.size());                                         \
        return 0;                                                              \
    }                                                                          \
    int main(int argc, char **argv) {                                          \
        if (argc > 1) return winafl_target(argv[1]);                           \
        const auto data = afl_detail::read_stdin();                            \
        run(data.data(), data.size());                                         \
        return 0;                                                              \
    }

#endif
