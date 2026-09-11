#pragma once

// A non-owning reference to something callable - the type to reach for when a
// callback crosses a boundary a template cannot.
//
// Two of those boundaries exist in this tree and they pull in opposite
// directions. A shared library's ABI is one: a header template that touches a
// class's internals has to have those internals exported to be instantiable by
// a consumer, which for `order_book` would mean publishing
// `detail::resting_order` and a private accessor - the whole of what `detail/`
// exists to keep in. A latency budget is the other: `std::move_only_function`
// may allocate and always costs an indirect call, which TODO.md #12 flags on
// `MatchingEngine::TradeSink`.
//
// This settles both without settling for either. It is two pointers, never
// allocates, and is trivially copyable - so a function taking one can live in a
// .cpp and be exported, while its caller still passes an ordinary lambda.


#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

namespace exchange::core::util {
namespace detail {

/**
 * @brief @c std::addressof without @c <memory>.
 */
template <class T>
[[nodiscard]] constexpr T *address_of(T &value) noexcept {
	return __builtin_addressof(value);
}

/**
 * @brief Erase a callable's constness on the way into the stored @c void*.
 *
 * The two non-@c const specialisations keep one pointer for both a mutable and
 * a @c const target, so the qualifier cannot live in the member's type - it
 * lives in the thunk's, which casts back to @c std::remove_reference_t<F> and
 * so recovers @c const @c F whenever @c F deduced from a @c const lvalue. The
 * round-trip restores exactly the qualifier it erased and nothing is ever
 * written through a pointer that lost one, which is what separates this from
 * the const-stripping the guideline is aimed at.
 */
template <class T>
[[nodiscard]] constexpr void *erase_const(T *pointer) noexcept {
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
	return const_cast<void *>(static_cast<const void *>(pointer));
}

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
 * In practice that is free, because the shape this is *for* is an argument
 * passed to a function that calls it and returns:
 *
 * @code
 * book.for_each_resting([&](const resting_view &order) { seen.push(order); });
 * @endcode
 *
 * The lambda is a temporary living to the end of the full expression, which
 * outlasts the call. What is not safe is storing one:
 *
 * @code
 * function_ref<void(int)> kept = [](int) {};  // dangles immediately
 * @endcode
 *
 * @warning So: pass them, do not keep them. A member of this type is almost
 *          always a bug, and the one place it is not - a member whose lifetime
 * is visibly shorter than the callable's - should say why in a comment.
 *
 * @note Trivially copyable and the width of two pointers, so passing one by
 *       value passes it in registers.
 *
 * @par Which of the four to name
 * @c const and @c noexcept are independent parts of a function *type*, so each
 * combination is its own specialisation - there is no member to toggle, because
 * the thunk's own type changes with them. These are the four, and C++26's
 * @c std::function_ref carries exactly the same set:
 *
 * <table>
 * <tr><th>signature<th>target invoked as<th>may throw
 * <tr><td>@c R(Args...)               <td>@c F&       <td>yes
 * <tr><td>@c R(Args...) noexcept      <td>@c F&       <td>no
 * <tr><td>@c R(Args...) const         <td>@c const @c F& <td>yes
 * <tr><td>@c R(Args...) const noexcept<td>@c const @c F& <td>no
 * </table>
 *
 * Pick by what the *callee* is entitled to assume, not by what the callable at
 * one call site happens to be. @c const says answering must not mutate the
 * target, which is what makes a query a query; @c noexcept is checked against
 * @c is_nothrow_invocable_r_v, so it turns "this callback must not throw" from
 * a comment into a compile error at the binding. Reach for the strict corner
 * unless something needs the licence, and prefer the alias when one exists.
 * @see predicate_ref.hpp
 *
 * @par Rebinding is deleted in all four, and that is not mere discipline
 * From C++26's @c std::function_ref, and the reason is worth keeping: the
 * converting constructor is implicit, so without the deleted assignment
 * @c ref @c = @c [](){...} would compile, bind to a temporary, and dangle the
 * instant the statement ended. Copying one @c function_ref onto another stays
 * fine - that is the implicit copy assignment, and both then refer to a
 * callable somebody else is keeping alive.
 */
template <class...>
class function_ref;

template <class R, class... Args>
class function_ref<R(Args...)> {
public:
	constexpr function_ref() noexcept = delete;

