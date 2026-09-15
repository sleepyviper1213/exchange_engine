include_guard(GLOBAL)


set(EXCHANGE_SANITIZER_CONFIGS "")

function(copy_sanitizer_runtime target)
    # Stub. sanitizers/Address.cmake replaces this with the MSVC DLL staging
    # command when AddressSanitizer is registered.
endfunction()

function(_exchange_add_sanitizer_flags config)
    set(_cond "$<CONFIG:${config}>")
    set(_opts)
    foreach(_f IN LISTS ARGN)
        list(APPEND _opts "$<${_cond}:${_f}>")
    endforeach()
    add_compile_options(${_opts})
    add_link_options(${_opts})
endfunction()

function(_exchange_sanitizer_uninstrumented config reason)
    message(STATUS "${reason}; '${config}' builds WITHOUT instrumentation.")
endfunction()

if(NOT EXCHANGE_SANITIZER)
    set(EXCHANGE_SANITIZER_CONFIGS
        ""
        CACHE INTERNAL "Registered sanitizer configuration type names" FORCE)
    message(
        STATUS
            "sanitizers: configs not registered (EXCHANGE_SANITIZER=OFF)")
    return()
endif()
# ---------------------------------------------------------------------------
# Register <Name> as a RelWithDebInfo-based configuration type
# ---------------------------------------------------------------------------
macro(_exchange_register_sanitizer_config config_name)
    string(TOUPPER "${config_name}" _ob_san_upper)

    if(CMAKE_CONFIGURATION_TYPES
       AND NOT EXCHANGE_ENABLE_COVERAGE
       AND NOT "${config_name}" IN_LIST CMAKE_CONFIGURATION_TYPES)
        list(APPEND CMAKE_CONFIGURATION_TYPES "${config_name}")
        set(CMAKE_CONFIGURATION_TYPES
            "${CMAKE_CONFIGURATION_TYPES}"
            CACHE STRING "Supported configuration types" FORCE)
    endif()

    foreach(_lang C CXX)
        set(CMAKE_${_lang}_FLAGS_${_ob_san_upper}
            "${CMAKE_${_lang}_FLAGS_RELWITHDEBINFO}"
            CACHE
                STRING
                "Flags used by the ${_lang} compiler for the ${config_name} build type."
                FORCE)
        mark_as_advanced(CMAKE_${_lang}_FLAGS_${_ob_san_upper})
    endforeach()

    foreach(_type EXE SHARED MODULE STATIC)
        set(CMAKE_${_type}_LINKER_FLAGS_${_ob_san_upper}
            "${CMAKE_${_type}_LINKER_FLAGS_RELWITHDEBINFO}"
            CACHE
                STRING
                "Linker flags for ${_type} targets in the ${config_name} build type."
                FORCE)
        mark_as_advanced(CMAKE_${_type}_LINKER_FLAGS_${_ob_san_upper})
    endforeach()

    set(CMAKE_MAP_IMPORTED_CONFIG_${_ob_san_upper} Release RelWithDebInfo "")
    list(APPEND EXCHANGE_SANITIZER_CONFIGS "${config_name}")
    unset(_ob_san_upper)
endmacro()

# ---------------------------------------------------------------------------
# Exactly one family
# ---------------------------------------------------------------------------
function(_exchange_normalize_sanitizer_token token out_var)
    string(TOUPPER "${token}" _u)
    string(REPLACE "-" "" _u "${_u}")
    string(REPLACE "_" "" _u "${_u}")
    string(REPLACE "SANITIZER" "" _u "${_u}")
    if(_u STREQUAL "ADDRESS" OR _u STREQUAL "ASAN")
        set(${out_var}
            Address
            PARENT_SCOPE)
    elseif(_u STREQUAL "THREAD" OR _u STREQUAL "TSAN")
        set(${out_var}
            Thread
            PARENT_SCOPE)
    elseif(_u STREQUAL "UNDEFINED" OR _u STREQUAL "UBSAN")
        set(${out_var}
            Undefined
            PARENT_SCOPE)
    elseif(_u STREQUAL "LEAK" OR _u STREQUAL "LSAN")
        set(${out_var}
            Leak
            PARENT_SCOPE)
    elseif(_u STREQUAL "MEMORY" OR _u STREQUAL "MSAN")
        set(${out_var}
            Memory
            PARENT_SCOPE)
    elseif(_u STREQUAL "HWADDRESS" OR _u STREQUAL "HWASAN")
        set(${out_var}
            HWAddress
            PARENT_SCOPE)
    else()
        set(${out_var}
            ""
            PARENT_SCOPE)
    endif()
endfunction()

if(EXCHANGE_SANITIZER MATCHES "[,;]")
    message(
        FATAL_ERROR
            "EXCHANGE_SANITIZER='${EXCHANGE_SANITIZER}' selects more than one family. "
            "ASan, TSan, MSan and HWASan cannot be mixed — pick exactly one of: "
            "Address, Thread, Undefined, Leak, Memory, HWAddress. "
            "Use a second build tree for a second sanitizer.")
endif()

_exchange_normalize_sanitizer_token("${EXCHANGE_SANITIZER}" _ob_san)
if(NOT _ob_san)
    message(
        FATAL_ERROR
            "EXCHANGE_SANITIZER='${EXCHANGE_SANITIZER}' is not one of: "
            "Address, Thread, Undefined, Leak, Memory, HWAddress.")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/sanitizers/${_ob_san}.cmake")
unset(_ob_san)

set(EXCHANGE_SANITIZER_CONFIGS
    "${EXCHANGE_SANITIZER_CONFIGS}"
    CACHE INTERNAL "Registered sanitizer configuration type names" FORCE)

if(CMAKE_BUILD_TYPE)
    set(_ob_type_strings Debug Release RelWithDebInfo MinSizeRel)
    list(APPEND _ob_type_strings ${EXCHANGE_SANITIZER_CONFIGS})
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS ${_ob_type_strings})
    unset(_ob_type_strings)
endif()

if(CMAKE_BUILD_TYPE MATCHES "Sanitizer" AND EXCHANGE_BUILD_BENCHMARKS)
    message(
        WARNING "CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} with benchmarks enabled: "
                "numbers are not representative. Build benches as Release.")
endif()

list(GET EXCHANGE_SANITIZER_CONFIGS 0 _ob_san_cfg)
message(STATUS "sanitizers: extra build type: ${_ob_san_cfg}")
if(NOT CMAKE_CONFIGURATION_TYPES)
    message(
        STATUS
            "sanitizers: single-config — cmake -DCMAKE_BUILD_TYPE=${_ob_san_cfg}"
    )
else()
    message(
        STATUS
            "sanitizers: multi-config  — cmake --build --config ${_ob_san_cfg}")
endif()
unset(_ob_san_cfg)
