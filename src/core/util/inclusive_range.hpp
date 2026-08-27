#pragma once

#include <concepts>

namespace exchange::core::util {

/**
 * @brief A closed interval @c [first, last] over an integral type.
 *
 * Both ends are inclusive, so a range of one number has @c first() ==
 * @c last(). The type carries no domain meaning of its own - it answers
 * questions about where a value sits relative to the interval and nothing else,
 * which is what lets the same template serve a market_data sequence span and
 * anything else that needs a bounded run of integers.
 *
 * @warning A range is @b not required to be ordered, and the constructor does
 *          not check. @c first() > @c last() is representable on purpose: it is
 *          what a malformed venue frame looks like, and a consumer has to be
 *          able to hold one long enough to ask @c is_ordered() and reject it -
 *          see the @c observe({9, 4}) case in depth_sequencer.test.cpp. An
 *          assertion here would delete that path, and because
 *          @c enable_hardening keeps assertions live in optimised builds it
 *          would turn untrusted input into a process abort. Validating input is
 *          the caller's job, and a caller refuses by returning a reason rather
 *          than by dying.
 */
template <std::integral T>
class inclusive_range {
public:
	constexpr inclusive_range() noexcept = default;

	constexpr inclusive_range(T first, T last) noexcept
		: first_(first), last_(last) {}

	/// @brief First value covered (inclusive).
	[[nodiscard]] constexpr T first() const noexcept { return first_; }

	/// @brief Last value covered (inclusive).
	[[nodiscard]] constexpr T last() const noexcept { return last_; }

	/// @brief Whether @p value falls inside this range.
	[[nodiscard]] constexpr bool covers(T value) const noexcept {
		return first_ <= value && value <= last_;
	}

	/// @brief Whether the range is well-formed (@c first <= @c last). An
	///        unordered range covers nothing, so a caller that skips this check
	///        cannot conclude anything from the others.
	[[nodiscard]] constexpr bool is_ordered() const noexcept {
		return first_ <= last_;
	}

	/// @brief Whether the range covers exactly one value.
	///
	/// The degenerate case: a market_data snapshot is always this, and so is
	/// any diff frame that coalesced nothing. Worth asking rather than
	/// comparing the bounds at the call site, because @c "1..1" is a range
	/// notation nobody wants to read.
	[[nodiscard]] constexpr bool is_identity() const noexcept {
		return first_ == last_;
	}

	/// @brief Whether this range is wholly below @p value - everything it
	///        carries is already behind the caller.
	[[nodiscard]] constexpr bool ends_before(T value) const noexcept {
		return last_ < value;
	}

	/// @brief Whether this range starts past @p value, leaving a hole between
	///        them.
	[[nodiscard]] constexpr bool begins_after(T value) const noexcept {
		return first_ > value;
	}

	/// @brief Whether this range reaches back below @p value - it re-covers
	///        ground the caller has already passed.
	[[nodiscard]] constexpr bool begins_before(T value) const noexcept {
		return first_ < value;
	}

	/// @brief The highest value this range covers - the watermark a consumer
	///        advances to once it has taken the range.
	[[nodiscard]] constexpr T last_covered() const noexcept { return last_; }

	[[nodiscard]] constexpr bool
	operator==(const inclusive_range &) const = default;

private:
	T first_ = 0; ///< First value covered (inclusive).
	T last_  = 0; ///< Last value covered (inclusive).
};

} // namespace exchange::core::util
