cmake_minimum_required(VERSION 4.2)
project(order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put every executable and DLL (and, on MSVC, their PDBs) in one directory per
# config. A built exe then finds order_book.dll beside it with no copy step, and
# vcpkg's application-local deployment (VCPKG_APPLOCAL_DEPS, on by default for the
# dynamic triplets) drops the third-party DLLs into that same directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(cmake/Sanitize.cmake)
include(cmake/Profile.cmake)

find_package(fmt REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(benchmark CONFIG REQUIRED)
find_package(simdjson CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Register tests with CTest so `ctest` / the test presets can find them.
enable_testing()
#
## Copies MSVC's AddressSanitizer runtime DLL next to <target> in the Sanitize
## config, so the instrumented executable can launch outside a Developer prompt
## (0xC0000135 / STATUS_DLL_NOT_FOUND otherwise). The DLL ships beside cl.exe and
## is not a TARGET_RUNTIME_DLL. No-op on non-MSVC and in non-Sanitize configs.
#function(order_book_copy_asan_runtime target)
#    if (MSVC)
#        get_filename_component(_msvc_bindir "${CMAKE_CXX_COMPILER}" DIRECTORY)
#        add_custom_command(TARGET ${target} POST_BUILD
#                COMMAND ${CMAKE_COMMAND} -E
#                $<IF:$<CONFIG:Sanitize>,copy_if_different,true>
#                "${_msvc_bindir}/clang_rt.asan_dynamic-x86_64.dll"
#                "$<TARGET_FILE_DIR:${target}>")
#    endif ()
#endfunction()

# Boost.Asio/Beast (pulled in by the tools) warns and assumes Windows 7 when the
# target Windows version is unset. Pin it explicitly to silence the warning.
# 0x0601 = Windows 7 (Boost's own suggestion); raise to 0x0A00 for Windows 10 APIs.
if (WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0601)
endif ()

add_subdirectory(src)
add_subdirectory(test)

add_subdirectory(benchmark)

# AFL++ coverage-guided fuzz harnesses. Off by default; configure with an AFL++
# compiler (afl-clang-fast/afl-clang-lto) to instrument. See fuzz/README.md.
option(ORDERBOOK_ENABLE_FUZZING "Build the AFL++ fuzz harnesses" OFF)
if (ORDERBOOK_ENABLE_FUZZING)
    add_subdirectory(fuzz)
endif ()