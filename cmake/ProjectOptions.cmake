include_guard(GLOBAL)

option(ORDER_BOOK_BUILD_TESTS
    "Build tests"
    ${PROJECT_IS_TOP_LEVEL})
option(ORDER_BOOK_BUILD_BENCHMARKS
    "Build benchmarks"
    ${PROJECT_IS_TOP_LEVEL})
option(ORDER_BOOK_WITH_DPDK
    "Build the Linux DPDK kernel-bypass transport"
    OFF)
option(ORDER_BOOK_WITH_NUMA
    "Bind arena pools to NUMA-local memory via libnuma (Linux only)"
    OFF)
option(ORDER_BOOK_ENABLE_CCACHE
    "Use ccache as the compiler launcher when found"
    ${PROJECT_IS_TOP_LEVEL})
option(ORDER_BOOK_ENABLE_IPO
    "Enable IPO/LTO on the Release and RelWithDebInfo configurations"
    ${PROJECT_IS_TOP_LEVEL})
option(ORDER_BOOK_WARNINGS_AS_ERRORS
    "Treat compiler warnings as errors"
    OFF)
option(ORDER_BOOK_ENABLE_CCACHE
    "Use ccache as the compiler launcher when found"
    ${PROJECT_IS_TOP_LEVEL})
option(ORDER_BOOK_ENABLE_SANITIZERS
    "Use sanitizers(address, thread, etc.)"
    ${PROJECT_IS_TOP_LEVEL})
option(ORDER_BOOK_ENABLE_PCH
    "Precompile the test suite's common headers when unity batching is off"
    ${PROJECT_IS_TOP_LEVEL})
option(ORDER_BOOK_ENABLE_UNITY_BUILD
    "Compile src/ and the test suite in unity batches"
    OFF)
option(ORDER_BOOK_ENABLE_COVERAGE
    "Instrument for coverage reporting"
    OFF)
option(ORDER_BOOK_ENABLE_HARDENING
    "Enable runtime hardening (keep assert() live, stdlib assertions, stack/buffer protection)"
    OFF)
option(ORDER_BOOK_ENABLE_STATIC_ANALYZERS
    "Run clang-tidy and cppcheck"
    OFF)
option(ORDER_BOOK_ENABLE_CLANG_TIDY
    "Run clang-tidy only"
    OFF)
option(ORDER_BOOK_ENABLE_CPPCHECK
    "Run cppcheck only"
    OFF)
option(ORDER_BOOK_HARDENING_UBSAN_TRAP
    "Trap on undefined behaviour in hardened builds; adds hot-path checks"
    OFF)

# if(ORDER_BOOK_ENABLE_UNITY_BUILD AND CMAKE_GENERATOR MATCHES "Visual Studio")
#     message(STATUS
#         "unity: NOT enabled — the ${CMAKE_GENERATOR} generator lists sources "
#         "without their per-file settings, which loses object-name "
#         "disambiguation (MSB8027 on same-named test files) and drops "
#         "SKIP_UNITY_BUILD_INCLUSION sources from the build entirely "
#         "(LNK2019). Configure with a Ninja-based preset to batch.")
#     set(ORDER_BOOK_ENABLE_UNITY_BUILD OFF)
# endif()

if(ORDER_BOOK_ENABLE_UNITY_BUILD AND NOT ORDER_BOOK_WARNINGS_AS_ERRORS)
    message(WARNING
        "Warnings as errors is important in unity build. A macro redefined in"
        " a subsequent source file could affect drastically the compile code.")
endif()

if(ORDER_BOOK_ENABLE_UNITY_BUILD)
    set(CMAKE_UNITY_BUILD ON)
    set(CMAKE_UNITY_BUILD_MODE BATCH)
    set(CMAKE_UNITY_BUILD_BATCH_SIZE 8)
    message(STATUS "unity: src/ in batches of 8, order_test in batches of 16")
endif()

if(PROJECT_IS_TOP_LEVEL
    AND NOT CMAKE_CONFIGURATION_TYPES
    AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE
        Release
        CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)


if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

endif()

# Prevent in-source builds
if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
    message(FATAL_ERROR "
        In-source builds are strictly prohibited!
        Please clear the generated files (e.g., CMakeCache.txt) and use an out-of-source build:
        cmake -S . -B build
    ")
endif()

if(WIN32)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

include(cmake/CCache.cmake)
include(cmake/Linker.cmake)
include(cmake/InterProceduralOptimisation.cmake)
include(cmake/Sanitizers.cmake)
include(cmake/StaticAnalyzers.cmake)
include(cmake/Warnings.cmake)
include(cmake/Hardening.cmake)
include(cmake/Malloc.cmake)
include(cmake/Numa.cmake)
include(cmake/Dpdk.cmake)
