include_guard(GLOBAL)
include(CheckCXXCompilerFlag)

set(EXCHANGE_COVERAGE_CONFIGS Debug RelWithDebInfo)

if(EXCHANGE_ENABLE_COVERAGE)
    if(MSVC OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR
            "EXCHANGE_ENABLE_COVERAGE is ON but ${CMAKE_CXX_COMPILER_ID} has "
            "no coverage backend here. The instrumented toolchains are the "
            "windows-mingw, linux-gcc and macos-arm64-llvm -coverage presets.")
    endif()

    if(CMAKE_CONFIGURATION_TYPES)
        set(EXCHANGE_COVERAGE_TREE_CONFIGS "${CMAKE_CONFIGURATION_TYPES}")
    else()
        set(EXCHANGE_COVERAGE_TREE_CONFIGS "${CMAKE_BUILD_TYPE}")
    endif()

    set(EXCHANGE_COVERAGE_USABLE_CONFIGS "")
    foreach(_config IN LISTS EXCHANGE_COVERAGE_TREE_CONFIGS)
        if("${_config}" IN_LIST EXCHANGE_COVERAGE_CONFIGS)
            list(APPEND EXCHANGE_COVERAGE_USABLE_CONFIGS "${_config}")
        endif()
    endforeach()

    if(NOT EXCHANGE_COVERAGE_TREE_CONFIGS)
        set(EXCHANGE_COVERAGE_TREE_CONFIGS
            "no configuration (CMAKE_BUILD_TYPE is empty)")
    endif()

    if(NOT EXCHANGE_COVERAGE_USABLE_CONFIGS)
        message(FATAL_ERROR
            "EXCHANGE_ENABLE_COVERAGE is ON but this tree builds "
            "${EXCHANGE_COVERAGE_TREE_CONFIGS}. Instrumentation is gated to "
            "${EXCHANGE_COVERAGE_CONFIGS}, so every line would report as "
            "unexecuted. Configure one of those, or use a -coverage preset.")
    endif()

    check_cxx_compiler_flag(-fprofile-update=atomic
        EXCHANGE_HAS_PROFILE_UPDATE_ATOMIC)

    cmake_language(DEFER CALL _exchange_write_coverage_manifest)
endif()

# llvm-cov needs every instrumented artefact named; each module here is a
# SHARED library carrying its own coverage mapping. Recording the targets and
# resolving them with file(GENERATE) hands scripts/coverage.sh the build
# system's own answer, per configuration, instead of a guess reconstructed by
# matching module names against everything on disk.
function(_exchange_write_coverage_manifest)
    get_property(_targets GLOBAL PROPERTY EXCHANGE_COVERAGE_TARGETS)
    if(NOT _targets)
        return()
    endif()

    set(_content "")
    foreach(_target IN LISTS _targets)
        string(APPEND _content "$<TARGET_FILE:${_target}>\n")
    endforeach()

    list(JOIN EXCHANGE_COVERAGE_CONFIGS "," _configs)
    file(GENERATE
        OUTPUT "${CMAKE_BINARY_DIR}/coverage-objects-$<CONFIG>.txt"
        CONTENT "${_content}"
        CONDITION "$<CONFIG:${_configs}>")
endfunction()

function(enable_coverage target)
    if(NOT EXCHANGE_ENABLE_COVERAGE)
        return()
    endif()

    set_property(GLOBAL APPEND PROPERTY EXCHANGE_COVERAGE_TARGETS "${target}")

    list(JOIN EXCHANGE_COVERAGE_CONFIGS "," _configs)
    set(_when "$<CONFIG:${_configs}>")

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        target_compile_options(${target} PRIVATE "$<${_when}:--coverage>")
        # GCC-only, and a driver error under clang-tidy. Absolute paths in the
        # .gcno are a convenience; gcovr is given --root either way.
        if(NOT CMAKE_CXX_CLANG_TIDY)
            target_compile_options(${target} PRIVATE
                "$<${_when}:-fprofile-abs-path>")
        endif()
        target_link_options(${target} PRIVATE "$<${_when}:--coverage>")
    else()
        target_compile_options(${target} PRIVATE
            "$<${_when}:-fprofile-instr-generate>"
            "$<${_when}:-fcoverage-mapping>")
        target_link_options(${target} PRIVATE
            "$<${_when}:-fprofile-instr-generate>")
    endif()

    # order_test drives the concurrency suites from one process; a non-atomic
    # counter update is a data race that silently undercounts.
    if(EXCHANGE_HAS_PROFILE_UPDATE_ATOMIC)
        target_compile_options(${target} PRIVATE
            "$<${_when}:-fprofile-update=atomic>")
    endif()
endfunction()
