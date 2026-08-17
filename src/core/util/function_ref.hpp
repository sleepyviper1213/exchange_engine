#pragma once

// A non-owning reference to something callable — the type to reach for when a
// callback crosses a boundary a template cannot.
//
// Two of those boundaries exist in this tree and they pull in opposite
// directions. A shared library's ABI is one: a header template that touches a
// class's internals has to have those internals exported to be instantiable by
// a consumer, which for `order_book` would mean publishing
// `detail::resting_order` and a private accessor — the whole of what `detail/`
// exists to keep in. A latency budget is the other: `std::function` may
// allocate and always costs an indirect call, which TODO.md #12 flags on
// `MatchingEngine::TradeSink`.
//
// This settles both without settling for either. It is two pointers, never
// allocates, and is trivially copyable — so a function taking one can live in a
// .cpp and be exported, while its caller still passes an ordinary lambda.


#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace exchange::core::util {


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
 *          always a bug, and the one place it is not — a member whose lifetime
 * is visibly shorter than the callable's — should say why in a comment.
 *
 * @note Trivially copyable and the width of two pointers, so passing one by
 *       value passes it in registers.
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
	 */
	template <class Func>
		requires (!std::same_as<std::remove_cvref_t<Func>, function_ref>) &&
					 std::invocable<Func &, Args...> &&
					 (std::same_as<R, void> ||
					  std::convertible_to<std::invoke_result_t<Func &, Args...>,
										  R>)
	// NOLINTNEXTLINE(google-explicit-constructor) — implicit is the design
	constexpr function_ref(Func &&func) noexcept
		: object_(const_cast<void *>(
			  static_cast<const void *>(std::addressof(func)))),
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

	/**
	 * @brief Rebinding to a callable is deleted, not merely discouraged.
	 *
	 * From C++26's @c std::function_ref, and the reason is worth keeping: the
	 * converting constructor is implicit, so without this @c ref @c = @c [](){...}
	 * would compile, bind to a temporary, and dangle the instant the statement
	 * ended. Copying one @c function_ref onto another stays fine — that is the
	 * implicit copy assignment, and both then refer to a callable somebody else is
	 * keeping alive.
	 */
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

template <class R, class... Args>
class function_ref<R(Args...) noexcept> {
public:
	template <class F>
		requires (!std::same_as<std::remove_cvref_t<F>, function_ref>) &&
					 std::is_nothrow_invocable_r_v<R, F &, Args...>
	// NOLINTNEXTLINE(google-explicit-constructor) — implicit is the design
	constexpr function_ref(F &&callable) noexcept
		: object_(const_cast<void *>(
			  static_cast<const void *>(std::addressof(callable)))),
		  invoke_([](void *object, Args... args) noexcept -> R {
			  using target = std::remove_reference_t<F>;
			  return std::invoke_r<R>(*static_cast<target *>(object),
									  std::forward<Args>(args)...);
		  }) {}

	/// @brief Call the referenced callable. @pre It is still alive.
	constexpr R operator()(Args... args) const noexcept {
		return invoke_(object_, std::forward<Args>(args)...);
	}

	/**
	 * @brief Rebinding to a callable is deleted, not merely discouraged.
	 *
	 * From C++26's @c std::function_ref, and the reason is worth keeping: the
	 * converting constructor is implicit, so without this @c ref @c = @c [](){...}
	 * would compile, bind to a temporary, and dangle the instant the statement
	 * ended. Copying one @c function_ref onto another stays fine — that is the
	 * implicit copy assignment, and both then refer to a callable somebody else is
	 * keeping alive.
	 */
	template <class Other>
		requires (!std::same_as<std::remove_cvref_t<Other>, function_ref>)
	function_ref &operator=(Other &&) = delete;

private:
	void *object_                          = nullptr;
	R (*invoke_)(void *, Args...) noexcept = nullptr;
};

template <class R, class... Args>
class function_ref<R(Args...) const> {
public:
	template <class F>
		requires (!std::same_as<std::remove_cvref_t<F>, function_ref>) &&
					 std::is_invocable_r_v<
						 R, const std::remove_reference_t<F> &, Args...>
	// NOLINTNEXTLINE(google-explicit-constructor) — implicit is the design
	constexpr function_ref(F &&callable) noexcept
		: object_(std::addressof(callable)),
		  invoke_([](const void *object, Args... args) -> R {
			  using target = const std::remove_reference_t<F>;
			  return std::invoke_r<R>(*static_cast<target *>(object),
									  std::forward<Args>(args)...);
		  }) {}

	/// @brief Call the referenced callable. @pre It is still alive.
	constexpr R operator()(Args... args) const {
		return invoke_(object_, std::forward<Args>(args)...);
	}

	/**
	 * @brief Rebinding to a callable is deleted, not merely discouraged.
	 *
	 * From C++26's @c std::function_ref, and the reason is worth keeping: the
	 * converting constructor is implicit, so without this @c ref @c = @c [](){...}
	 * would compile, bind to a temporary, and dangle the instant the statement
	 * ended. Copying one @c function_ref onto another stays fine — that is the
	 * implicit copy assignment, and both then refer to a callable somebody else is
	 * keeping alive.
	 */
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
 * may do*. Four specialisations for two independent bits is what C++26's
 * @c std::function_ref carries, and for the same reason — @c noexcept is part
 * of a function type, so each combination needs its own thunk pointer type and
 * there is no member to toggle.
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
	// NOLINTNEXTLINE(google-explicit-constructor) — implicit is the design
	constexpr function_ref(F &&callable) noexcept
		: object_(std::addressof(callable)),
		  invoke_([](const void *object, Args... args) noexcept -> R {
			  using target = const std::remove_reference_t<F>;
			  return std::invoke_r<R>(*static_cast<target *>(object),
									  std::forward<Args>(args)...);
		  }) {}

	/// @brief Call the referenced callable. @pre It is still alive.
	constexpr R operator()(Args... args) const noexcept {
		return invoke_(object_, std::forward<Args>(args)...);
	}

	/**
	 * @brief Rebinding to a callable is deleted, not merely discouraged.
	 *
	 * From C++26's @c std::function_ref, and the reason is worth keeping: the
	 * converting constructor is implicit, so without this @c ref @c = @c [](){...}
	 * would compile, bind to a temporary, and dangle the instant the statement
	 * ended. Copying one @c function_ref onto another stays fine — that is the
	 * implicit copy assignment, and both then refer to a callable somebody else is
	 * keeping alive.
	 */
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
