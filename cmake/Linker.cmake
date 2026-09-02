include_guard(GLOBAL)
include(CheckLinkerFlag)

# Linker *selection* only. ELF hardening flags (-z relro/now/noexecstack)
# live in cmake/Hardening.cmake so ORDER_BOOK_LINKER=DEFAULT still hardens.

set(ORDER_BOOK_LINKER "AUTO" CACHE STRING
    "Linker selection: AUTO, DEFAULT, LLD, MOLD, BFD")
set_property(CACHE ORDER_BOOK_LINKER PROPERTY STRINGS AUTO DEFAULT LLD MOLD BFD)

if(NOT ORDER_BOOK_LINKER MATCHES "^(AUTO|DEFAULT|LLD|MOLD|BFD)$")
    message(FATAL_ERROR
            "ORDER_BOOK_LINKER='${ORDER_BOOK_LINKER}' is not one of "
            "AUTO, DEFAULT, LLD, MOLD, BFD.")
endif()

# CMAKE_LINKER_TYPE is 3.29+. Before that, fall back to -fuse-ld=.
macro(_order_book_apply_linker kind)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
        set(CMAKE_LINKER_TYPE "${kind}")
    else()
        add_link_options("${ORDER_BOOK_LINKER_FLAG_${kind}}")
    endif()
    message(STATUS "linker: ${kind} (${ORDER_BOOK_LINKER_FLAG_${kind}})")
endmacro()

if(ORDER_BOOK_LINKER STREQUAL "DEFAULT")
    message(STATUS "linker: toolchain default, untouched (ORDER_BOOK_LINKER=DEFAULT)")
    return()
endif()

if(MSVC)
    if(ORDER_BOOK_LINKER STREQUAL "LLD")
        find_program(ORDER_BOOK_LLD_LINK NAMES lld-link lld-link.exe)
        mark_as_advanced(ORDER_BOOK_LLD_LINK)
        if(ORDER_BOOK_LLD_LINK)
            if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
                set(CMAKE_LINKER_TYPE LLD)
            else()
                set(CMAKE_LINKER "${ORDER_BOOK_LLD_LINK}")
                message(WARNING
                    "linker: CMake < 3.29; set CMAKE_LINKER=${ORDER_BOOK_LLD_LINK}. "
                    "Prefer CMake 3.29+ CMAKE_LINKER_TYPE=LLD.")
            endif()
            message(STATUS "linker: lld-link (${ORDER_BOOK_LLD_LINK})")
            return()
        endif()
        message(WARNING
                "linker: lld-link NOT found on PATH — falling back to link.exe. "
                "Install LLVM for Windows and re-configure.")
    elseif(NOT ORDER_BOOK_LINKER STREQUAL "AUTO")
        message(WARNING
                "linker: ${ORDER_BOOK_LINKER} does not target PE/COFF — "
                "falling back to link.exe.")
    endif()
    if(MSVC_VERSION LESS 1950)
        add_link_options("$<$<CONFIG:Debug>:/DEBUG:FASTLINK>")
        message(STATUS "linker: link.exe, /DEBUG:FASTLINK on Debug "
                "(-D ORDER_BOOK_LINKER=LLD switches to lld-link)")
    else()
        message(STATUS
                "linker: link.exe — /DEBUG:FASTLINK dropped by MSVC "
                "${MSVC_VERSION} (LNK4315), so Debug links with /DEBUG:FULL. "
                "-D ORDER_BOOK_LINKER=LLD switches to lld-link")
    endif()
    return()
endif()

set(ORDER_BOOK_LINKER_FLAG_LLD "-fuse-ld=lld")
set(ORDER_BOOK_LINKER_FLAG_MOLD "-fuse-ld=mold")
set(ORDER_BOOK_LINKER_FLAG_BFD "-fuse-ld=bfd")

set(_ob_candidates ${ORDER_BOOK_LINKER})
if(ORDER_BOOK_LINKER STREQUAL "AUTO")
    if(APPLE)
        message(STATUS "linker: toolchain default, untouched — ld-prime is "
                "already parallel (-D ORDER_BOOK_LINKER=LLD overrides)")
        return()
    elseif(WIN32)
        set(_ob_candidates LLD)
    else()
        set(_ob_candidates MOLD LLD)
    endif()
endif()

if(WIN32 AND "MOLD" IN_LIST _ob_candidates)
    list(REMOVE_ITEM _ob_candidates MOLD)
    if(NOT _ob_candidates)
        message(WARNING
                "linker: mold does not target PE/COFF — linking with the "
                "toolchain default. Use -D ORDER_BOOK_LINKER=LLD on Windows.")
        return()
    endif()
    message(WARNING "linker: mold does not target PE/COFF; ignoring MOLD.")
endif()

foreach(_ob_candidate IN LISTS _ob_candidates)
    check_linker_flag(CXX "${ORDER_BOOK_LINKER_FLAG_${_ob_candidate}}"
            ORDER_BOOK_HAS_LINKER_${_ob_candidate})
    if(ORDER_BOOK_HAS_LINKER_${_ob_candidate})
        _order_book_apply_linker(${_ob_candidate})
        return()
    endif()
endforeach()

message(WARNING
        "linker: none of '${_ob_candidates}' is usable by ${CMAKE_CXX_COMPILER_ID} "
        "— linking with the toolchain default. Install LLVM (ld.lld) or mold and "
        "put it on the PATH the configure step sees (restart the IDE after "
        "changing PATH), then re-configure.")
