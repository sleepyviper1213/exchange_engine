# Thread.cmake — adds a dedicated "ThreadSanitizer" build configuration.
#
# ThreadSanitizer catches data races that AddressSanitizer cannot. It is a
# Clang/AppleClang/GCC feature whose runtime ships only on macOS and Linux;
# neither MSVC nor GCC/Clang on Windows (MinGW) provide a TSan runtime. There the
# configuration builds WITHOUT instrumentation, and CMakePresets deliberately
# generates *-tsan presets ONLY for the macOS toolchains.
#
# Build it with the multi-config generators via `--config ThreadSanitizer`, or via
# a macos-*-tsan preset. The configuration mirrors RelWithDebInfo (optimized +
# debug info) and layers ThreadSanitizer on top. It is a SEPARATE config from
# AddressSanitizer because ASan and TSan cannot be combined in one binary.

# --- 1. Register the ThreadSanitizer configuration ------------------------------
# Append it to the multi-config type list so `--config ThreadSanitizer` is accepted
# and CLion offers it. FORCE updates the cache the IDE reads back after configuring.
if (CMAKE_CONFIGURATION_TYPES AND NOT "ThreadSanitizer" IN_LIST CMAKE_CONFIGURATION_TYPES)
    list(APPEND CMAKE_CONFIGURATION_TYPES ThreadSanitizer)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}"
            CACHE STRING "Supported configuration types" FORCE)
endif ()

# --- 2. Base flags: mirror RelWithDebInfo (optimized + debug info) ---------------
# A brand-new configuration starts with empty flags; without this it would build
# at -O0 with no debug info — nearly useless for a sanitizers run.
foreach (_lang C CXX)
    set(CMAKE_${_lang}_FLAGS_THREADSANITIZER "${CMAKE_${_lang}_FLAGS_RELWITHDEBINFO}"
            CACHE STRING "Flags used by the ${_lang} compiler for the ThreadSanitizer build type." FORCE)
endforeach ()
foreach (_type EXE SHARED MODULE STATIC)
    set(CMAKE_${_type}_LINKER_FLAGS_THREADSANITIZER "${CMAKE_${_type}_LINKER_FLAGS_RELWITHDEBINFO}"
            CACHE STRING "Linker flags for ${_type} targets in the ThreadSanitizer build type." FORCE)
endforeach ()

# --- 3. Map imported (vcpkg) targets: ThreadSanitizer -> Release -----------------
# fmt/simdjson/gtest/boost/openssl export only Debug and Release variants. Without
# this map, resolving their import libraries in the ThreadSanitizer config fails.
set(CMAKE_MAP_IMPORTED_CONFIG_THREADSANITIZER Release RelWithDebInfo "")

# --- 4. ThreadSanitizer instrumentation, applied only in this config -------------
set(TSAN_CONDITION "$<CONFIG:ThreadSanitizer>")

if (MSVC)
    # MSVC exposes /fsanitize=address only — there is no ThreadSanitizer for it.
    message(STATUS
            "MSVC has no ThreadSanitizer; the 'ThreadSanitizer' configuration will "
            "build WITHOUT instrumentation. Run TSan on the macOS/Linux toolchains.")
    set(TSAN_FLAGS "")
elseif (WIN32)
    # MinGW/GCC on Windows ship no libtsan — instrumenting here would fail at link.
    message(STATUS
            "MinGW cannot link ThreadSanitizer; the 'ThreadSanitizer' configuration "
            "will build WITHOUT instrumentation on Windows.")
    set(TSAN_FLAGS "")
else ()
    set(TSAN_FLAGS
            "$<${TSAN_CONDITION}:-fsanitize=thread>"
            "$<${TSAN_CONDITION}:-fno-omit-frame-pointer>"
            "$<${TSAN_CONDITION}:-g>")
endif ()

add_compile_options(${TSAN_FLAGS})
add_link_options(${TSAN_FLAGS})
