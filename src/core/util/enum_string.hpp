#pragma once
// X-macro helpers that generate a scoped enum together with a string accessor
// from one list, so the enumerators and their strings can never drift apart.
// The pre-C++26-reflection idiom, kept in one place so every enum spells it the
// same way - replace the generated accessors with std::meta once the toolchain
// provides static reflection.
//
// Describe the enum ONCE as a "list macro" that invokes its argument @c X per
// enumerator as @c X(enumerator, "label"):
//
//   #define PARSE_ERROR_LIST(X)      // (each physical line ends with a
//       X(empty, "empty number")     //  backslash to continue the macro)
//       X(negative_scale, "negative scale")
//
// then expand it inside the enum's namespace:
//
//   enum class parse_error : std::uint8_t {
//       EXCHANGE_ENUM_VALUES(PARSE_ERROR_LIST)
//   };
//   EXCHANGE_ENUM_LABEL(parse_error, message, PARSE_ERROR_LIST) // "empty
//   number"
//
// EXCHANGE_ENUM_NAME maps each enumerator to its own identifier text; use it
// when the string is just the name. EXCHANGE_ENUM_LABEL maps to the supplied
// label; use it for human messages the name cannot express (as parse_error
// does). The "label" field also documents each enumerator inline, so it is
// never wasted even when only the name is used.
//
// Each accessor is constexpr/noexcept and returns a static std::string_view;
// an out-of-range value yields an empty view.
//
// --- printing -------------------------------------------------------------
// Both macros also emit the enum's fmt extension point, so ONE declaration
// gives you the accessor and printing together:
//
//   fmt::print("{}", parse_error::empty);              // "empty number"
//   const std::string s = fmt::to_string(err);         // owned copy
//
// That is the project's uniform enum-to-string conversion: there is no
// per-enum to_string-returning-std::string, and no hand-written formatter.
// fmt::to_string is the only spelling that allocates, so a caller that just
// needs a view keeps using the generated accessor and allocates nothing.
//
// The hook is an ADL-found `format_as` returning std::string_view, which costs
// this header nothing - no fmt include, here or in any enum's header - and
// makes the enum inherit the string format specifiers, so `{:>8}` works. It is
// deliberately NOT a fmt::formatter specialisation: providing both for one type
// is disallowed, and format_as is what fmt documents for "formattable as some
// other type with the same specifiers".
// @see https://fmt.dev/12.0/api/#formatting-user-defined-types

#include <concepts>
#include <optional>
#include <string_view>
#include <type_traits>

/// @brief Expand a list's enumerators as a `name,` sequence for the enum body.
#define EXCHANGE_ENUM_VALUE(name, label) name,

#define EXCHANGE_ENUM_VALUES(list) list(EXCHANGE_ENUM_VALUE)

/**
 * @brief Make @p Enum formattable, rendering as whatever @p func returns.
 *
 * Emitted for you by EXCHANGE_ENUM_NAME / EXCHANGE_ENUM_LABEL. Invoke it
 * directly only when an enum needs both accessors and you must pick which one
 * is the display form - pair the other with the @c _ONLY variant, since two
 * format_as overloads for one type is a redefinition.
 */
#define EXCHANGE_ENUM_FORMAT_AS(Enum, func)                                    \
	[[nodiscard]] constexpr std::string_view format_as(Enum value) noexcept {  \
		return func(value);                                                    \
	}

/*
 * @brief Define @p func mapping each enumerator to its own identifier text.
 *	      Accessor only - prefer EXCHANGE_ENUM_NAME, which also makes the enum
 * 		  printable.
 */
#define EXCHANGE_ENUM_NAME_CASE(name, label)                                   \
	case name: return #name;

#define EXCHANGE_ENUM_NAME_ONLY(Enum, func, list)                              \
	[[nodiscard]] constexpr std::string_view func(Enum value) noexcept {       \
		using enum Enum;                                                       \
		switch (value) { list(EXCHANGE_ENUM_NAME_CASE) }                       \
		return {};                                                             \
	}

/// @brief Define @p func mapping each enumerator to its provided label.
///        Accessor only - prefer EXCHANGE_ENUM_LABEL.
#define EXCHANGE_ENUM_LABEL_CASE(name, label)                                  \
	case name: return (label);

#define EXCHANGE_ENUM_LABEL_ONLY(Enum, func, list)                             \
	[[nodiscard]] constexpr std::string_view func(Enum value) noexcept {       \
		using enum Enum;                                                       \
		switch (value) { list(EXCHANGE_ENUM_LABEL_CASE) }                      \
		return {};                                                             \
	}

