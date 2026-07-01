# Sanitize.cmake — adds a dedicated "Sanitize" build configuration (AddressSanitizer).
#
# Build it with the multi-config generators via `--config Sanitize`, or pick it
# in CLion's configuration dropdown. The configuration mirrors RelWithDebInfo
# (optimized + debug info) and layers AddressSanitizer on top.
#
# Compiler support: MSVC (/fsanitize=address) and Clang/AppleClang/GCC on
# macOS/Linux. GCC/Clang on Windows (MinGW) ship no sanitizer runtimes, so there
# the Sanitize config builds WITHOUT instrumentation — use the windows-msvc
# preset for ASan on Windows.

# --- 1. Register the Sanitize configuration -------------------------------------
# Append it to the multi-config type list so `--config Sanitize` is accepted and
# CLion offers it. FORCE updates the cache the IDE reads back after configuring.
if (CMAKE_CONFIGURATION_TYPES AND NOT "Sanitize" IN_LIST CMAKE_CONFIGURATION_TYPES)
    list(APPEND CMAKE_CONFIGURATION_TYPES Sanitize)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}"
            CACHE STRING "Supported configuration types" FORCE)
endif ()

# --- 2. Base flags: mirror RelWithDebInfo (optimized + debug info) ---------------
# A brand-new configuration starts with empty flags; without this it would build
# at -O0 with no debug info — nearly useless for a sanitizer run.
foreach (_lang C CXX)
    set(CMAKE_${_lang}_FLAGS_SANITIZE "${CMAKE_${_lang}_FLAGS_RELWITHDEBINFO}"
            CACHE STRING "Flags used by the ${_lang} compiler for the Sanitize build type." FORCE)
endforeach ()
foreach (_type EXE SHARED MODULE STATIC)
    set(CMAKE_${_type}_LINKER_FLAGS_SANITIZE "${CMAKE_${_type}_LINKER_FLAGS_RELWITHDEBINFO}"
            CACHE STRING "Linker flags for ${_type} targets in the Sanitize build type." FORCE)
endforeach ()

# --- 3. Map imported (vcpkg) targets: Sanitize -> Release ------------------------
# fmt/simdjson/gtest/boost/openssl export only Debug and Release variants. Without
# this map, resolving their import libraries in the Sanitize config fails.
set(CMAKE_MAP_IMPORTED_CONFIG_SANITIZE Release RelWithDebInfo "")

# --- 4. AddressSanitizer instrumentation, applied only in the Sanitize config ---
set(SANITIZE_CONDITION "$<CONFIG:Sanitize>")

if (MSVC)
    set(SANITIZE_FLAGS "$<${SANITIZE_CONDITION}:/fsanitize=address>")
    # ASan needs a PDB for symbolized reports (RelWithDebInfo already sets /Zi;
    # kept explicit so the flag set is self-describing).
    add_compile_options("$<${SANITIZE_CONDITION}:/Zi>")
elseif (WIN32)
    # MinGW/GCC on Windows have no libasan — instrumenting here would fail at link
    # (cannot find -lasan). Leave the Sanitize config uninstrumented and say so.
    message(STATUS
            "MinGW cannot link AddressSanitizer; the 'Sanitize' configuration "
            "will build WITHOUT instrumentation here. Use the windows-msvc "
            "preset for ASan on Windows.")
    set(SANITIZE_FLAGS "")
else ()
    set(SANITIZE_FLAGS
            "$<${SANITIZE_CONDITION}:-fsanitize=address>"
            "$<${SANITIZE_CONDITION}:-fno-omit-frame-pointer>"
            "$<${SANITIZE_CONDITION}:-g>")
endif ()

add_compile_options(${SANITIZE_FLAGS})
add_link_options(${SANITIZE_FLAGS})
