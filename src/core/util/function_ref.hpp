#pragma once

// A non-owning reference to something callable - the type to reach for when a
// callback crosses a boundary a template cannot.

#include <functional>

#ifdef __cpp_lib_function_ref
namespace exchange::core::util {
using std::function_ref;
}
#else
#include "core/util/attributes.hpp"

#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

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

/**
 * The bound entity: either the address of a referenced object, or a function
 * itself. They need separate members rather than one void* because converting
 * a function pointer to void* is only conditionally supported, and because a
 * function pointer has nothing for a void* to point *at*. Only the member the
 * thunk stored is ever read back.
 */
union bound_entity {
	void *object;
	void (*function)();

	constexpr explicit bound_entity(void *pointer) noexcept : object(pointer) {}

	constexpr explicit bound_entity(void (*pointer)()) noexcept
		: function(pointer) {}
};

template <class R, class... Args>
class function_ref_base {
protected:
	using object_ptr = bound_entity;
	// Args&&... rather than Args..., which is how C++26 spells the thunk: a
	// by-value parameter would otherwise be moved into operator() and then
	// again into the thunk. Measured on a move-counting type, 2 moves per call
	// against 1. noexcept is added by the derived class.
	using invoker_t = R (*)(object_ptr, Args &&...);

	// NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
	object_ptr object_;
	invoker_t invoke_ = nullptr;

	// NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

	constexpr function_ref_base(object_ptr target, invoker_t thunk) noexcept
		: object_(target), invoke_(thunk) {}
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
// NOLINTBEGIN(bugprone-macro-parentheses)

// ===========================================================================
// 1. R(Args...)
// ===========================================================================
#define FRF_CV
#define FRF_NOEXCEPT
#define FRF_INVOKE_QUAL
#define FRF_CONSTRAINT(R, F, Args) std::is_invocable_r_v<R, F &, Args...>

#include "function_ref_impl.hpp"

// ===========================================================================
// 2. R(Args...) noexcept
// ===========================================================================
#define FRF_CV
#define FRF_NOEXCEPT noexcept
#define FRF_INVOKE_QUAL
#define FRF_CONSTRAINT(R, F, Args)                                             \
	std::is_nothrow_invocable_r_v<R, F &, Args...>

#include "function_ref_impl.hpp"

// ===========================================================================
// 3. R(Args...) const
// ===========================================================================
#define FRF_CV const
#define FRF_NOEXCEPT
#define FRF_INVOKE_QUAL const
#define FRF_CONSTRAINT(R, F, Args)                                             \
	std::is_invocable_r_v<R, const std::remove_reference_t<F> &, Args...>

#include "function_ref_impl.hpp"

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
// NOLINTEND(bugprone-macro-parentheses)

// ---------------------------------------------------------------------------
// Deduction guide
// ---------------------------------------------------------------------------
#if __cplusplus >= 201'703L
template <class R, class... Args>
function_ref(R (*)(Args...)) -> function_ref<R(Args...)>;
#endif

} // namespace exchange::core::util
#endif