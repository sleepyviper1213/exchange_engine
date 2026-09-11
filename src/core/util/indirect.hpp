#pragma once
// An indirect value: T held behind a pointer, copied when the holder is copied.
//
// C++26's std::indirect, as far as this tree needs it. It exists so a pimpl
// member can be *copyable* - a unique_ptr pimpl cannot be, and every class that
// wanted deep-copy semantics had to write the copy constructor out by hand.
//
// The constructor set follows [indirect.ctor] rather than only the shapes this
// tree happens to call. That is deliberate: a vocabulary type that accepts less
// than the standard one teaches the wrong spelling at every call site, and one
// that accepts *more* - this had an allocator-extended constructor missing its
// `explicit` - compiles code the standard rejects, which is the worse of the
// two. @see the tests in test/core/util/indirect.test.cpp

#include "attributes.hpp"

#include <compare>
#include <initializer_list>
#include <memory>
#include <type_traits>
#include <utility>

namespace exchange::core::util {

/**
 * @brief T stored indirectly, with value semantics.
 *
 * @tparam T The owned type. May be incomplete where the constructor that needs
 *        it is not instantiated, which is what makes this usable as a pimpl
 *        member: the class template can be named in a header and the special
 *        members defined in the .cpp beside the complete type.
 * @tparam Allocator Where the storage comes from.
 *
 * @par Valueless
 * Moving from an @c indirect leaves it *valueless* - it owns nothing, and the
 * only operations defined on it are assignment, destruction and
 * @c valueless_after_move. Dereferencing one is undefined. This is the same
 * contract the standard states and the reason it gives the state a name rather
 * than calling it empty: an @c indirect is never legitimately empty, so a
 * caller that has to ask is a caller that moved.
 */
template <typename T, typename Allocator = std::allocator<T>>
class indirect {
	using traits = std::allocator_traits<Allocator>;

	/// @brief Is @p U this type, or the tag that selects an in-place
	///        constructor? Either would let the converting constructor below
	///        hijack a copy, a move, or an emplace.
	template <typename U>
	static constexpr bool IS_OWN_TAG =
		std::is_same_v<std::remove_cvref_t<U>, indirect> ||
		std::is_same_v<std::remove_cvref_t<U>, std::in_place_t>;

public:
	using value_type     = T;
	using allocator_type = Allocator;
	using pointer        = typename traits::pointer;
	using const_pointer  = typename traits::const_pointer;

	// --- construction ------------------------------------------------------
	//
	// Twelve overloads, in [indirect.ctor]'s order. Each allocator-extended
	// form takes the allocator rather than selecting one, and every one of them
	// is `explicit` - including the tagged ones, where it is easy to assume the
	// tag already prevents an accidental conversion. It does not: without
	// `explicit`, `indirect<T> x = {std::allocator_arg, a, std::in_place, ...}`
	// is a valid copy-initialisation, and the standard rules it out.
	//
	// The Constraints from the standard are `requires` clauses, so a form T
	// cannot satisfy drops out of overload resolution instead of hard-erroring
	// inside create(). That is what makes `std::is_constructible_v<indirect<X>,
	// std::in_place_t, Args...>` answerable rather than a compile error. The
	// Mandates are static_asserts, because a Mandate is meant to be a hard
	// error with a legible message.

	/// @brief An owned T built with no arguments.
	constexpr explicit indirect()
		requires std::is_default_constructible_v<Allocator>
		: indirect(std::in_place) {}

	/// @copydoc indirect()
	constexpr explicit indirect(std::allocator_arg_t /*tag*/,
								const Allocator &alloc)
		: alloc_(alloc), p_(create(alloc_)) {}

	/**
	 * @brief A deep copy: @p other's value is constructed afresh here.
	 *
	 * The allocator comes from @c select_on_container_copy_construction, which
	 * is the container rule - a copy does not inherit the source's allocator
	 * unless the allocator says it should.
	 */
	constexpr indirect(const indirect &other)
		: alloc_(traits::select_on_container_copy_construction(other.alloc_)),
		  p_(other.valueless_after_move() ? nullptr
										  : create(alloc_, *other.p_)) {
		static_assert(std::is_copy_constructible_v<T>,
					  "indirect<T> can only be copied if T can be");
	}

