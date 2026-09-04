include_guard(GLOBAL)

# ThreadSanitizer build configuration (mirrors RelWithDebInfo + TSan). Separate
# from AddressSanitizer because ASan and TSan cannot coexist in one binary. TSan's
# runtime exists only for Clang/AppleClang/GCC on macOS/Linux; MSVC and MinGW have
# none, so the config is uninstrumented on Windows and only the macOS and Linux
# toolchains get *-tsan presets.

_order_book_register_sanitizer_config(ThreadSanitizer)

if(WIN32)
    _order_book_sanitizer_uninstrumented(ThreadSanitizer
        "Windows has no ThreadSanitizer runtime for MSVC or MinGW")
else()
    _order_book_add_sanitizer_flags(ThreadSanitizer
        -fsanitize=thread -fno-omit-frame-pointer -g)
    message(STATUS "sanitizers: ThreadSanitizer instruments with TSan")
endif()
