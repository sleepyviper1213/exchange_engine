# Homebrew LLVM toolchain for Apple Silicon (default: llvm@22).
#
# Pins the C/C++ compiler to Homebrew's LLVM and links against *its* libc++ /
# libunwind rather than AppleClang's, so C++26 features and the standard-library
# ABI are consistent across the project and its vcpkg dependencies.
#
# This file is chainloaded by vcpkg (VCPKG_CHAINLOAD_TOOLCHAIN_FILE), so both the
# ports and the project itself build with the same compiler + stdlib.
#
# Override the install prefix if Homebrew is elsewhere or you use another version:
#   -DHOMEBREW_LLVM_PREFIX=/opt/homebrew/opt/llvm@22
# or export HOMEBREW_LLVM_PREFIX before configuring.

if (NOT HOMEBREW_LLVM_PREFIX)
    if (DEFINED ENV{HOMEBREW_LLVM_PREFIX})
        set(HOMEBREW_LLVM_PREFIX "$ENV{HOMEBREW_LLVM_PREFIX}")
    else ()
        set(HOMEBREW_LLVM_PREFIX "/opt/homebrew/opt/llvm")
    endif ()
endif ()

if (NOT EXISTS "${HOMEBREW_LLVM_PREFIX}/bin/clang++")
    message(FATAL_ERROR
            "Homebrew LLVM not found at '${HOMEBREW_LLVM_PREFIX}'. "
            "Install it with `brew install llvm@22`, or pass "
            "-DHOMEBREW_LLVM_PREFIX=<prefix> (see `brew --prefix llvm`).")
endif ()

set(CMAKE_C_COMPILER   "${HOMEBREW_LLVM_PREFIX}/bin/clang"   CACHE FILEPATH "Homebrew LLVM clang"   FORCE)
set(CMAKE_CXX_COMPILER "${HOMEBREW_LLVM_PREFIX}/bin/clang++" CACHE FILEPATH "Homebrew LLVM clang++" FORCE)

# Use the LLVM toolchain's own runtime libraries and bake an rpath so binaries
# find them at run time. Mirrors the `brew info llvm` caveats.
set(_hb_llvm_link
        "-L${HOMEBREW_LLVM_PREFIX}/lib/c++ -Wl,-rpath,${HOMEBREW_LLVM_PREFIX}/lib/c++ \
-L${HOMEBREW_LLVM_PREFIX}/lib/unwind -lunwind")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_hb_llvm_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_hb_llvm_link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_hb_llvm_link}")
unset(_hb_llvm_link)
