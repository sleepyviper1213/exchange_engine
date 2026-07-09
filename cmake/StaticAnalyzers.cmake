include_guard(GLOBAL)

if (ORDER_BOOK_ENABLE_STATIC_ANALYZERS) 
	set(ORDER_BOOK_ENABLE_CLANG_TIDY ON)
	set(ORDER_BOOK_ENABLE_CPPCHECK ON)
endif()

if(ORDER_BOOK_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY clang-tidy)

    if(CLANG_TIDY)
        set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY})
    endif()

endif()

if(ORDER_BOOK_ENABLE_CPPCHECK)

    find_program(CPPCHECK cppcheck)

    if(CPPCHECK)
        set(CMAKE_CXX_CPPCHECK ${CPPCHECK})
    endif()

endif()