	/// @copydoc indirect(const indirect &)
	/// @note Takes @p alloc as given rather than consulting the allocator's
	///       copy-construction preference: the caller naming one has already
	///       made that choice.
	constexpr indirect(std::allocator_arg_t /*tag*/, const Allocator &alloc,
					   const indirect &other)
		: alloc_(alloc),
		  p_(other.valueless_after_move() ? nullptr
										  : create(alloc_, *other.p_)) {
		static_assert(std::is_copy_constructible_v<T>,
					  "indirect<T> can only be copied if T can be");
	}

	/// @brief Take @p other's value. No allocation, so unconditionally
	///        @c noexcept.
	/// @post @p other is valueless.
	constexpr indirect(indirect &&other) noexcept
		: alloc_(std::move(other.alloc_)),
		  p_(std::exchange(other.p_, nullptr)) {}

	/**
	 * @brief Take @p other's value if @p alloc can free it, otherwise move the
	 *        value into fresh storage of our own.
	 *
	 * @post @p other is valueless either way - including the path that had to
	 *       reallocate, where its now-moved-from object is destroyed through
	 *       *its* allocator rather than left behind.
	 * @note @c noexcept only when the allocator type guarantees any two
	 *       instances are interchangeable, because that is exactly when the
	 *       reallocating branch is unreachable.
	 */
	constexpr indirect(
		std::allocator_arg_t /*tag*/, const Allocator &alloc,
		indirect &&other) noexcept(traits::is_always_equal::value)
		: alloc_(alloc) {
		if (other.valueless_after_move()) return;

		// One condition rather than two arms with the same body: our allocator
		// can free other's storage either because the type guarantees it or
		// because these two instances happen to compare equal.
		const bool can_adopt =
			traits::is_always_equal::value || alloc_ == other.alloc_;
		if (can_adopt) {
			p_ = std::exchange(other.p_, nullptr);
			return;
		}

		// A pointer from another allocator cannot be handed to ours, so the
		// value moves rather than the ownership - and other still ends up
		// valueless, which is the postcondition.
		p_ = create(alloc_, std::move(*other.p_));
		other.reset();
	}

	/// @brief An owned T constructed from @p u.
	/// @note The workhorse spelling - @c indirect<std::string>{"x"} - and the
	///       one whose absence forced every call site to say @c std::in_place.
	template <typename U = T>
		requires (!IS_OWN_TAG<U> && std::is_constructible_v<T, U> &&
				  std::is_default_constructible_v<Allocator>)
	constexpr explicit indirect(U &&u)
		: p_(create(alloc_, std::forward<U>(u))) {}

	/// @copydoc indirect(U &&)
	template <typename U = T>
		requires (!IS_OWN_TAG<U> && std::is_constructible_v<T, U>)
	constexpr explicit indirect(std::allocator_arg_t /*tag*/,
								const Allocator &alloc, U &&u)
		: alloc_(alloc), p_(create(alloc_, std::forward<U>(u))) {}

	/// @brief An owned T constructed in place from @p us.
	template <typename... Us>
		requires (std::is_constructible_v<T, Us...> &&
				  std::is_default_constructible_v<Allocator>)
	constexpr explicit indirect(std::in_place_t /*tag*/, Us &&...us)
		: p_(create(alloc_, std::forward<Us>(us)...)) {}

	/// @copydoc indirect(std::in_place_t, Us &&...)
	template <typename... Us>
		requires (std::is_constructible_v<T, Us...>)
	constexpr explicit indirect(std::allocator_arg_t /*tag*/,
								const Allocator &alloc, std::in_place_t /*tag*/,
								Us &&...us)
		: alloc_(alloc), p_(create(alloc_, std::forward<Us>(us)...)) {}