	/**
	 * @brief Bind to @p callable, which must outlive this reference.
	 *
	 * Implicit on purpose: the whole ergonomic point is that a call site passes
	 * a lambda and never names this type. The constraint excludes @c
	 * function_ref itself so that copying one goes through the copy constructor
	 * rather than wrapping a reference in a reference.
	 *
	 * @note The thunk calls @c std::invoke_r, which carries its own
	 *       @c is_invocable_r_v constraint - but that is no substitute for this
	 *       one, in all four specialisations. It sits inside the lambda body
	 *       rather than in the immediate context of this template, so a bad
	 *       target is a hard error several frames deep instead of a
	 *       non-participating candidate; and being a *constraint*, an
	 *       unconstrained constructor would report it only after already
	 *       winning overload resolution and making @c function_ref
	 *       implicitly convertible-from-anything to every trait that asks.
	 *       The @c noexcept specialisations need it for a second reason: @c
	 *       invoke_r checks invocability, never nothrow-invocability, so
	 *       dropping the constraint there trades a compile error for a
	 *       @c std::terminate.
	 *
	 * @note @c Func @c && is here to deduce the target's value category and
	 *       constness, not to forward it: only the address is taken, which is
	 *       why the constraint above is spelled on @c Func @c & rather than on
	 *       @c Func, and why C++26's @c std::function_ref binds the same way.
	 *       A @c std::forward would yield an xvalue with nothing to move into,
	 *       so @c cppcoreguidelines-missing-std-forward is silenced rather
	 *       than satisfied - in all four specialisations.
	 */
	template <class Func>
		requires (!std::same_as<std::remove_cvref_t<Func>, function_ref>) &&
					 std::is_invocable_r_v<R, Func &, Args...>
	// Implicit is the design, and Func && only deduces - @see the notes.
	// NOLINTNEXTLINE(google-explicit-constructor,cppcoreguidelines-missing-std-forward)
	constexpr function_ref(Func &&func) noexcept
		: object_(detail::erase_const(detail::address_of(func))),
		  invoke_([](void *object, Args... args) -> R {
			  using target = std::remove_reference_t<Func>;
			  return std::invoke_r<R>(*static_cast<target *>(object),
									  std::forward<Args>(args)...);
		  }) {}

	// Copy and move stay implicit, and that is load-bearing rather than an
	// oversight: a function_ref is two pointers meant to be passed *by value*,
	// so deleting the copy constructor would make every signature that takes
	// one uncallable and would cost it the trivial copyability asserted below.
	/// @brief Call the referenced callable.
	/// @pre It is still alive. @see the class warning.
	constexpr R operator()(Args... args) const {
		return invoke_(object_, std::forward<Args>(args)...);
	}

	/// @brief Rebinding to a callable is deleted, not merely
	///        discouraged. @see the class note on why.
	template <class Other>
		requires (!std::same_as<std::remove_cvref_t<Other>, function_ref>)
	function_ref &operator=(Other &&) = delete;

private:
	/// The callable, type-erased. Const is cast away on the way in and restored
	/// by the thunk, which knows the type it erased: a @c const F is invoked
	/// through a @c const target because that is what @c F deduced to.
	void *object_                 = nullptr;
	R (*invoke_)(void *, Args...) = nullptr;
};

/**
 * @brief A borrowed callable that promises not to throw.
 *
 * The constraint is @c is_nothrow_invocable_r_v, not merely invocable, so a
 * callable that has not said @c noexcept does not bind. That refusal is the
 * point: it is the caller stating the promise in the one place that knows
 * whether it holds. @see the primary template for the other three.
 */
template <class R, class... Args>
class function_ref<R(Args...) noexcept> {
public:
	template <class F>
		requires (!std::same_as<std::remove_cvref_t<F>, function_ref>) &&
					 std::is_nothrow_invocable_r_v<R, F &, Args...>
	// Implicit is the design, and F && only deduces - @see the notes.
	// NOLINTNEXTLINE(google-explicit-constructor,cppcoreguidelines-missing-std-forward)
	constexpr function_ref(F &&callable) noexcept
		: object_(detail::erase_const(detail::address_of(callable))),
		  invoke_([](void *object, Args... args) noexcept -> R {
			  using target = std::remove_reference_t<F>;
			  return std::invoke_r<R>(*static_cast<target *>(object),
									  std::forward<Args>(args)...);
		  }) {}

	/// @brief Call the referenced callable. @pre It is still alive.
	constexpr R operator()(Args... args) const noexcept {
		return invoke_(object_, std::forward<Args>(args)...);
	}

