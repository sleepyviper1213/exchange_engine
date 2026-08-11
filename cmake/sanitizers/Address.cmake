# AddressSanitizer build configuration (mirrors RelWithDebInfo + ASan).
# MSVC uses /fsanitize=address; Clang/AppleClang/GCC on macOS/Linux use
# -fsanitize=address. GCC/Clang on Windows (MinGW) ship no libasan, so the config
# is uninstrumented there — use the windows-msvc preset for ASan on Windows.

if (CMAKE_CONFIGURATION_TYPES AND NOT "AddressSanitizer" IN_LIST CMAKE_CONFIGURATION_TYPES)
    list(APPEND CMAKE_CONFIGURATION_TYPES AddressSanitizer)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}"
            CACHE STRING "Supported configuration types" FORCE)
endif ()

# Mirror RelWithDebInfo so the run is optimized with debug info; a fresh config
# would otherwise be -O0 with no symbols.
foreach (_lang C CXX)
    set(CMAKE_${_lang}_FLAGS_ADDRESSSANITIZER "${CMAKE_${_lang}_FLAGS_RELWITHDEBINFO}"
            CACHE STRING "Flags used by the ${_lang} compiler for the AddressSanitizer build type." FORCE)
endforeach ()
foreach (_type EXE SHARED MODULE STATIC)
    set(CMAKE_${_type}_LINKER_FLAGS_ADDRESSSANITIZER "${CMAKE_${_type}_LINKER_FLAGS_RELWITHDEBINFO}"
            CACHE STRING "Linker flags for ${_type} targets in the AddressSanitizer build type." FORCE)
endforeach ()

# vcpkg deps export only Debug/Release, so resolve their imports as Release here.
set(CMAKE_MAP_IMPORTED_CONFIG_ADDRESSSANITIZER Release RelWithDebInfo "")

set(ASAN_CONDITION "$<CONFIG:AddressSanitizer>")

if (MSVC)
    # /fsanitize=address is compile-only; the linker pulls the ASan runtime from
    # object metadata, so passing it to link.exe yields LNK4044. /Zi gives a PDB
    # for symbolized reports; ASan forces incremental linking off (LNK4300).
    add_compile_options("$<${ASAN_CONDITION}:/fsanitize=address>"
            "$<${ASAN_CONDITION}:/Zi>")
    # vcpkg's prebuilt libs (CLI11, …) are built WITHOUT the MSVC ASan STL
    # container annotations; our objects have them on, which trips LNK2038
    # 'annotate_string'/'annotate_vector' mismatches at link. Opt out of the
    # annotations for the ASan config so both sides agree.
    add_compile_definitions("$<${ASAN_CONDITION}:_DISABLE_STRING_ANNOTATION=1>"
            "$<${ASAN_CONDITION}:_DISABLE_VECTOR_ANNOTATION=1>")
    add_link_options("$<${ASAN_CONDITION}:/INCREMENTAL:NO>")
    # /fsanitize=address links clang_rt.asan_dynamic-*.dll dynamically, and that
    # DLL ships beside cl.exe rather than anywhere on PATH. Locate it here so
    # copy_sanitizer_runtime below can put it next to each executable.
    get_filename_component(_msvc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB _asan_runtime_candidates
            "${_msvc_bin}/clang_rt.asan_dynamic-*.dll")
    if (_asan_runtime_candidates)
        list(GET _asan_runtime_candidates 0 _asan_runtime)
        set(ASAN_RUNTIME_DLL "${_asan_runtime}" CACHE FILEPATH
                "MSVC AddressSanitizer runtime, copied beside ASan executables")
        mark_as_advanced(ASAN_RUNTIME_DLL)
    else ()
        message(STATUS "No clang_rt.asan_dynamic DLL beside ${CMAKE_CXX_COMPILER}; "
                "AddressSanitizer executables may not start")
    endif ()
elseif (WIN32)
    # MinGW has no libasan — instrumenting would fail at link.
    message(STATUS "MinGW cannot link AddressSanitizer; 'AddressSanitizer' builds "
            "WITHOUT instrumentation. Use the windows-msvc preset for ASan on Windows.")
else ()
    # GCC/Clang need -fsanitize=address at BOTH compile and link.
    set(_asan
            "$<${ASAN_CONDITION}:-fsanitize=address>"
            "$<${ASAN_CONDITION}:-fno-omit-frame-pointer>"
            "$<${ASAN_CONDITION}:-g>")
    add_compile_options(${_asan})
    add_link_options(${_asan})
endif ()

# Put the AddressSanitizer runtime next to @p target's executable.
#
# Only MSVC needs this. Its ASan runtime is a DLL that the loader must find
# before main() runs, and it lives in the toolchain rather than on PATH — so
# without the copy an ASan build produces a binary that cannot start at all,
# which CTest reports as every test failing rather than as a missing DLL. The
# GCC/Clang toolchains link libasan the ordinary way and need nothing.
#
# The copy is per *configuration*, not per build tree: every preset here uses a
# multi-config generator, so which configuration is being built is not known
# until build time. Hence the generator expression — in any configuration but
# AddressSanitizer the whole argument list expands to nothing and the command
# degenerates to `cmake -E true`, which is why COMMAND_EXPAND_LISTS is required.
function(copy_sanitizer_runtime target)
    if (NOT MSVC OR NOT ASAN_RUNTIME_DLL)
        return()
    endif ()
    add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E
            "$<IF:$<CONFIG:AddressSanitizer>,copy_if_different,true>"
            "$<$<CONFIG:AddressSanitizer>:${ASAN_RUNTIME_DLL};$<TARGET_FILE_DIR:${target}>>"
            COMMAND_EXPAND_LISTS
            VERBATIM
            COMMENT "Staging the AddressSanitizer runtime for ${target}")
endfunction()
