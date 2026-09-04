include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(ORDER_BOOK_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(ORDER_BOOK_BUILD_BENCHMARKS "Build benchmarks" ${PROJECT_IS_TOP_LEVEL})
option(ORDER_BOOK_WITH_DPDK "Build the Linux DPDK kernel-bypass transport" OFF)
option(ORDER_BOOK_WITH_NUMA
       "Bind arena pools to NUMA-local memory via libnuma (Linux only)" OFF)
option(ORDER_BOOK_ENABLE_CCACHE
       "Use ccache as the compiler launcher when found" ${PROJECT_IS_TOP_LEVEL})
option(ORDER_BOOK_ENABLE_IPO
       "Enable IPO/LTO on the Release and RelWithDebInfo configurations"
       ${PROJECT_IS_TOP_LEVEL})
option(ORDER_BOOK_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

option(ORDER_BOOK_ENABLE_PCH
       "Precompile the test suite's common headers when unity batching is off"
       ${PROJECT_IS_TOP_LEVEL})
option(ORDER_BOOK_ENABLE_UNITY_BUILD "Compile in unity batches" OFF)
option(ORDER_BOOK_ENABLE_COVERAGE "Instrument for coverage reporting" OFF)
option(ORDER_BOOK_ENABLE_STATIC_ANALYZERS "Run clang-tidy and cppcheck" OFF)
option(ORDER_BOOK_ENABLE_CLANG_TIDY "Run clang-tidy only" OFF)
option(ORDER_BOOK_ENABLE_CPPCHECK "Run cppcheck only" OFF)
option(ORDER_BOOK_WARN_UNSAFE_BUFFERS
    "Enable Clang -Wunsafe-buffer-usage (noisy; never promoted to error)"
    OFF)

# ON / fast    → enable with libc++ fast mode (recommended default)
#
# Hardening does NOT undefine NDEBUG. Release keeps assert() off; use
# libstdc++/libc++ lightweight checks instead. See ${CMAKE_CURRENT_LIST_DIR}/Hardening.cmake.
set(ORDER_BOOK_HARDENING
    "ON"
    CACHE STRING "Hardening level: OFF | ON | none | fast | extensive | debug")
set_property(
    CACHE ORDER_BOOK_HARDENING
    PROPERTY STRINGS
             OFF
             ON
             none
             fast
             extensive
             debug)

option(ORDER_BOOK_HARDENING_UBSAN_TRAP
       "Trap on undefined behaviour instead of continuing (Clang/GCC)" OFF)

set(ORDER_BOOK_SANITIZER
    "OFF"
    CACHE STRING "Which sanitizer config to register (exactly one): Address | Thread | Undefined | Leak | Memory | HWAddress")

set_property(
    CACHE ORDER_BOOK_SANITIZER
    PROPERTY STRINGS
             OFF
             Address
             Thread
             Undefined
             Leak
             Memory
             HWAddress)
# ---------------------------------------------------------------------------
# Cross-option guards
# ---------------------------------------------------------------------------

if(ORDER_BOOK_ENABLE_UNITY_BUILD)
    if(ORDER_BOOK_ENABLE_COVERAGE)
        message(
            FATAL_ERROR
                "Misleading metrics when combining unity build and code coverage."
        )
    endif()
    if(NOT ORDER_BOOK_WARNINGS_AS_ERRORS)
        message(
            WARNING
                "Warnings as errors is important in unity build. A macro redefined in"
                " a subsequent source file could affect the compiled code drastically."
        )
    endif()

    if(CMAKE_GENERATOR MATCHES "Visual Studio")
        message(
            FATAL_ERROR
                "ORDER_BOOK_ENABLE_UNITY_BUILD is not supported under the "
                "${CMAKE_GENERATOR} generator: object-name disambiguation is "
                "lost (MSB8027) and SKIP_UNITY_BUILD_INCLUSION sources are "
                "dropped rather than compiled (LNK2019). Use a Ninja preset.")
    endif()

    set(CMAKE_UNITY_BUILD ON)
    set(CMAKE_UNITY_BUILD_MODE BATCH)
    set(CMAKE_UNITY_BUILD_BATCH_SIZE 8)
    message(STATUS "Unity build: enabled with batch size of 8.")
endif()

if(ORDER_BOOK_HARDENING_UBSAN_TRAP AND ORDER_BOOK_SANITIZER)
    message(
        WARNING
            "ORDER_BOOK_HARDENING_UBSAN_TRAP plus sanitizer configs both enable UBSan. "
            "Trap flags are suppressed under sanitizer configs.")
endif()

# ---------------------------------------------------------------------------
# Language / visibility / build type
# ---------------------------------------------------------------------------

if(PROJECT_IS_TOP_LEVEL
   AND NOT CMAKE_CONFIGURATION_TYPES
   AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE
        Release
        CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_BUILD_TYPE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release
                                                 RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
    set(CMAKE_COLOR_DIAGNOSTICS ON)
endif()

# Prevent in-source builds
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(
        FATAL_ERROR
            "
        In-source builds are strictly prohibited!
        Please clear the generated files (e.g., CMakeCache.txt) and use an out-of-source build:
        cmake -S . -B build
    ")
endif()

if(WIN32)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/CCache.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/Linker.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/InterProceduralOptimisation.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/Sanitizers.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/StaticAnalyzers.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/Warnings.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/Hardening.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/Malloc.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/Numa.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/Dpdk.cmake)
