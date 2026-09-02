include_guard(GLOBAL)
include(CheckCXXCompilerFlag)
include(CheckLinkerFlag)
include(CheckPIESupported)

# Hardening flags follow Red Hat's recommended GCC/linker set
# (https://developers.redhat.com/blog/2018/03/21/compiler-and-linker-flags-gcc)
# plus current OpenSSF updates (FORTIFY_SOURCE=3, -Wl,-z,noexecstack,
# AArch64 BTI/PAC).
#
# Intentionally NOT applied:
#   -UNDEBUG / /UNDEBUG     — not a mitigation; keeps assert() in Release
#   -O2 / -g / -pipe        — CMAKE_BUILD_TYPE owns these
#   -Wl,-z,nodlopen         — hostile to DPDK PMDs / plugins
#   -fstrict-flex-arrays=3  — breaks C flexible-array idioms in DPDK
#   -mcet                   — obsolete; -fcf-protection supersedes it
#
# Stdlib hardening macros and _FORTIFY_SOURCE change inline functions in
# headers: they are PUBLIC so every TU that includes our headers agrees.

set(ORDER_BOOK_HARDENING_COMPILE_OPTIONS "")
set(ORDER_BOOK_HARDENING_PUBLIC_COMPILE_OPTIONS "")
set(ORDER_BOOK_HARDENING_LINK_OPTIONS "")
set(ORDER_BOOK_HARDENING_EXE_LINK_OPTIONS "")
set(ORDER_BOOK_HARDENING_DEFINITIONS "")
set(ORDER_BOOK_HARDENING_C_COMPILE_OPTIONS "")

# ---------------------------------------------------------------------------
# Normalize the user value
# ---------------------------------------------------------------------------
string(TOUPPER "${ORDER_BOOK_HARDENING}" _harden_level)

set(_enable_hardening FALSE)
set(_libcpp_mode "")

if(_harden_level STREQUAL "OFF" OR _harden_level STREQUAL "FALSE" OR _harden_level STREQUAL "0")
    set(_enable_hardening FALSE)
elseif(_harden_level STREQUAL "ON" OR _harden_level STREQUAL "TRUE" OR _harden_level STREQUAL "1" OR _harden_level STREQUAL "FAST")
    set(_enable_hardening TRUE)
    set(_libcpp_mode "FAST")
elseif(_harden_level STREQUAL "NONE")
    set(_enable_hardening TRUE)
    set(_libcpp_mode "NONE")
elseif(_harden_level STREQUAL "EXTENSIVE")
    set(_enable_hardening TRUE)
    set(_libcpp_mode "EXTENSIVE")
elseif(_harden_level STREQUAL "DEBUG")
    set(_enable_hardening TRUE)
    set(_libcpp_mode "DEBUG")
else()
    message(FATAL_ERROR
        "ORDER_BOOK_HARDENING must be one of: "
        "OFF, ON, none, fast, extensive, debug "
        "(got '${ORDER_BOOK_HARDENING}')")
endif()

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
function(_order_book_cxx_flag_supported flag out_var)
    string(MAKE_C_IDENTIFIER "ORDER_BOOK_HAS_${flag}" _cachevar)
    string(TOUPPER "${_cachevar}" _cachevar)
    if(NOT DEFINED ${_cachevar})
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            set(CMAKE_REQUIRED_FLAGS "-Werror=unused-command-line-argument")
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set(CMAKE_REQUIRED_FLAGS "-Werror")
        endif()
        check_cxx_compiler_flag("${flag}" ${_cachevar})
    endif()
    set(${out_var} ${${_cachevar}} PARENT_SCOPE)
endfunction()

function(_order_book_ld_flag_supported flag out_var)
    string(MAKE_C_IDENTIFIER "ORDER_BOOK_HAS_LD_${flag}" _cachevar)
    string(TOUPPER "${_cachevar}" _cachevar)
    if(NOT DEFINED ${_cachevar})
        check_linker_flag(CXX "${flag}" ${_cachevar})
    endif()
    set(${out_var} ${${_cachevar}} PARENT_SCOPE)
endfunction()

macro(_order_book_harden_cflag flag)
    _order_book_cxx_flag_supported("${flag}" _ob_ok)
    if(_ob_ok)
        list(APPEND ORDER_BOOK_HARDENING_COMPILE_OPTIONS "${flag}")
        string(APPEND _c_log "${flag} ")
    endif()
endmacro()

macro(_order_book_harden_ldflag flag)
    _order_book_ld_flag_supported("${flag}" _ob_ok)
    if(_ob_ok)
        list(APPEND ORDER_BOOK_HARDENING_LINK_OPTIONS "${flag}")
        string(APPEND _l_log "${flag} ")
    endif()
