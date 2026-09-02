include_guard(GLOBAL)

# HWAddressSanitizer. Cheap ASan-like checking using ARM MTE / TBI.
# First-class on AArch64 Clang; x86-64 Clang may accept it (probed).

include(CheckCXXCompilerFlag)

_order_book_register_sanitizer_config(HWAddressSanitizer)

if(WIN32)
    _order_book_sanitizer_uninstrumented(HWAddressSanitizer
        "HWAddressSanitizer is not available on Windows")
elseif(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    _order_book_sanitizer_uninstrumented(HWAddressSanitizer
        "HWAddressSanitizer requires Clang")
else()
    set(_ob_hwasan_save "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "-Werror=unused-command-line-argument")
    check_cxx_compiler_flag("-fsanitize=hwaddress" ORDER_BOOK_HAS_HWASAN)
    set(CMAKE_REQUIRED_FLAGS "${_ob_hwasan_save}")
    unset(_ob_hwasan_save)
    if(ORDER_BOOK_HAS_HWASAN)
        _order_book_add_sanitizer_flags(HWAddressSanitizer
            -fsanitize=hwaddress
            -fno-omit-frame-pointer
            -g)
        message(STATUS "sanitizers: HWAddressSanitizer instruments with HWASan")
    else()
        _order_book_sanitizer_uninstrumented(HWAddressSanitizer
            "-fsanitize=hwaddress is not supported by this Clang/target")
    endif()
endif()