	/// @brief Rebinding to a callable is deleted, not merely
	///        discouraged. @see the class note on why.
	template <class Other>
		requires (!std::same_as<std::remove_cvref_t<Other>, function_ref>)
	function_ref &operator=(Other &&) = delete;

private:
	void *object_                          = nullptr;
	R (*invoke_)(void *, Args...) noexcept = nullptr;
};

/**
 * @brief A borrowed callable invoked through a @c const reference.
 *
 * So answering cannot mutate the target - which is what a caller asking a
 * *question* is entitled to assume - and the pointer is stored @c const rather
 * than cast to @c void* and back. A callable whose only @c operator() is
 * non-const, a mutable lambda among them, does not bind here and wants the
 * unqualified specialisation instead.
 */
template <class R, class... Args>
class function_ref<R(Args...) const> {
public:
	template <class F>
		requires (!std::same_as<std::remove_cvref_t<F>, function_ref>) &&
					 std::is_invocable_r_v<
						 R, const std::remove_reference_t<F> &, Args...>
	// Implicit is the design, and F && only deduces - @see the notes.
	// NOLINTNEXTLINE(google-explicit-constructor,cppcoreguidelines-missing-std-forward)
	constexpr function_ref(F &&callable) noexcept
		: object_(detail::address_of(callable)),
		  invoke_([](const void *object, Args... args) -> R {
			  using target = const std::remove_reference_t<F>;
			  return std::invoke_r<R>(*static_cast<target *>(object),
									  std::forward<Args>(args)...);
		  }) {}

	/// @brief Call the referenced callable. @pre It is still alive.
	constexpr R operator()(Args... args) const {
		return invoke_(object_, std::forward<Args>(args)...);
	}

	/// @brief Rebinding to a callable is deleted, not merely
	///        discouraged. @see the class note on why.
	template <class Other>
		requires (!std::same_as<std::remove_cvref_t<Other>, function_ref>)
	function_ref &operator=(Other &&) = delete;

private:
	const void *object_;
	R (*invoke_)(const void *, Args...);
};

/**
 * @brief A borrowed callable invoked through a @c const reference, and
 * promising not to throw.
 *
 * The two qualifiers are independent and both mean what they mean above: @c
 * const constrains *how* the target is invoked, @c noexcept constrains *what it
 * may do*. @see the primary template's table for choosing between the four.
 *
 * @par The constraint is the point
 * Nothrow-invocable, not merely invocable. @c spsc_queue::consume_all documents
 * that its callback runs before the read cursor is published, so a throw there
 * is unrecoverable by construction; checking it here turns that from a promise
 * made on the callable's behalf into a compile error at the call site.
 */
template <class R, class... Args>
class function_ref<R(Args...) const noexcept> {
public:
	template <class F>
		requires (!std::same_as<std::remove_cvref_t<F>, function_ref>) &&
					 std::is_nothrow_invocable_r_v<
						 R, const std::remove_reference_t<F> &, Args...>
	// Implicit is the design, and F && only deduces - @see the notes.
	// NOLINTNEXTLINE(google-explicit-constructor,cppcoreguidelines-missing-std-forward)
	constexpr function_ref(F &&callable) noexcept
		: object_(detail::address_of(callable)),
		  invoke_([](const void *object, Args... args) noexcept -> R {
			  using target = const std::remove_reference_t<F>;
			  return std::invoke_r<R>(*static_cast<target *>(object),
									  std::forward<Args>(args)...);
		  }) {}

	/// @brief Call the referenced callable. @pre It is still alive.
	constexpr R operator()(Args... args) const noexcept {
		return invoke_(object_, std::forward<Args>(args)...);
	}

	/// @brief Rebinding to a callable is deleted, not merely
	///        discouraged. @see the class note on why.
	template <class Other>
		requires (!std::same_as<std::remove_cvref_t<Other>, function_ref>)
	function_ref &operator=(Other &&) = delete;

private:
	const void *object_;
	R (*invoke_)(const void *, Args...) noexcept;
};

static_assert(std::is_trivially_copyable_v<function_ref<void()>>);
static_assert(std::is_trivially_copyable_v<function_ref<void() noexcept>>);
static_assert(std::is_trivially_copyable_v<function_ref<void() const>>);
static_assert(
	std::is_trivially_copyable_v<function_ref<void() const noexcept>>);
static_assert(
	sizeof(function_ref<void()>) == 2 * sizeof(void *),
	"a function_ref is a pointer to the callable and a pointer to the "
	"thunk, and nothing else");
static_assert(sizeof(function_ref<void() noexcept>) ==
				  sizeof(function_ref<void()>),
			  "the noexcept variant differs in the thunk's type, not its size");

} // namespace exchange::core::util
