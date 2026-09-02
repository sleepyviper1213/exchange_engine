include_guard(GLOBAL)

# AddressSanitizer build type (RelWithDebInfo + ASan). GCC/Clang also get UBSan
# in this config: they coexist, and that is the usual CI pairing. TSan cannot,
# which is why ThreadSanitizer is separate.
#
# MSVC uses /fsanitize=address; Clang/AppleClang/GCC on macOS/Linux use
# -fsanitize=address,undefined. GCC/Clang on Windows (MinGW) ship no libasan, so
# the config is uninstrumented there — use the windows-msvc preset for ASan on
# Windows.

_order_book_register_sanitizer_config(AddressSanitizer)

set(ASAN_CONDITION "$<CONFIG:AddressSanitizer>")

if(MSVC)
    # /fsanitize=address is compile-only; the linker pulls the ASan runtime from
    # object metadata, so passing it to link.exe yields LNK4044. /Zi gives a PDB
    # for symbolized reports; ASan forces incremental linking off (LNK4300).
    add_compile_options("$<${ASAN_CONDITION}:/fsanitize=address>"
                        "$<${ASAN_CONDITION}:/Zi>")
    # vcpkg's prebuilt libs (CLI11, crc32c, …) are built WITHOUT the MSVC ASan
    # STL container annotations; our objects have them on, which trips LNK2038
    # mismatches at link. Opt out for the ASan config so both sides agree.
    #
    # One macro per annotated container, and the list grows with the STL: 14.51
    # (v145) added std::optional to the string and vector annotations that were
    # here already, and a missing macro is a link error naming the container -
    # 'annotate_optional' was crc32c.lib against our own objects. The set lives
    # in __msvc_sanitizer_annotate_container.hpp, which is the file to re-read
    # when a toolset upgrade brings a fourth.
    add_compile_definitions(
        "$<${ASAN_CONDITION}:_DISABLE_STRING_ANNOTATION=1>"
        "$<${ASAN_CONDITION}:_DISABLE_VECTOR_ANNOTATION=1>"
        "$<${ASAN_CONDITION}:_DISABLE_OPTIONAL_ANNOTATION=1>")
    add_link_options("$<${ASAN_CONDITION}:/INCREMENTAL:NO>")
    # /fsanitize=address links clang_rt.asan_dynamic-*.dll dynamically, and that
    # DLL ships beside cl.exe rather than anywhere on PATH. Locate it here so
    # copy_sanitizer_runtime below can put it next to each executable.
    get_filename_component(_msvc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        set(_asan_arch "aarch64")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_asan_arch "x86_64")
    else()
        set(_asan_arch "i386")
    endif()
    file(GLOB _asan_runtime_candidates
         "${_msvc_bin}/clang_rt.asan_dynamic-${_asan_arch}.dll")
    if(NOT _asan_runtime_candidates)
        file(GLOB _asan_runtime_candidates
             "${_msvc_bin}/clang_rt.asan_dynamic-*.dll")
    endif()
    if(_asan_runtime_candidates)
        list(GET _asan_runtime_candidates 0 _asan_runtime)
        set(ASAN_RUNTIME_DLL
            "${_asan_runtime}"
            CACHE
                FILEPATH
                "MSVC AddressSanitizer runtime, copied beside ASan executables")
        mark_as_advanced(ASAN_RUNTIME_DLL)
    else()
        message(
            STATUS "No clang_rt.asan_dynamic DLL beside ${CMAKE_CXX_COMPILER}; "
                   "AddressSanitizer executables may not start")
    endif()
    unset(_msvc_bin)
    unset(_asan_arch)
    unset(_asan_runtime_candidates)
elseif(WIN32)
    # MinGW has no libasan — instrumenting would fail at link.
    message(
        STATUS
            "MinGW cannot link AddressSanitizer; 'AddressSanitizer' builds "
            "WITHOUT instrumentation. Use the windows-msvc preset for ASan on Windows."
    )
else()
    # GCC/Clang need -fsanitize at BOTH compile and link. UBSan is folded in: it
    # composes with ASan, unlike TSan. -fno-sanitize-recover so CI dies on UB
    # instead of printing and continuing.
    _order_book_add_sanitizer_flags(
        AddressSanitizer -fsanitize=address,undefined -fno-omit-frame-pointer
        -fno-sanitize-recover=all -g)
    message(STATUS "sanitizers: AddressSanitizer instruments with ASan+UBSan")
endif()

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
    if(NOT MSVC OR NOT ASAN_RUNTIME_DLL)
        return()
    endif()
    if(NOT TARGET ${target})
        message(
            FATAL_ERROR
                "copy_sanitizer_runtime: '${target}' is not a CMake target")
    endif()
    get_target_property(_ob_type ${target} TYPE)
    if(NOT _ob_type STREQUAL "EXECUTABLE")
        return()
    endif()
    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND
            "${CMAKE_COMMAND}" -E
            "$<IF:$<CONFIG:AddressSanitizer>,copy_if_different,true>"
            "$<$<CONFIG:AddressSanitizer>:${ASAN_RUNTIME_DLL};$<TARGET_FILE_DIR:${target}>>"
        COMMAND_EXPAND_LISTS VERBATIM
        COMMENT "Staging the AddressSanitizer runtime for ${target}")
endfunction()
