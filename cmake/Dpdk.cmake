include_guard(GLOBAL)

# DPDK discovery for the kernel-bypass RX transport (Linux only).
#
# DPDK ships pkg-config metadata (libdpdk.pc) rather than a CMake config
# package, and its link line is large and order-sensitive — the rte_* driver
# libraries need whole-archive/linker-group handling that only the .pc file
# gets right. So discovery goes through PkgConfig::DPDK instead of a hand-rolled
# find_library sweep.
#
# Enable with -D ORDER_BOOK_WITH_DPDK=ON (see ProjectOptions.cmake), with
# libdpdk's pkg-config directory on PKG_CONFIG_PATH.
#
# Two targets, because the rte_* machinery must not leak past the transport
# library. All DPDK headers are confined to dpdk.cpp, while transport.hpp,
# transport/fwd.hpp and dpdk.hpp switch their declared surface on the macro:
#
#   exchange_engine::dpdk_api  macro only — link PUBLIC, so consumers compiling
#                              against transport's headers agree with how it was
#                              built.
#   exchange_engine::dpdk      the full compile + link line — link PRIVATE, so
#                              the driver libraries stay inside transport.
#
# Both are always defined (empty INTERFACE targets when the option is off), so
# targets link them unconditionally and only their source lists test the option.

add_library(order_book_dpdk_api INTERFACE)
add_library(exchange_engine::dpdk_api ALIAS order_book_dpdk_api)

add_library(order_book_dpdk INTERFACE)
add_library(exchange_engine::dpdk ALIAS order_book_dpdk)

# The implementation needs the macro too, not just the link line.
target_link_libraries(order_book_dpdk INTERFACE order_book_dpdk_api)

if(NOT ORDER_BOOK_WITH_DPDK)
    message(STATUS "DPDK transport: OFF")
    return()
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
            "ORDER_BOOK_WITH_DPDK is Linux-only; "
            "target system is ${CMAKE_SYSTEM_NAME}.")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK IMPORTED_TARGET GLOBAL libdpdk)
if(NOT DPDK_FOUND)
    message(FATAL_ERROR
            "ORDER_BOOK_WITH_DPDK=ON but pkg-config could not find libdpdk. "
            "Install the DPDK development package (Debian/Ubuntu: dpdk-dev, "
            "Fedora: dpdk-devel) or point PKG_CONFIG_PATH at the directory "
            "holding libdpdk.pc, or configure with -DORDER_BOOK_WITH_DPDK=OFF.")
endif()

target_compile_definitions(order_book_dpdk_api INTERFACE ORDER_BOOK_WITH_DPDK=1)
target_link_libraries(order_book_dpdk INTERFACE PkgConfig::DPDK)

message(STATUS "DPDK transport: ON (libdpdk ${DPDK_VERSION})")
