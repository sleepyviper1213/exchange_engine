#pragma once
// Helpers shared by the venue's two JSONL decoders, defined once.
//
// Private to market_data/binance - include only from its sources, never from a
// public header.
//
// `depth_feed.cpp` and `trade_feed.cpp` are the same shape over different
// payloads, and so are `binance_depth.cpp` and `binance_trade.cpp`. Each pair
// had grown an identical copy of these, which compiles per file and is a
// redefinition the moment `ORDER_BOOK_ENABLE_UNITY_BUILD` concatenates the
// module into one translation unit - anonymous namespaces merge along with
// everything else in a batch. Inline in a named namespace is the shape that
// survives it.
//
// @see docs/directory_layout.md, and the same fix in
// transport/rest/detail/wire.hpp

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <simdjson.h>
#include <string_view>

namespace exchange::market_data::binance::detail {

/**
 * @brief Whether @p ch is line whitespace worth trimming before a decode.
 *
 * The carriage return above all: a capture written on one platform and replayed
 * on another carries CRLF, and simdjson would call the trailing return a parse
 * error rather than what it is.
 */
[[nodiscard]] constexpr bool is_blank(char ch) noexcept {
	return ch == ' ' || ch == '\t' || ch == '\r';
}

/// @brief Trim leading and trailing blanks from @p line.
[[nodiscard]] constexpr std::string_view
trimmed(std::string_view line) noexcept {
	while (!line.empty() && is_blank(line.front())) line.remove_prefix(1);
	while (!line.empty() && is_blank(line.back())) line.remove_suffix(1);
	return line;
}

/**
 * @brief Advance past the next non-blank line of @p jsonl and return it,
 *        trimmed.
 *
 * The last thing the two replay feeds had in common and had each written out in
 * full: find the newline, take the line, step past it, count it, trim it, skip
 * it if it is empty. Identical in @c depth_feed.cpp and @c trade_feed.cpp down
 * to the comment, differing only in what the caller then did with the frame.
 *
 * @param jsonl The whole buffer.
 * @param[in,out] at Offset of the next unread line; advanced past what is
 *        returned.
 * @param[in,out] line 1-based line counter; incremented per line consumed,
 *        blank ones included, so it names the returned frame's real position.
 * @return The frame, or nothing once the buffer is spent.
 *
 * @note State by reference rather than a cursor object, because the two feeds
 *       that call this are declared in *public* headers and this one is not
 *       includable from those - it needs @c \<simdjson.h\> for the helper
 *       below. Keeping the state on the feeds costs two out-parameters and
 *       keeps the private header private.
 *
 * @note The line is stepped over *before* the caller can fail on it, which is
 *       what makes a feed resumable after a malformed frame rather than stuck
 *       re-reading it. The advance therefore has to happen even on the caller's
 *       error paths, and it does, because it happens here.
 *
 * @note Blank lines are skipped here rather than by the caller, which is what
 *       lets both feeds drop their loop: one call yields one decodable frame or
 *       says the buffer is finished.
 */
[[nodiscard]] constexpr std::optional<std::string_view>
next_frame(std::string_view jsonl, std::size_t &at,
		   std::uint64_t &line) noexcept {
	while (at < jsonl.size()) {
		const std::size_t eol      = jsonl.find('\n', at);
		const std::string_view raw = jsonl.substr(
			at,
			eol == std::string_view::npos ? std::string_view::npos : eol - at);
		at = eol == std::string_view::npos ? jsonl.size() : eol + 1;
		++line;

		if (const std::string_view frame = trimmed(raw); !frame.empty())
			return frame;
	}
	return std::nullopt;
}

/**
 * @brief Read an optional unsigned scalar, or @p fallback when it is absent or
 *        holds another type.
 *
 * Those two cases leave the iterator usable, so the caller takes the fallback
 * and reads on. Any other error is a structural fault: On-Demand parses lazily,
 * so a document that survived @c iterate() can still turn out to be garbage
 * here, and simdjson has already abandoned the iterator by the time it reports
 * it. Querying such a document again trips its depth assertions, so the error
 * is propagated and parsing stops.
 *
 * @tparam Error The decoder's own parse-error aggregate.
 * @tparam Kind The enumerator it carries for malformed JSON.
 * @note Templated on the error type rather than duplicated per decoder: the
 *       two copies this replaces differed in nothing else, and having the same
 *       function name in two anonymous namespaces with two return types is an
 *       ambiguating redeclaration in a unity batch.
 */
template <typename Error, typename Kind>
[[nodiscard]] std::expected<std::uint64_t, Error>
read_optional_u64(simdjson::ondemand::document &doc, std::string_view key,
				  Kind invalid_json, std::uint64_t fallback = 0) {
	using namespace simdjson;

	std::uint64_t value = fallback;
	const auto err      = doc[key].get(value);
	if (!err || err == NO_SUCH_FIELD || err == INCORRECT_TYPE) return value;
	return std::unexpected(Error{invalid_json, error_message(err)});
}

} // namespace exchange::market_data::binance::detail