	/**
	 * @brief An owned T constructed in place from @p ilist and @p us.
	 *
	 * A separate overload rather than a case of the variadic one above, and it
	 * has to be: a braced-init-list is a non-deduced context, so
	 * @c indirect<std::vector<int>>(std::in_place, {1, 2, 3}) cannot bind to
	 * @c Us&&... at all. The standard splits it for the same reason.
	 */
	template <typename I, typename... Us>
		requires (
			std::is_constructible_v<T, std::initializer_list<I> &, Us...> &&
			std::is_default_constructible_v<Allocator>)
	constexpr explicit indirect(std::in_place_t /*tag*/,
								std::initializer_list<I> ilist, Us &&...us)
		: p_(create(alloc_, ilist, std::forward<Us>(us)...)) {}

	/// @copydoc indirect(std::in_place_t, std::initializer_list<I>, Us &&...)
	template <typename I, typename... Us>
		requires (std::is_constructible_v<T, std::initializer_list<I> &, Us...>)
	constexpr explicit indirect(std::allocator_arg_t /*tag*/,
								const Allocator &alloc, std::in_place_t /*tag*/,
								std::initializer_list<I> ilist, Us &&...us)
		: alloc_(alloc), p_(create(alloc_, ilist, std::forward<Us>(us)...)) {}

	constexpr ~indirect() { reset(); }

	// --- assignment --------------------------------------------------------

	constexpr indirect &operator=(const indirect &other) {
		static_assert(std::is_copy_constructible_v<T>,
					  "indirect<T> can only be copied if T can be");
		if (this == &other) return *this;

		if (other.valueless_after_move()) {
			reset();
			return *this;
		}

		if constexpr (traits::propagate_on_container_copy_assignment::value) {
			// The incoming allocator may not be able to free what we hold, so
			// the old value goes before the allocator is replaced - not after,
			// which would deallocate through the wrong one.
			if constexpr (!traits::is_always_equal::value)
				if (alloc_ != other.alloc_) reset();
			alloc_ = other.alloc_;
		}

		// Assign into the existing object where there is one: that is one
		// fewer allocation, and it is what the value semantics promise.
		if (p_ != nullptr) *p_ = *other.p_;
		else p_ = create(alloc_, *other.p_);
		return *this;
	}

	constexpr indirect &operator=(indirect &&other) noexcept(
		traits::propagate_on_container_move_assignment::value ||
		traits::is_always_equal::value) {
		if (this == &other) return *this;

		reset();
		if constexpr (traits::propagate_on_container_move_assignment::value)
			alloc_ = std::move(other.alloc_);
		p_ = std::exchange(other.p_, nullptr);
		return *this;
	}

	// --- observers ---------------------------------------------------------

	/// @brief The owned value.
	///
	/// Four ref-qualified overloads rather than one deduced-this template. That
	/// is a correctness matter: `*std::forward<Self>(self).p_` dereferences a
	/// `T *const` on a const object, which yields `T &` and handed out mutable
	/// access to a const indirect's value.
	/// @pre Not valueless.
	[[nodiscard]] constexpr T &operator*() & noexcept { return *p_; }

	[[nodiscard]] constexpr const T &operator*() const & noexcept {
		return *p_;
	}

	[[nodiscard]] constexpr T &&operator*() && noexcept {
		return std::move(*p_);
	}

	[[nodiscard]] constexpr const T &&operator*() const && noexcept {
		return std::move(*p_);
	}

	/// @pre Not valueless.
	[[nodiscard]] constexpr const_pointer operator->() const noexcept {
		return p_;
	}

	/// @pre Not valueless.
	[[nodiscard]] constexpr pointer operator->() noexcept { return p_; }

	/// @brief Whether this owns nothing because it was moved from.
	[[nodiscard]] constexpr bool valueless_after_move() const noexcept {
		return p_ == nullptr;
	}

	[[nodiscard]] constexpr allocator_type get_allocator() const noexcept {
		return alloc_;
	}

	// --- swap --------------------------------------------------------------

	/// @note Swaps the pointers, so it never allocates and never touches the
	///       owned values - a valueless operand is fine on either side.
	constexpr void
	swap(indirect &other) noexcept(traits::propagate_on_container_swap::value ||
								   traits::is_always_equal::value) {
		using std::swap;
		if constexpr (traits::propagate_on_container_swap::value)
			swap(alloc_, other.alloc_);
		swap(p_, other.p_);
	}

