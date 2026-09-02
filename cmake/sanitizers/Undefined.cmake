include_guard(GLOBAL)

# UndefinedBehaviorSanitizer as its own RelWithDebInfo-based config.
# AddressSanitizer already folds UBSan in on GCC/Clang; use this config when
# you want UB without ASan's memory overhead.

_order_book_register_sanitizer_config(UndefinedSanitizer)

if(WIN32)
    _order_book_sanitizer_uninstrumented(UndefinedSanitizer
        "Windows has no stable UBSan runtime for cl.exe/MinGW")
else()
    _order_book_add_sanitizer_flags(UndefinedSanitizer
        -fsanitize=undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
        -g)
    message(STATUS "sanitizers: UndefinedSanitizer instruments with UBSan")
endif()
