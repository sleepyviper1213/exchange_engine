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
    set(ASAN_FLAGS "$<${ASAN_CONDITION}:/fsanitize=address>")
    add_compile_options("$<${ASAN_CONDITION}:/Zi>") # PDB for symbolized reports
elseif (WIN32)
    # MinGW has no libasan — instrumenting would fail at link.
    message(STATUS "MinGW cannot link AddressSanitizer; 'AddressSanitizer' builds "
            "WITHOUT instrumentation. Use the windows-msvc preset for ASan on Windows.")
    set(ASAN_FLAGS "")
else ()
    set(ASAN_FLAGS
            "$<${ASAN_CONDITION}:-fsanitize=address>"
            "$<${ASAN_CONDITION}:-fno-omit-frame-pointer>"
            "$<${ASAN_CONDITION}:-g>")
endif ()

add_compile_options(${ASAN_FLAGS})
add_link_options(${ASAN_FLAGS})