endmacro()

# $<OR:$<CONFIG:AddressSanitizer>,$<CONFIG:ThreadSanitizer>,...> or empty
set(_ob_san_cfg_genex "")
foreach(_cfg IN LISTS ORDER_BOOK_SANITIZER_CONFIGS)
    if(_ob_san_cfg_genex STREQUAL "")
        set(_ob_san_cfg_genex "$<CONFIG:${_cfg}>")
    else()
        set(_ob_san_cfg_genex "$<OR:${_ob_san_cfg_genex},$<CONFIG:${_cfg}>>")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Stdlib detection (compiler ID is not the stdlib)
# ---------------------------------------------------------------------------
set(_uses_libcpp FALSE)
set(_uses_libstdcxx FALSE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_uses_libstdcxx TRUE)
elseif(MSVC)
    # MSVC STL
elseif(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(_uses_libcpp TRUE)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(CMAKE_CXX_FLAGS MATCHES "-stdlib=libc\\+\\+")
        set(_uses_libcpp TRUE)
    else()
        set(_uses_libstdcxx TRUE)
    endif()
endif()

# ---------------------------------------------------------------------------
# Main logic
# ---------------------------------------------------------------------------
set(_c_log "")
set(_l_log "")
set(_d_log "")

if(_enable_hardening)

    if(MSVC)
        list(APPEND ORDER_BOOK_HARDENING_COMPILE_OPTIONS /sdl /GS /guard:cf)
        list(APPEND ORDER_BOOK_HARDENING_LINK_OPTIONS
             /guard:cf /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA)
        string(APPEND _c_log "/sdl /GS /guard:cf ")
        string(APPEND _l_log "/guard:cf /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA ")

        if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|X86")
            list(APPEND ORDER_BOOK_HARDENING_LINK_OPTIONS /CETCOMPAT)
            string(APPEND _l_log "/CETCOMPAT ")
        endif()

        list(APPEND ORDER_BOOK_HARDENING_DEFINITIONS _MSVC_STL_HARDENING=1)
        string(APPEND _d_log "_MSVC_STL_HARDENING=1 ")

    else()
        # --- Red Hat CFLAGS (probed) ---------------------------------------
        _order_book_harden_cflag(-fstack-protector-strong)
        _order_book_harden_cflag(-fstack-clash-protection)
        _order_book_harden_cflag(-fasynchronous-unwind-tables)
        _order_book_harden_cflag(-grecord-gcc-switches)
        _order_book_harden_cflag(-ftrivial-auto-var-init=zero)

        # Control-flow integrity: CET on x86, BTI+PAC on AArch64.
        # -mcet is obsolete; -fcf-protection is the surviving flag.
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i.86|x86")
            _order_book_harden_cflag(-fcf-protection=full)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64")
            _order_book_harden_cflag(-mbranch-protection=standard)
        else()
            _order_book_harden_cflag(-fcf-protection=full)
            _order_book_harden_cflag(-mbranch-protection=standard)
        endif()

        # Fedora packaging metadata; skipped everywhere the plugin is absent.
        _order_book_harden_cflag(-fplugin=annobin)

        # RH: -Werror=format-security is a security flag, not a style warning.
        _order_book_harden_cflag(-Werror=format-security)

        # C only (RH CFLAGS). Harmless no-op on C++ TUs via COMPILE_LANGUAGE.
        _order_book_cxx_flag_supported(-Werror=implicit-function-declaration _has_ifd)
        if(_has_ifd)
            list(APPEND ORDER_BOOK_HARDENING_C_COMPILE_OPTIONS
                 -Werror=implicit-function-declaration)
            string(APPEND _c_log "-Werror=implicit-function-declaration (C) ")
        endif()

        # libstdc++ lightweight bounds checks (RH: -D_GLIBCXX_ASSERTIONS)
        if(_uses_libstdcxx)
            list(APPEND ORDER_BOOK_HARDENING_DEFINITIONS _GLIBCXX_ASSERTIONS)
            string(APPEND _d_log "_GLIBCXX_ASSERTIONS ")
        endif()

        # libc++ hardening mode (no-op unless actually using libc++)
        if(_uses_libcpp AND _libcpp_mode)
            if(_libcpp_mode STREQUAL "NONE")
                set(_macro _LIBCPP_HARDENING_MODE_NONE)
            elseif(_libcpp_mode STREQUAL "FAST")
                set(_macro _LIBCPP_HARDENING_MODE_FAST)
            elseif(_libcpp_mode STREQUAL "EXTENSIVE")
                set(_macro _LIBCPP_HARDENING_MODE_EXTENSIVE)
            elseif(_libcpp_mode STREQUAL "DEBUG")
                set(_macro _LIBCPP_HARDENING_MODE_DEBUG)
            endif()
            if(_macro)
                list(APPEND ORDER_BOOK_HARDENING_DEFINITIONS
                     _LIBCPP_HARDENING_MODE=${_macro})
                string(APPEND _d_log "_LIBCPP_HARDENING_MODE=${_macro} ")
            endif()
            unset(_macro)
        endif()

        # _FORTIFY_SOURCE requires -O1+. OpenSSF: 3 on GCC 12+ / Clang 16+.
        # PUBLIC because fortified glibc wrappers live in headers.
        if(NOT WIN32)
            set(_fortify 2)
            if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
                AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12)
               OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang"
                   AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16))
                set(_fortify 3)
            endif()

            if(_ob_san_cfg_genex)
                set(_ob_fortify_skip "$<OR:$<CONFIG:Debug>,${_ob_san_cfg_genex}>")
            else()
                set(_ob_fortify_skip "$<CONFIG:Debug>")
            endif()
            list(APPEND ORDER_BOOK_HARDENING_PUBLIC_COMPILE_OPTIONS
                 "$<$<NOT:${_ob_fortify_skip}>:-U_FORTIFY_SOURCE>"
                 "$<$<NOT:${_ob_fortify_skip}>:-D_FORTIFY_SOURCE=${_fortify}>")
            string(APPEND _c_log
                   "-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=${_fortify} (non-Debug, non-sanitizer) ")
            unset(_fortify)
            unset(_ob_fortify_skip)
        endif()

        # --- Red Hat / OpenSSF LDFLAGS (ELF) --------------------------------
        if(NOT APPLE)
            _order_book_harden_ldflag(-Wl,-z,relro)
            _order_book_harden_ldflag(-Wl,-z,now)
            _order_book_harden_ldflag(-Wl,-z,noexecstack)
            _order_book_harden_ldflag(-Wl,-z,separate-code)
            _order_book_harden_ldflag(-Wl,--as-needed)
            _order_book_harden_ldflag(-Wl,--no-copy-dt-needed-entries)

            # -z defs catches underlinking. DPDK PMDs often have unresolved
            # symbols at intermediate link, so skip it when DPDK is on.
            if(NOT ORDER_BOOK_WITH_DPDK)
                _order_book_ld_flag_supported(-Wl,-z,defs _has_defs)
                if(_has_defs)
                    list(APPEND ORDER_BOOK_HARDENING_EXE_LINK_OPTIONS -Wl,-z,defs)
                    string(APPEND _l_log "-Wl,-z,defs (exe) ")
                endif()
            endif()
        endif()

        check_pie_supported(OUTPUT_VARIABLE _ob_pie_msg LANGUAGES CXX)
    endif()

    # Optional UBSan trap (not a RH flag; opt-in)
    if(ORDER_BOOK_HARDENING_UBSAN_TRAP AND NOT MSVC)
        _order_book_cxx_flag_supported("-fsanitize=undefined" _has_ubsan)
        _order_book_cxx_flag_supported("-fsanitize-trap=undefined" _has_ubsan_trap)
        if(_has_ubsan AND _has_ubsan_trap)
            # Do not stack trap-UBSan on top of sanitizer configs that already
            # pass -fsanitize=undefined (or an incompatible sanitizer runtime).
            if(_ob_san_cfg_genex)
                set(_ubsan_ok "$<NOT:${_ob_san_cfg_genex}>")
                list(APPEND ORDER_BOOK_HARDENING_COMPILE_OPTIONS
                     "$<${_ubsan_ok}:-fsanitize=undefined>"
                     "$<${_ubsan_ok}:-fsanitize-trap=undefined>")
                list(APPEND ORDER_BOOK_HARDENING_LINK_OPTIONS
                     "$<${_ubsan_ok}:-fsanitize=undefined>"
                     "$<${_ubsan_ok}:-fsanitize-trap=undefined>")
            else()
                list(APPEND ORDER_BOOK_HARDENING_COMPILE_OPTIONS
                     -fsanitize=undefined -fsanitize-trap=undefined)
                list(APPEND ORDER_BOOK_HARDENING_LINK_OPTIONS
                     -fsanitize=undefined -fsanitize-trap=undefined)
            endif()
            string(APPEND _c_log "-fsanitize=undefined -fsanitize-trap=undefined (non-sanitizer configs) ")
            string(APPEND _l_log "-fsanitize=undefined -fsanitize-trap=undefined (non-sanitizer configs) ")
            unset(_ubsan_ok)
        endif()
    endif()

    string(STRIP "${_c_log}" _c_log)
    string(STRIP "${_d_log}" _d_log)
    string(STRIP "${_l_log}" _l_log)

    message(STATUS "hardening: compile  -> ${_c_log}")
    message(STATUS "hardening: defines  -> ${_d_log}")
    if(_l_log)
        message(STATUS "hardening: link     -> ${_l_log}")
    endif()
    if(_uses_libcpp)
        message(STATUS "hardening: libc++   -> ${_libcpp_mode}")
    elseif(_uses_libstdcxx)
        message(STATUS "hardening: libstdc++ -> assertions")
    endif()
    if(NOT MSVC AND CMAKE_CXX_LINK_PIE_SUPPORTED)
        message(STATUS "hardening: PIE      -> ON (executables)")
    endif()

    unset(_c_log)
    unset(_d_log)
    unset(_l_log)

