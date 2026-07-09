include_guard(GLOBAL)

function(set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)

        if(ORDER_BOOK_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()

    else()

        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic
                                                 -Wconversion -Wshadow)

        if(ORDER_BOOK_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()

    endif()

endfunction()
