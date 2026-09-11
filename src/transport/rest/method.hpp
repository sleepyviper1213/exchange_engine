#pragma once
// The HTTP verbs this module sends, and the one rule read off them.
//
// Header-only: four names and a classification, all resolvable at compile time.
// The mapping onto Beast's own `verb` is deliberately *not* here - it lives in
// detail/wire.hpp, so a translation unit that only builds requests never
// includes Beast.

#include <cstdint>
#include <string_view>

namespace exchange::transport::rest {

/**
 * @brief The HTTP methods this module can send.
 *
 * @note @c del rather than @c delete, which is a keyword. Beast spells the same
 *       problem @c verb::delete_; the trailing underscore is this project's
 *       private-member convention, so the abbreviation is used instead.
 */
enum class method : std::uint8_t { get, post, put, del };

/// @brief @p m as it appears on the request line.
/// @note @c constexpr and defined here rather than exported from a source
///       file - it is a four-entry table, and a caller formatting a method
///       should not need the library to be linked to do it.
[[nodiscard]] constexpr std::string_view to_string(method m) noexcept {
	switch (m) {
	case method::get: return "GET";
	case method::post: return "POST";
	case method::put: return "PUT";
	case method::del: return "DELETE";
	}
	return "GET";
}

/// @brief @p m for fmt, via the table above. @see to_string
[[nodiscard]] constexpr std::string_view format_as(method m) noexcept {
	return to_string(m);
}

/**
 * @brief Whether re-sending @p m unchanged is guaranteed to be harmless.
 *
 * RFC 9110 §9.2.2: GET, PUT and DELETE are idempotent, POST is not. This is not
 * a stylistic distinction here - it decides whether a request that went out and
 * was never answered may be sent again. A re-sent GET costs a round trip; a
 * re-sent POST that the venue *did* process is a second order.
 */
[[nodiscard]] constexpr bool is_idempotent(method m) noexcept {
	return m != method::post;
}

} // namespace exchange::transport::rest
