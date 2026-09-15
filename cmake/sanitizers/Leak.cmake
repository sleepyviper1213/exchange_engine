include_guard(GLOBAL)

# LeakSanitizer standalone. On Linux this is the ASan leak detector without
# the rest of ASan. Darwin has no standalone LSan (use AddressSanitizer).

_exchange_register_sanitizer_config(LeakSanitizer)

if(WIN32 OR APPLE)
    _exchange_sanitizer_uninstrumented(LeakSanitizer
        "Standalone LeakSanitizer is Linux-only (on macOS use AddressSanitizer)")
else()
    _exchange_add_sanitizer_flags(LeakSanitizer
        -fsanitize=leak
        -fno-omit-frame-pointer
        -g)
    message(STATUS "sanitizers: LeakSanitizer instruments with LSan")
endif()
