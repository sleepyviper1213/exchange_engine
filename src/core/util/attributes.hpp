#pragma once

// --- lifetime diagnostics -------------------------------------------------
//
// EXCHANGE_POINTER and EXCHANGE_OWNER are a pair, and which one a type gets is
// decided by whether it keeps the callable alive. A view (function_ref) is a
// POINTER; a container (move_only_function) is an OWNER. Swapping them is
// worse than omitting both: a POINTER on an owner makes clang reject
// `move_only_function<void()> f = []{...};`, which is the normal use.
//
// The pair is what earns its keep - clang diagnoses a POINTER initialised from
// an OWNER temporary, so `function_ref<void()> r = make_move_only_function();`
// is caught at compile time. Neither attribute changes codegen or layout.
//
// This is deliberately a diagnostic rather than a constraint, and a constraint
// could not do the job: `requires is_lvalue_reference_v<F>` on the converting
// constructor rejects the *safe* case too, because both spellings hand it the
// same prvalue closure -
//
//     any_of([](int x) noexcept { ... });       // safe: lives for the call
//     function_ref<void(int)> r = [&] { ... };  // dangles at the `;`
//
// - and value category is all a constraint can see. `spsc_queue::consume_all`
// and every one of its call sites are the first form, so rejecting rvalues
// would break the idiom function_ref exists for. Clang's flow analysis does
// separate them, so `set_warnings` promotes it: -Werror=dangling, which covers
// -Wdangling and -Wreturn-stack-address. Clang-only - GCC ignores the gsl::
// attributes and has no equivalent - so the gate lives on the clang presets.
//
// C++26 draws the same line. Its converting constructor takes rvalues; the one
// that refuses them (`!is_rvalue_reference_v<U&&>`) is the nontype_t overload
// binding a *separate* object to a compile-time callable, where no
// argument-passing idiom exists and a temporary is never what was meant.

#ifdef __has_cpp_attribute
#if __has_cpp_attribute(clang::lifetimebound)
#define EXCHANGE_LIFETIMEBOUND [[clang::lifetimebound]]
#elif __has_cpp_attribute(msvc::lifetimebound)
#define EXCHANGE_LIFETIMEBOUND [[msvc::lifetimebound]]
#endif
#if __has_cpp_attribute(gsl::Pointer)
#define EXCHANGE_POINTER [[gsl::Pointer]]
#define EXCHANGE_OWNER [[gsl::Owner]]
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
#ifndef EXCHANGE_POINTER
#define EXCHANGE_POINTER
#define EXCHANGE_OWNER
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

// --- deleted-function reasons ---------------------------------------------
//
// C++26 lets `= delete("why")` carry the explanation into the diagnostic, which
// is worth having on the deletions here: what a caller does about `use of
// deleted function operator=` is not obvious, and the reason is a lifetime rule
// rather than a typo.
//
// Guarded on the language version as well as the feature macro because Clang
// defines __cpp_deleted_function under -std=c++23, where the syntax is still an
// extension - and -Wc++26-extensions plus WARNINGS_AS_ERRORS would fail the
// build. GCC sets neither until -std=c++2c. Both report __cplusplus 202400
// there, against 202302 for C++23.

#if defined(__cpp_deleted_function) && __cplusplus > 202'302L
#define EXCHANGE_DELETE(reason) = delete (reason)
#else
#define EXCHANGE_DELETE(reason) = delete
#endif

// --- codegen hints --------------------------------------------------------

#ifdef __has_cpp_attribute
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

#ifdef __has_cpp_attribute
#if __has_cpp_attribute(msvc::no_unique_address)
#define EXCHANGE_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#elif __has_cpp_attribute(no_unique_address)
#define EXCHANGE_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif
#endif

#ifndef EXCHANGE_NO_UNIQUE_ADDRESS
#define EXCHANGE_NO_UNIQUE_ADDRESS
#endif
