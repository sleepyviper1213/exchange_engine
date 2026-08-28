include_guard(GLOBAL)

function(enable_coverage target)
    if(NOT ORDER_BOOK_ENABLE_COVERAGE OR MSVC)
        return()
    endif()

    set(_when "$<CONFIG:Debug,RelWithDebInfo>")

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        target_compile_options(${target} PRIVATE
            "$<${_when}:--coverage>"
            "$<${_when}:-fprofile-abs-path>")
        target_link_options(${target} PRIVATE
            "$<${_when}:--coverage>")

    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${target} PRIVATE
            "$<${_when}:-fprofile-instr-generate>"
            "$<${_when}:-fcoverage-mapping>")
        target_link_options(${target} PRIVATE
            "$<${_when}:-fprofile-instr-generate>")
    endif()
endfunction()
