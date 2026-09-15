include_guard(GLOBAL)
include(CheckCXXCompilerFlag)

# Compiler warning sets. Clang and GCC are *not* interchangeable: do not inherit
# Clang-only flags into the GNU list.
#
# C++-only flags are wrapped in COMPILE_LANGUAGE:CXX so DPDK C TUs in the same
# target do not see -Wnon-virtual-dtor / -Wsuggest-override.

set(EXCHANGE_WARNINGS_MSVC
    /W4
    /permissive-
    /utf-8
    /Zc:__cplusplus
    /Zc:preprocessor
    /w14062
    /w14242
    /w14254
    /w14263
    /w14265
    /w14287
    /w14289
    /w14296
    /w14311
    /w14545
    /w14546
    /w14547
    /w14549
    /w14555
    /w14619
    /w14640
    /w14826
    /w14905
    /w14906
    /w14928)

set(EXCHANGE_WARNINGS_COMMON
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wcast-align
    -Wdouble-promotion
    -Wformat=2
    -Wformat-security
    -Wimplicit-fallthrough
    -Wnon-virtual-dtor
    -Wnull-dereference
    -Woverloaded-virtual
    -Wunused)

set(EXCHANGE_WARNINGS_GNU_EXTRA
    -Wduplicated-branches
    -Wduplicated-cond
    -Wlogical-op
    -Wmisleading-indentation
    -Wsuggest-override
    -Wno-interference-size)

# ---------------------------------------------------------------------------
# Probe: treat "unused argument" as failure on Clang; -Werror on GCC
# ---------------------------------------------------------------------------
function(_exchange_warning_flag_supported flag out_var)
    string(MAKE_C_IDENTIFIER "EXCHANGE_HAS_WFLAG_${flag}" _cachevar)
    string(TOUPPER "${_cachevar}" _cachevar)
    if(NOT DEFINED ${_cachevar})
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            set(CMAKE_REQUIRED_FLAGS "-Werror=unused-command-line-argument")
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set(CMAKE_REQUIRED_FLAGS "-Werror")
        endif()
        check_cxx_compiler_flag("${flag}" ${_cachevar})
    endif()
    set(${out_var}
        ${${_cachevar}}
        PARENT_SCOPE)
endfunction()

function(_exchange_add_cxx_options target visibility)
    foreach(_flag IN LISTS ARGN)
        target_compile_options(${target} ${visibility}
                               "$<$<COMPILE_LANGUAGE:CXX>:${_flag}>")
    endforeach()
endfunction()

function(set_warnings target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "set_warnings: '${target}' is not a CMake target")
    endif()

    get_target_property(_ob_target_type ${target} TYPE)
    if(_ob_target_type STREQUAL "INTERFACE_LIBRARY")
        return()
    endif()

    # if(MSVC) is true for clang-cl as well — that frontend wants MSVC flags.
    if(MSVC)
        set(_ob_warnings ${EXCHANGE_WARNINGS_MSVC})
        if(EXCHANGE_WARNINGS_AS_ERRORS)
            list(APPEND _ob_warnings /WX)
        endif()
        _exchange_add_cxx_options(${target} PRIVATE ${_ob_warnings})
        # C4996 on the CRT's "unsafe" functions demands _dupenv_s and friends,
        # which exist only here; std::getenv is the portable spelling and every
        # call site checks the null it returns. Narrower than /wd4996, which
        # would also silence [[deprecated]] on our own declarations.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        return()
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(_ob_warnings ${EXCHANGE_WARNINGS_COMMON})

        _exchange_warning_flag_supported(-Wsuggest-override _has_suggest)
        if(_has_suggest)
            list(APPEND _ob_warnings -Wsuggest-override)
        endif()

        if(EXCHANGE_WARN_UNSAFE_BUFFERS)
            _exchange_warning_flag_supported(-Wunsafe-buffer-usage _has_ubu)
            if(_has_ubu)
                list(APPEND _ob_warnings -Wunsafe-buffer-usage)
                # Never promote this to -Werror: it fires on most pointer
                # arithmetic and is an adoption aid, not a CI gate.
                list(APPEND _ob_warnings -Wno-error=unsafe-buffer-usage)
            endif()
            _exchange_warning_flag_supported(-fsafe-buffer-usage-suggestions
                                               _has_sbus)
            if(_has_sbus)
                list(APPEND _ob_warnings -fsafe-buffer-usage-suggestions)
            endif()
        endif()

        _exchange_warning_flag_supported(-Werror=dangling _has_dangling)
        if(_has_dangling)
            list(APPEND _ob_warnings -Werror=dangling)
        endif()

        if(EXCHANGE_WARNINGS_AS_ERRORS)
            list(APPEND _ob_warnings -Werror)
        endif()
        _exchange_add_cxx_options(${target} PRIVATE ${_ob_warnings})
        return()
    endif()

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(_ob_warnings ${EXCHANGE_WARNINGS_COMMON}
                         ${EXCHANGE_WARNINGS_GNU_EXTRA})
        if(EXCHANGE_WARNINGS_AS_ERRORS)
            list(APPEND _ob_warnings -Werror)
            # Both are produced by the -O2 optimiser after inlining, so they
            # carry a header's location rather than a variable's and escape the
            # -isystem suppression Boost is included with. Reported, never a
            # gate - same rule as -Wunsafe-buffer-usage above.
            list(APPEND _ob_warnings -Wno-error=null-dereference
                                     -Wno-error=maybe-uninitialized)
        endif()
        _exchange_add_cxx_options(${target} PRIVATE ${_ob_warnings})
        return()
    endif()

    message(
        AUTHOR_WARNING
            "No warning set for CXX compiler '${CMAKE_CXX_COMPILER_ID}'")
endfunction()