/// @brief Identifier-text accessor @p func, plus the fmt hook that prints
///        @p Enum as that text.
#define EXCHANGE_ENUM_NAME(Enum, func, list)                                   \
	EXCHANGE_ENUM_NAME_ONLY(Enum, func, list)                                  \
	EXCHANGE_ENUM_FORMAT_AS(Enum, func)

/// @brief Label accessor @p func, plus the fmt hook that prints @p Enum as that
///        label.
#define EXCHANGE_ENUM_LABEL(Enum, func, list)                                  \
	EXCHANGE_ENUM_LABEL_ONLY(Enum, func, list)                                 \
	EXCHANGE_ENUM_FORMAT_AS(Enum, func)

// --- parsing ---------------------------------------------------------------
// The result is std::optional, not a std::unreachable() and not a throw:
// unfamiliar text is an input a caller can legitimately receive, not an
// invariant the code guarantees, and the only thing a richer error type could
// carry back is the input string the caller already holds. A caller that has
// validated the text upstream still spells that assumption at its own call site
// (`*from_string(s)`), where it is visible, instead of inheriting it from here.
//
// @warning Unlike the switch-based accessors, duplicate text is not a compile
//          error here - the entry listed first wins.

/// @brief Define @p func mapping identifier text back to its enumerator, or to
///        std::nullopt when no enumerator spells itself that way.
#define EXCHANGE_ENUM_FROM_NAME_CASE(name, label)                              \
	if (text == #name) return enum_type::name;

#define EXCHANGE_ENUM_FROM_NAME(Enum, func, list)                              \
	[[nodiscard]] constexpr std::optional<Enum> func(                          \
		std::string_view text) noexcept {                                      \
		using enum_type = Enum;                                                \
		list(EXCHANGE_ENUM_FROM_NAME_CASE) return std::nullopt;                \
	}

/// @brief Define @p func mapping a label back to its enumerator, or to
///        std::nullopt when no enumerator carries that label.
#define EXCHANGE_ENUM_FROM_LABEL_CASE(name, label)                             \
	if (text == (label)) return enum_type::name;

// Workaround for no noexcept guarantee of LLVM libc++
// string_view::string_view(const char*) constructor
#define EXCHANGE_ENUM_FROM_LABEL(Enum, func, list)                             \
	[[nodiscard]] constexpr std::optional<Enum> func(                          \
		std::string_view text) noexcept {                                      \
		using enum_type = Enum;                                                \
		list(EXCHANGE_ENUM_FROM_LABEL_CASE) return std::nullopt;               \
	}                                                                          \
	[[nodiscard]] constexpr std::optional<Enum> func(                          \
		const char *text) noexcept {                                           \
		return text ? func(std::string_view{                                   \
						  text,                                                \
						  std::char_traits<char>::length(text)})               \
					: std::nullopt;                                            \
	}

// --- enumerators with values the author chooses ----------------------------
//
// Everything above numbers the enumerators 0, 1, 2 - which is what you want
// right up until the values *mean* something. A flag enum needs 1, 2, 4, 8; a
// wire protocol needs the codes the protocol assigns; a status enum may need to
// leave gaps where retired members were. Writing the enum body by hand for
// those cases is what the X-macro exists to prevent: the enumerators and their
// strings drift apart the first time somebody adds one to the body and not to
// the list.
//
// So there is a parallel family taking a THREE-argument list -
// @c X(enumerator, value, "label") - with the same three products:
//
//   #define BREACH_LIST(X)
//       X(NONE,       0,       "no rule was broken")
//       X(HALTED,     1U << 0, "the circuit breaker is open")
//       X(PRICE_BAND, 1U << 1, "price is outside the band")
//
//   enum class breach : std::uint32_t {
//       EXCHANGE_ENUM_VALUED_VALUES(BREACH_LIST)
//   };
//   EXCHANGE_ENUM_VALUED_NAME(breach, to_string, BREACH_LIST)
//   EXCHANGE_ENUM_VALUED_LABEL_ONLY(breach, describe, BREACH_LIST)
//
// Pick the family by whether the values carry meaning, not by taste: the
// two-argument form stays the default because a list that does not care what
// the numbers are should not have to state them, and every stated number is one
// more thing that can be stated wrongly.
//
// @warning The accessors are switches, so two enumerators sharing one value
//          will not compile - an alias belongs outside the list, declared in
//          the enum body after the macro expands.

/// @brief Expand a valued list as a `name = value,` sequence for the enum body.
#define EXCHANGE_ENUM_VALUED_VALUE(name, value, label) name = (value),

#define EXCHANGE_ENUM_VALUED_VALUES(list) list(EXCHANGE_ENUM_VALUED_VALUE)

/// @brief Identifier-text accessor over a valued list. Accessor only - prefer
///        EXCHANGE_ENUM_VALUED_NAME, which also makes the enum printable.
#define EXCHANGE_ENUM_VALUED_NAME_CASE(name, value, label)                     \
	case name: return #name;

#define EXCHANGE_ENUM_VALUED_NAME_ONLY(Enum, func, list)                       \
	[[nodiscard]] constexpr std::string_view func(Enum value) noexcept {       \
		using enum Enum;                                                       \
		switch (value) { list(EXCHANGE_ENUM_VALUED_NAME_CASE) }                \
		return {};                                                             \
	}

/// @brief Label accessor over a valued list. Accessor only - prefer
///        EXCHANGE_ENUM_VALUED_LABEL.
#define EXCHANGE_ENUM_VALUED_LABEL_CASE(name, value, label)                    \
	case name: return (label);

#define EXCHANGE_ENUM_VALUED_LABEL_ONLY(Enum, func, list)                      \
	[[nodiscard]] constexpr std::string_view func(Enum value) noexcept {       \
		using enum Enum;                                                       \
		switch (value) { list(EXCHANGE_ENUM_VALUED_LABEL_CASE) }               \
		return {};                                                             \
	}

/// @brief Identifier-text accessor over a valued list, plus the fmt hook.
#define EXCHANGE_ENUM_VALUED_NAME(Enum, func, list)                            \
	EXCHANGE_ENUM_VALUED_NAME_ONLY(Enum, func, list)                           \
	EXCHANGE_ENUM_FORMAT_AS(Enum, func)

/// @brief Label accessor over a valued list, plus the fmt hook.
#define EXCHANGE_ENUM_VALUED_LABEL(Enum, func, list)                           \
	EXCHANGE_ENUM_VALUED_LABEL_ONLY(Enum, func, list)                          \
	EXCHANGE_ENUM_FORMAT_AS(Enum, func)

/// @brief Identifier-text parser over a valued list. The inverse of
///        EXCHANGE_ENUM_VALUED_NAME.
#define EXCHANGE_ENUM_VALUED_FROM_NAME_CASE(name, value, label)                \
	if (text == #name) return enum_type::name;

#define EXCHANGE_ENUM_VALUED_FROM_NAME(Enum, func, list)                       \
	[[nodiscard]] constexpr std::optional<Enum> func(                          \
		std::string_view text) noexcept {                                      \
		using enum_type = Enum;                                                \
		list(EXCHANGE_ENUM_VALUED_FROM_NAME_CASE) return std::nullopt;         \
	}

/// @brief Label parser over a valued list. The inverse of
///        EXCHANGE_ENUM_VALUED_LABEL.
#define EXCHANGE_ENUM_VALUED_FROM_LABEL_CASE(name, value, label)               \
	if (text == (label)) return enum_type::name;

#define EXCHANGE_ENUM_VALUED_FROM_LABEL(Enum, func, list)                      \
	[[nodiscard]] constexpr std::optional<Enum> func(                          \
		std::string_view text) noexcept {                                      \
		using enum_type = Enum;                                                \
		list(EXCHANGE_ENUM_VALUED_FROM_LABEL_CASE) return std::nullopt;        \
	}

/**
 * @brief Invoke @p macro once per enumerator of a valued list.
 *
 * The escape hatch for a product these macros do not generate - a lookup table,
 * a bit-index mapping, a registry - kept here so such a thing is still driven
 * by the one list rather than by a second one written beside it. @p macro is
 * invoked as @c macro(name, value, label).
 */
#define EXCHANGE_ENUM_VALUED_FOR_EACH(list, macro) list(macro)

namespace exchange::core::util {

/**
 * @brief A scoped enum declared through the helpers above - one whose
 *        @c format_as hook is visible, so it prints and converts uniformly.
 *
 * Use it to constrain generic code that means "any of our enums" rather than
 * "any enum": an enum without the hook fails to match here, at the call, with a
 * readable message, instead of deep inside fmt's argument machinery.
 *
 * @note The requirement is spelled unqualified so it is found by ADL in the
 *       enum's own namespace, which is where the macro puts it.
 */
template <typename E>
concept formattable_enum = std::is_enum_v<E> && requires(E value) {
	{ format_as(value) } -> std::same_as<std::string_view>;
};

} // namespace exchange::core::util
