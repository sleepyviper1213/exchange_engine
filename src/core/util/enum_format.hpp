#pragma once
// How the project's scoped enums turn into text and back, on magic_enum.
//
// magic_enum reads an enumerator's identifier out of the compiler's own
// function signature, so the enum body is the only list there is - which is
// the guarantee the X-macro lists this replaced existed to give. What is left
// for an enum to decide is which text it reads as, and there are three cases:
//
// 1. Its identifier. Nothing to write. It prints through the hook below,
//    parses with magic_enum::enum_cast and reads with magic_enum::enum_name.
//
// 2. A phrase instead of the identifier - an error category's message, a
//    venue's spelling such as "100ms". Specialise
//    magic_enum::customize::enum_name beside the enum:
//
//      template <>
//      constexpr magic_enum::customize::customize_t
//      magic_enum::customize::enum_name(ns::depth_error value) noexcept {
//          switch (value) {
//              using enum ns::depth_error;
//          case invalid_json: return "invalid JSON";
//          case bad_number: return "invalid number";
//          }
//          return default_tag;
//      }
//
//    The phrase then *is* the name: it is what prints, what enum_name returns
//    and what enum_cast accepts, so a venue spelling round-trips with no second
//    table, and the identifier is no longer accepted as input.
//
// 3. A description beside the name (reject_reason, breach): the name prints,
//    and a describe() written as the same kind of switch holds the prose.
//
// Neither switch has a `default`, and that is what keeps text and enumerators
// from drifting: an enumerator added without its text is -Wswitch under GCC
// and Clang and C4062 under MSVC, both errors under the project's warning set.
//
// A flag enum - one bit per enumerator, such as breach - also sets
// customize::enum_range<E>::is_flags, so reflection probes the type's bits
// rather than [-128, 127]. Its zero enumerator is then not reflected: it has
// an empty name and is absent from magic_enum::enum_values.

#include <magic_enum/magic_enum.hpp>

#include <concepts>
#include <string_view>
#include <type_traits>

namespace exchange::core::util {

/**
 * @brief The fmt hook every project enum prints through: its magic_enum name.
 *
 * fmt finds @c format_as by argument-dependent lookup alone, so an enum's
 * namespace reaches this with `using core::util::format_as;` - one line per
 * namespace instead of one function per enum. The enum then inherits the
 * string format specifiers, so `{:>8}` works.
 *
 * @par Why not magic_enum_format.hpp
 * Its @c fmt::formatter is a partial specialisation matching *every* enum, so
 * it is ambiguous against any enum that already has a @c format_as - fmt's own
 * for @c std::byte, and @c transport::rest::method's. It also emits nothing
 * unless @c <fmt/format.h> was included before it, which makes whether an enum
 * prints depend on include order. A @c format_as is opt-in per namespace,
 * keeps fmt out of every enum's header, and loses overload resolution to a
 * hand-written one rather than colliding with it.
 *
 * @return An empty view for a value with no enumerator, never an integer.
 */
template <typename E>
	requires std::is_scoped_enum_v<E>
[[nodiscard]] constexpr std::string_view format_as(E value) noexcept {
	return magic_enum::enum_name(value);
}

namespace detail {

// Unqualified lookup stops at this declaration, so the call in the concept
// below finds a format_as by ADL or not at all. Without it the generic hook
// above would be found by ordinary lookup and match every enum, hooked or not.
void format_as() = delete;

template <typename E>
concept has_format_as = requires(E value) {
	{ format_as(value) } -> std::same_as<std::string_view>;
};

} // namespace detail

/**
 * @brief A scoped enum whose namespace provides a @c format_as, so it prints
 *        and converts uniformly.
 *
 * Use it to constrain generic code that means "any of our enums" rather than
 * "any enum": an enum without the hook fails to match here, at the call, with a
 * readable message, instead of deep inside fmt's argument machinery.
 */
template <typename E>
concept formattable_enum = std::is_enum_v<E> && detail::has_format_as<E>;

} // namespace exchange::core::util