	friend constexpr void
	swap(indirect &lhs, indirect &rhs) noexcept(noexcept(lhs.swap(rhs))) {
		lhs.swap(rhs);
	}

	// --- comparison --------------------------------------------------------

	template <typename U, typename Alloc2>
	friend constexpr bool operator==(
		const indirect &lhs,
		const indirect<U, Alloc2> &rhs) noexcept(noexcept(*lhs == *rhs)) {
		if (lhs.valueless_after_move() || rhs.valueless_after_move())
			return lhs.valueless_after_move() == rhs.valueless_after_move();
		return *lhs == *rhs;
	}

	/// @brief Orders by value, and a valueless operand orders before every
	///        operand that holds one. @see [indirect.relops]
	///
	/// @note The return type is spelled out because the two branches do not
	///       agree: comparing the valueless flags gives @c strong_ordering
	///       while comparing the values gives whatever @c T yields, and a
	///       deduced @c auto rejected any @c T whose ordering was weaker than
	///       strong. The flag comparison converts; the values decide the type.
	template <typename U, typename Alloc2>
	friend constexpr std::compare_three_way_result_t<T, U>
	operator<=>(const indirect &lhs, const indirect<U, Alloc2> &rhs) {
		using ordering = std::compare_three_way_result_t<T, U>;
		if (lhs.valueless_after_move() || rhs.valueless_after_move())
			// Negated, and that is the whole content of the branch: the
			// standard spells it `!lhs.valueless <=> !rhs.valueless`, so a
			// valueless operand orders *before* one holding a value. Comparing
			// the flags directly inverts it, because `true > false`.
			return static_cast<ordering>(!lhs.valueless_after_move() <=>
										 !rhs.valueless_after_move());
		return *lhs <=> *rhs;
	}

private:
	EXCHANGE_NO_UNIQUE_ADDRESS Allocator alloc_{};
	pointer p_ = nullptr;

	constexpr void reset() noexcept {
		if (valueless_after_move()) return;
		traits::destroy(alloc_, p_);
		traits::deallocate(alloc_, p_, 1);
		p_ = nullptr;
	}

	template <typename... Args>
	[[nodiscard]] static constexpr pointer create(Allocator &alloc,
												  Args &&...args) {
		pointer memory = traits::allocate(alloc, 1);
		try {
			traits::construct(alloc, memory, std::forward<Args>(args)...);
			return memory;
		} catch (...) {
			// The storage is ours until construct() succeeds, so a throwing
			// constructor must not leak it.
			traits::deallocate(alloc, memory, 1);
			throw;
		}
	}
};

/// @brief Deduce @c indirect<T> from a value, so @c indirect{x} works.
template <typename T>
indirect(T) -> indirect<T>;

template <typename T, typename... Args>
[[nodiscard]] constexpr indirect<T> make_indirect(Args &&...args) {
	return indirect<T>(std::in_place, std::forward<Args>(args)...);
}

template <typename T, typename Alloc, typename... Args>
[[nodiscard]] constexpr indirect<T, Alloc> allocate_indirect(const Alloc &alloc,
															 Args &&...args) {
	return indirect<T, Alloc>(std::allocator_arg,
							  alloc,
							  std::in_place,
							  std::forward<Args>(args)...);
}

} // namespace exchange::core::util

namespace std {

/// @brief Hashes the owned value. @see [indirect.hash]
///
/// A partial specialisation, so it is opened inside @c std rather than declared
/// with a qualified name - only *explicit* specialisations may be written from
/// outside the namespace.
template <typename T, typename Alloc>
	requires is_default_constructible_v<hash<T>>
struct hash<exchange::core::util::indirect<T, Alloc>> {
	/// @pre The argument is not valueless - the standard leaves hashing one
	///      undefined rather than giving it a value, and so does this.
	[[nodiscard]] size_t
	operator()(const exchange::core::util::indirect<T, Alloc> &value) const
		noexcept(noexcept(hash<T>{}(*value))) {
		return hash<T>{}(*value);
	}
};

} // namespace std
