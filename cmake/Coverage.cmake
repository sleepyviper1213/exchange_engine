include_guard(GLOBAL)
include(CheckCXXCompilerFlag)


function(enable_coverage target)
    if(NOT ORDER_BOOK_ENABLE_COVERAGE OR MSVC)
        return()
    endif()

    

    set(_when "$<CONFIG:Debug,RelWithDebInfo>")

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        target_compile_options(${target} PRIVATE "$<${_when}:--coverage>")
        # GCC-only, and a driver error under clang-tidy. Absolute paths in the
        # .gcno are a convenience; gcovr is given --root either way.
        if(NOT CMAKE_CXX_CLANG_TIDY)
            target_compile_options(${target} PRIVATE
                "$<${_when}:-fprofile-abs-path>")
        endif()
        target_link_options(${target} PRIVATE
            "$<${_when}:--coverage>")

    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${target} PRIVATE
            "$<${_when}:-fprofile-instr-generate>"
            "$<${_when}:-fcoverage-mapping>")
        target_link_options(${target} PRIVATE
            "$<${_when}:-fprofile-instr-generate>")
    endif()

    check_cxx_compiler_flag(-fprofile-update=atomic
        ORDER_BOOK_HAS_PROFILE_UPDATE_ATOMIC)
        
    if(ORDER_BOOK_HAS_PROFILE_UPDATE_ATOMIC)
        target_compile_options(${target} PRIVATE
            "$<${_when}:-fprofile-update=atomic>")
    endif()
endfunction()
