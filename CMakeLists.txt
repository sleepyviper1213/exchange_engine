cmake_minimum_required(VERSION 4.2)
project(order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/sanitizers/Address.cmake)
include(cmake/sanitizers/Thread.cmake)
include(cmake/Profile.cmake)

find_package(fmt REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(benchmark CONFIG REQUIRED)
find_package(simdjson CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()


enable_testing()

# Boost.Asio/Beast (pulled in by the tools) warns and assumes Windows 7 when the
# target Windows version is unset. Pin it explicitly to silence the warning.
# 0x0601 = Windows 7 (Boost's own suggestion); raise to 0x0A00 for Windows 10 APIs.
if (WIN32)
    # Put every executable and DLL (and, on MSVC, their PDBs) in one directory per
    # config. A built exe then finds order_book.dll beside it with no copy step, and
    # vcpkg's application-local deployment (VCPKG_APPLOCAL_DEPS, on by default for the
    # dynamic triplets) drops the third-party DLLs into that same directory.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    add_compile_definitions(_WIN32_WINNT=0x0601)
endif ()

add_subdirectory(src)
add_subdirectory(test)

add_subdirectory(benchmark)