include_guard(GLOBAL)
include(CheckCXXCompilerFlag)



# Runtime hardening for a target, toggled by ORDER_BOOK_ENABLE_HARDENING.
#
# The headline effect is that assert() stays live even in optimized builds: the
# project's defensive precondition checks (e.g. the hazard-pointer thread-cache
# invariants, which turn a corrupted index into a clean abort instead of an
# out-of-bounds access) are otherwise stripped whenever the build type defines
# NDEBUG. On top of that it turns on standard-library bounds/precondition
# checking and the usual stack/buffer protections.
#
# Every flag beyond the baseline is probed rather than assumed: the four
# supported toolchains disagree about all of them, and -fcf-protection in
# particular is x86-only, so macos-arm64-* must not receive it.
#
# Applied PRIVATE, so it only hardens the given target's own translation units.
# Header-only code (like the hazard-pointer library) is compiled into whichever
# target includes it, so hardening must be enabled on those targets too — hence
# it is applied to the test target, not just the library.
set(ORDER_BOOK_HARDENING_COMPILE_OPTIONS "")
set(ORDER_BOOK_HARDENING_LINK_OPTIONS "")
set(ORDER_BOOK_HARDENING_DEFINITIONS "")

macro(_order_book_harden_flag flag)
    string(MAKE_C_IDENTIFIER "ORDER_BOOK_HAS_${flag}" _ob_harden_var)
    string(TOUPPER "${_ob_harden_var}" _ob_harden_var)
    check_cxx_compiler_flag("${flag}" ${_ob_harden_var})
    if(${_ob_harden_var})
        list(APPEND ORDER_BOOK_HARDENING_COMPILE_OPTIONS ${flag})
    endif()
endmacro()

if(ORDER_BOOK_ENABLE_HARDENING)
    if(MSVC)
        # /U NDEBUG re-enables assert(); /sdl adds extra security checks; /GS is
        # buffer-overrun detection; /guard:cf is Control Flow Guard, which needs
        # the switch at both compile and link. The link-only flags are x64
        # defaults or x64-only, which windows-msvc guarantees.
        list(APPEND ORDER_BOOK_HARDENING_COMPILE_OPTIONS
            /UNDEBUG /sdl /GS /guard:cf)
        list(APPEND ORDER_BOOK_HARDENING_LINK_OPTIONS
            /guard:cf /DYNAMICBASE /NXCOMPAT /CETCOMPAT)
        # The counterpart to _GLIBCXX_ASSERTIONS. _CONTAINER_DEBUG_LEVEL
        # was removed (STL1006) and _ITERATOR_DEBUG_LEVEL is unusable here:
        # it carries a detect_mismatch pragma, so it would LNK2038 against
        # vcpkg prebuilts. _MSVC_STL_HARDENING carries none.
        list(APPEND ORDER_BOOK_HARDENING_DEFINITIONS _MSVC_STL_HARDENING=1)
    else()
        # Target options are placed after the build type's flags, so -UNDEBUG
        # reliably cancels a -DNDEBUG coming from Release/RelWithDebInfo.
        list(APPEND ORDER_BOOK_HARDENING_COMPILE_OPTIONS -UNDEBUG)

        _order_book_harden_flag(-fstack-protector-strong)
        _order_book_harden_flag(-fcf-protection)
        _order_book_harden_flag(-fstack-clash-protection)

        # Standard-library precondition and bounds checking.
        list(APPEND ORDER_BOOK_HARDENING_DEFINITIONS
            $<$<CXX_COMPILER_ID:GNU>:_GLIBCXX_ASSERTIONS>
            $<$<CXX_COMPILER_ID:Clang,AppleClang>:_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST>)

        # _FORTIFY_SOURCE needs optimisation to do anything, and on mingw-w64 it
        # pulls in libssp which is awkward to link — restrict it to optimized,
        # non-Windows builds. -U first avoids redefinition warnings when the
        # toolchain already predefines it. Level 3 needs GCC 12 or Clang 16.
        if(NOT WIN32)
            set(_ob_fortify_level 2)
            if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
                AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12)
               OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang"
                   AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16))
                set(_ob_fortify_level 3)
            endif()
            list(APPEND ORDER_BOOK_HARDENING_COMPILE_OPTIONS
                $<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE>
                $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=${_ob_fortify_level}>)
            unset(_ob_fortify_level)
        endif()
    endif()

    # Trap-on-UB rather than the minimal runtime: -fsanitize-minimal-runtime is
    # Clang-only, whereas -fsanitize-trap=undefined works on GCC too and needs
    # no sanitizer runtime linked at all — it lowers a failed check to ud2.
    if(ORDER_BOOK_HARDENING_UBSAN_TRAP AND NOT MSVC)
        check_cxx_compiler_flag(
            "-fsanitize=undefined -fsanitize-trap=undefined"
            ORDER_BOOK_HAS_UBSAN_TRAP)
        if(ORDER_BOOK_HAS_UBSAN_TRAP)
            list(APPEND ORDER_BOOK_HARDENING_COMPILE_OPTIONS
                -fsanitize=undefined -fsanitize-trap=undefined)
        endif()
    endif()

    message(STATUS "hardening: compile ${ORDER_BOOK_HARDENING_COMPILE_OPTIONS}")
    if(ORDER_BOOK_HARDENING_LINK_OPTIONS)
        message(STATUS "hardening: link ${ORDER_BOOK_HARDENING_LINK_OPTIONS}")
    endif()
    if(ORDER_BOOK_HARDENING_DEFINITIONS)
        message(STATUS "hardening: defines ${ORDER_BOOK_HARDENING_DEFINITIONS}")
    endif()
else()
    message(STATUS "hardening: off (ORDER_BOOK_ENABLE_HARDENING=OFF)")
endif()

function(enable_hardening target)
    if(NOT ORDER_BOOK_ENABLE_HARDENING)
        return()
    endif()

    target_compile_options(${target}
        PRIVATE ${ORDER_BOOK_HARDENING_COMPILE_OPTIONS})
    target_compile_definitions(${target}
        PRIVATE ${ORDER_BOOK_HARDENING_DEFINITIONS})
    if(ORDER_BOOK_HARDENING_LINK_OPTIONS)
        target_link_options(${target}
            PRIVATE ${ORDER_BOOK_HARDENING_LINK_OPTIONS})
    endif()
endfunction()
