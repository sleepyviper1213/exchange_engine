include_guard(GLOBAL)

# Runtime hardening for a target, toggled by ORDER_BOOK_ENABLE_HARDENING.
#
# The headline effect is that assert() stays live even in optimized builds: the
# project's defensive precondition checks (e.g. the hazard-pointer thread-cache
# invariants, which turn a corrupted index into a clean abort instead of an
# out-of-bounds access) are otherwise stripped whenever the build type defines
# NDEBUG. On top of that it turns on standard-library bounds/precondition
# checking and the usual stack/buffer protections.
#
# Applied PRIVATE, so it only hardens the given target's own translation units.
# Header-only code (like the hazard-pointer library) is compiled into whichever
# target includes it, so hardening must be enabled on those targets too — hence
# it is applied to the test target, not just the library.
function(enable_hardening target)
    if(NOT ORDER_BOOK_ENABLE_HARDENING)
        return()
    endif()

    if(MSVC)
        # /U NDEBUG re-enables assert(); /sdl adds extra security checks; /GS is
        # buffer-overrun detection.
        target_compile_options(${target} PRIVATE /UNDEBUG /sdl /GS)
        return()
    endif()

    # GCC / Clang. Target options are placed after the build type's flags, so
    # -UNDEBUG reliably cancels a -DNDEBUG coming from Release/RelWithDebInfo.
    target_compile_options(${target} PRIVATE -UNDEBUG -fstack-protector-strong)

    # Standard-library precondition and bounds checking.
    target_compile_definitions(
        ${target}
        PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:_GLIBCXX_ASSERTIONS>
            $<$<CXX_COMPILER_ID:Clang,AppleClang>:_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST>
    )

    # _FORTIFY_SOURCE needs optimization to do anything, and on mingw-w64 it
    # pulls in libssp which is awkward to link — restrict it to optimized,
    # non-Windows builds. -U first avoids redefinition warnings when the
    # toolchain already predefines it.
    if(NOT WIN32)
        target_compile_options(
            ${target}
            PRIVATE $<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE>
                    $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>)
    endif()
endfunction()
