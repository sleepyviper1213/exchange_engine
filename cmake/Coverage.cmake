include_guard(GLOBAL)

function(enable_coverage target)

    if(NOT ORDER_BOOK_ENABLE_COVERAGE)
        return()
    endif()

	if (MSVC)
	   	return()
	endif()
    target_compile_options(${target} PRIVATE --coverage)
    target_link_options(${target} PRIVATE --coverage)

endfunction()
