include_guard(GLOBAL)

# libnuma discovery for the arena's node-local pools (Linux only).
#
# arena::init(size, node) binds a pool to one NUMA node's local memory so hot
# allocations stay near the thread touching them. That path is compiled in only
# when this module finds libnuma; arena::init(size) alone remains available
# everywhere. Because arena.hpp itself includes <numa.h> and switches its
# declared surface on ORDER_BOOK_WITH_NUMA, both the include directory and the
# macro are part of core's PUBLIC interface — a consumer compiling against a
# NUMA-enabled core must see the same declarations core was built with.
#
# Enable with -D ORDER_BOOK_WITH_NUMA=ON (see ProjectOptions.cmake).
#
# exchange_engine::numa is always defined — an empty INTERFACE target when the
# option is off — so targets link it unconditionally and only their source
# lists need to test the option.

add_library(order_book_numa INTERFACE)
add_library(exchange_engine::numa ALIAS order_book_numa)

if(NOT ORDER_BOOK_WITH_NUMA)
    message(STATUS "NUMA support: OFF")
    return()
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
            "ORDER_BOOK_WITH_NUMA is Linux-only (libnuma); "
            "target system is ${CMAKE_SYSTEM_NAME}.")
endif()

find_path(NUMA_INCLUDE_DIR NAMES numa.h)
find_library(NUMA_LIBRARY NAMES numa)
if(NOT NUMA_INCLUDE_DIR OR NOT NUMA_LIBRARY)
    message(FATAL_ERROR
            "ORDER_BOOK_WITH_NUMA=ON but libnuma was not found. "
            "Install it (Debian/Ubuntu: libnuma-dev, Fedora: numactl-devel) "
            "or configure with -DORDER_BOOK_WITH_NUMA=OFF.")
endif()

target_include_directories(order_book_numa INTERFACE ${NUMA_INCLUDE_DIR})
target_link_libraries(order_book_numa INTERFACE ${NUMA_LIBRARY})
target_compile_definitions(order_book_numa INTERFACE ORDER_BOOK_WITH_NUMA)

message(STATUS "NUMA support: ON (${NUMA_LIBRARY})")
