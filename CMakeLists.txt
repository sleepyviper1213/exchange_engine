cmake_minimum_required(VERSION 3.30)

project(
    order_book
    VERSION 0.1.0
    LANGUAGES CXX)


include(cmake/ProjectOptions.cmake)
include(cmake/CCache.cmake)
include(cmake/Sanitizers.cmake)
include(cmake/Dependencies.cmake)
include(cmake/Coverage.cmake)
include(cmake/StaticAnalyzers.cmake)
include(cmake/Warnings.cmake)
include(cmake/Hardening.cmake)
include(cmake/ExportHeaders.cmake)
include(cmake/Malloc.cmake)
include(cmake/Numa.cmake)
include(cmake/Dpdk.cmake)

add_subdirectory(configured_files)
add_subdirectory(src)

if(ORDER_BOOK_BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()

if(ORDER_BOOK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
