#pragma once

// --- lifetime diagnostics -------------------------------------------------

#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::lifetimebound)
#define EXCHANGE_LIFETIMEBOUND [[clang::lifetimebound]]
#elif __has_cpp_attribute(msvc::lifetimebound)
#define EXCHANGE_LIFETIMEBOUND [[msvc::lifetimebound]]
#endif
#if __has_cpp_attribute(clang::lifetime_capture_by)
#define EXCHANGE_CAPTURED_BY(...) [[clang::lifetime_capture_by(__VA_ARGS__)]]
#endif
#if __has_cpp_attribute(clang::coro_return_type)
#define EXCHANGE_CORO_RETURN_TYPE clang::coro_return_type
#define EXCHANGE_CORO_LIFETIMEBOUND clang::coro_lifetimebound
#define EXCHANGE_CORO_WRAPPER [[clang::coro_wrapper]]
#define EXCHANGE_CORO_NO_LIFETIMEBOUND [[clang::coro_disable_lifetimebound]]
#endif
#endif

#ifndef EXCHANGE_LIFETIMEBOUND
#define EXCHANGE_LIFETIMEBOUND
#endif
#ifndef EXCHANGE_CAPTURED_BY
#define EXCHANGE_CAPTURED_BY(...)
#endif
#ifndef EXCHANGE_CORO_RETURN_TYPE
#define EXCHANGE_CORO_RETURN_TYPE maybe_unused
#define EXCHANGE_CORO_LIFETIMEBOUND maybe_unused
#define EXCHANGE_CORO_WRAPPER
#define EXCHANGE_CORO_NO_LIFETIMEBOUND
#endif

// --- codegen hints --------------------------------------------------------

#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(gnu::hot)
#define EXCHANGE_HOT [[gnu::hot]]
#endif
#if __has_cpp_attribute(gnu::flatten)
#define EXCHANGE_FLATTEN [[gnu::flatten]]
#elif __has_cpp_attribute(msvc::forceinline_calls)
#define EXCHANGE_FLATTEN [[msvc::forceinline_calls]]
#endif
#endif

#ifndef EXCHANGE_HOT
#define EXCHANGE_HOT
#endif
#ifndef EXCHANGE_FLATTEN
#define EXCHANGE_FLATTEN
#endif

// --- layout ---------------------------------------------------------------
//
// MSVC parses [[no_unique_address]] and then ignores it - honouring it would
// have broken its ABI - so the empty member it is applied to keeps occupying a
// byte plus the padding that follows. The spelling MSVC does honour is
// [[msvc::no_unique_address]], hence the order below: the vendor form wins
// where it exists, and everything else takes the standard one.
//
// Measured on an `empty e; long long x;` pair: 16 bytes under the MSVC target
// with the standard attribute against 8 with the vendor one, and 8 under MinGW
// GCC with the standard one. Writing [[no_unique_address]] directly therefore
// silently costs 8 bytes per use on one of the two Windows presets.

#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(msvc::no_unique_address)
#define EXCHANGE_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#elif __has_cpp_attribute(no_unique_address)
#define EXCHANGE_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif
#endif

#ifndef EXCHANGE_NO_UNIQUE_ADDRESS
#define EXCHANGE_NO_UNIQUE_ADDRESS
#endif
