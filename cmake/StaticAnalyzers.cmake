include_guard(GLOBAL)

if (ORDER_BOOK_ENABLE_STATIC_ANALYZERS) 
	set(ORDER_BOOK_ENABLE_CLANG_TIDY ON)
	set(ORDER_BOOK_ENABLE_CPPCHECK ON)
endif()

if(ORDER_BOOK_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY clang-tidy)

    if(CLANG_TIDY)
        # clang-tidy re-parses the compiler's own command line. On a GCC build
        # that line carries flags clang rejects: unknown -W..., and -f... it
        # accepts but does not use, which -Werror then promotes.
        set(CMAKE_CXX_CLANG_TIDY
            "${CLANG_TIDY}"
            "--extra-arg=-Wno-unknown-warning-option"
            "--extra-arg=-Wno-unused-command-line-argument"
            # Google Benchmark's BENCHMARK() expands __COUNTER__, which clang
            # calls a C2y extension under -Wpedantic and GCC accepts silently.
            # No first-party code uses it.
            "--extra-arg=-Wno-c2y-extensions")

        # An unknown -f... is a driver error instead, which no -Wno- suppresses,
        # so the flag itself has to go. CMake spells
        # CMAKE_VISIBILITY_INLINES_HIDDEN as -fno-keep-inline-dllexport on
        # Windows-GNU; dropping it costs a larger object in a tree that is
        # analysed and measured, never shipped.
        if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set(CMAKE_CXX_COMPILE_OPTIONS_VISIBILITY_INLINES_HIDDEN "")

            # clang defaults to the MSVC triple on Windows, so without this
            # clang-tidy analyses a MinGW build against MSVC's STL and reports
            # on headers the compiler never reads.
            list(APPEND CMAKE_CXX_CLANG_TIDY
                 "--extra-arg=--target=x86_64-w64-windows-gnu")
        endif()

        # GCC enables sized deallocation in C++14 and later; clang does not, so
        # without this clang-tidy cannot see
        # operator delete(void*, size_t, align_val_t) and core/memory/ fails to
        # parse.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            list(APPEND CMAKE_CXX_CLANG_TIDY
                 "--extra-arg=-fsized-deallocation")
        endif()
    endif()

endif()

if(ORDER_BOOK_ENABLE_CPPCHECK)

    find_program(CPPCHECK cppcheck)

    if(CPPCHECK)
        # cppcheck has no coroutine support: co_return does not read as a
        # return, so every awaitable looks like a path that falls off the end.
        # Not scoped to a path - coroutines live in transport/, session/ and
        # app/, and a list would rot. Nothing is lost: falling off the end of a
        # non-void function is already a compiler diagnostic.
        set(CMAKE_CXX_CPPCHECK
            "${CPPCHECK}"
            "--inline-suppr"
            # Without it, TEST(...) does not expand and cppcheck reports a
            # syntax error for the whole file - it then analyses nothing there.
            # boost.cfg ships alongside it if transport/ ever needs the same.
            "--library=googletest"
            "--suppress=missingReturn"
            "--suppress=*:*third_party*")
    endif()

endif()
