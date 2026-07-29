#pragma once
// X-macro helpers that generate a scoped enum together with a string accessor
// from one list, so the enumerators and their strings can never drift apart.
// The pre-C++26-reflection idiom, kept in one place so every enum spells it the
// same way — replace the generated accessors with std::meta once the toolchain
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
//   EXCHANGE_ENUM_LABEL(parse_error, message, PARSE_ERROR_LIST) // "empty number"
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
// this header nothing — no fmt include, here or in any enum's header — and
// makes the enum inherit the string format specifiers, so `{:>8}` works. It is
// deliberately NOT a fmt::formatter specialisation: providing both for one type
// is disallowed, and format_as is what fmt documents for "formattable as some
// other type with the same specifiers".
// @see https://fmt.dev/12.0/api/#formatting-user-defined-types

#include <concepts>
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
 * is the display form — pair the other with the @c _ONLY variant, since two
 * format_as overloads for one type is a redefinition.
 */
#define EXCHANGE_ENUM_FORMAT_AS(Enum, func)                                    \
	[[nodiscard]] constexpr std::string_view format_as(Enum value) noexcept {  \
		return func(value);                                                    \
	}

/* 
 * @brief Define @p func mapping each enumerator to its own identifier text.
 *	      Accessor only — prefer EXCHANGE_ENUM_NAME, which also makes the enum
 * 		  printable.
 */
#define EXCHANGE_ENUM_NAME_CASE(name, label)                                   \
	case name: return #name;
#define EXCHANGE_ENUM_NAME_ONLY(Enum, func, list)                              \
	[[nodiscard]] constexpr std::string_view func(Enum value) noexcept {        \
		using enum Enum;                                                       \
		switch (value) { list(EXCHANGE_ENUM_NAME_CASE) }                       \
		return {};                                                             \
	}

/// @brief Define @p func mapping each enumerator to its provided label.
///        Accessor only — prefer EXCHANGE_ENUM_LABEL.
#define EXCHANGE_ENUM_LABEL_CASE(name, label)                                  \
	case name: return label;
#define EXCHANGE_ENUM_LABEL_ONLY(Enum, func, list)                             \
	[[nodiscard]] constexpr std::string_view func(Enum value) noexcept {        \
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

namespace exchange::core::util {

/**
 * @brief A scoped enum declared through the helpers above — one whose
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
