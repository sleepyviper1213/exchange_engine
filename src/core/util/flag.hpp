#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace exchange::core::util {

/**
 * @brief Fallback for the opt-in hook. Never defined, never a match for
 *        anything but the ellipsis, and that is the point.
 */
std::false_type exchange_enable_flags(...);

/**
 * @brief A scoped enum whose author opted it in as a set of bits.
 *
 * Scoped is required, not merely preferred: an unscoped enum already converts
 * to its underlying integer on its own, so wrapping one would leave in place
 * the very conversion this type exists to remove.
 */
template <typename E>
concept flag_enum =
	std::is_scoped_enum_v<E> && std::is_unsigned_v<std::underlying_type_t<E>> &&
	requires(E bit) {
		{ exchange_enable_flags(bit) } -> std::same_as<std::true_type>;
	};

/**
 * @brief A set of @p E's bits — a value of a *different* type from @p E itself.
 *
 * @par Why one bit and a set of bits are different types
 * They answer different questions. @p E names one flag; @c flag<E> names a
 * combination, possibly empty. Collapsing them is what makes
 * `if (caps == order_caps::POST_ONLY)` look right when it is in fact asking
 * whether the set holds *exactly* that one flag and nothing else, which is
 * almost never the question. Here the two do not compare at all, and @c test /
 * @c any_of / @c all_of each say which question is being asked.
 *
 * @par The conversions, and there are only two
 * A single @p E converts *in* implicitly, because a one-flag set is
 * unambiguously that flag and requiring `flag{x}` at every call site buys
 * nothing. Everything else is named: @c bits() is the only way to the
 * underlying integer, @c from_bits the only way back, and @c operator bool is
 * explicit, so a flag set never takes part in arithmetic or in an overload
 * resolution it was not meant for.
 *
 * @note Trivially copyable and exactly as wide as @p E's underlying type, so
 *       passing one by value is passing the integer.
 */
template <flag_enum E>
class flag {
public:
	using underlying_type = std::underlying_type_t<E>;

	/// @brief The empty set.
	constexpr flag() noexcept = default;

	/// @brief The set holding exactly @p bit. Implicit on purpose — see the
	///        class note on conversions.
	constexpr flag(E bit) noexcept : bits_(std::to_underlying(bit)) {}

	/**
	 * @brief Reinterpret a raw integer as a set of @p E's bits.
	 *
	 * Named rather than a constructor, and the only door in: an implicit one
	 * would let any integer that happens to be in scope become a flag set,
	 * which is the conversion this type exists to remove. Use it at a boundary
	 * that genuinely carries bits — a wire field, a config word — and nowhere
	 * else.
	 *
	 * @warning Unchecked. Bits @p E names no enumerator for are kept as-is and
	 *          @c bits() hands them back; @c test only ever asks about bits the
	 *          caller named, so they are inert rather than dangerous.
	 */
	[[nodiscard]] static constexpr flag
	from_bits(underlying_type raw) noexcept {
		flag result;
		result.bits_ = raw;
		return result;
	}

	/// @brief The raw bits. The only way out, and deliberately verbose at the
	///        call site.
	[[nodiscard]] constexpr underlying_type bits() const noexcept {
		return bits_;
	}

	/// @brief Whether the set holds anything at all. Explicit, so a flag set
	///        never silently becomes a condition's integer or an overload's
	///        @c bool argument.
	[[nodiscard]] explicit constexpr operator bool() const noexcept {
		return bits_ != underlying_type{};
	}

	/// @brief Whether the set is empty.
	[[nodiscard]] constexpr bool is_empty() const noexcept {
		return bits_ == underlying_type{};
	}

	/// @brief How many bits are set.
	[[nodiscard]] constexpr std::size_t count() const noexcept {
		return static_cast<std::size_t>(std::popcount(unsigned_bits()));
	}

	/// @brief Whether every bit of @p wanted is present. A single enumerator is
	///        the ordinary case; a multi-bit @p wanted asks about all of them.
	[[nodiscard]] constexpr bool test(flag wanted) const noexcept {
		return (bits_ & wanted.bits_) == wanted.bits_;
	}

	/// @brief Whether every bit of @p wanted is present. @see test
	[[nodiscard]] constexpr bool all_of(flag wanted) const noexcept {
		return test(wanted);
	}

	/// @brief Whether at least one bit of @p wanted is present.
	[[nodiscard]] constexpr bool any_of(flag wanted) const noexcept {
		return (bits_ & wanted.bits_) != underlying_type{};
	}

	/// @brief Whether no bit of @p wanted is present.
	[[nodiscard]] constexpr bool none_of(flag wanted) const noexcept {
		return !any_of(wanted);
	}

	/// @brief Add every bit of @p added.
	constexpr flag &set(flag added) noexcept { return *this |= added; }

	/// @brief Remove every bit of @p removed.
	constexpr flag &reset(flag removed) noexcept {
		bits_ = narrow(bits_ & ~removed.bits_);
		return *this;
	}

	/// @brief Toggle every bit of @p toggled.
	constexpr flag &flip(flag toggled) noexcept { return *this ^= toggled; }

	/// @brief Drop every bit @p mask does not name — the safe half of @c ~.
	[[nodiscard]] constexpr flag masked_by(flag mask) const noexcept {
		return *this & mask;
	}

	constexpr flag &operator|=(flag other) noexcept {
		bits_ = narrow(bits_ | other.bits_);
		return *this;
	}

	constexpr flag &operator&=(flag other) noexcept {
		bits_ = narrow(bits_ & other.bits_);
		return *this;
	}

