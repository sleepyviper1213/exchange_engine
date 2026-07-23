include_guard(GLOBAL)

# Compiler cache (ccache) as the compile launcher, when available. Prints its
# status so a missing ccache is never a silent no-op — find_program itself is
# quiet, which is how it went unnoticed that no cache was in use.
#
# Opt out with -D ORDER_BOOK_ENABLE_CCACHE=OFF.
option(ORDER_BOOK_ENABLE_CCACHE "Use ccache as the compiler launcher when found" ON)

if(NOT ORDER_BOOK_ENABLE_CCACHE)
    message(STATUS "ccache: disabled (ORDER_BOOK_ENABLE_CCACHE=OFF)")
    return()
endif()

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "ccache: enabled (${CCACHE_PROGRAM})")
else()
    message(WARNING
        "ccache: NOT found on PATH — building WITHOUT a compiler cache. "
        "Install ccache and make sure it is on the PATH the configure process "
        "sees (restart the IDE after changing PATH), then re-configure.")
endif()
