#pragma once

// A non-owning reference to something callable - the type to reach for when a
// callback crosses a boundary a template cannot.
#ifdef __clang__
#include <__concepts/invocable.h>
#include <__concepts/same_as.h>
#include <__functional/invoke.h>
#include <__memory/addressof.h>
#include <__type_traits/add_pointer.h>
#include <__type_traits/remove_cvref.h>
#include <__type_traits/remove_reference.h>
#elifdef __GNUC__
#include <bits/invoke.h>
#include <bits/move.h>
#include <bits/stl_function.h>

#include <type_traits>
#elifdef _MSC_VER
#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#else
#error "Unsupported compiler"
#endif
namespace exchange::core::util {

namespace detail {
/**
 * Erase const from a pointer so it can be stored as void*.
 * The thunk later casts it back to the correct const/non-const type.
 */
template <class T>
[[nodiscard]] constexpr void *erase_const(T *pointer) noexcept {
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
	return const_cast<void *>(static_cast<const void *>(pointer));
}

template <class R, class... Args>
class function_ref_base {
protected:
	using object_ptr = void *;
	using invoker_t  = R (*)(object_ptr,
                            Args...); // noexcept is added by the derived class

	// NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
	object_ptr object_ = nullptr;
	invoker_t invoke_  = nullptr;
	// NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

	constexpr function_ref_base() noexcept = default;

	template <class Func>
	constexpr function_ref_base(Func &&func, invoker_t thunk) noexcept
		: object_(erase_const(std::addressof(func))), invoke_(thunk) {}

public:
	// Rebinding is deleted in the derived class
	template <class Other>
		requires (!std::same_as<std::remove_cvref_t<Other>, function_ref_base>)
	function_ref_base &operator=(Other &&) = delete;
};

} // namespace detail

/**
 * @brief A borrowed callable: two pointers, no ownership, no allocation.
 *
 * @tparam R Return type.
 * @tparam Args Parameter types.
 *
 * @par Why non-owning, and what that costs the caller
 * Because owning would mean allocating, and the point is to be usable where
 * allocating is not. What it costs is a lifetime obligation that the type
 * cannot check: this stores a pointer to the callable, so the callable must
 * outlive every call made through it.
 *
 */
template <class...>
class function_ref;

// ===========================================================================
// 1. R(Args...)
// ===========================================================================
#define FRF_CV
#define FRF_NOEXCEPT
#define FRF_INVOKE_QUAL
#define FRF_CONSTRAINT(R, F, Args) std::is_invocable_r_v<R, F &, Args...>

#include "function_ref_impl.hpp"

#undef FRF_CV
#undef FRF_NOEXCEPT
#undef FRF_INVOKE_QUAL
#undef FRF_CONSTRAINT

// ===========================================================================
// 2. R(Args...) noexcept
// ===========================================================================
#define FRF_CV
#define FRF_NOEXCEPT noexcept
#define FRF_INVOKE_QUAL
#define FRF_CONSTRAINT(R, F, Args)                                             \
	std::is_nothrow_invocable_r_v<R, F &, Args...>

#include "function_ref_impl.hpp"

#undef FRF_CV
#undef FRF_NOEXCEPT
#undef FRF_INVOKE_QUAL
#undef FRF_CONSTRAINT

// ===========================================================================
// 3. R(Args...) const
// ===========================================================================
#define FRF_CV const
#define FRF_NOEXCEPT
#define FRF_INVOKE_QUAL const
#define FRF_CONSTRAINT(R, F, Args)                                             \
	std::is_invocable_r_v<R, const std::remove_reference_t<F> &, Args...>

#include "function_ref_impl.hpp"

#undef FRF_CV
#undef FRF_NOEXCEPT
#undef FRF_INVOKE_QUAL
#undef FRF_CONSTRAINT

// ===========================================================================
// 4. R(Args...) const noexcept
// ===========================================================================
#define FRF_CV const
#define FRF_NOEXCEPT noexcept
#define FRF_INVOKE_QUAL const
#define FRF_CONSTRAINT(R, F, Args)                                             \
	std::is_nothrow_invocable_r_v<R,                                           \
								  const std::remove_reference_t<F> &,          \
								  Args...>

#include "function_ref_impl.hpp"

#undef FRF_CV
#undef FRF_NOEXCEPT
#undef FRF_INVOKE_QUAL
#undef FRF_CONSTRAINT

// ---------------------------------------------------------------------------
// Deduction guide
// ---------------------------------------------------------------------------
#if __cplusplus >= 201'703L
template <class R, class... Args>
function_ref(R (*)(Args...)) -> function_ref<R(Args...)>;
#endif
} // namespace exchange::core::util
