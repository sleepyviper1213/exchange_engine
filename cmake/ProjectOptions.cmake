include_guard(GLOBAL)

option(ORDER_BOOK_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(ORDER_BOOK_BUILD_BENCHMARKS "Build benchmarks" ${PROJECT_IS_TOP_LEVEL})
option(ORDER_BOOK_WITH_DPDK
       "Build the Linux DPDK kernel-bypass transport"
       OFF)
option(ORDER_BOOK_WITH_NUMA
       "Bind arena pools to NUMA-local memory via libnuma (Linux only)"
       OFF)
option(ORDER_BOOK_ENABLE_COVERAGE OFF)
option(ORDER_BOOK_ENABLE_HARDENING
       "Enable runtime hardening (keep assert() live, stdlib assertions, stack/buffer protection)"
       OFF)
option(ORDER_BOOK_ENABLE_CLANG_TIDY OFF)
option(ORDER_BOOK_ENABLE_CPPCHECK OFF)
option(ORDER_BOOK_ENABLE_STATIC_ANALYZERS OFF)
option(ORDER_BOOK_ENABLE_CCACHE "Enable ccache in development phase" ${PROJECT_IS_TOP_LEVEL})

if(ORDER_BOOK_BUILD_TESTS)
    add_compile_definitions(ORDER_BOOK_BUILD_TESTS)
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

if (WIN32)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif ()
