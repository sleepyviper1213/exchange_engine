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
//   EXCHANGE_ENUM_LABEL(parse_error, message,   PARSE_ERROR_LIST) // -> "empty number"
//   EXCHANGE_ENUM_NAME (parse_error, to_string, PARSE_ERROR_LIST) // -> "empty"
//
// EXCHANGE_ENUM_NAME maps each enumerator to its own identifier text; use it
// when the string is just the name. EXCHANGE_ENUM_LABEL maps to the supplied
// label; use it for human messages the name cannot express (as parse_error
// does). The "label" field also documents each enumerator inline, so it is
// never wasted even when only the name is used.
//
// Each accessor is constexpr/noexcept and returns a static std::string_view;
// an out-of-range value yields an empty view.
#include <string_view>

/// @brief Expand a list's enumerators as a `name,` sequence for the enum body.
#define EXCHANGE_ENUM_VALUE(name, label) name,
#define EXCHANGE_ENUM_VALUES(list) list(EXCHANGE_ENUM_VALUE)

/// @brief Define @p func mapping each enumerator to its own identifier text.
#define EXCHANGE_ENUM_NAME_CASE(name, label)                                   \
	case name: return #name;
#define EXCHANGE_ENUM_NAME(Enum, func, list)                                   \
	[[nodiscard]] constexpr std::string_view func(Enum value) noexcept {       \
		using enum Enum;                                                        \
		switch (value) { list(EXCHANGE_ENUM_NAME_CASE) }                        \
		return {};                                                             \
	}

/// @brief Define @p func mapping each enumerator to its provided label.
#define EXCHANGE_ENUM_LABEL_CASE(name, label)                                  \
	case name: return label;
#define EXCHANGE_ENUM_LABEL(Enum, func, list)                                  \
	[[nodiscard]] constexpr std::string_view func(Enum value) noexcept {       \
		using enum Enum;                                                        \
		switch (value) { list(EXCHANGE_ENUM_LABEL_CASE) }                       \
		return {};                                                             \
	}