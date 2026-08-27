include_guard(GLOBAL)


if(NOT ORDER_BOOK_ENABLE_CCACHE)
    message(STATUS "ccache: disabled (ORDER_BOOK_ENABLE_CCACHE=OFF)")
    return()
endif()

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM AND NOT CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
    message(STATUS
        "ccache: found (${CCACHE_PROGRAM}) but the ${CMAKE_GENERATOR} "
        "generator never runs a compiler launcher — NOT enabled. Configure "
        "with a Ninja-based preset to get a compiler cache.")
elseif(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "ccache: enabled (${CCACHE_PROGRAM})")
else()
    message(WARNING
        "ccache: NOT found on PATH — building WITHOUT a compiler cache. "
        "Install ccache and make sure it is on the PATH the configure process "
        "sees (restart the IDE after changing PATH), then re-configure.")
endif()
