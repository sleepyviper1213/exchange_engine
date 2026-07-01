# Profile.cmake — adds a "Profile" build configuration for Visual Studio's
# instrumentation profiler (MSVC only).
#
# It mirrors RelWithDebInfo (optimized + debug info) and adds the linker's
# /PROFILE option, which instrumentation profiling requires (it also implies
# /DEBUG and disables incremental linking). Build via `--config Profile` or the
# IDE configuration dropdown — no separate configure preset is needed.
#
# /PROFILE is a Microsoft linker feature, so this module is a no-op on non-MSVC
# toolchains (there the CPU-sampling story is RelWithDebInfo + a profiler like
# Instruments/perf).

if (NOT MSVC)
    return()
endif ()

# Register the configuration for the multi-config generators (FORCE so the IDE
# picks it up from the cache).
if (CMAKE_CONFIGURATION_TYPES AND NOT "Profile" IN_LIST CMAKE_CONFIGURATION_TYPES)
    list(APPEND CMAKE_CONFIGURATION_TYPES Profile)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}"
            CACHE STRING "Supported configuration types" FORCE)
endif ()

# Compile flags mirror RelWithDebInfo (/O2 /Ob1 /Zi /DNDEBUG).
foreach (_lang C CXX)
    set(CMAKE_${_lang}_FLAGS_PROFILE "${CMAKE_${_lang}_FLAGS_RELWITHDEBINFO}"
            CACHE STRING "Flags used by the ${_lang} compiler for the Profile build type." FORCE)
endforeach ()

# /PROFILE is a link.exe option, so add it to the executable/shared/module linker
# flags (not the static archiver, which would reject it).
foreach (_type EXE SHARED MODULE)
    set(CMAKE_${_type}_LINKER_FLAGS_PROFILE
            "${CMAKE_${_type}_LINKER_FLAGS_RELWITHDEBINFO} /PROFILE"
            CACHE STRING "Linker flags for ${_type} targets in the Profile build type." FORCE)
endforeach ()
set(CMAKE_STATIC_LINKER_FLAGS_PROFILE "${CMAKE_STATIC_LINKER_FLAGS_RELWITHDEBINFO}"
        CACHE STRING "Static archiver flags for the Profile build type." FORCE)

# vcpkg imports only ship Debug/Release variants; resolve Profile against Release.
set(CMAKE_MAP_IMPORTED_CONFIG_PROFILE Release RelWithDebInfo "")
