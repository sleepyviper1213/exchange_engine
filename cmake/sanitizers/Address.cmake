# Compiler support: MSVC (/fsanitize=address) and Clang/AppleClang/GCC on
# macOS/Linux. GCC/Clang on Windows (MinGW) ship no sanitizers runtimes, so there
# the configuration builds WITHOUT instrumentation — use the windows-msvc preset
# for ASan on Windows.

# --- 1. Register the AddressSanitizer configuration -----------------------------
# Append it to the multi-config type list so `--config AddressSanitizer` is
# accepted and CLion offers it. FORCE updates the cache the IDE reads back after
# configuring.
if (CMAKE_CONFIGURATION_TYPES AND NOT "AddressSanitizer" IN_LIST CMAKE_CONFIGURATION_TYPES)
    list(APPEND CMAKE_CONFIGURATION_TYPES AddressSanitizer)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}"
            CACHE STRING "Supported configuration types" FORCE)
endif ()

# --- 2. Base flags: mirror RelWithDebInfo (optimized + debug info) ---------------
# A brand-new configuration starts with empty flags; without this it would build
# at -O0 with no debug info — nearly useless for a sanitizers run.
foreach (_lang C CXX)
    set(CMAKE_${_lang}_FLAGS_ADDRESSSANITIZER "${CMAKE_${_lang}_FLAGS_RELWITHDEBINFO}"
            CACHE STRING "Flags used by the ${_lang} compiler for the AddressSanitizer build type." FORCE)
endforeach ()
foreach (_type EXE SHARED MODULE STATIC)
    set(CMAKE_${_type}_LINKER_FLAGS_ADDRESSSANITIZER "${CMAKE_${_type}_LINKER_FLAGS_RELWITHDEBINFO}"
            CACHE STRING "Linker flags for ${_type} targets in the AddressSanitizer build type." FORCE)
endforeach ()

# --- 3. Map imported (vcpkg) targets: AddressSanitizer -> Release ----------------
# fmt/simdjson/gtest/boost/openssl export only Debug and Release variants. Without
# this map, resolving their import libraries in the AddressSanitizer config fails.
set(CMAKE_MAP_IMPORTED_CONFIG_ADDRESSSANITIZER Release RelWithDebInfo "")

# --- 4. AddressSanitizer instrumentation, applied only in this config ------------
set(ASAN_CONDITION "$<CONFIG:AddressSanitizer>")

if (MSVC)
    set(ASAN_FLAGS "$<${ASAN_CONDITION}:/fsanitize=address>")
    # ASan needs a PDB for symbolized reports (RelWithDebInfo already sets /Zi;
    # kept explicit so the flag set is self-describing).
    add_compile_options("$<${ASAN_CONDITION}:/Zi>")
elseif (WIN32)
    # MinGW/GCC on Windows have no libasan — instrumenting here would fail at link
    # (cannot find -lasan). Leave the config uninstrumented and say so.
    message(STATUS
            "MinGW cannot link AddressSanitizer; the 'AddressSanitizer' configuration "
            "will build WITHOUT instrumentation here. Use the windows-msvc preset "
            "for ASan on Windows.")
    set(ASAN_FLAGS "")
else ()
    set(ASAN_FLAGS
            "$<${ASAN_CONDITION}:-fsanitize=address>"
            "$<${ASAN_CONDITION}:-fno-omit-frame-pointer>"
            "$<${ASAN_CONDITION}:-g>")
endif ()

add_compile_options(${ASAN_FLAGS})
add_link_options(${ASAN_FLAGS})
