# ThreadSanitizer build configuration (mirrors RelWithDebInfo + TSan). Separate
# from AddressSanitizer because ASan and TSan cannot coexist in one binary. TSan's
# runtime exists only for Clang/AppleClang/GCC on macOS/Linux; MSVC and MinGW have
# none, so the config is uninstrumented on Windows and only the macOS toolchains
# get *-tsan presets.

if(CMAKE_CONFIGURATION_TYPES AND NOT "ThreadSanitizer" IN_LIST CMAKE_CONFIGURATION_TYPES)
    list(APPEND CMAKE_CONFIGURATION_TYPES ThreadSanitizer)
    set(CMAKE_CONFIGURATION_TYPES
        "${CMAKE_CONFIGURATION_TYPES}"
        CACHE STRING "Supported configuration types" FORCE)
endif()

# Mirror RelWithDebInfo so the run is optimized with debug info; a fresh config
# would otherwise be -O0 with no symbols.
foreach(_lang C CXX)
    set(CMAKE_${_lang}_FLAGS_THREADSANITIZER
        "${CMAKE_${_lang}_FLAGS_RELWITHDEBINFO}"
        CACHE STRING "Flags used by the ${_lang} compiler for the ThreadSanitizer build type." FORCE)
endforeach()
foreach(_type EXE SHARED MODULE STATIC)
    set(CMAKE_${_type}_LINKER_FLAGS_THREADSANITIZER
        "${CMAKE_${_type}_LINKER_FLAGS_RELWITHDEBINFO}"
        CACHE STRING "Linker flags for ${_type} targets in the ThreadSanitizer build type." FORCE)
endforeach()

# vcpkg deps export only Debug/Release, so resolve their imports as Release here.
set(CMAKE_MAP_IMPORTED_CONFIG_THREADSANITIZER Release RelWithDebInfo "")

set(TSAN_CONDITION "$<CONFIG:ThreadSanitizer>")

if(WIN32)
    # No TSan runtime for MSVC or MinGW on Windows.
    message(STATUS "Windows has no ThreadSanitizer; 'ThreadSanitizer' builds WITHOUT "
        "instrumentation. Run TSan on the macOS/Linux toolchains.")
    set(TSAN_FLAGS "")
else()
    set(TSAN_FLAGS
        "$<${TSAN_CONDITION}:-fsanitize=thread>"
        "$<${TSAN_CONDITION}:-fno-omit-frame-pointer>"
        "$<${TSAN_CONDITION}:-g>")
endif()

add_compile_options(${TSAN_FLAGS})
add_link_options(${TSAN_FLAGS})
