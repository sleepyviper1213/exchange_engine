include_guard(GLOBAL)

# Global allocator backend selection.
#
# The engine's custom allocators (memory::MallocResource and everything layered
# on ::operator new) route through the process's global malloc/new. Replacing
# that global allocator is a pure link-time choice — no code change — so this
# module exposes one INTERFACE target, exchange_engine::malloc, that executables
# link. Selecting tcmalloc or jemalloc makes every unqualified allocation (and
# every MallocResource-backed container) dispatch into that allocator.
#
#   -D USE_ALLOC=system    (default) the platform C library allocator
#   -D USE_ALLOC=mimalloc  mimalloc        (vcpkg feature "mimalloc")
#   -D USE_ALLOC=tcmalloc  Google tcmalloc (vcpkg feature "tcmalloc" -> gperftools)
#   -D USE_ALLOC=jemalloc  jemalloc        (vcpkg feature "jemalloc")
#
# Off by default so stock builds are unaffected. The allocator libraries are
# vcpkg manifest FEATURES, not default dependencies, so a plain configure never
# fetches them — activate the matching feature before configuring the toolchain:
#   -D VCPKG_MANIFEST_FEATURES=mimalloc   (or set it in your CMakePresets)
# Pick one the toolchain can build: mimalloc works on MSVC/MinGW/Linux; jemalloc's
# C++ shim does NOT compile on GCC 16 MinGW (std::__throw_bad_alloc removed), so
# prefer mimalloc (or system/tcmalloc) there.

set(USE_ALLOC "system" CACHE STRING "Global malloc backend: system, mimalloc, tcmalloc, or jemalloc")
set_property(CACHE USE_ALLOC PROPERTY STRINGS system mimalloc tcmalloc jemalloc)

add_library(USE_ALLOC INTERFACE)
add_library(exchange_engine::malloc ALIAS USE_ALLOC)

if(USE_ALLOC STREQUAL "system")
    message(STATUS "Malloc backend: system (default)")

elseif(USE_ALLOC STREQUAL "mimalloc")
    find_package(mimalloc CONFIG QUIET)
    if(TARGET mimalloc)
        target_link_libraries(USE_ALLOC INTERFACE mimalloc)
    elseif(TARGET mimalloc-static)
        target_link_libraries(USE_ALLOC INTERFACE mimalloc-static)
    else()
        find_library(EE_MIMALLOC_LIB NAMES mimalloc)
        if(NOT EE_MIMALLOC_LIB)
            message(FATAL_ERROR
                "USE_ALLOC=mimalloc but no mimalloc library was found. "
                "Activate the vcpkg 'mimalloc' feature (VCPKG_MANIFEST_FEATURES=mimalloc).")
        endif()
        target_link_libraries(USE_ALLOC INTERFACE ${EE_MIMALLOC_LIB})
    endif()
    # Windows note: transparent malloc/new override also needs mimalloc-redirect.dll
    # beside the exe — vcpkg's applocal step copies it next to linked consumers.
    message(STATUS "Malloc backend: mimalloc")

elseif(USE_ALLOC STREQUAL "tcmalloc")
    # Prefer the vcpkg config target; fall back to a bare library search.
    find_package(unofficial-gperftools CONFIG QUIET)
    if(TARGET unofficial::gperftools::tcmalloc_minimal)
        target_link_libraries(USE_ALLOC INTERFACE unofficial::gperftools::tcmalloc_minimal)
    elseif(TARGET unofficial::gperftools::tcmalloc)
        target_link_libraries(USE_ALLOC INTERFACE unofficial::gperftools::tcmalloc)
    else()
        find_library(EE_TCMALLOC_LIB NAMES tcmalloc_minimal tcmalloc)
        if(NOT EE_TCMALLOC_LIB)
            message(FATAL_ERROR
                "USE_ALLOC=tcmalloc but no tcmalloc library was found. "
                "Install gperftools (e.g. 'vcpkg install gperftools') for this toolchain.")
        endif()
        target_link_libraries(USE_ALLOC INTERFACE ${EE_TCMALLOC_LIB})
    endif()
    message(STATUS "Malloc backend: tcmalloc")

elseif(USE_ALLOC STREQUAL "jemalloc")
    find_package(unofficial-jemalloc CONFIG QUIET)
    if(TARGET unofficial::jemalloc::jemalloc)
        target_link_libraries(USE_ALLOC INTERFACE unofficial::jemalloc::jemalloc)
    else()
        find_library(EE_JEMALLOC_LIB NAMES jemalloc)
        if(NOT EE_JEMALLOC_LIB)
            message(FATAL_ERROR
                "USE_ALLOC=jemalloc but no jemalloc library was found. "
                "Install jemalloc (e.g. 'vcpkg install jemalloc') for this toolchain.")
        endif()
        target_link_libraries(USE_ALLOC INTERFACE ${EE_JEMALLOC_LIB})
    endif()
    message(STATUS "Malloc backend: jemalloc")

else()
    message(FATAL_ERROR "Unknown USE_ALLOC='${USE_ALLOC}' — use system, mimalloc, tcmalloc, or jemalloc")
endif()