	constexpr flag &operator^=(flag other) noexcept {
		bits_ = narrow(bits_ ^ other.bits_);
		return *this;
	}

	/**
	 * @brief Every bit of the underlying type this set does not hold.
	 *
	 * @warning The complement is over the *storage*, not over the enumerators —
	 *          @p E almost never names all 8, 16 or 32 bits, and the ones it
	 * does not name come back set. That is what makes `~x` a mask rather than
	 *          a set of flags; combine it with @c & (or use @c reset, which
	 * does exactly that) instead of storing it.
	 */
	[[nodiscard]] constexpr flag operator~() const noexcept {
		return from_bits(narrow(~bits_));
	}

	[[nodiscard]] friend constexpr flag operator|(flag lhs, flag rhs) noexcept {
		return lhs |= rhs;
	}

	[[nodiscard]] friend constexpr flag operator&(flag lhs, flag rhs) noexcept {
		return lhs &= rhs;
	}

	[[nodiscard]] friend constexpr flag operator^(flag lhs, flag rhs) noexcept {
		return lhs ^= rhs;
	}

	/// @brief Set equality — same bits, not "overlaps". @see any_of
	[[nodiscard]] constexpr bool
	operator==(const flag &) const noexcept = default;

	/**
	 * @brief Comparing a set to a single flag is deleted, not answered.
	 *
	 * The implicit @p E conversion would otherwise make `caps ==
	 * capability::POST_ONLY` compile and read as "is POST_ONLY set" while
	 * meaning "is POST_ONLY the *only* thing set" — the classic flags bug, and
	 * one that behaves correctly right up until a second flag is added.
	 * Deleting it costs a caller one character: @c test for the first reading,
	 * @c "== flag{x}" for the second.
	 */
	friend bool operator==(flag, E) = delete;
	friend bool operator==(E, flag) = delete;

private:
	/// @brief The bits as an unsigned value, for the operations that require
	/// one.
	[[nodiscard]] constexpr std::make_unsigned_t<underlying_type>
	unsigned_bits() const noexcept {
		return static_cast<std::make_unsigned_t<underlying_type>>(bits_);
	}

	/// @brief Put a promoted result back in the underlying type.
	///
	/// A bitwise operator on anything narrower than @c int promotes both
	/// operands, so the result is an @c int whichever type went in. Without
	/// this the narrowing back would be implicit — exactly the silent integral
	/// conversion the class exists to remove, and one the warning set rejects
	/// besides. Templated on the promoted type rather than pinned to @c int so
	/// a 64-bit underlying type, which does not promote, is not truncated on
	/// the way back.
	template <typename Promoted>
	[[nodiscard]] static constexpr underlying_type
	narrow(Promoted wide) noexcept {
		return static_cast<underlying_type>(wide);
	}

	underlying_type bits_{};
};

} // namespace exchange::core::util

/**
 * @brief Opt @p Enum in as a flag enum, enabling @c flag<Enum> and the bitwise
 *        operators that build one from bare enumerators.
 *
 * Write it **inside the enum's own namespace**, with the unqualified name,
 * right under the enum — that is where a reader looks to find out whether
 * combining two of these is meaningful, and it is what puts the operators
 * somewhere ADL will find them from the call site:
 *
 * @code
 * enum class order_caps : std::uint8_t { POST_ONLY = 1U << 0, HIDDEN = 1U << 1
 * }; EXCHANGE_ENABLE_FLAGS(order_caps)
 * @endcode
 *
 * The hook is an ADL-found function rather than a trait specialisation for
 * exactly that reason: a specialisation would have to be written in
 * @c exchange::core::util, which means closing the enum's namespace, and the
 * operators would then be in the wrong namespace to be found at all.
 *
 * @note @c operator&= and friends live on @c flag itself as hidden friends;
 * only the enumerator-to-enumerator forms need generating, since neither of
 *       their operands is a @c flag yet.
 */
// NOLINTBEGIN(bugprone-macro-parentheses) — Enum is a type, and parenthesising
// a type is not a thing C++ lets you do here.
#define EXCHANGE_ENABLE_FLAGS(Enum)                                            \
	[[maybe_unused]] constexpr std::true_type exchange_enable_flags(           \
		Enum /*unused*/) noexcept {                                            \
		return {};                                                             \
	}                                                                          \
	[[nodiscard]] constexpr ::exchange::core::util::flag<Enum> operator|(      \
		Enum lhs,                                                              \
		Enum rhs) noexcept {                                                   \
		return ::exchange::core::util::flag<Enum>{lhs} |                       \
			   ::exchange::core::util::flag<Enum>{rhs};                        \
	}                                                                          \
	[[nodiscard]] constexpr ::exchange::core::util::flag<Enum> operator&(      \
		Enum lhs,                                                              \
		Enum rhs) noexcept {                                                   \
		return ::exchange::core::util::flag<Enum>{lhs} &                       \
			   ::exchange::core::util::flag<Enum>{rhs};                        \
	}                                                                          \
	[[nodiscard]] constexpr ::exchange::core::util::flag<Enum> operator^(      \
		Enum lhs,                                                              \
		Enum rhs) noexcept {                                                   \
		return ::exchange::core::util::flag<Enum>{lhs} ^                       \
			   ::exchange::core::util::flag<Enum>{rhs};                        \
	}                                                                          \
	[[nodiscard]] constexpr ::exchange::core::util::flag<Enum> operator~(      \
		Enum bit) noexcept {                                                   \
		return ~::exchange::core::util::flag<Enum>{bit};                       \
	}
// NOLINTEND(bugprone-macro-parentheses)
