include_guard(GLOBAL)

set(ORDER_BOOK_WARNINGS_MSVC
    /W4
    /permissive-
    /w14242
    /w14254
    /w14263
    /w14265
    /w14287
    /w14289
    /w14296
    /w14311
    /w14545
    /w14546
    /w14547
    /w14549
    /w14555
    /w14619
    /w14640
    /w14826
    /w14905
    /w14906
    /w14928)

set(ORDER_BOOK_WARNINGS_CLANG
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wcast-align
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    -Wnon-virtual-dtor
    -Wnull-dereference
    -Woverloaded-virtual
    -Wunused)

set(ORDER_BOOK_WARNINGS_GNU
    ${ORDER_BOOK_WARNINGS_CLANG}
    -Wduplicated-branches
    -Wduplicated-cond
    -Wlogical-op
    -Wmisleading-indentation
    -Wsuggest-override)

function(set_warnings target)
    get_target_property(_ob_target_type ${target} TYPE)
    if(_ob_target_type STREQUAL "INTERFACE_LIBRARY")
        return()
    endif()

    if(MSVC)
        set(_ob_warnings ${ORDER_BOOK_WARNINGS_MSVC})
        if(ORDER_BOOK_WARNINGS_AS_ERRORS)
            list(APPEND _ob_warnings /WX)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(_ob_warnings ${ORDER_BOOK_WARNINGS_CLANG})
        if(ORDER_BOOK_WARNINGS_AS_ERRORS)
            list(APPEND _ob_warnings -Werror)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(_ob_warnings ${ORDER_BOOK_WARNINGS_GNU})
        if(ORDER_BOOK_WARNINGS_AS_ERRORS)
            list(APPEND _ob_warnings -Werror)
        endif()
    else()
        message(AUTHOR_WARNING
                "No warning set for CXX compiler '${CMAKE_CXX_COMPILER_ID}'")
        return()
    endif()

    target_compile_options(${target} PRIVATE ${_ob_warnings})
endfunction()
