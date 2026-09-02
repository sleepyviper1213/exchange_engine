include_guard(GLOBAL)

# LeakSanitizer standalone. On Linux this is the ASan leak detector without
# the rest of ASan. Darwin has no standalone LSan (use AddressSanitizer).

_order_book_register_sanitizer_config(LeakSanitizer)

if(WIN32 OR APPLE)
    _order_book_sanitizer_uninstrumented(LeakSanitizer
        "Standalone LeakSanitizer is Linux-only (on macOS use AddressSanitizer)")
else()
    _order_book_add_sanitizer_flags(LeakSanitizer
        -fsanitize=leak
        -fno-omit-frame-pointer
        -g)
    message(STATUS "sanitizers: LeakSanitizer instruments with LSan")
endif()
