include_guard(GLOBAL)

if(NOT ORDER_BOOK_ENABLE_IPO)
    message(STATUS "IPO/LTO: off (ORDER_BOOK_ENABLE_IPO=OFF)")
    return()
endif()

set(_ob_link_default "${CMAKE_LINKER_TYPE}")
set(_ob_link_lto "${CMAKE_LINKER_TYPE}")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_LINKER_TYPE STREQUAL "LLD")
    set(_ob_link_lto BFD)
endif()

set(CMAKE_LINKER_TYPE "${_ob_link_lto}")
include(CheckIPOSupported)
check_ipo_supported(RESULT _ob_has_ipo OUTPUT _ob_ipo_log LANGUAGES CXX)
set(CMAKE_LINKER_TYPE "${_ob_link_default}")

if(NOT _ob_has_ipo)
    message(WARNING
            "IPO/LTO: ${CMAKE_CXX_COMPILER_ID} cannot link with it here — "
            "building without. ${_ob_ipo_log}")
    return()
endif()

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)

if(_ob_link_lto STREQUAL _ob_link_default)
    message(STATUS "IPO/LTO: on for Release and RelWithDebInfo")
else()
    set(CMAKE_LINKER_TYPE
        "$<IF:$<CONFIG:Release,RelWithDebInfo>,${_ob_link_lto},${_ob_link_default}>")
    message(STATUS
            "IPO/LTO: on for Release and RelWithDebInfo, which link with "
            "${_ob_link_lto} — ${_ob_link_default} cannot read this compiler's "
            "LTO objects. Other configurations keep ${_ob_link_default}.")
endif()

unset(_ob_link_default)
unset(_ob_link_lto)
unset(_ob_has_ipo)
unset(_ob_ipo_log)