else()
    message(STATUS "hardening: off")
endif()

unset(_ob_san_cfg_genex)

set_property(GLOBAL PROPERTY ORDER_BOOK_HARDENING_ENABLED ${_enable_hardening})

# ---------------------------------------------------------------------------
# Public helper
# ---------------------------------------------------------------------------
function(enable_hardening target)
    get_property(_on GLOBAL PROPERTY ORDER_BOOK_HARDENING_ENABLED)
    if(NOT _on)
        return()
    endif()

    if(NOT TARGET ${target})
        message(FATAL_ERROR "enable_hardening: '${target}' is not a CMake target")
    endif()

    get_target_property(_type ${target} TYPE)

    if(_type STREQUAL "INTERFACE_LIBRARY")
        set(_priv INTERFACE)
        set(_pub INTERFACE)
    elseif(_type STREQUAL "STATIC_LIBRARY")
        set(_priv PRIVATE)
        set(_pub PUBLIC)
    else()
        set(_priv PRIVATE)
        set(_pub PUBLIC)
    endif()

    if(ORDER_BOOK_HARDENING_COMPILE_OPTIONS)
        target_compile_options(${target} ${_priv}
            ${ORDER_BOOK_HARDENING_COMPILE_OPTIONS})
    endif()

    if(ORDER_BOOK_HARDENING_PUBLIC_COMPILE_OPTIONS)
        target_compile_options(${target} ${_pub}
            ${ORDER_BOOK_HARDENING_PUBLIC_COMPILE_OPTIONS})
    endif()

    if(ORDER_BOOK_HARDENING_C_COMPILE_OPTIONS)
        foreach(_flag IN LISTS ORDER_BOOK_HARDENING_C_COMPILE_OPTIONS)
            target_compile_options(${target} ${_priv}
                "$<$<COMPILE_LANGUAGE:C>:${_flag}>")
        endforeach()
    endif()

    # PUBLIC: header-inline checks must agree across TUs (ODR).
    if(ORDER_BOOK_HARDENING_DEFINITIONS)
        target_compile_definitions(${target} ${_pub}
            ${ORDER_BOOK_HARDENING_DEFINITIONS})
    endif()

    if(ORDER_BOOK_HARDENING_LINK_OPTIONS)
        if(_type STREQUAL "STATIC_LIBRARY")
            target_link_options(${target} INTERFACE
                ${ORDER_BOOK_HARDENING_LINK_OPTIONS})
        elseif(_type STREQUAL "INTERFACE_LIBRARY")
            target_link_options(${target} INTERFACE
                ${ORDER_BOOK_HARDENING_LINK_OPTIONS})
        else()
            target_link_options(${target} PRIVATE
                ${ORDER_BOOK_HARDENING_LINK_OPTIONS})
        endif()
    endif()

    if(ORDER_BOOK_HARDENING_EXE_LINK_OPTIONS AND _type STREQUAL "EXECUTABLE")
        target_link_options(${target} PRIVATE
            ${ORDER_BOOK_HARDENING_EXE_LINK_OPTIONS})
    endif()

    # RH: -fpie -Wl,-pie on executables; PIC on libs so they can be linked in.
    if(NOT MSVC AND NOT _type STREQUAL "INTERFACE_LIBRARY")
        if(CMAKE_CXX_LINK_PIE_SUPPORTED)
            set_property(TARGET ${target} PROPERTY POSITION_INDEPENDENT_CODE ON)
        endif()
    endif()
endfunction()
