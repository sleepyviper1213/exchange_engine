include_guard(GLOBAL)

# MemorySanitizer (Clang, Linux). Finds uninitialized reads. Every linked
# TU — including libstdc++/libc++ and third-party deps — must be built with
# MSan or you get false positives. GCC has no MSan.

_exchange_register_sanitizer_config(MemorySanitizer)

if(WIN32 OR APPLE)
    _exchange_sanitizer_uninstrumented(MemorySanitizer
        "MemorySanitizer is Clang on Linux only")
elseif(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    _exchange_sanitizer_uninstrumented(MemorySanitizer
        "MemorySanitizer requires Clang (CMAKE_CXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID})")
else()
    _exchange_add_sanitizer_flags(MemorySanitizer
        -fsanitize=memory
        -fsanitize-memory-track-origins=2
        -fno-omit-frame-pointer
        -g)
    message(STATUS "sanitizers: MemorySanitizer instruments with MSan (track-origins=2)")
    message(STATUS "sanitizers: MSan requires every dependency instrumented, including the STL")
endif()
